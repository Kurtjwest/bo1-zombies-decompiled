// Entry point of the native macOS build. Sets up the Win32 compatibility layer, finds the game's run folder (the
// original game's data: zone/, main/*.iwd, video/, ...), makes it the working directory, and runs the engine's own
// WinMain on the main thread (which is also the Cocoa event thread, as the Windows message loop is there).
//
// Run folder, first match wins:
//   1. --gamedir <path> (or --gamedir=<path>) on the command line
//   2. $BO1_GAME_DIR
//   3. the folder holding BO1Zombies.app (the Windows build's exe sits in its run folder the same way)
//   4. the current working directory
//   5. the folder chosen last time (~/Library/Application Support/BO1Zombies/gamedir.txt)
//   6. a folder picker
//
// With no engine arguments (a Finder double-click) the game starts the way Play.bat starts it on Windows.

#include <windows.h>

#include "../win32compat/w32_platform.h"
#include "cocoa_bridge.h"

#include <dirent.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

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

bool HasChild(const std::string &dir, const char *name, bool wantDir)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return false;
    bool found = false;
    while (dirent *e = readdir(d))
    {
        if (strcasecmp(e->d_name, name) != 0)
            continue;
        std::string full = dir + "/" + e->d_name;
        struct stat st;
        if ((stat)(full.c_str(), &st) == 0 && (S_ISDIR(st.st_mode) == wantDir))
        {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

// The original game's files: zone/ (fastfiles) and main/ (iwds)
bool LooksLikeGameDir(const std::string &dir)
{
    return !dir.empty() && IsDir(dir) && HasChild(dir, "zone", true) && HasChild(dir, "main", true);
}

std::string RealPath(const std::string &p)
{
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf))
        return buf;
    return p;
}

std::string ExecutableDir()
{
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return ".";
    std::string exe = RealPath(buf);
    size_t slash = exe.rfind('/');
    return slash == std::string::npos ? "." : exe.substr(0, slash);
}

// <dir>/BO1Zombies.app/Contents/MacOS -> <dir>
std::string BundleParentDir()
{
    std::string dir = ExecutableDir();
    const std::string tail = "/Contents/MacOS";
    if (dir.size() > tail.size() && dir.compare(dir.size() - tail.size(), tail.size(), tail) == 0)
    {
        std::string bundle = dir.substr(0, dir.size() - tail.size());
        size_t slash = bundle.rfind('/');
        if (slash != std::string::npos)
            return bundle.substr(0, slash);
    }
    return dir;
}

std::string SavedGameDirFile() { return std::string(Bridge_AppSupportPath()) + "/gamedir.txt"; }

std::string LoadSavedGameDir()
{
    FILE *f = (fopen)(SavedGameDirFile().c_str(), "r");
    if (!f)
        return "";
    char buf[PATH_MAX] = {};
    if (!fgets(buf, sizeof(buf), f))
        buf[0] = 0;
    fclose(f);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    return buf;
}

void SaveGameDir(const std::string &dir)
{
    FILE *f = (fopen)(SavedGameDirFile().c_str(), "w");
    if (!f)
        return;
    fprintf(f, "%s\n", dir.c_str());
    fclose(f);
}

std::string FindGameDir(const std::string &fromArgs)
{
    if (!fromArgs.empty())
        return RealPath(fromArgs);
    if (const char *env = getenv("BO1_GAME_DIR"))
        if (*env)
            return RealPath(env);
    std::string bundleParent = BundleParentDir();
    if (LooksLikeGameDir(bundleParent))
        return bundleParent;
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) && LooksLikeGameDir(cwd))
        return RealPath(cwd);
    std::string saved = LoadSavedGameDir();
    if (LooksLikeGameDir(saved))
        return saved;
    for (;;)
    {
        char chosen[PATH_MAX];
        if (!Bridge_ChooseFolder("Choose your Call of Duty: Black Ops folder (the one with zone and main in it).", chosen, sizeof(chosen)))
            return "";
        if (LooksLikeGameDir(chosen))
        {
            SaveGameDir(chosen);
            return chosen;
        }
        Bridge_MessageBox("BO1 Zombies",
                          "That folder doesn't have the game's zone and main folders in it. Choose the Call of Duty: Black Ops "
                          "folder (copied from a Windows or Steam install).",
                          0, 2);
    }
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
    w32::User32Init(); // Cocoa application object; the folder picker below needs it

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

    std::string gameDir = FindGameDir(gameDirArg);
    if (gameDir.empty())
        return 1;
    if (!LooksLikeGameDir(gameDir))
    {
        std::string msg = "No Call of Duty: Black Ops game files in\n" + gameDir +
                          "\n\nThe folder needs the original game's zone and main folders.";
        Bridge_MessageBox("BO1 Zombies", msg.c_str(), 0, 2);
        return 1;
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
        // Play.bat's defaults: the zombies boot, windowed borderless at the top-left corner. Play.bat also sets
        // fs_b and fs_h to the game folder; both default to the working directory, which is the game folder here,
        // and the engine splits its command line at every '+', so a folder path with a '+' in it can't be passed.
        const char *defaults[] = {"+set", "bo1_zombies", "1", "+set", "logfile", "2", "+set", "net_ip", "127.0.0.1",
                                  "+set", "r_fullscreen", "0", "+set", "r_borderless", "1", "+set", "vid_xpos", "0",
                                  "+set", "vid_ypos", "0"};
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
