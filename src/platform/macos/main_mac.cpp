// Entry point of the native macOS build. Sets up the Win32 compatibility layer, installs the app's own game files
// into its game folder, makes that folder the working directory, and runs the engine's own WinMain on the main thread
// (which is also the Cocoa event thread, as the Windows message loop is there).
//
// The game folder is the app's Application Support folder, ~/Library/Application Support/BO1Zombies, and the game
// reads and writes nothing outside it:
//   - the original game's files (zone/, main/*.iwd, ...): put there by setup-mac.sh
//   - the repository's own files (mods/, main/playlists_sp.info): inside the app (Contents/Resources/gamedata),
//     installed into the game folder at every start where they are missing or changed (and removed again when a
//     later build no longer has them, unless they were changed: .bundled-files lists what the app installed)
//   - what the game writes: players/ (settings, profiles), main/ (console_mp.log, games_mp.log), screenshots/, the
//     shader cache (shadercache/)
// so BO1Zombies.app runs from any folder, with nothing next to it. --gamedir <path> (or --gamedir=<path>) runs the
// game on another folder instead (the test builds do).
//
// With no engine arguments (a Finder double-click) the game starts the way Play.bat starts it on Windows.

#include <windows.h>

#include "../win32compat/w32_platform.h"
#include "cocoa_bridge.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <set>
#include <string>
#include <vector>

int __stdcall WinMain(HINSTANCE__ *hInstance, HINSTANCE__ *hPrevInstance, char *lpCmdLine, int nCmdShow);

