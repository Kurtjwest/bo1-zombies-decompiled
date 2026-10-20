// Shader variants and render pipelines, compiled off the render thread, and the shader cache: a record of the
// pipelines earlier runs created, compiled again in the background as soon as the game loads their shaders.
//
// Metal compiles a Direct3D 9 shader's Metal translation when a draw first needs a variant of it, and a render pipeline
// (the vertex layout, blending and target formats are compiled into the GPU code) when a draw first needs that
// combination. Each takes the Metal compiler milliseconds to tens of milliseconds; done at the draw, the frames where a
// new place, weapon or effect first appears stalled for as long as all of its compiles took. Here:
//  - a draw whose variant or pipeline is not compiled yet is skipped, and its compile goes to the front of the
//    workers' queue (it shows a few frames later); draws that feed an occlusion query wait instead, and so does every
//    draw with BO1_SYNC_SHADERS=1;
//  - every pipeline a draw creates is recorded (in Application Support/BO1Zombies/shadercache), and the next time the
//    game creates the shaders of a recorded pipeline (while a level loads), its variants and the pipeline are compiled
//    in the background, so that they are ready before a draw needs them.
#include "d3d9_internal.h"
#include "../common/platform_bridge.h"
#include "../win32compat/w32_platform.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace d3d9mtl
{
void ShaderVariant::Release()
{
    if (refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    if (fn)
        rhi::ReleaseFunction(fn);
    if (lib)
        rhi::ReleaseLibrary(lib);
    delete this;
}

void PipelineEntry::Release()
{
    if (refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
    if (pipeline)
        rhi::ReleasePipeline(pipeline);
    delete this;
}

namespace shadercache
{
namespace
{
using Clock = std::chrono::steady_clock;

// ---- the compiler: worker threads and their queues -------------------------------------------------------------------
struct Job
{
    ShaderVariant *variant = nullptr;
    PipelineEntry *pipeline = nullptr;
};

struct Compiler
{
    std::mutex lock;
    std::condition_variable work; // workers: a job was queued
    std::condition_variable done; // threads waiting for a compile another thread is running
    std::deque<Job> urgent;       // what draws are waiting for
    std::deque<Job> background;   // the cache's prewarming, in the order the shaders were created
    int running = 0;
    int workers = 0;
};
// Never destroyed: the workers are detached and still blocked on its condition variables when exit() runs the static
// destructors (destroying a condition variable that has waiters is undefined, and blocks forever with glibc).
Compiler &g_compiler = *new Compiler;
std::once_flag g_startOnce;
bool g_sync;

// statistics (for the log)
std::atomic<uint32_t> g_variantsCompiled{0}, g_pipelinesCreated{0};
std::atomic<uint32_t> g_prewarmPipelines{0};
std::atomic<uint64_t> g_compileNs{0};
std::atomic<uint32_t> g_skippedDraws{0}, g_waits{0};
bool g_logVerbose;

// the game folder (main_posix.cpp), where the game keeps everything it writes
std::string GameFolder()
{
    const char *dir = w32::GameDirectory();
    if (dir && *dir)
        return dir;
    const char *appSupport = Bridge_AppSupportPath();
    return appSupport ? appSupport : "";
}

void DumpFailedShader(const ShaderVariant *v, const std::string &src)
{
    if (!getenv("BO1_DUMP_FAILED_SHADERS"))
        return;
    // (in the game folder, as everything the game writes)
    const std::string base = GameFolder();
    if (base.empty())
        return;
    const std::string dir = base + "/shaderdumps";
    mkdir(dir.c_str(), 0755);
    char path[1024];
#if defined(BO1_RHI_OPENGL)
    const char *ext = "glsl";
#else
    const char *ext = "metal";
#endif
    snprintf(path, sizeof(path), "%s/bo1_%s_%u_%llx.%s", dir.c_str(), v->isPixel ? "ps" : "vs", v->shaderId,
             (unsigned long long)v->key, ext);
    if (FILE *f = fopen(path, "w"))
    {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
}

void Finish(std::atomic<int> &state, int value)
{
    state.store(value, std::memory_order_release);
    {
        std::lock_guard<std::mutex> g(g_compiler.lock); // (orders the store before a waiter's check)
    }
    g_compiler.done.notify_all();
}

void WaitFor(const std::atomic<int> &state)
{
    std::unique_lock<std::mutex> lk(g_compiler.lock);
    g_compiler.done.wait(lk, [&] { return state.load(std::memory_order_acquire) != kCompilePending; });
}

// compiles v on this thread unless another thread has claimed it
bool RunVariant(ShaderVariant *v)
{
    if (v->claimed.exchange(true, std::memory_order_acq_rel))
        return false;
    if (v->cancelled.load(std::memory_order_acquire))
    {
        v->source.clear();
        Finish(v->state, kCompileFailed);
        return true;
    }
    const Clock::time_point start = Clock::now();
    std::string src;
    src.swap(v->source);
    char err[4096] = "";
    rhi::Library *lib = rhi::CompileLibrary(src.c_str(), err, sizeof(err));
    rhi::Function *fn = lib ? rhi::GetFunction(lib, "bo1_main") : nullptr;
    g_compileNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count(),
                          std::memory_order_relaxed);
    g_variantsCompiled.fetch_add(1, std::memory_order_relaxed);
    if (!lib)
    {
        Log("%s shader %u (variant %llx) failed to compile:\n%s", v->isPixel ? "pixel" : "vertex", v->shaderId,
            (unsigned long long)v->key, err);
        DumpFailedShader(v, src);
        Finish(v->state, kCompileFailed);
        return true;
    }
    if (!fn)
    {
        Log("%s shader %u: no entry point in the translation", v->isPixel ? "pixel" : "vertex", v->shaderId);
        rhi::ReleaseLibrary(lib);
        Finish(v->state, kCompileFailed);
        return true;
    }
    v->lib = lib;
    v->fn = fn;
    Finish(v->state, kCompileReady);
    return true;
}

// waits until v is compiled, compiling it here if no other thread has started
void EnsureVariant(ShaderVariant *v)
{
    if (v->state.load(std::memory_order_acquire) != kCompilePending)
        return;
    if (!RunVariant(v))
        WaitFor(v->state);
}

bool RunPipeline(PipelineEntry *e)
{
    if (e->claimed.exchange(true, std::memory_order_acq_rel))
        return false;
    // (queued after their variants: those are compiled or being compiled; one nobody started is compiled here)
    ShaderVariant *vs = e->vs, *ps = e->ps;
    e->vs = e->ps = nullptr; // (the pipeline does not need them once it exists)
    EnsureVariant(vs);
    EnsureVariant(ps);
    if (!vs->Ready() || !ps->Ready())
    {
        Finish(e->state, kCompileFailed);
        vs->Release();
        ps->Release();
        return true;
    }
    const Clock::time_point start = Clock::now();
    rhi::PipelineDesc d = e->desc;
    d.vertexFunction = vs->fn;
    d.fragmentFunction = ps->fn;
    char err[1024] = "";
    rhi::Pipeline *p = rhi::CreatePipeline(d, err, sizeof(err));
    if (!p && d.blend[0].enable)
    {
        // a target format that cannot blend (32-bit float on older GPUs): draw unblended rather than not at all
        for (int i = 0; i < 4; ++i)
            d.blend[i].enable = 0;
        p = rhi::CreatePipeline(d, err, sizeof(err));
    }
    g_compileNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count(),
                          std::memory_order_relaxed);
    g_pipelinesCreated.fetch_add(1, std::memory_order_relaxed);
    if (!p)
        LogOnce(err, "pipeline (vs %u / ps %u) failed: %s", e->vsId, e->psId, err);
    e->pipeline = p;
    Finish(e->state, p ? kCompileReady : kCompileFailed);
    vs->Release();
    ps->Release();
    return true;
}

void RunJob(const Job &j)
{
    if (j.variant)
    {
        RunVariant(j.variant);
        j.variant->Release();
    }
    else if (j.pipeline)
    {
        RunPipeline(j.pipeline);
        j.pipeline->Release();
    }
}

void WorkerLoop()
{
#ifdef __MACH__
    // the compiles a draw waits for are as urgent as the frame; prewarming runs while the level loads
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    pthread_setname_np("bo1 shader compiler");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "bo1 shadercomp"); // (15 characters at most)
#endif
    Compiler &c = g_compiler;
    for (;;)
    {
        Job j;
        {
            std::unique_lock<std::mutex> lk(c.lock);
            c.work.wait(lk, [&] { return !c.urgent.empty() || !c.background.empty(); });
            if (!c.urgent.empty())
            {
                j = c.urgent.front();
                c.urgent.pop_front();
            }
            else
            {
                j = c.background.front();
                c.background.pop_front();
            }
            ++c.running;
        }
        RunJob(j);
        {
            std::lock_guard<std::mutex> g(c.lock);
            --c.running;
        }
        c.done.notify_all();
    }
}

void StartWorkers()
{
    g_sync = getenv("BO1_SYNC_SHADERS") && atoi(getenv("BO1_SYNC_SHADERS")) != 0;
    g_logVerbose = getenv("BO1_SHADER_LOG") && atoi(getenv("BO1_SHADER_LOG")) != 0;
    // the Metal compiler runs in its own processes; a few requests in flight keep it busy
    unsigned n = std::thread::hardware_concurrency();
    n = n > 8 ? 4 : n > 4 ? 3 : 2;
    g_compiler.workers = (int)n;
    for (unsigned i = 0; i < n; ++i)
        std::thread(WorkerLoop).detach();
}

void Push(const Job &j, bool urgent)
{
    {
        std::lock_guard<std::mutex> g(g_compiler.lock);
        (urgent ? g_compiler.urgent : g_compiler.background).push_back(j);
    }
    g_compiler.work.notify_one();
}

// ---- the record --------------------------------------------------------------------------------------------------
// shadercache/pipelines.bin: a header, then one record per pipeline a draw created: the hashes of its two shaders'
// bytecode and its PipelineKey without the run's shader ids. A record that was cut short (the game ended while it
// was written) is dropped. A file of another version or key size is set aside and started over.
constexpr char kMagic[8] = {'B', 'O', '1', 'P', 'S', 'O', 'C', '1'};
constexpr uint32_t kVersion = 1; // (raise when the variant keys or prefixes change meaning)

struct FileHeader
{
    char magic[8];
    uint32_t version;
    uint32_t keySize;
};

struct Record
{
    uint64_t vsHash;
    uint64_t psHash;
    PipelineKey key; // vsId / psId zero
};
struct RecordHash
{
    size_t operator()(const Record &r) const noexcept
    {
        return (size_t)HashBytes(&r.key, kPipelineKeyBytes, r.vsHash * 0x9E3779B97F4A7C15ull ^ r.psHash);
    }
};
struct RecordEq
{
    bool operator()(const Record &a, const Record &b) const noexcept
    {
        return a.vsHash == b.vsHash && a.psHash == b.psHash && PipelineKeyEq()(a.key, b.key);
    }
};

// (all guarded by the device lock: shaders are created on the loading threads, pipelines on the render thread; never
// destroyed either, so a thread still drawing or loading when exit() runs does not touch freed containers)
std::vector<Record> &g_records = *new std::vector<Record>;
std::unordered_set<Record, RecordHash, RecordEq> &g_known = *new std::unordered_set<Record, RecordHash, RecordEq>;
std::unordered_multimap<uint64_t, uint32_t> &g_byVs = *new std::unordered_multimap<uint64_t, uint32_t>; // hash -> index
std::unordered_multimap<uint64_t, uint32_t> &g_byPs = *new std::unordered_multimap<uint64_t, uint32_t>;
// the shader codes some shader object uses now (ShaderCode::liveObjects), by hash
std::unordered_map<uint64_t, ShaderCode *> &g_liveVs = *new std::unordered_map<uint64_t, ShaderCode *>;
std::unordered_map<uint64_t, ShaderCode *> &g_livePs = *new std::unordered_map<uint64_t, ShaderCode *>;
std::vector<Record> &g_unwritten = *new std::vector<Record>;
std::string &g_path = *new std::string;
FILE *g_file;
bool g_loaded;
Clock::time_point g_lastWrite;
uint32_t g_loadedCount;

void Index(uint32_t i)
{
    const Record &r = g_records[i];
    g_byVs.emplace(r.vsHash, i);
    g_byPs.emplace(r.psHash, i);
}

void Load()
{
    g_loaded = true;
    const std::string base = GameFolder();
    if (base.empty())
        return;
    std::string dir = base + "/shadercache";
    mkdir(dir.c_str(), 0755);
    g_path = dir + "/pipelines.bin";
    FILE *f = fopen(g_path.c_str(), "rb");
    long good = 0;
    bool fresh = true;
    if (f)
    {
        FileHeader h;
        if (fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, kMagic, sizeof(kMagic)) && h.version == kVersion &&
            h.keySize == sizeof(PipelineKey))
        {
            fresh = false;
            good = (long)sizeof(h);
            Record r;
            while (fread(&r, sizeof(r), 1, f) == 1)
            {
                good += (long)sizeof(r);
                if (g_known.insert(r).second)
                {
                    g_records.push_back(r);
                    Index((uint32_t)g_records.size() - 1);
                }
            }
        }
        fclose(f);
        if (fresh)
        {
            // another version's: kept beside the new one
            const std::string old = g_path + ".old";
            rename(g_path.c_str(), old.c_str());
        }
    }
    g_loadedCount = (uint32_t)g_records.size();
    if (!fresh)
    {
        if (truncate(g_path.c_str(), good) != 0)
            Log("shader cache: cannot trim %s (%s)", g_path.c_str(), strerror(errno));
        g_file = fopen(g_path.c_str(), "ab");
    }
    else
    {
        g_file = fopen(g_path.c_str(), "wb");
        if (g_file)
        {
            FileHeader h;
            memcpy(h.magic, kMagic, sizeof(kMagic));
            h.version = kVersion;
            h.keySize = sizeof(PipelineKey);
            fwrite(&h, sizeof(h), 1, g_file);
            fflush(g_file);
        }
    }
    if (!g_file)
        Log("shader cache: cannot write %s (%s)", g_path.c_str(), strerror(errno));
    Log("shader cache: %u pipelines recorded by earlier runs (%s)", g_loadedCount, g_path.c_str());
}

void WriteUnwritten()
{
    if (g_file && !g_unwritten.empty())
    {
        fwrite(g_unwritten.data(), sizeof(Record), g_unwritten.size(), g_file);
        fflush(g_file);
    }
    g_unwritten.clear();
    g_lastWrite = Clock::now();
}

// ---- frame statistics (the log) ----------------------------------------------------------------------------------
Clock::time_point g_lastPresent, g_lastHitchLog, g_prewarmStart;
uint32_t g_lastVariants, g_lastPipelines;     // at the start of the prewarming
uint32_t g_frameVariants, g_framePipelines;   // at the last present
bool g_prewarmActive;
} // namespace

void Start()
{
    std::call_once(g_startOnce, StartWorkers);
    if (!g_loaded)
        Load();
}

bool SyncCompiles() { return g_sync; }

void Queue(ShaderVariant *v, bool urgent)
{
    if (urgent && v->urgentQueued.exchange(true, std::memory_order_acq_rel))
        return; // (already at the front)
    v->Ref();
    Push(Job{v, nullptr}, urgent);
}

void Queue(PipelineEntry *e, bool urgent)
{
    if (urgent && e->urgentQueued.exchange(true, std::memory_order_acq_rel))
        return;
    e->Ref();
    Push(Job{nullptr, e}, urgent);
}

void CompileVariantNow(ShaderVariant *v)
{
    g_waits.fetch_add(1, std::memory_order_relaxed);
    EnsureVariant(v);
}

void CreatePipelineNow(PipelineEntry *e)
{
    g_waits.fetch_add(1, std::memory_order_relaxed);
    if (e->state.load(std::memory_order_acquire) != kCompilePending)
        return;
    if (!RunPipeline(e))
        WaitFor(e->state);
}

void NoteSkippedDraw() { g_skippedDraws.fetch_add(1, std::memory_order_relaxed); }

void Drain()
{
    // queued work is dropped (its variants and pipelines stay pending, never drawn); running work finishes
    std::deque<Job> dropped;
    {
        std::unique_lock<std::mutex> lk(g_compiler.lock);
        dropped.swap(g_compiler.urgent);
        for (const Job &j : g_compiler.background)
            dropped.push_back(j);
        g_compiler.background.clear();
        g_compiler.done.wait(lk, [&] { return g_compiler.running == 0; });
    }
    for (const Job &j : dropped)
    {
        // (a dropped compile can be queued again by the next draw that needs it)
        if (j.variant)
        {
            j.variant->urgentQueued.store(false, std::memory_order_relaxed);
            j.variant->Release();
        }
        if (j.pipeline)
        {
            j.pipeline->urgentQueued.store(false, std::memory_order_relaxed);
            j.pipeline->Release();
        }
    }
    WriteUnwritten();
}

void RecordPipeline(const ShaderBase &vs, const ShaderBase &ps, const PipelineKey &key)
{
    Record r;
    memset(&r, 0, sizeof(r)); // (padding included: records are written as bytes)
    r.vsHash = vs.m_hash;
    r.psHash = ps.m_hash;
    memcpy(&r.key, &key, kPipelineKeyBytes);
    r.key.vsId = 0;
    r.key.psId = 0;
    if (!g_known.insert(r).second)
        return;
    g_records.push_back(r);
    Index((uint32_t)g_records.size() - 1);
    g_unwritten.push_back(r);
}

void OnShaderCreated(Device *dev, ShaderBase *s)
{
    ShaderCode *c = s->m_shared;
    if (!c || !c->info.valid || c->liveObjects != 1)
        return; // (another shader object with this code exists: its pipelines were warmed up with the first)
    const bool pixel = c->info.isPixel;
    (pixel ? g_livePs : g_liveVs)[c->hash] = c;
    auto range = (pixel ? g_byPs : g_byVs).equal_range(c->hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        const Record &r = g_records[it->second];
        auto other = pixel ? g_liveVs.find(r.vsHash) : g_livePs.find(r.psHash);
        if (other == (pixel ? g_liveVs.end() : g_livePs.end()))
            continue; // (the record is warmed up when its other shader is created)
        ShaderCode *vs = pixel ? other->second : c;
        ShaderCode *ps = pixel ? c : other->second;
        PipelineKey k;
        memcpy(&k, &r.key, sizeof(k));
        k.vsId = vs->id;
        k.psId = ps->id;
        if (dev->Prewarm(k, vs, ps))
        {
            if (!g_prewarmActive)
            {
                g_prewarmActive = true;
                g_prewarmStart = Clock::now();
                g_lastVariants = g_variantsCompiled.load();
                g_lastPipelines = g_pipelinesCreated.load();
            }
            g_prewarmPipelines.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void OnShaderDestroyed(ShaderBase *s)
{
    ShaderCode *c = s->m_shared;
    if (!c || c->liveObjects)
        return;
    auto &live = c->info.isPixel ? g_livePs : g_liveVs;
    auto it = live.find(c->hash);
    if (it != live.end() && it->second == c)
        live.erase(it);
}

void OnPresent()
{
    const Clock::time_point now = Clock::now();
    const double frameMs =
        g_lastPresent.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(now - g_lastPresent).count() : 0;
    g_lastPresent = now;
    if (!g_unwritten.empty() && (g_unwritten.size() >= 256 || now - g_lastWrite > std::chrono::seconds(2)))
        WriteUnwritten();

    bool idle;
    {
        std::lock_guard<std::mutex> g(g_compiler.lock);
        idle = g_compiler.urgent.empty() && g_compiler.background.empty() && g_compiler.running == 0;
    }
    if (g_prewarmActive && idle)
    {
        // (the bigger warm-ups only: shaders are created a few at a time between them)
        g_prewarmActive = false;
        const uint32_t warmed = g_prewarmPipelines.exchange(0);
        if (warmed >= 32 || g_logVerbose)
            Log("shader cache: %u pipelines warmed up (%u shader variants, %u pipelines compiled) in %.0f ms", warmed,
                g_variantsCompiled.load() - g_lastVariants, g_pipelinesCreated.load() - g_lastPipelines,
                std::chrono::duration<double, std::milli>(now - g_prewarmStart).count());
    }
    // A frame that took long because draws waited for compiles (with BO1_SHADER_LOG=1: every frame that compiled
    // anything): what the compiler did meanwhile. At most every few seconds.
    const uint32_t skipped = g_skippedDraws.exchange(0), waits = g_waits.exchange(0);
    const uint32_t variants = g_variantsCompiled.load(), pipelines = g_pipelinesCreated.load();
    const uint32_t frameVariants = variants - g_frameVariants, framePipelines = pipelines - g_framePipelines;
    g_frameVariants = variants;
    g_framePipelines = pipelines;
    const bool compiled = frameVariants || framePipelines || skipped || waits;
    if ((g_logVerbose && compiled) || (waits && frameMs > 100.0 && now - g_lastHitchLog > std::chrono::seconds(5)))
    {
        g_lastHitchLog = now;
        Log("frame took %.0f ms: %u shader variants and %u pipelines compiled meanwhile, %u draws waited for theirs, "
            "%u were skipped until theirs are ready (%u variants, %u pipelines and %.0f ms of compiling so far)",
            frameMs, frameVariants, framePipelines, waits, skipped, variants, pipelines, g_compileNs.load() / 1e6);
    }
}
} // namespace shadercache
} // namespace d3d9mtl
