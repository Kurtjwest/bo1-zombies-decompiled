// Win32 file API on POSIX for the macOS port: CreateFile/ReadFile(Ex)/WriteFile, overlapped I/O with completion
// routines (delivered as APCs in alertable waits, as on Windows), directory enumeration with wildcards, and
// Windows path handling (backslashes, drive letters, case-insensitive name resolution).

#include <windows.h>
#include <io.h>
#include "kernel32_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <copyfile.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace w32
{
KFile::~KFile() {}
void KFile::OnClose()
{
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
    if (deleteOnClose && !path.empty())
        unlink(path.c_str());
}
KMapping::~KMapping()
{
    if (fd >= 0)
        close(fd);
}

static std::string Normalize(const char *winPath)
{
    std::string p = winPath ? winPath : "";
    for (char &c : p)
        if (c == '\\')
            c = '/';
    // "C:/..." -> "/..." (a drive letter is meaningless here; absolute Windows paths are taken as rooted)
    if (p.size() >= 2 && isalpha((unsigned char)p[0]) && p[1] == ':')
        p = p.substr(2);
    // collapse duplicate separators
    std::string out;
    out.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i)
    {
        if (p[i] == '/' && !out.empty() && out.back() == '/')
            continue;
        out.push_back(p[i]);
    }
    return out;
}

bool ResolveCaseInsensitive(const std::string &path, std::string &out)
{
    struct stat st;
    if (lstat(path.c_str(), &st) == 0)
    {
        out = path;
        return true;
    }
    // walk component by component
    std::string cur = path.size() && path[0] == '/' ? "/" : "";
    size_t i = cur.size();
    while (i <= path.size())
    {
        size_t j = path.find('/', i);
        if (j == std::string::npos)
            j = path.size();
        std::string comp = path.substr(i, j - i);
        if (!comp.empty() && comp != ".")
        {
            std::string base = cur.empty() ? "." : cur;
            std::string cand = (cur.empty() || cur.back() == '/') ? cur + comp : cur + "/" + comp;
            if (comp == ".." || lstat(cand.c_str(), &st) == 0)
            {
                cur = cand;
            }
            else
            {
                DIR *d = opendir(base.c_str());
                if (!d)
                    return false;
                bool found = false;
                while (dirent *e = readdir(d))
                {
                    if (strcasecmp(e->d_name, comp.c_str()) == 0)
                    {
                        cur = (cur.empty() || cur.back() == '/') ? cur + e->d_name : cur + "/" + e->d_name;
                        found = true;
                        break;
                    }
                }
                closedir(d);
                if (!found)
                    return false;
            }
        }
        i = j + 1;
    }
    out = cur.empty() ? path : cur;
    return true;
}

std::string HostPath(const char *winPath)
{
    std::string p = Normalize(winPath);
    std::string r;
    if (ResolveCaseInsensitive(p, r))
        return r;
    // the final component may not exist yet (creation): resolve the directory part only
    size_t slash = p.rfind('/');
    if (slash != std::string::npos && slash > 0)
    {
        std::string dir;
        if (ResolveCaseInsensitive(p.substr(0, slash), dir))
            return dir + p.substr(slash);
    }
    return p;
}

static void UnixToFileTime(const struct timespec &ts, FILETIME *ft)
{
    unsigned long long v = (unsigned long long)ts.tv_sec * 10000000ull + (unsigned long long)ts.tv_nsec / 100ull + 116444736000000000ull;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

static DWORD AttributesFromStat(const struct stat &st, const char *name)
{
    DWORD a = 0;
    if (S_ISDIR(st.st_mode))
        a |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st.st_mode & S_IWUSR))
        a |= FILE_ATTRIBUTE_READONLY;
    if (name && name[0] == '.' && strcmp(name, ".") && strcmp(name, ".."))
        a |= FILE_ATTRIBUTE_HIDDEN;
    if (!a)
        a = FILE_ATTRIBUTE_NORMAL;
    return a;
}

static KFile *AsFile(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return nullptr;
    KObject *o = (KObject *)h;
    return o->type == KType::File ? static_cast<KFile *>(o) : nullptr;
}