namespace
{
bool IsDir(const std::string &p)
{
    struct stat st;
    return (stat)(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// the entry of dir named name (any case, as the file system finds it), or ""
std::string FindChild(const std::string &dir, const char *name, bool wantDir)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return "";
    std::string found;
    while (dirent *e = readdir(d))
    {
        if (strcasecmp(e->d_name, name) != 0)
            continue;
        std::string full = dir + "/" + e->d_name;
        struct stat st;
        if ((stat)(full.c_str(), &st) == 0 && (S_ISDIR(st.st_mode) == wantDir))
        {
            found = full;
            break;
        }
    }
    closedir(d);
    return found;
}

bool HasFileWithExtension(const std::string &dir, const char *ext)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return false;
    const size_t extLen = strlen(ext);
    bool found = false;
    while (dirent *e = readdir(d))
    {
        const size_t n = strlen(e->d_name);
        if (n > extLen && strcasecmp(e->d_name + n - extLen, ext) == 0)
        {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

// The original game's files: zone/ (fastfiles) and main/, with its iwds in it if needIwds (the app's game folder,
// where the app itself puts main/playlists_sp.info)
bool LooksLikeGameDir(const std::string &dir, bool needIwds)
{
    if (dir.empty() || !IsDir(dir) || FindChild(dir, "zone", true).empty())
        return false;
    const std::string main = FindChild(dir, "main", true);
    return !main.empty() && (!needIwds || HasFileWithExtension(main, ".iwd"));
}

std::string RealPath(const std::string &p)
{
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf))
        return buf;
    return p;
}

// ~/... for messages
std::string ShortPath(const std::string &p)
{
    const char *home = getenv("HOME");
    const size_t n = home ? strlen(home) : 0;
    if (n && p.compare(0, n, home) == 0 && (p.size() == n || p[n] == '/'))
        return "~" + p.substr(n);
    return p;
}

bool ReadWholeFile(const std::string &path, std::vector<char> &out)
{
    FILE *f = (fopen)(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.insert(out.end(), buf, buf + n);
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// What a path is, without following a link at its end
enum class Kind
{
    None,
    File,
    Dir,
    Link,
    Other
};
Kind KindOf(const std::string &p)
{
    struct stat st;
    if (lstat(p.c_str(), &st) != 0)
        return Kind::None;
    if (S_ISLNK(st.st_mode))
        return Kind::Link;
    if (S_ISDIR(st.st_mode))
        return Kind::Dir;
    return S_ISREG(st.st_mode) ? Kind::File : Kind::Other;
}

// the entry of dir named name in any case (the file system's own spelling of it), or ""
std::string FindEntry(const std::string &dir, const char *name)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return "";
    std::string found;
    while (dirent *e = readdir(d))
    {
        if (strcasecmp(e->d_name, name) == 0)
        {
            found = dir + "/" + e->d_name;
            break;
        }
    }
    closedir(d);
    return found;
}

// rel (a path below dir) is a file reached through folders only, no links
bool IsPlainFileBelow(const std::string &dir, const std::string &rel)
{
    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos)
        return false;
    std::string p = dir;
    for (size_t start = 0;;)
    {
        const size_t slash = rel.find('/', start);
        p += "/" + rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        const Kind k = KindOf(p);
        if (slash == std::string::npos)
            return k == Kind::File;
        if (k != Kind::Dir)
            return false;
        start = slash + 1;
    }
}

// FNV-1a of a file's contents: whether a file the app installed is still the one it installed
uint64_t ContentHash(const std::vector<char> &data)
{
    uint64_t h = 14695981039346656037ull;
    for (const char c : data)
    {
        h ^= (unsigned char)c;
        h *= 1099511628211ull;
    }
    return h;
}

// The files the app installed in the game folder: their paths in it and their contents' hashes, kept there in
// .bundled-files, so that what a later build no longer has (removed from the repository) can be removed again.
using InstalledFiles = std::map<std::string, uint64_t>;

InstalledFiles ReadInstalledFiles(const std::string &file)
{
    InstalledFiles m;
    FILE *f = (fopen)(file.c_str(), "r");
    if (!f)
        return m;
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        char *end = nullptr;
        const unsigned long long h = strtoull(line, &end, 16);
        if (end != line && *end == ' ' && end[1])
            m[end + 1] = h;
    }
    fclose(f);
    return m;
}

void WriteInstalledFiles(const std::string &file, const InstalledFiles &m)
{
    const std::string tmp = file + ".new";
    FILE *f = (fopen)(tmp.c_str(), "w");
    if (!f)
        return;
    fputs("# Written by BO1Zombies.app: the files it installed in this folder (contents' hash, path). One a later\n"
          "# build no longer has is removed again, unless it was changed.\n",
          f);
    for (const auto &e : m)
        fprintf(f, "%016llx %s\n", (unsigned long long)e.second, e.first.c_str());
    const bool ok = fclose(f) == 0;
    if (!ok || (rename)(tmp.c_str(), file.c_str()) != 0)
        unlink(tmp.c_str());
}

struct InstallState
{
    InstalledFiles now;            // the app's files that are in the game folder as the app has them
    std::set<std::string> bundled; // every file the app has (paths in the game folder)
    int installed = 0, failed = 0, removed = 0;
};

// The app's own files (src, rel: its path below Contents/Resources/gamedata) into the game folder (dst): the ones it
// lacks, and the ones whose contents differ (written by an earlier build of the app, or changed). Everything else
// there is left alone, and so are links in it (a mods folder kept elsewhere, say): nothing is written through them.
void InstallBundledFiles(const std::string &src, const std::string &dst, const std::string &rel, InstallState &state)
{
    DIR *d = opendir(src.c_str());
    if (!d)
    {
        ++state.failed;
        return;
    }
    std::vector<std::string> names;
    while (dirent *e = readdir(d))
        if (e->d_name[0] != '.') // (., .., .DS_Store)
            names.push_back(e->d_name);
    closedir(d);
    if (KindOf(dst) == Kind::None && mkdir(dst.c_str(), 0755) != 0)
    {
        ++state.failed;
        fprintf(stderr, "[gamedir] could not create %s\n", dst.c_str());
        return;
    }
    for (const std::string &name : names)
    {
        const std::string s = src + "/" + name;
        struct stat st;
        if ((stat)(s.c_str(), &st) != 0)
            continue;
        // (the game folder's own spelling of the name, if it has the entry in another case)
        std::string t = FindEntry(dst, name.c_str());
        if (t.empty())
            t = dst + "/" + name;
        const std::string path = rel + t.substr(t.rfind('/') + 1);
        const Kind k = KindOf(t);
        if (S_ISDIR(st.st_mode))
        {
            if (k == Kind::None || k == Kind::Dir)
                InstallBundledFiles(s, t, path + "/", state);
            else if (k != Kind::Link)
            {
                ++state.failed;
                fprintf(stderr, "[gamedir] could not install %s: a file is in the way\n", t.c_str());
            }
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;
        state.bundled.insert(path);
        if (k == Kind::Link)
            continue;
        if (k != Kind::None && k != Kind::File)
        {
            ++state.failed;
            fprintf(stderr, "[gamedir] could not install %s: a folder is in the way\n", t.c_str());
            continue;
        }
        std::vector<char> want, have;
        if (!ReadWholeFile(s, want))
        {
            ++state.failed;
            continue;
        }
        if (k == Kind::File && ReadWholeFile(t, have) && have == want)
        {
            state.now[path] = ContentHash(want);
            continue;
        }
        // written beside it and renamed over it: a crash midway leaves the old file, not half of the new one
        const std::string tmp = t + ".installing";
        FILE *f = (fopen)(tmp.c_str(), "wb");
        bool ok = f && fwrite(want.data(), 1, want.size(), f) == want.size();
        if (f && fclose(f) != 0)
            ok = false;
        if (ok && (rename)(tmp.c_str(), t.c_str()) == 0)
        {
            ++state.installed;
            state.now[path] = ContentHash(want);
            continue;
        }
        unlink(tmp.c_str());
        ++state.failed;
        fprintf(stderr, "[gamedir] could not install %s\n", t.c_str());
    }
}

void InstallBundledGameData(const std::string &gameDir)
{
    const char *res = Bridge_BundleResourcePath();
    if (!res || !*res)
        return;
    const std::string src = std::string(res) + "/gamedata";
    if (!IsDir(src))
        return; // (a build without the bundle: the test builds)
    const std::string listFile = gameDir + "/.bundled-files";
    const InstalledFiles before = ReadInstalledFiles(listFile);
    InstallState state;
    InstallBundledFiles(src, gameDir, "", state);
    InstalledFiles next = state.now;
    for (const auto &e : before)
    {
        // (still the app's, if not installed this time; and after a failure nothing is removed: the app's own list of
        // files may be incomplete)
        if (state.bundled.count(e.first) || state.failed)
        {
            next.insert(e);
            continue;
        }
        // a file an earlier build installed that this one doesn't have: removed, unless it was changed since
        const std::string full = gameDir + "/" + e.first;
        std::vector<char> have;
        if (!IsPlainFileBelow(gameDir, e.first) || !ReadWholeFile(full, have) || ContentHash(have) != e.second)
            continue;
        if (unlink(full.c_str()) != 0)
        {
            next.insert(e); // (tried again next time)
            continue;
        }
        ++state.removed;
        // and the folders that leaves empty
        for (size_t slash = full.rfind('/'); slash != std::string::npos && slash > gameDir.size();
             slash = full.rfind('/', slash - 1))
            if (rmdir(full.substr(0, slash).c_str()) != 0)
                break;
    }
    if (next != before)
        WriteInstalledFiles(listFile, next);
    if (state.installed || state.failed || state.removed)
        fprintf(stderr, "[gamedir] %s: %d of the app's files installed, %d it no longer has removed%s\n",
                gameDir.c_str(), state.installed, state.removed, state.failed ? "; some could not be written" : "");
}

void AppendArg(std::string &cmd, const std::string &arg)
{
    if (!cmd.empty())
        cmd += ' ';
    bool quote = arg.empty() || arg.find_first_of(" \t") != std::string::npos;
    if (quote)
        cmd += '"';
    cmd += arg;
    if (quote)
        cmd += '"';
}
} // namespace

int main(int argc, char **argv)
{
    w32::PlatformInit(argc, argv);
    w32::User32Init(); // Cocoa application object; the message boxes below need it

    // split our own options from the engine's command line
    std::string gameDirArg;
    std::vector<std::string> engineArgs;
    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];
        if (!strncmp(a, "-psn_", 5)) // process serial number Finder passed to apps on old systems
            continue;
        if (!strcmp(a, "--gamedir") && i + 1 < argc)
        {
            gameDirArg = argv[++i];
            continue;
        }
        if (!strncmp(a, "--gamedir=", 10))
        {
            gameDirArg = a + 10;
            continue;
        }
        if (!strncmp(a, "-NS", 3) && i + 1 < argc) // -NSDocumentRevisionsDebugMode YES etc. from Xcode
        {
            ++i;
            continue;
        }
        engineArgs.push_back(a);
    }

    std::string gameDir;
    if (!gameDirArg.empty())
    {
        gameDir = RealPath(gameDirArg);
        if (!LooksLikeGameDir(gameDir, false))
        {
            std::string msg = "No Call of Duty: Black Ops game files in\n" + gameDir +
                              "\n\nThe folder needs the original game's zone and main folders.";
            Bridge_MessageBox("BO1 Zombies", msg.c_str(), 0, 2);
            return 1;
        }
    }
    else
    {
        gameDir = Bridge_AppSupportPath(); // (created if needed)
        if (gameDir.empty() || !IsDir(gameDir))
        {
            Bridge_MessageBox("BO1 Zombies", "Can't create the game's folder in ~/Library/Application Support.", 0, 2);
            return 1;
        }
        InstallBundledGameData(gameDir);
        if (!LooksLikeGameDir(gameDir, true))
        {
            std::string msg =
                "The game's files aren't installed yet. The game runs from\n" + ShortPath(gameDir) +
                "\nand needs the original game's zone and main folders there.\n\n"
                "In the BO1 Zombies folder (the repository), run\n"
                "./setup-mac.sh \"/path/to/Call of Duty Black Ops\"\n"
                "with the folder of a Black Ops PC install (Steam or retail): it copies the files there. After an "
                "earlier setup, which put them next to the app, ./setup-mac.sh on its own moves them there.\n\n"
                "Open the game's folder now?";
            if (Bridge_MessageBox("BO1 Zombies", msg.c_str(), 4, 1) == 0)
                Bridge_OpenURL(gameDir.c_str());
            return 1;
        }
    }
    if (chdir(gameDir.c_str()) != 0)
    {
        std::string msg = "Can't open the game folder " + gameDir;
        Bridge_MessageBox("BO1 Zombies", msg.c_str(), 0, 2);
        return 1;
    }
    w32::SetGameDirectory(gameDir.c_str());

    std::string cmdline;
    if (engineArgs.empty())
    {
        // Play.bat's defaults: the zombies boot. Play.bat also sets fs_b and fs_h to the game folder; both default
        // to the working directory, which is the game folder here (and the engine splits its command line at every
        // '+', so a folder path with a '+' in it couldn't be passed). Play.bat's display settings (windowed borderless
        // at the top-left corner) are left to the game's options and config: full screen by default (r_fullscreen,
        // native full screen at r_mode), or a window.
        const char *defaults[] = {"+set", "bo1_zombies", "1", "+set", "logfile", "2", "+set", "net_ip", "127.0.0.1"};
        for (const char *d : defaults)
            AppendArg(cmdline, d);
    }
    else
    {
        for (const std::string &a : engineArgs)
            AppendArg(cmdline, a);
    }

    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);
    return WinMain((HINSTANCE__ *)GetModuleHandleA(nullptr), nullptr, buf.data(), SW_SHOWDEFAULT);
}