static KFile g_stdHandles[3];
static bool g_stdInit;
static void InitStd()
{
    if (g_stdInit)
        return;
    g_stdInit = true;
    for (int i = 0; i < 3; ++i)
    {
        g_stdHandles[i].fd = i;
        g_stdHandles[i].refs = 1 << 30;
    }
}

// Wildcard match with Windows semantics ("*.*" matches names without a dot too), case-insensitive.
static bool WildMatch(const char *pattern, const char *name)
{
    if (!strcmp(pattern, "*.*") || !strcmp(pattern, "*"))
        return true;
    return fnmatch(pattern, name, FNM_CASEFOLD) == 0;
}

static void FillFindData(const std::string &dir, const std::string &name, WIN32_FIND_DATAA *fd)
{
    memset(fd, 0, sizeof(*fd));
    std::string full = dir.empty() ? name : dir + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) == 0)
    {
        fd->dwFileAttributes = AttributesFromStat(st, name.c_str());
        fd->nFileSizeLow = (DWORD)st.st_size;
        fd->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
        UnixToFileTime(st.st_birthtimespec, &fd->ftCreationTime);
        UnixToFileTime(st.st_atimespec, &fd->ftLastAccessTime);
        UnixToFileTime(st.st_mtimespec, &fd->ftLastWriteTime);
    }
    else
    {
        fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
    strncpy(fd->cFileName, name.c_str(), sizeof(fd->cFileName) - 1);
}

static KFind *FindOpen(const char *spec)
{
    std::string p = Normalize(spec);
    std::string dir, pattern;
    size_t slash = p.rfind('/');
    if (slash == std::string::npos)
    {
        dir = ".";
        pattern = p;
    }
    else
    {
        dir = slash == 0 ? "/" : p.substr(0, slash);
        pattern = p.substr(slash + 1);
    }
    std::string rdir;
    if (!ResolveCaseInsensitive(dir, rdir))
        return nullptr;
    DIR *d = opendir(rdir.c_str());
    if (!d)
        return nullptr;
    KFind *f = new KFind();
    f->dir = rdir;
    while (dirent *e = readdir(d))
    {
        if (WildMatch(pattern.c_str(), e->d_name))
            f->names.push_back(e->d_name);
    }
    closedir(d);
    if (f->names.empty())
    {
        f->Unref();
        return nullptr;
    }
    return f;
}
} // namespace w32

using namespace w32;

extern "C" {

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD flags, HANDLE)
{
    (void)share;
    if (!name)
    {
        SetError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    std::string path = HostPath(name);
    int oflags = 0;
    const bool rd = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0;
    const bool wr = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    if (rd && wr)
        oflags = O_RDWR;
    else if (wr)
        oflags = O_WRONLY;
    else
        oflags = O_RDONLY;
    switch (disposition)
    {
    case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
    default: break;
    }
    struct stat st;
    bool existed = stat(path.c_str(), &st) == 0;
    if (existed && S_ISDIR(st.st_mode) && !(flags & FILE_FLAG_BACKUP_SEMANTICS))
    {
        SetError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    int fd = open(path.c_str(), oflags | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        SetError(ErrnoToWin32(errno));
        return INVALID_HANDLE_VALUE;
    }
    if (flags & FILE_FLAG_NO_BUFFERING)
        fcntl(fd, F_NOCACHE, 1);
    KFile *f = new KFile();
    f->fd = fd;
    f->path = path;
    f->append = (access & FILE_APPEND_DATA) && !(access & FILE_WRITE_DATA) && !(access & GENERIC_WRITE);
    f->deleteOnClose = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    // Windows reports ERROR_ALREADY_EXISTS for OPEN_ALWAYS/CREATE_ALWAYS on an existing file
    SetError((existed && (disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS)) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return (HANDLE)f;
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE tmpl)
{
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), nullptr, nullptr);
    return CreateFileA(buf, access, share, sa, disposition, flags, tmpl);
}

static BOOL DoRead(KFile *f, LPVOID buf, DWORD n, LPDWORD out, LPOVERLAPPED ov)
{
    ssize_t r;
    if (ov)
    {
        off_t off = (off_t)(((unsigned long long)ov->OffsetHigh << 32) | ov->Offset);
        r = pread(f->fd, buf, n, off);
    }
    else
    {
        r = read(f->fd, buf, n);
    }
    if (r < 0)
    {
        SetError(ErrnoToWin32(errno));
        if (out)
            *out = 0;
        if (ov)
        {
            ov->Internal = (ULONG_PTR)ERROR_INVALID_FUNCTION;
            ov->InternalHigh = 0;
        }
        return FALSE;
    }
    if (out)
        *out = (DWORD)r;
    if (ov)
    {
        ov->Internal = 0;
        ov->InternalHigh = (ULONG_PTR)r;
        if (r == 0 && n)
        {
            ov->Internal = ERROR_HANDLE_EOF;
            SetError(ERROR_HANDLE_EOF);
        }
        if (ov->hEvent)
            SetEvent(ov->hEvent);
    }
    return TRUE;
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD out, LPOVERLAPPED ov)
{
    InitStd();
    KFile *f = AsFile(h);
    if (!f)
    {
        SetError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return DoRead(f, buf, n, out, ov);
}

namespace
{
struct Completion
{
    LPOVERLAPPED_COMPLETION_ROUTINE routine;
    DWORD error;
    DWORD bytes;
    LPOVERLAPPED ov;
};
void RunCompletion(void *p)
{
    Completion *c = (Completion *)p;
    c->routine(c->error, c->bytes, c->ov);
    delete c;
}
} // namespace

// The read is performed immediately; its completion routine runs when the thread next waits alertably, exactly
// when Windows would call it.
BOOL ReadFileEx(HANDLE h, LPVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE routine)
{
    KFile *f = AsFile(h);
    if (!f || !ov)
    {
        SetError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DWORD got = 0;
    BOOL ok = DoRead(f, buf, n, &got, ov);
    if (!ok)
        return FALSE;
    if (routine)
        QueueApc(CurrentThread(), RunCompletion, new Completion{routine, (DWORD)ov->Internal == ERROR_HANDLE_EOF ? (DWORD)ERROR_HANDLE_EOF : 0u, got, ov});
    SetError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD out, LPOVERLAPPED ov)
{
    InitStd();
    KFile *f = (h == GetStdHandle(STD_OUTPUT_HANDLE) || h == GetStdHandle(STD_ERROR_HANDLE)) ? (KFile *)h : AsFile(h);
    if (!f)
    {
        SetError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    ssize_t r;
    if (ov && !f->append)
    {
        off_t off = (off_t)(((unsigned long long)ov->OffsetHigh << 32) | ov->Offset);
        r = pwrite(f->fd, buf, n, off);
    }
    else
    {
        if (f->append)
            lseek(f->fd, 0, SEEK_END);
        r = write(f->fd, buf, n);
    }
    if (r < 0)
    {
        SetError(ErrnoToWin32(errno));
        if (out)
            *out = 0;
        return FALSE;
    }
    if (out)
        *out = (DWORD)r;
    if (ov)
    {
        ov->Internal = 0;
        ov->InternalHigh = (ULONG_PTR)r;
        if (ov->hEvent)
            SetEvent(ov->hEvent);
    }
    return TRUE;
}

BOOL WriteFileEx(HANDLE h, LPCVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE routine)
{
    DWORD wrote = 0;
    if (!WriteFile(h, buf, n, &wrote, ov))
        return FALSE;
    if (routine)
        QueueApc(CurrentThread(), RunCompletion, new Completion{routine, 0, wrote, ov});
    return TRUE;
}

BOOL GetOverlappedResult(HANDLE, LPOVERLAPPED ov, LPDWORD bytes, BOOL)
{
    if (!ov)
        return FALSE;
    if (bytes)
        *bytes = (DWORD)ov->InternalHigh;
    if (ov->Internal && ov->Internal != ERROR_HANDLE_EOF)
    {
        SetError((DWORD)ov->Internal);
        return FALSE;
    }
    if (ov->Internal == ERROR_HANDLE_EOF)
    {
        SetError(ERROR_HANDLE_EOF);
        return ov->InternalHigh != 0;
    }
    return TRUE;
}
BOOL CancelIo(HANDLE) { return TRUE; }
BOOL CancelIoEx(HANDLE, LPOVERLAPPED) { return TRUE; }

BOOL FlushFileBuffers(HANDLE h)
{
    KFile *f = AsFile(h);
    return f && fsync(f->fd) == 0;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    KFile *f = AsFile(h);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0)
    {
        SetError(ERROR_INVALID_HANDLE);
        return INVALID_FILE_SIZE;
    }
    if (high)
        *high = (DWORD)((unsigned long long)st.st_size >> 32);
    return (DWORD)st.st_size;
}
BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    KFile *f = AsFile(h);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0)
        return FALSE;
    size->QuadPart = st.st_size;
    return TRUE;
}
DWORD SetFilePointer(HANDLE h, LONG dist, PLONG high, DWORD method)
{
    KFile *f = AsFile(h);
    if (!f)
    {
        SetError(ERROR_INVALID_HANDLE);
        return INVALID_SET_FILE_POINTER;
    }
    long long d = high ? (((long long)*high << 32) | (unsigned int)dist) : (long long)dist;
    int whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    off_t r = lseek(f->fd, (off_t)d, whence);
    if (r < 0)
    {
        SetError(ErrnoToWin32(errno));
        return INVALID_SET_FILE_POINTER;
    }
    if (high)
        *high = (LONG)((unsigned long long)r >> 32);
    SetError(ERROR_SUCCESS);
    return (DWORD)r;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newPos, DWORD method)
{
    KFile *f = AsFile(h);
    if (!f)
        return FALSE;
    int whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    off_t r = lseek(f->fd, (off_t)dist.QuadPart, whence);
    if (r < 0)
        return FALSE;
    if (newPos)
        newPos->QuadPart = r;
    return TRUE;
}
BOOL SetEndOfFile(HANDLE h)
{
    KFile *f = AsFile(h);
    if (!f)
        return FALSE;
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    return ftruncate(f->fd, pos) == 0;
}
DWORD GetFileType(HANDLE h)
{
    InitStd();
    if (h == GetStdHandle(STD_OUTPUT_HANDLE) || h == GetStdHandle(STD_ERROR_HANDLE) || h == GetStdHandle(STD_INPUT_HANDLE))
        return FILE_TYPE_CHAR;
    return AsFile(h) ? FILE_TYPE_DISK : FILE_TYPE_UNKNOWN;
}
BOOL GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w)
{
    KFile *f = AsFile(h);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0)
        return FALSE;
    if (c)
        UnixToFileTime(st.st_birthtimespec, c);
    if (a)
        UnixToFileTime(st.st_atimespec, a);
    if (w)
        UnixToFileTime(st.st_mtimespec, w);
    return TRUE;
}
BOOL SetFileTime(HANDLE, const FILETIME *, const FILETIME *, const FILETIME *) { return TRUE; }
BOOL GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info)
{
    KFile *f = AsFile(h);
    struct stat st;
    if (!f || !info || fstat(f->fd, &st) != 0)
        return FALSE;
    memset(info, 0, sizeof(*info));
    info->dwFileAttributes = AttributesFromStat(st, nullptr);
    UnixToFileTime(st.st_birthtimespec, &info->ftCreationTime);
    UnixToFileTime(st.st_atimespec, &info->ftLastAccessTime);
    UnixToFileTime(st.st_mtimespec, &info->ftLastWriteTime);
    info->dwVolumeSerialNumber = (DWORD)st.st_dev;
    info->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
    info->nFileSizeLow = (DWORD)st.st_size;
    info->nNumberOfLinks = (DWORD)st.st_nlink;
    info->nFileIndexHigh = (DWORD)((unsigned long long)st.st_ino >> 32);
    info->nFileIndexLow = (DWORD)st.st_ino;
    return TRUE;
}

DWORD GetFileAttributesA(LPCSTR name)
{
    std::string p = HostPath(name);
    struct stat st;
    if (stat(p.c_str(), &st) != 0)
    {
        SetError(ErrnoToWin32(errno));
        return INVALID_FILE_ATTRIBUTES;
    }
    const char *base = strrchr(p.c_str(), '/');
    return AttributesFromStat(st, base ? base + 1 : p.c_str());
}
DWORD GetFileAttributesW(LPCWSTR name)
{
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), nullptr, nullptr);
    return GetFileAttributesA(buf);
}
BOOL GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS, LPVOID out)
{
    std::string p = HostPath(name);
    struct stat st;
    if (stat(p.c_str(), &st) != 0)
    {
        SetError(ErrnoToWin32(errno));
        return FALSE;
    }
    WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)out;
    d->dwFileAttributes = AttributesFromStat(st, nullptr);
    UnixToFileTime(st.st_birthtimespec, &d->ftCreationTime);
    UnixToFileTime(st.st_atimespec, &d->ftLastAccessTime);
    UnixToFileTime(st.st_mtimespec, &d->ftLastWriteTime);
    d->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}
BOOL SetFileAttributesA(LPCSTR name, DWORD attrs)
{
    std::string p = HostPath(name);
    struct stat st;
    if (stat(p.c_str(), &st) != 0)
        return FALSE;
    mode_t m = st.st_mode;
    if (attrs & FILE_ATTRIBUTE_READONLY)
        m &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    else
        m |= S_IWUSR;
    return chmod(p.c_str(), m) == 0;
}
BOOL DeleteFileA(LPCSTR name)
{
    std::string p = HostPath(name);
    if (unlink(p.c_str()) != 0)
    {
        SetError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}
BOOL DeleteFileW(LPCWSTR name)
{
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), nullptr, nullptr);
    return DeleteFileA(buf);
}
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL failIfExists)
{
    std::string s = HostPath(from), d = HostPath(to);
    struct stat st;
    if (failIfExists && stat(d.c_str(), &st) == 0)
    {
        SetError(ERROR_FILE_EXISTS);
        return FALSE;
    }
    if (copyfile(s.c_str(), d.c_str(), nullptr, COPYFILE_ALL) != 0)
    {
        SetError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}
BOOL MoveFileA(LPCSTR from, LPCSTR to)
{
    std::string s = HostPath(from), d = HostPath(to);
    struct stat st;
    if (stat(d.c_str(), &st) == 0)
    {
        SetError(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    if (rename(s.c_str(), d.c_str()) != 0)
    {
        SetError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
    std::string s = HostPath(from), d = HostPath(to);
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(d.c_str(), &st) == 0)
    {
        SetError(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    if (rename(s.c_str(), d.c_str()) != 0)
    {
        if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED) && copyfile(s.c_str(), d.c_str(), nullptr, COPYFILE_ALL) == 0)
        {
            unlink(s.c_str());
            return TRUE;
        }
        SetError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}
BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES)
{
    std::string p = HostPath(name);
    if (mkdir(p.c_str(), 0755) != 0)
    {
        SetError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}
BOOL CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES sa)
{
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), nullptr, nullptr);
    return CreateDirectoryA(buf, sa);
}
BOOL RemoveDirectoryA(LPCSTR name)
{
    std::string p = HostPath(name);
    if (rmdir(p.c_str()) != 0)
    {
        SetError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}
int _mkdir(const char *path)
{
    std::string p = HostPath(path);
    return mkdir(p.c_str(), 0755);
}

// CRT path translation (msvc_crt.h maps the engine's fopen/stat/rename/... onto these)
const char *bo1_hostpath(const char *path, char *buf, size_t size)
{
    if (!path || !buf || !size)
        return path;
    std::string p = HostPath(path);
    size_t n = p.size() < size - 1 ? p.size() : size - 1;
    memcpy(buf, p.data(), n);
    buf[n] = 0;
    return buf;
}
// MSVC's text mode for reading: "\r\n" reads as "\n", and Ctrl-Z (0x1A) ends the file. The game's text files have
// Windows line endings (localization.txt starts "english\r\n", and the language name read from it goes into every
// localized path: zone\english\...), so a file opened for reading in text mode is converted into an anonymous
// temporary file, which then behaves as the CRT's text stream did (sizes and positions are those of the converted
// text). If no temporary file can be made, the file is read as it is.
static FILE *OpenTextForReading(const std::string &path)
{
    FILE *raw = fopen(path.c_str(), "rb");
    if (!raw)
        return nullptr;
    FILE *out = tmpfile();
    if (!out)
        return raw;
    static const size_t kChunk = 64 * 1024;
    std::vector<char> in(kChunk), conv(kChunk + 2);
    bool cr = false; // a '\r' not yet written: dropped if a '\n' follows
    bool end = false;
    size_t n;
    while (!end && (n = fread(in.data(), 1, kChunk, raw)) > 0)
    {
        size_t o = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const char c = in[i];
            if (c == 0x1A)
            {
                end = true;
                break;
            }
            if (cr)
            {
                cr = false;
                if (c != '\n')
                    conv[o++] = '\r';
            }
            if (c == '\r')
            {
                cr = true;
                continue;
            }
            conv[o++] = c;
        }
        if (o && fwrite(conv.data(), 1, o, out) != o)
        {
            // (out of temporary space) the file as it is, from the start
            fclose(out);
            rewind(raw);
            return raw;
        }
    }
    if (cr)
        fputc('\r', out);
    fclose(raw);
    rewind(out);
    return out;
}

FILE *bo1_fopen(const char *path, const char *mode)
{
    if (!path || !mode)
    {
        errno = EINVAL;
        return nullptr;
    }
    // MSVC accepts mode flags POSIX doesn't know ('t' text, 'c'/'n' commit, 'S'/'R'/'T'/'D' hints, ccs=...)
    char m[16];
    size_t j = 0;
    for (const char *s = mode; *s && j < sizeof(m) - 1; ++s)
    {
        if (*s == ',')
            break;
        if (*s == 'r' || *s == 'w' || *s == 'a' || *s == '+' || *s == 'b' || *s == 'x')
            m[j++] = *s;
    }
    m[j] = 0;
    std::string p = HostPath(path);
    // without 'b' the CRT opens in text mode (its default _fmode); reading is where that changes the bytes
    if (m[0] == 'r' && !strchr(m, '+') && !strchr(m, 'b'))
        return OpenTextForReading(p);
    return fopen(p.c_str(), m);
}
FILE *bo1_freopen(const char *path, const char *mode, FILE *f)
{
    if (!path)
        return freopen(nullptr, mode, f);
    std::string p = HostPath(path);
    return freopen(p.c_str(), mode, f);
}
int bo1_stat(const char *path, struct stat *st)
{
    std::string p = HostPath(path);
    return stat(p.c_str(), st);
}
int bo1_rename(const char *from, const char *to)
{
    std::string s = HostPath(from), d = HostPath(to);
    return rename(s.c_str(), d.c_str());
}
int bo1_remove(const char *path)
{
    std::string p = HostPath(path);
    return remove(p.c_str());
}
DWORD GetCurrentDirectoryA(DWORD n, LPSTR buf)
{
    char tmp[4096];
    if (!getcwd(tmp, sizeof(tmp)))
        return 0;
    DWORD len = (DWORD)strlen(tmp);
    if (!buf || n <= len)
        return len + 1;
    memcpy(buf, tmp, len + 1);
    return len;
}
BOOL SetCurrentDirectoryA(LPCSTR name)
{
    std::string p = HostPath(name);
    if (chdir(p.c_str()) != 0)
    {
        SetError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}
DWORD GetFullPathNameA(LPCSTR name, DWORD n, LPSTR buf, LPSTR *filePart)
{
    std::string p = Normalize(name);
    if (p.empty() || p[0] != '/')
    {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd)))
            cwd[0] = 0;
        p = std::string(cwd) + "/" + p;
    }
    // resolve "." and ".." lexically
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size())
    {
        size_t j = p.find('/', i);
        if (j == std::string::npos)
            j = p.size();
        std::string c = p.substr(i, j - i);
        if (c == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (!c.empty() && c != ".")
            parts.push_back(c);
        i = j + 1;
    }
    std::string out;
    for (auto &c : parts)
        out += "/" + c;
    if (out.empty())
        out = "/";
    if (!buf || n <= out.size())
        return (DWORD)out.size() + 1;
    memcpy(buf, out.c_str(), out.size() + 1);
    if (filePart)
    {
        char *s = strrchr(buf, '/');
        *filePart = s ? s + 1 : buf;
    }
    return (DWORD)out.size();
}
DWORD GetTempPathA(DWORD n, LPSTR buf)
{
    const char *t = getenv("TMPDIR");
    std::string p = t ? t : "/tmp/";
    if (p.back() != '/')
        p += '/';
    if (!buf || n <= p.size())
        return (DWORD)p.size() + 1;
    memcpy(buf, p.c_str(), p.size() + 1);
    return (DWORD)p.size();
}
UINT GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR out)
{
    static unsigned counter = (unsigned)getpid();
    UINT u = unique ? unique : (++counter & 0xFFFF);
    snprintf(out, MAX_PATH, "%s/%.3s%04X.tmp", HostPath(dir).c_str(), prefix ? prefix : "tmp", u);
    if (!unique)
    {
        int fd = open(out, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0)
            close(fd);
    }
    return u;
}
BOOL GetDiskFreeSpaceExA(LPCSTR dir, PULARGE_INTEGER availToCaller, PULARGE_INTEGER total, PULARGE_INTEGER totalFree)
{
    struct statfs s;
    std::string p = dir ? HostPath(dir) : std::string(".");
    if (statfs(p.c_str(), &s) != 0)
        return FALSE;
    unsigned long long bs = s.f_bsize;
    if (availToCaller)
        availToCaller->QuadPart = bs * s.f_bavail;
    if (total)
        total->QuadPart = bs * s.f_blocks;
    if (totalFree)
        totalFree->QuadPart = bs * s.f_bfree;
    return TRUE;
}
UINT GetDriveTypeA(LPCSTR) { return DRIVE_FIXED; }
DWORD GetLogicalDrives(void) { return 1u << 2; } // "C:"

HANDLE FindFirstFileA(LPCSTR spec, LPWIN32_FIND_DATAA data)
{
    KFind *f = FindOpen(spec);
    if (!f)
    {
        SetError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    FillFindData(f->dir, f->names[0], data);
    f->next = 1;
    return (HANDLE)f;
}
HANDLE FindFirstFileExA(LPCSTR spec, FINDEX_INFO_LEVELS, LPVOID data, FINDEX_SEARCH_OPS, LPVOID, DWORD)
{
    return FindFirstFileA(spec, (LPWIN32_FIND_DATAA)data);
}
BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return FALSE;
    KFind *f = (KFind *)h;
    if (f->type != KType::Find || f->next >= f->names.size())
    {
        SetError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    FillFindData(f->dir, f->names[f->next++], data);
    return TRUE;
}
BOOL FindClose(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return FALSE;
    ((KObject *)h)->Unref();
    return TRUE;
}

intptr_t _findfirst64i32(const char *spec, struct _finddata64i32_t *info)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(spec, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = ENOENT;
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->attrib = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _A_SUBDIR : _A_NORMAL;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        info->attrib |= _A_RDONLY;
    info->size = fd.nFileSizeLow;
    strncpy(info->name, fd.cFileName, sizeof(info->name) - 1);
    return (intptr_t)h;
}
int _findnext64i32(intptr_t h, struct _finddata64i32_t *info)
{
    WIN32_FIND_DATAA fd;
    if (!FindNextFileA((HANDLE)h, &fd))
    {
        errno = ENOENT;
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->attrib = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _A_SUBDIR : _A_NORMAL;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        info->attrib |= _A_RDONLY;
    info->size = fd.nFileSizeLow;
    strncpy(info->name, fd.cFileName, sizeof(info->name) - 1);
    return 0;
}
int _findclose(intptr_t h) { return FindClose((HANDLE)h) ? 0 : -1; }

void _splitpath(const char *path, char *drive, char *dir, char *fname, char *ext)
{
    const char *p = path;
    if (drive)
        drive[0] = 0;
    if (p[0] && p[1] == ':')
    {
        if (drive)
        {
            drive[0] = p[0];
            drive[1] = ':';
            drive[2] = 0;
        }
        p += 2;
    }
    const char *lastSep = nullptr;
    for (const char *s = p; *s; ++s)
        if (*s == '/' || *s == '\\')
            lastSep = s;
    const char *name = lastSep ? lastSep + 1 : p;
    if (dir)
    {
        size_t n = name - p;
        memcpy(dir, p, n);
        dir[n] = 0;
    }
    const char *dot = strrchr(name, '.');
    if (!dot)
        dot = name + strlen(name);
    if (fname)
    {
        size_t n = dot - name;
        memcpy(fname, name, n);
        fname[n] = 0;
    }
    if (ext)
        strcpy(ext, dot);
}
void _makepath(char *path, const char *drive, const char *dir, const char *fname, const char *ext)
{
    path[0] = 0;
    if (drive && *drive)
        strcat(path, drive);
    if (dir && *dir)
    {
        strcat(path, dir);
        size_t n = strlen(path);
        if (path[n - 1] != '/' && path[n - 1] != '\\')
            strcat(path, "/");
    }
    if (fname)
        strcat(path, fname);
    if (ext && *ext)
    {
        if (*ext != '.')
            strcat(path, ".");
        strcat(path, ext);
    }
}
char *_fullpath(char *absPath, const char *relPath, size_t maxLength)
{
    char tmp[4096];
    DWORD n = GetFullPathNameA(relPath, sizeof(tmp), tmp, nullptr);
    if (!n || n >= sizeof(tmp))
        return nullptr;
    if (!absPath)
        return strdup(tmp);
    if (n >= maxLength)
        return nullptr;
    memcpy(absPath, tmp, n + 1);
    return absPath;
}

HANDLE GetStdHandle(DWORD which)
{
    InitStd();
    if (which == STD_INPUT_HANDLE)
        return (HANDLE)&g_stdHandles[0];
    if (which == STD_OUTPUT_HANDLE)
        return (HANDLE)&g_stdHandles[1];
    if (which == STD_ERROR_HANDLE)
        return (HANDLE)&g_stdHandles[2];
    return INVALID_HANDLE_VALUE;
}
BOOL AllocConsole(void) { return TRUE; }
BOOL FreeConsole(void) { return TRUE; }
BOOL AttachConsole(DWORD) { return TRUE; }
BOOL SetConsoleTitleA(LPCSTR) { return TRUE; }
BOOL WriteConsoleA(HANDLE h, const void *buf, DWORD n, LPDWORD out, LPVOID)
{
    return WriteFile(h, buf, n, out, nullptr);
}

// file mappings (read-only views of files and anonymous shared memory)
HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD, DWORD hi, DWORD lo, LPCSTR)
{
    KMapping *m = new KMapping();
    KFile *f = AsFile(file);
    if (f)
        m->fd = dup(f->fd);
    m->size = ((size_t)hi << 32) | lo;
    return (HANDLE)m;
}
HANDLE OpenFileMappingA(DWORD, BOOL, LPCSTR)
{
    SetError(ERROR_FILE_NOT_FOUND);
    return nullptr;
}
LPVOID MapViewOfFile(HANDLE h, DWORD access, DWORD offHi, DWORD offLo, SIZE_T bytes)
{
    KMapping *m = (KMapping *)h;
    if (!m || m->type != KType::Mapping)
        return nullptr;
    size_t size = bytes ? bytes : m->size;
    off_t off = (off_t)(((unsigned long long)offHi << 32) | offLo);
    int prot = PROT_READ | ((access & FILE_MAP_WRITE) ? PROT_WRITE : 0);
    void *p;
    if (m->fd >= 0)
    {
        if (!size)
        {
            struct stat st;
            fstat(m->fd, &st);
            size = (size_t)st.st_size - (size_t)off;
        }
        p = mmap(nullptr, size, prot, MAP_SHARED, m->fd, off);
    }
    else
        p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
BOOL UnmapViewOfFile(LPCVOID) { return TRUE; }

} // extern "C"
