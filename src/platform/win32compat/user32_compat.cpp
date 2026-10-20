// user32/gdi32/shell32 for the macOS port, on top of the Cocoa bridge.
//
// Windows are NSWindows. Cocoa input is translated into Win32 messages (virtual-key codes, PC scan codes, MK_
// button flags, wheel deltas) queued for PeekMessage/DispatchMessage, so the engine's window procedure and input
// code run unchanged. While the game hides the cursor (mouse look) the mouse is decoupled from the cursor and
// GetCursorPos/SetCursorPos operate on a virtual cursor fed by raw mouse deltas, which is what the engine's
// "read position, recenter" mouse code expects.

#include <windows.h>
#include <universal/ptr32.h>
#include "kernel32_internal.h"
#include "main_thread.h"
#include "w32_platform.h"
#include "../common/platform_bridge.h"

#include "mac_keycodes.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kWndMagic = 0x57494E44; // 'WIND'

struct Win32Wnd
{
    uint32_t magic = kWndMagic;
    WNDPROC proc = nullptr;
    std::string className;
    std::string title;
    DWORD style = 0;
    DWORD exStyle = 0;
    LONG_PTR userData = 0;
    LONG_PTR id = 0;
    HINSTANCE instance = nullptr;
    // read on any thread (the main thread's pump, the renderer), written by whichever thread destroys the window
    std::atomic<BridgeWindow *> bw{nullptr};
    std::atomic<bool> destroyed{false};
    std::map<std::string, HANDLE> props;
    // What the engine asked for (units): the frame (CreateWindowEx, SetWindowPos) and the client size, which is the
    // size the game draws at. The view can show that scaled: in native full screen, or after the player resized the
    // window. Client coordinates (GetClientRect, ScreenToClient, the mouse messages) stay the game's own.
    RECT frame{};
    std::atomic<int> clientW{0}, clientH{0};
    bool topmost = false;                    // WS_EX_TOPMOST, or HWND_TOPMOST / HWND_NOTOPMOST in SetWindowPos
    std::atomic<bool> wantFullScreen{false}; // the engine's full-screen window (see IsFullScreenRequest)
    bool styleDirty = false;                 // GWL_STYLE changed: the Cocoa window takes it at the next SetWindowPos
};

std::map<std::string, WNDCLASSEXA> g_classes;
// Windows are only ever added (a destroyed one stays, flagged destroyed): an append-only list the main thread's pump
// can walk while the render thread creates a window (a std::vector reallocating under the reader was a
// use-after-free). The element is published before the count.
struct WindowList
{
    static constexpr size_t kMax = 4096;
    std::atomic<Win32Wnd *> items[kMax] = {};
    std::atomic<size_t> count{0};
    std::mutex addLock;
    void push_back(Win32Wnd *w)
    {
        std::lock_guard<std::mutex> g(addLock);
        const size_t n = count.load(std::memory_order_relaxed);
        if (n >= kMax)
            return; // never expected: windows are created at startup and on vid_restart
        items[n].store(w, std::memory_order_relaxed);
        count.store(n + 1, std::memory_order_release);
    }
    struct It
    {
        const WindowList *l;
        size_t i;
        Win32Wnd *operator*() const { return l->items[i].load(std::memory_order_relaxed); }
        It &operator++()
        {
            ++i;
            return *this;
        }
        bool operator!=(const It &o) const { return i != o.i; }
    };
    It begin() const { return {this, 0}; }
    It end() const { return {this, count.load(std::memory_order_acquire)}; }
};
WindowList g_windows;
std::recursive_mutex g_queueLock;
std::deque<MSG> g_queue;
DWORD g_mainThreadId;
bool g_bridgeReady;
int g_cursorCount;         // ShowCursor display count (>= 0 visible)
bool g_captured;           // mouse decoupled from the cursor
LONG g_vcurX, g_vcurY;     // virtual cursor (screen units)
// The screens, for keeping the virtual cursor on the desktop (main thread: refreshed when the mouse is captured)
constexpr int kMaxCursorScreens = 8;
RECT g_cursorScreens[kMaxCursorScreens];
int g_cursorScreenCount;
WPARAM g_buttons;          // MK_* currently down
bool g_appActive = true;
bool g_quitPosted;
int g_quitCode;
DWORD g_lastMsgTime;
HWND g_focus;
char *g_clipboardText;
std::map<std::string, UINT> g_registeredMessages;
UINT g_nextRegisteredMessage = 0xC000;
BYTE g_desktopTag;
BYTE g_dcTag;
BYTE g_cursorTag;
BYTE g_iconTag;
BYTE g_hookTag;

Win32Wnd *W(HWND h)
{
    Win32Wnd *w = (Win32Wnd *)h;
    if (!w || h == (HWND)&g_desktopTag)
        return nullptr;
    for (Win32Wnd *x : g_windows)
        if (x == w)
            return w->destroyed ? nullptr : w;
    return nullptr;
}

std::string Lower(const char *s)
{
    std::string r = s ? s : "";
    for (char &c : r)
        c = (char)tolower((unsigned char)c);
    return r;
}

#if defined(__APPLE__)
const int kTitleBar = 28; // macOS standard title bar height (points)
#else
// Linux: the window manager draws (and sizes) the decorations around the client area SDL positions, so a window's
// rectangle is its client rectangle as far as the engine's sizing arithmetic goes
const int kTitleBar = 0;
#endif

int TitleBarUnits(DWORD style)
{
    if ((style & WS_CAPTION) == WS_CAPTION && !(style & WS_POPUP))
        return (int)(kTitleBar / Bridge_UnitScale() + 0.5);
    return 0;
}

bool IsBorderless(DWORD style) { return (style & WS_POPUP) || (style & WS_CAPTION) != WS_CAPTION; }

// ---- key translation: macOS virtual key -> Windows virtual key, PC scan code, extended flag ----
struct KeyMap
{
    uint8_t vk;
    uint8_t scan;
    uint8_t ext;
};
KeyMap g_keyMap[128];
uint16_t g_vkToMac[256][2];

void InitKeyMap()
{
    static bool done;
    if (done)
        return;
    done = true;
    memset(g_keyMap, 0, sizeof(g_keyMap));
    memset(g_vkToMac, 0xFF, sizeof(g_vkToMac));
    auto set = [](int mac, int vk, int scan, int ext) {
        g_keyMap[mac] = {(uint8_t)vk, (uint8_t)scan, (uint8_t)ext};
        if (g_vkToMac[vk][0] == 0xFFFF)
            g_vkToMac[vk][0] = (uint16_t)mac;
        else
            g_vkToMac[vk][1] = (uint16_t)mac;
    };
    const char *letters = "ASDFHGZXCVBQWERYT";
    static const int letterCodes[] = {kVK_ANSI_A, kVK_ANSI_S, kVK_ANSI_D, kVK_ANSI_F, kVK_ANSI_H, kVK_ANSI_G, kVK_ANSI_Z, kVK_ANSI_X, kVK_ANSI_C, kVK_ANSI_V, kVK_ANSI_B, kVK_ANSI_Q, kVK_ANSI_W, kVK_ANSI_E, kVK_ANSI_R, kVK_ANSI_Y, kVK_ANSI_T};
    (void)letters;
    static const struct { int mac; char ch; } alpha[] = {
        {kVK_ANSI_A, 'A'}, {kVK_ANSI_B, 'B'}, {kVK_ANSI_C, 'C'}, {kVK_ANSI_D, 'D'}, {kVK_ANSI_E, 'E'}, {kVK_ANSI_F, 'F'}, {kVK_ANSI_G, 'G'},
        {kVK_ANSI_H, 'H'}, {kVK_ANSI_I, 'I'}, {kVK_ANSI_J, 'J'}, {kVK_ANSI_K, 'K'}, {kVK_ANSI_L, 'L'}, {kVK_ANSI_M, 'M'}, {kVK_ANSI_N, 'N'},
        {kVK_ANSI_O, 'O'}, {kVK_ANSI_P, 'P'}, {kVK_ANSI_Q, 'Q'}, {kVK_ANSI_R, 'R'}, {kVK_ANSI_S, 'S'}, {kVK_ANSI_T, 'T'}, {kVK_ANSI_U, 'U'},
        {kVK_ANSI_V, 'V'}, {kVK_ANSI_W, 'W'}, {kVK_ANSI_X, 'X'}, {kVK_ANSI_Y, 'Y'}, {kVK_ANSI_Z, 'Z'}};
    // PC set-1 scan codes for the letters
    static const uint8_t letterScan[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                           0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    (void)letterCodes;
    for (auto &a : alpha)
        set(a.mac, a.ch, letterScan[a.ch - 'A'], 0);
    static const struct { int mac; int vk; int scan; } digits[] = {
        {kVK_ANSI_1, '1', 0x02}, {kVK_ANSI_2, '2', 0x03}, {kVK_ANSI_3, '3', 0x04}, {kVK_ANSI_4, '4', 0x05}, {kVK_ANSI_5, '5', 0x06},
        {kVK_ANSI_6, '6', 0x07}, {kVK_ANSI_7, '7', 0x08}, {kVK_ANSI_8, '8', 0x09}, {kVK_ANSI_9, '9', 0x0A}, {kVK_ANSI_0, '0', 0x0B}};
    for (auto &d : digits)
        set(d.mac, d.vk, d.scan, 0);
    set(kVK_ANSI_Minus, VK_OEM_MINUS, 0x0C, 0);
    set(kVK_ANSI_Equal, VK_OEM_PLUS, 0x0D, 0);
    set(kVK_Delete, VK_BACK, 0x0E, 0);
    set(kVK_Tab, VK_TAB, 0x0F, 0);
    set(kVK_ANSI_LeftBracket, VK_OEM_4, 0x1A, 0);
    set(kVK_ANSI_RightBracket, VK_OEM_6, 0x1B, 0);
    set(kVK_Return, VK_RETURN, 0x1C, 0);
    set(kVK_Control, VK_CONTROL, 0x1D, 0);
    set(kVK_ANSI_Semicolon, VK_OEM_1, 0x27, 0);
    set(kVK_ANSI_Quote, VK_OEM_7, 0x28, 0);
    set(kVK_ANSI_Grave, VK_OEM_3, 0x29, 0);
    set(kVK_ISO_Section, VK_OEM_3, 0x29, 0); // the key left of "1" on ISO keyboards: the console key
    set(kVK_Shift, VK_SHIFT, 0x2A, 0);
    set(kVK_ANSI_Backslash, VK_OEM_5, 0x2B, 0);
    set(kVK_ANSI_Comma, VK_OEM_COMMA, 0x33, 0);
    set(kVK_ANSI_Period, VK_OEM_PERIOD, 0x34, 0);
    set(kVK_ANSI_Slash, VK_OEM_2, 0x35, 0);
    set(kVK_RightShift, VK_SHIFT, 0x36, 0);
    set(kVK_ANSI_KeypadMultiply, VK_MULTIPLY, 0x37, 0);
    set(kVK_Option, VK_MENU, 0x38, 0);
    set(kVK_Space, VK_SPACE, 0x39, 0);
    set(kVK_CapsLock, VK_CAPITAL, 0x3A, 0);
    set(kVK_F1, VK_F1, 0x3B, 0);
    set(kVK_F2, VK_F2, 0x3C, 0);
    set(kVK_F3, VK_F3, 0x3D, 0);
    set(kVK_F4, VK_F4, 0x3E, 0);
    set(kVK_F5, VK_F5, 0x3F, 0);
    set(kVK_F6, VK_F6, 0x40, 0);
    set(kVK_F7, VK_F7, 0x41, 0);
    set(kVK_F8, VK_F8, 0x42, 0);
    set(kVK_F9, VK_F9, 0x43, 0);
    set(kVK_F10, VK_F10, 0x44, 0);
    set(kVK_ANSI_KeypadClear, VK_NUMLOCK, 0x45, 0);
    set(kVK_ANSI_Keypad7, VK_NUMPAD7, 0x47, 0);
    set(kVK_ANSI_Keypad8, VK_NUMPAD8, 0x48, 0);
    set(kVK_ANSI_Keypad9, VK_NUMPAD9, 0x49, 0);
    set(kVK_ANSI_KeypadMinus, VK_SUBTRACT, 0x4A, 0);
    set(kVK_ANSI_Keypad4, VK_NUMPAD4, 0x4B, 0);
    set(kVK_ANSI_Keypad5, VK_NUMPAD5, 0x4C, 0);
    set(kVK_ANSI_Keypad6, VK_NUMPAD6, 0x4D, 0);
    set(kVK_ANSI_KeypadPlus, VK_ADD, 0x4E, 0);
    set(kVK_ANSI_Keypad1, VK_NUMPAD1, 0x4F, 0);
    set(kVK_ANSI_Keypad2, VK_NUMPAD2, 0x50, 0);
    set(kVK_ANSI_Keypad3, VK_NUMPAD3, 0x51, 0);
    set(kVK_ANSI_Keypad0, VK_NUMPAD0, 0x52, 0);
    set(kVK_ANSI_KeypadDecimal, VK_DECIMAL, 0x53, 0);
    set(kVK_F11, VK_F11, 0x57, 0);
    set(kVK_F12, VK_F12, 0x58, 0);
    set(kVK_F13, VK_F13, 0x64, 0);
    set(kVK_F14, VK_F14, 0x65, 0);
    set(kVK_F15, VK_F15, 0x66, 0);
    set(kVK_F16, VK_F16, 0x67, 0);
    set(kVK_F17, VK_F17, 0x68, 0);
    set(kVK_F18, VK_F18, 0x69, 0);
    set(kVK_F19, VK_F19, 0x6A, 0);
    set(kVK_ANSI_KeypadEnter, VK_RETURN, 0x1C, 1);
    set(kVK_RightControl, VK_CONTROL, 0x1D, 1);
    set(kVK_ANSI_KeypadDivide, VK_DIVIDE, 0x35, 1);
    set(kVK_RightOption, VK_MENU, 0x38, 1);
    set(kVK_Home, VK_HOME, 0x47, 1);
    set(kVK_UpArrow, VK_UP, 0x48, 1);
    set(kVK_PageUp, VK_PRIOR, 0x49, 1);
    set(kVK_LeftArrow, VK_LEFT, 0x4B, 1);
    set(kVK_RightArrow, VK_RIGHT, 0x4D, 1);
    set(kVK_End, VK_END, 0x4F, 1);
    set(kVK_DownArrow, VK_DOWN, 0x50, 1);
    set(kVK_PageDown, VK_NEXT, 0x51, 1);
    set(kVK_Help, VK_INSERT, 0x52, 1);
    set(kVK_ForwardDelete, VK_DELETE, 0x53, 1);
    set(kVK_Command, VK_LWIN, 0x5B, 1);
    set(kVK_RightCommand, VK_RWIN, 0x5C, 1);
    set(kVK_Escape, VK_ESCAPE, 0x01, 0);
    set(kVK_ANSI_KeypadEquals, VK_OEM_PLUS, 0x0D, 0);
    set(kVK_VolumeUp, VK_VOLUME_UP, 0x30, 1);
    set(kVK_VolumeDown, VK_VOLUME_DOWN, 0x2E, 1);
    set(kVK_Mute, VK_VOLUME_MUTE, 0x20, 1);
}

int ScanToVk(int scan, bool ext)
{
    InitKeyMap();
    for (int i = 0; i < 128; ++i)
        if (g_keyMap[i].vk && g_keyMap[i].scan == scan && (bool)g_keyMap[i].ext == ext)
            return g_keyMap[i].vk;
    return 0;
}

int VkToScan(int vk)
{
    InitKeyMap();
    if (vk < 0 || vk > 255)
        return 0;
    uint16_t mac = g_vkToMac[vk][0];
    if (mac == 0xFFFF)
    {
        if (vk == VK_LSHIFT) return 0x2A;
        if (vk == VK_RSHIFT) return 0x36;
        if (vk == VK_LCONTROL || vk == VK_RCONTROL) return 0x1D;
        if (vk == VK_LMENU || vk == VK_RMENU) return 0x38;
        return 0;
    }
    return g_keyMap[mac].scan;
}

UINT VkToChar(UINT vk)
{
    if (vk >= '0' && vk <= '9')
        return vk;
    if (vk >= 'A' && vk <= 'Z')
        return vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return '0' + (vk - VK_NUMPAD0);
    switch (vk)
    {
    case VK_OEM_1: return ';';
    case VK_OEM_PLUS: return '=';
    case VK_OEM_COMMA: return ',';
    case VK_OEM_MINUS: return '-';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_2: return '/';
    case VK_OEM_3: return '`';
    case VK_OEM_4: return '[';
    case VK_OEM_5: return '\\';
    case VK_OEM_6: return ']';
    case VK_OEM_7: return '\'';
    case VK_SPACE: return ' ';
    case VK_RETURN: return '\r';
    case VK_BACK: return 8;
    case VK_TAB: return 9;
    case VK_ESCAPE: return 27;
    case VK_MULTIPLY: return '*';
    case VK_ADD: return '+';
    case VK_SUBTRACT: return '-';
    case VK_DECIMAL: return '.';
    case VK_DIVIDE: return '/';
    default: return 0;
    }
}

UINT ShiftedChar(UINT c)
{
    if (c >= 'a' && c <= 'z')
        return c - 32;
    static const char *from = "1234567890-=[]\\;',./`";
    static const char *to = "!@#$%^&*()_+{}|:\"<>?~";
    const char *p = strchr(from, (int)c);
    return p && c ? (UINT)to[p - from] : c;
}

HWND MainWindow()
{
    for (Win32Wnd *w : g_windows)
        if (!w->destroyed && w->bw && Bridge_IsVisible(w->bw))
            return (HWND)w;
    for (Win32Wnd *w : g_windows)
        if (!w->destroyed)
            return (HWND)w;
    return nullptr;
}

void Post(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    MSG m;
    m.hwnd = h;
    m.message = msg;
    m.wParam = wp;
    m.lParam = lp;
    m.time = GetTickCount();
    m.pt.x = g_vcurX;
    m.pt.y = g_vcurY;
    std::lock_guard<std::recursive_mutex> g(g_queueLock);
    g_queue.push_back(m);
}

bool AppHasFocus()
{
    HWND fg = GetForegroundWindow();
    return fg != nullptr;
}

void RefreshCursorScreens()
{
    int n = Bridge_GetScreenCount();
    if (n > kMaxCursorScreens)
        n = kMaxCursorScreens;
    int count = 0;
    for (int i = 0; i < n; ++i)
    {
        BridgeRect r;
        if (Bridge_GetScreenRect(i, &r, nullptr, nullptr) && r.right > r.left && r.bottom > r.top)
            g_cursorScreens[count++] = {r.left, r.top, r.right, r.bottom};
    }
    g_cursorScreenCount = count;
}

// The Windows cursor never leaves the desktop: at a screen edge it stops, and the game's own menu cursor stops with
// it. The virtual cursor (fed by raw deltas while the mouse is captured) gets the same limits, or it would run on past
// the edge: the menu cursor would vanish there, the hidden system cursor would come back, the capture would be undone
// and redone every frame, and moving back would first have to make up the distance travelled off screen.
void ClampVirtualCursor()
{
    if (g_cursorScreenCount <= 0)
        return;
    LONG bestX = g_vcurX, bestY = g_vcurY;
    long long bestDist = -1;
    for (int i = 0; i < g_cursorScreenCount; ++i)
    {
        const RECT &r = g_cursorScreens[i];
        const LONG x = std::clamp<LONG>(g_vcurX, r.left, r.right - 1);
        const LONG y = std::clamp<LONG>(g_vcurY, r.top, r.bottom - 1);
        const long long dx = (long long)x - g_vcurX, dy = (long long)y - g_vcurY;
        const long long dist = dx * dx + dy * dy;
        if (bestDist < 0 || dist < bestDist)
        {
            bestDist = dist;
            bestX = x;
            bestY = y;
        }
        if (dist == 0)
            break; // on this screen
    }
    g_vcurX = bestX;
    g_vcurY = bestY;
}

// The system cursor's visibility as last sent to the main thread (-1: not yet): the engine sets its cursor count
// every frame, and only a change needs to reach the main thread.
std::atomic<int> g_cursorShown{-1};
void SetCursorShown(bool shown)
{
    if (g_cursorShown.exchange(shown ? 1 : 0) != (shown ? 1 : 0))
        Bridge_SetCursorVisible(shown ? 1 : 0);
}

void UpdateCapture()
{
    const bool want = g_cursorCount < 0 && AppHasFocus();
    if (want == g_captured)
        return;
    if (want)
    {
        RefreshCursorScreens();
        Bridge_GetCursorPos((int *)&g_vcurX, (int *)&g_vcurY);
        ClampVirtualCursor();
        SetCursorShown(false);
        Bridge_SetMouseCaptured(1);
    }
    else
    {
        Bridge_SetMouseCaptured(0);
        Bridge_WarpCursor((int)g_vcurX, (int)g_vcurY);
        if (g_cursorCount >= 0)
            SetCursorShown(true);
    }
    g_captured = want;
}

// The engine's full-screen window: a popup that is topmost (r_fullscreen 1: WS_POPUP + WS_EX_TOPMOST at the r_mode
// size, the window Direct3D's exclusive mode takes over on Windows) or that covers the screen it is on (a borderless
// one). macOS has no exclusive mode and no display mode switch: the window goes native full screen (a Space of its
// own) and shows the game, drawn at the size it asked for, stretched over the screen as a monitor would.
bool IsFullScreenRequest(DWORD style, bool topmost, int x, int y, int cx, int cy)
{
    if (!(style & WS_POPUP) || (style & WS_CHILD))
        return false;
    if (topmost)
        return true;
    BridgeRect full;
    const int screen = Bridge_ScreenForPoint(x + cx / 2, y + cy / 2);
    return Bridge_GetScreenRect(screen, &full, nullptr, nullptr) && x <= full.left && y <= full.top &&
           x + cx >= full.right && y + cy >= full.bottom;
}

// The screen a window at (x, y) belongs to: the one its top-left corner is on (an r_mode-sized full-screen window at
// a monitor's origin can reach well into the next monitor), else the one nearest to it.
int ScreenForWindow(int x, int y)
{
    const int n = Bridge_GetScreenCount();
    long long best = -1;
    int bestScreen = 0;
    for (int i = 0; i < n; ++i)
    {
        BridgeRect r;
        if (!Bridge_GetScreenRect(i, &r, nullptr, nullptr))
            continue;
        const long long dx = x < r.left ? r.left - x : x >= r.right ? x - (r.right - 1) : 0;
        const long long dy = y < r.top ? r.top - y : y >= r.bottom ? y - (r.bottom - 1) : 0;
        const long long d = dx * dx + dy * dy;
        if (best < 0 || d < best)
        {
            best = d;
            bestScreen = i;
        }
    }
    return bestScreen;
}

// A titled window keeps its title bar on the screen, below the menu bar (the engine's default position, 3,22, is
// under it). cx, cy: the frame's size.
void KeepFrameOnScreen(int *x, int *y, int cx, int cy)
{
    BridgeRect work;
    if (!Bridge_GetScreenRect(ScreenForWindow(*x, *y), nullptr, &work, nullptr))
        return;
    if (*x + cx > work.right)
        *x = work.right - cx;
    if (*y + cy > work.bottom)
        *y = work.bottom - cy;
    if (*x < work.left)
        *x = work.left;
    if (*y < work.top)
        *y = work.top;
}

// The client rect (units) of the window a full-screen window becomes when the player takes it out of full screen
// (the green button): the screen's shape, as the game draws for it, fitted into the screen's work area.
void FullScreenFallbackRect(int screen, int *x, int *y, int *cx, int *cy)
{
    BridgeRect full, work;
    if (!Bridge_GetScreenRect(screen, &full, &work, nullptr))
    {
        *x = *y = (int)0x80000000;
        *cx = 1280;
        *cy = 720;
        return;
    }
    const double aspect = (double)(full.right - full.left) / std::max(1, full.bottom - full.top);
    const int tb = TitleBarUnits(WS_CAPTION);
    double w = (work.right - work.left) * 0.8, h = (work.bottom - work.top - tb) * 0.8;
    if (w / h > aspect)
        w = h * aspect;
    else
        h = w / aspect;
    *cx = std::max(1, (int)lround(w));
    *cy = std::max(1, (int)lround(h));
    *x = (work.left + work.right - *cx) / 2;
    *y = (work.top + work.bottom - (*cy + tb)) / 2 + tb;
}

void ApplyStyle(Win32Wnd *w)
{
    w->styleDirty = false;
    Bridge_SetWindowStyle(w->bw, IsBorderless(w->style) ? 1 : 0, (w->style & WS_THICKFRAME) ? 1 : 0);
}

// The frame the engine asked for (x, y, cx, cy: the window rect, title bar included).
void ApplyFrame(Win32Wnd *w, int x, int y, int cx, int cy)
{
    if (!w->bw)
        return;
    const int tb = TitleBarUnits(w->style);
    const bool fs = IsFullScreenRequest(w->style, w->topmost, x, y, cx, cy);
    if (!fs && tb)
        KeepFrameOnScreen(&x, &y, cx, cy);
    w->frame = {x, y, x + cx, y + cy};
    w->clientW = std::max(cx, 1);
    w->clientH = std::max(cy - tb, 1);
    w->wantFullScreen = fs;
    Bridge_SetContentScaling(w->bw, w->clientW, w->clientH, fs ? 1 : 0);
    if (fs)
    {
        // (the Cocoa window keeps its windowed style and frame for when it leaves full screen)
        Bridge_SetFullScreen(w->bw, 1);
        return;
    }
    // a window leaving full screen gets its style and frame when the animation is over
    ApplyStyle(w);
    Bridge_SetClientRect(w->bw, x, y + tb, cx, cy - tb > 1 ? cy - tb : 1);
    Bridge_SetFullScreen(w->bw, 0);
}

// The engine's frame while the window is (or is going) full screen, else where the window actually is (the player
// can move it).
RECT RequestedFrame(HWND h, Win32Wnd *w)
{
    if (w->wantFullScreen || (w->bw && Bridge_IsFullScreen(w->bw)))
        return w->frame;
    RECT r = w->frame;
    GetWindowRect(h, &r);
    return r;
}

// Client coordinates are the game's (its back buffer's pixels). The view shows them scaled when it is not the
// client size: stretched over all of it for the engine's full screen (which draws for the screen's shape), else at
// their own aspect ratio, centred, with black bars. screen = origin + client * scale.
struct ClientMap
{
    double ox, oy, sx, sy;
};
ClientMap GetClientMap(Win32Wnd *w)
{
    BridgeRect c;
    Bridge_GetClientRect(w->bw, &c);
    ClientMap m = {(double)c.left, (double)c.top, 1.0, 1.0};
    const double vw = c.right - c.left, vh = c.bottom - c.top;
    const double lw = w->clientW, lh = w->clientH;
    if (vw <= 0 || vh <= 0 || lw <= 0 || lh <= 0 || (vw == lw && vh == lh))
        return m;
    if (w->wantFullScreen)
    {
        m.sx = vw / lw;
        m.sy = vh / lh;
    }
    else
    {
        const double s = std::min(vw / lw, vh / lh);
        m.sx = m.sy = s;
        m.ox += (vw - lw * s) / 2;
        m.oy += (vh - lh * s) / 2;
    }
    return m;
}

void OnBridgeEvent(const BridgeEvent *ev)
{
    InitKeyMap();
    HWND h = ev->userData ? (HWND)ev->userData : MainWindow();
    g_lastMsgTime = GetTickCount();
    switch (ev->type)
    {
    case BRIDGE_EV_KEY_DOWN:
    case BRIDGE_EV_KEY_UP:
    {
        if (ev->macKeyCode >= 128)
            return;
        KeyMap k = g_keyMap[ev->macKeyCode];
        if (!k.vk)
            return;
        const bool down = ev->type == BRIDGE_EV_KEY_DOWN;
        const bool alt = (ev->modifiers & BRIDGE_MOD_ALT) != 0;
        LPARAM lp = 1 | ((LPARAM)k.scan << 16) | ((LPARAM)k.ext << 24);
        if (alt)
            lp |= (LPARAM)1 << 29;
        if (!down)
            lp |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
        else if (ev->isRepeat)
            lp |= (LPARAM)1 << 30;
        UINT msg;
        if (alt || k.vk == VK_MENU || k.vk == VK_F10)
            msg = down ? WM_SYSKEYDOWN : WM_SYSKEYUP;
        else
            msg = down ? WM_KEYDOWN : WM_KEYUP;
        Post(h, msg, k.vk, lp);
        break;
    }
    case BRIDGE_EV_CHAR:
    {
        uint32_t c = ev->ch;
        if (c > 0xFFFF)
            c = '?';
        Post(h, WM_CHAR, c, 1);
        break;
    }
    case BRIDGE_EV_MOUSE_MOVE:
    {
        if (g_captured)
        {
            const double s = Bridge_UnitScale();
            static double fx, fy;
            fx += ev->dx / s;
            fy += ev->dy / s;
            LONG ix = (LONG)fx, iy = (LONG)fy;
            fx -= ix;
            fy -= iy;
            g_vcurX += ix;
            g_vcurY += iy;
            ClampVirtualCursor();
        }
        else
        {
            Bridge_GetCursorPos((int *)&g_vcurX, (int *)&g_vcurY);
        }
        POINT p = {g_vcurX, g_vcurY};
        ScreenToClient(h, &p);
        Post(h, WM_MOUSEMOVE, g_buttons, MAKELPARAM((short)p.x, (short)p.y));
        break;
    }
    case BRIDGE_EV_MOUSE_DOWN:
    case BRIDGE_EV_MOUSE_UP:
    {
        const bool down = ev->type == BRIDGE_EV_MOUSE_DOWN;
        static const WPARAM flags[5] = {MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2};
        int b = ev->button < 0 ? 0 : ev->button > 4 ? 4 : ev->button;
        if (down)
            g_buttons |= flags[b];
        else
            g_buttons &= ~flags[b];
        UINT msg;
        WPARAM wp = g_buttons;
        switch (b)
        {
        case 0: msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; break;
        case 1: msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; break;
        case 2: msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; break;
        default:
            msg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
            wp |= (WPARAM)(b == 3 ? XBUTTON1 : XBUTTON2) << 16;
            break;
        }
        POINT p = {g_vcurX, g_vcurY};
        if (!g_captured)
            Bridge_GetCursorPos((int *)&p.x, (int *)&p.y);
        ScreenToClient(h, &p);
        Post(h, msg, wp, MAKELPARAM((short)p.x, (short)p.y));
        break;
    }
    case BRIDGE_EV_WHEEL:
    {
        short delta = (short)(ev->dy > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
        Post(h, WM_MOUSEWHEEL, MAKEWPARAM(g_buttons, (WORD)delta), MAKELPARAM((short)g_vcurX, (short)g_vcurY));
        break;
    }
    case BRIDGE_EV_ACTIVATE:
    {
        g_appActive = ev->value != 0;
        HWND mw = MainWindow();
        if (!mw)
            break;
        Win32Wnd *w = W(mw);
        bool minimized = w && w->bw && Bridge_IsMinimized(w->bw);
        Post(mw, WM_ACTIVATEAPP, g_appActive ? 1 : 0, 0);
        Post(mw, WM_ACTIVATE, MAKEWPARAM(g_appActive ? WA_ACTIVE : WA_INACTIVE, minimized ? 1 : 0), 0);
        Post(mw, g_appActive ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
        if (!g_appActive)
        {
            g_buttons = 0;
            UpdateCapture();
        }
        break;
    }
    case BRIDGE_EV_MINIMIZE:
    {
        HWND mw = h ? h : MainWindow();
        Win32Wnd *w = W(mw);
        if (!w)
            break;
        RECT rc;
        GetClientRect(mw, &rc);
        Post(mw, WM_SIZE, ev->value ? SIZE_MINIMIZED : SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
        Post(mw, WM_ACTIVATE, MAKEWPARAM(ev->value ? WA_INACTIVE : WA_ACTIVE, ev->value ? 1 : 0), 0);
        break;
    }
    case BRIDGE_EV_RESIZE:
    {
        // The game keeps drawing at its own size when the view changes size under it: in native full screen (and on
        // the way in and out), and in a window the player resizes after taking the engine's full screen out of full
        // screen. A resizable window of the engine's own takes the new size.
        Win32Wnd *w = W(h);
        if (!w || !w->bw || w->wantFullScreen || Bridge_IsFullScreen(w->bw) || !(w->style & WS_THICKFRAME))
            break;
        w->clientW = std::max(ev->x, 1);
        w->clientH = std::max(ev->y, 1);
        Bridge_SetContentScaling(w->bw, w->clientW, w->clientH, 0);
        Post(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(w->clientW, w->clientH));
        break;
    }
    case BRIDGE_EV_MOVE:
    {
        // (a full-screen window's position is the screen's: the engine keeps the windowed one in vid_xpos/vid_ypos)
        Win32Wnd *w = W(h);
        if (w && w->bw && (w->wantFullScreen || Bridge_IsFullScreen(w->bw)))
            break;
        Post(h, WM_MOVE, 0, MAKELPARAM((short)ev->x, (short)ev->y));
        break;
    }
    case BRIDGE_EV_FULLSCREEN:
    {
        // the game window entered or left native full screen (also the player, with the green button or
        // Ctrl-Cmd-F): the cursor's screens are the same, but the window that captures it moved
        if (g_captured)
        {
            RefreshCursorScreens();
            ClampVirtualCursor();
        }
        break;
    }
    case BRIDGE_EV_CLOSE: Post(h, WM_CLOSE, 0, 0); break;
    case BRIDGE_EV_QUIT:
    {
        HWND mw = MainWindow();
        if (mw)
            Post(mw, WM_CLOSE, 0, 0);
        else
            exit(0);
        break;
    }
    default: break;
    }
}

void EnsureBridge()
{
    if (g_bridgeReady)
        return;
    g_bridgeReady = true;
    InitKeyMap();
    Bridge_Init(OnBridgeEvent);
}

void PumpIfMainThread(int waitMs)
{
    if (!g_bridgeReady)
        return;
    if (GetCurrentThreadId() != g_mainThreadId)
        return;
    Bridge_PumpEvents(waitMs);
    UpdateCapture();
}

bool MatchMsg(const MSG &m, HWND h, UINT mn, UINT mx)
{
    if (h && m.hwnd != h)
        return false;
    if (mn == 0 && mx == 0)
        return true;
    return m.message >= mn && m.message <= mx;
}

int MonitorIndex(HMONITOR m)
{
    int i = (int)(intptr_t)m - 1;
    return (i >= 0 && i < Bridge_GetScreenCount()) ? i : 0;
}
} // namespace

namespace w32
{
void User32Init()
{
    g_mainThreadId = GetCurrentThreadId();
    EnsureBridge();
    // the main thread drains the Cocoa events whenever it waits; the engine reads the resulting messages on its
    // render thread (main_thread.h)
    SetMainThreadPumpHook([] {
        Bridge_PumpEvents(0);
        UpdateCapture();
    });
    Bridge_GetCursorPos((int *)&g_vcurX, (int *)&g_vcurY);
}

void *GetMetalLayer(HWND hwnd)
{
    Win32Wnd *w = W(hwnd);
    if (!w)
    {
        for (Win32Wnd *x : g_windows)
        {
            if (!x->destroyed && x->bw)
            {
                w = x;
                break;
            }
        }
    }
    return (w && w->bw) ? Bridge_GetMetalLayer(w->bw) : nullptr;
}
} // namespace w32

extern "C" {

// ---------------------------------------------------------------- classes & windows
ATOM RegisterClassExA(const WNDCLASSEXA *wc)
{
    if (!wc || !wc->lpszClassName)
        return 0;
    g_classes[Lower(wc->lpszClassName)] = *wc;
    static ATOM next = 0xC001;
    return next++;
}
ATOM RegisterClassA(const WNDCLASSA *wc)
{
    WNDCLASSEXA ex;
    memset(&ex, 0, sizeof(ex));
    ex.cbSize = sizeof(ex);
    ex.style = wc->style;
    ex.lpfnWndProc = wc->lpfnWndProc;
    ex.hInstance = wc->hInstance;
    ex.hIcon = wc->hIcon;
    ex.hCursor = wc->hCursor;
    ex.lpszClassName = wc->lpszClassName;
    return RegisterClassExA(&ex);
}
BOOL UnregisterClassA(LPCSTR name, HINSTANCE)
{
    if (!name || (uintptr_t)name < 0x10000)
        return FALSE;
    return g_classes.erase(Lower(name)) ? TRUE : FALSE;
}
BOOL GetClassInfoA(HINSTANCE, LPCSTR name, LPWNDCLASSA wc)
{
    auto it = g_classes.find(Lower(name));
    if (it == g_classes.end())
        return FALSE;
    memset(wc, 0, sizeof(*wc));
    wc->lpfnWndProc = it->second.lpfnWndProc;
    wc->lpszClassName = it->second.lpszClassName;
    return TRUE;
}

HWND CreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    (void)parent;
    if (!g_mainThreadId)
        g_mainThreadId = GetCurrentThreadId();
    EnsureBridge();
    Win32Wnd *wnd = new Win32Wnd();
    wnd->className = className && (uintptr_t)className >= 0x10000 ? className : "";
    auto it = g_classes.find(Lower(wnd->className.c_str()));
    wnd->proc = it != g_classes.end() ? it->second.lpfnWndProc : DefWindowProcA;
    wnd->title = title ? title : "";
    wnd->style = style;
    wnd->exStyle = exStyle;
    wnd->instance = inst;
    wnd->id = (LONG_PTR)menu;
    g_windows.push_back(wnd);
    if (parent == HWND_MESSAGE)
        return (HWND)wnd; // message-only window: no Cocoa window
    if (style & WS_CHILD)
    {
        // controls (edit boxes, buttons) of Win32 dialogs: they keep their window procedure and messages, but get
        // no Cocoa window of their own (nothing draws GDI controls here)
        CREATESTRUCTA cs;
        memset(&cs, 0, sizeof(cs));
        cs.lpCreateParams = param;
        cs.hInstance = inst;
        cs.hMenu = menu;
        cs.hwndParent = parent;
        cs.cx = w;
        cs.cy = h;
        cs.x = x;
        cs.y = y;
        cs.style = (LONG)style;
        cs.lpszName = title;
        cs.lpszClass = className;
        cs.dwExStyle = exStyle;
        if (wnd->proc)
        {
            wnd->proc((HWND)wnd, WM_NCCREATE, 0, (LPARAM)&cs);
            if (wnd->proc((HWND)wnd, WM_CREATE, 0, (LPARAM)&cs) == -1)
            {
                DestroyWindow((HWND)wnd);
                return nullptr;
            }
        }
        return (HWND)wnd;
    }
    if (x == CW_USEDEFAULT)
        x = (int)0x80000000;
    if (w == CW_USEDEFAULT || w <= 0)
        w = 1280;
    if (h == CW_USEDEFAULT || h <= 0)
        h = 720;
    const int tb = TitleBarUnits(style);
    wnd->topmost = (exStyle & WS_EX_TOPMOST) != 0;
    const bool defaultPos = x == (int)0x80000000 || y == (int)0x80000000;
    const bool fs = !defaultPos && IsFullScreenRequest(style, wnd->topmost, x, y, w, h);
    if (!defaultPos && !fs && tb)
        KeepFrameOnScreen(&x, &y, w, h);
    wnd->frame = {x, y, x + w, y + h};
    wnd->clientW = std::max(w, 1);
    wnd->clientH = std::max(h - tb, 1);
    wnd->wantFullScreen = fs;
    if (fs)
    {
        // the engine's full-screen window: a titled window of the screen's shape (what it is once the player takes it
        // out of full screen), full screen as soon as it is shown
        int cx, cy, cw, ch;
        FullScreenFallbackRect(ScreenForWindow(x, y), &cx, &cy, &cw, &ch);
        wnd->bw = Bridge_CreateWindow(wnd->title.c_str(), cx, cy, cw, ch, 0, 1, wnd);
        Bridge_SetContentScaling(wnd->bw, wnd->clientW, wnd->clientH, 1);
        Bridge_SetFullScreen(wnd->bw, 1);
    }
    else
    {
        wnd->bw = Bridge_CreateWindow(wnd->title.c_str(), x, y == (int)0x80000000 ? y : y + tb, w, h - tb > 1 ? h - tb : 1,
                                      IsBorderless(style) ? 1 : 0, (style & WS_THICKFRAME) ? 1 : 0, wnd);
        Bridge_SetContentScaling(wnd->bw, wnd->clientW, wnd->clientH, 0);
    }
    CREATESTRUCTA cs;
    memset(&cs, 0, sizeof(cs));
    cs.lpCreateParams = param;
    cs.hInstance = inst;
    cs.hMenu = menu;
    cs.hwndParent = parent;
    cs.cx = w;
    cs.cy = h;
    cs.x = x;
    cs.y = y;
    cs.style = (LONG)style;
    cs.lpszName = title;
    cs.lpszClass = className;
    cs.dwExStyle = exStyle;
    if (wnd->proc)
    {
        wnd->proc((HWND)wnd, WM_NCCREATE, 0, (LPARAM)&cs);
        if (wnd->proc((HWND)wnd, WM_CREATE, 0, (LPARAM)&cs) == -1)
        {
            DestroyWindow((HWND)wnd);
            return nullptr;
        }
    }
    if (style & WS_VISIBLE)
        ShowWindow((HWND)wnd, SW_SHOW);
    return (HWND)wnd;
}

BOOL DestroyWindow(HWND h)
{
    Win32Wnd *w = W(h);
    if (!w)
        return FALSE;
    if (w->proc)
    {
        w->proc(h, WM_DESTROY, 0, 0);
        w->proc(h, WM_NCDESTROY, 0, 0);
    }
    // Detach the bridge window before destroying it. The renderer destroys its window from its own thread, so
    // Bridge_DestroyWindow waits while the main thread runs the teardown, and the main thread goes straight on to
    // its pump (UpdateCapture -> GetForegroundWindow -> MainWindow), which must not find the window still attached.
    BridgeWindow *bw = w->bw;
    w->bw = nullptr;
    w->destroyed = true;
    if (bw)
        Bridge_DestroyWindow(bw);
    if (g_focus == h)
        g_focus = nullptr;
    {
        std::lock_guard<std::recursive_mutex> g(g_queueLock);
        for (auto &m : g_queue)
            if (m.hwnd == h)
                m.message = WM_NULL;
    }
    return TRUE;
}

BOOL ShowWindow(HWND h, int cmd)
{
    Win32Wnd *w = W(h);
    if (!w || !w->bw)
        return FALSE;
    BOOL wasVisible = Bridge_IsVisible(w->bw);
    switch (cmd)
    {
    case SW_HIDE: Bridge_ShowWindow(w->bw, 0); w->style &= ~WS_VISIBLE; break;
    case SW_MINIMIZE:
    case SW_SHOWMINIMIZED:
    case SW_SHOWMINNOACTIVE:
    case SW_FORCEMINIMIZE: Bridge_MinimizeWindow(w->bw); break;
    case SW_RESTORE:
        Bridge_RestoreWindow(w->bw);
        Bridge_ShowWindow(w->bw, 1);
        w->style |= WS_VISIBLE;
        break;
    default:
        if (w->styleDirty && !w->wantFullScreen)
            ApplyStyle(w);
        Bridge_ShowWindow(w->bw, 1); // (a full-screen window goes full screen now: Bridge_SetFullScreen)
        w->style |= WS_VISIBLE;
        g_focus = h;
        if (!wasVisible)
        {
            // Windows activates a top-level window when it is shown: WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS.
            // The app's own activation (Bridge_Init, before WinMain) came while no window existed to receive it,
            // and without it the engine never marks itself active (no mouse input, cursor never released).
            if (g_appActive && Bridge_IsAppActive())
            {
                Post(h, WM_ACTIVATEAPP, 1, 0);
                Post(h, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
                Post(h, WM_SETFOCUS, 0, 0);
            }
        }
        break;
    }
    return wasVisible;
}
BOOL UpdateWindow(HWND) { return TRUE; }
BOOL IsWindow(HWND h) { return W(h) != nullptr; }
BOOL IsWindowVisible(HWND h)
{
    Win32Wnd *w = W(h);
    return w && w->bw && Bridge_IsVisible(w->bw);
}
BOOL IsIconic(HWND h)
{
    Win32Wnd *w = W(h);
    return w && w->bw && Bridge_IsMinimized(w->bw);
}
BOOL IsZoomed(HWND) { return FALSE; }
BOOL EnableWindow(HWND, BOOL) { return TRUE; }

BOOL SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    Win32Wnd *w = W(h);
    if (!w || !w->bw)
        return FALSE;
    // the z-order is only kept for what it says about the window: topmost is the engine's full-screen window
    bool zChanged = false;
    if (!(flags & SWP_NOZORDER))
    {
        if (after == HWND_TOPMOST && !w->topmost)
            w->topmost = zChanged = true;
        else if (after == HWND_NOTOPMOST && w->topmost)
        {
            w->topmost = false;
            zChanged = true;
        }
    }
    const RECT cur = RequestedFrame(h, w);
    if (flags & SWP_NOMOVE)
    {
        x = cur.left;
        y = cur.top;
    }
    if (flags & SWP_NOSIZE)
    {
        cx = cur.right - cur.left;
        cy = cur.bottom - cur.top;
    }
    if (!(flags & SWP_NOMOVE) || !(flags & SWP_NOSIZE) || (flags & SWP_FRAMECHANGED) || zChanged || w->styleDirty)
        ApplyFrame(w, x, y, cx, cy);
    if (flags & SWP_SHOWWINDOW)
        ShowWindow(h, SW_SHOW);
    if (flags & SWP_HIDEWINDOW)
        ShowWindow(h, SW_HIDE);
    return TRUE;
}
BOOL MoveWindow(HWND h, int x, int y, int w, int hh, BOOL) { return SetWindowPos(h, nullptr, x, y, w, hh, SWP_NOZORDER); }

BOOL GetWindowRect(HWND h, LPRECT r)
{
    if (h == (HWND)&g_desktopTag || h == nullptr)
    {
        BridgeRect full;
        Bridge_GetScreenRect(0, &full, nullptr, nullptr);
        r->left = full.left;
        r->top = full.top;
        r->right = full.right;
        r->bottom = full.bottom;
        return TRUE;
    }
    Win32Wnd *w = W(h);
    if (!w || !w->bw)
        return FALSE;
    // where the window is on the screen (in full screen: the screen, no title bar)
    BridgeRect c;
    Bridge_GetClientRect(w->bw, &c);
    const int tb = Bridge_IsFullScreen(w->bw) ? 0 : TitleBarUnits(w->style);
    r->left = c.left;
    r->top = c.top - tb;
    r->right = c.right;
    r->bottom = c.bottom;
    return TRUE;
}
BOOL GetClientRect(HWND h, LPRECT r)
{
    if (h == (HWND)&g_desktopTag || h == nullptr)
        return GetWindowRect(h, r);
    Win32Wnd *w = W(h);
    if (!w || !w->bw)
    {
        memset(r, 0, sizeof(*r));
        return FALSE;
    }
    // the size the game draws at, whatever size the view shows it at
    r->left = 0;
    r->top = 0;
    r->right = w->clientW;
    r->bottom = w->clientH;
    return TRUE;
}
BOOL AdjustWindowRectEx(LPRECT r, DWORD style, BOOL, DWORD)
{
    r->top -= TitleBarUnits(style);
    return TRUE;
}
BOOL AdjustWindowRect(LPRECT r, DWORD style, BOOL menu) { return AdjustWindowRectEx(r, style, menu, 0); }
BOOL ClientToScreen(HWND h, LPPOINT p)
{
    Win32Wnd *w = W(h);
    if (!w || !w->bw)
        return h == (HWND)&g_desktopTag;
    // (the point under the middle of the client pixel, so ScreenToClient gives the pixel back)
    const ClientMap m = GetClientMap(w);
    p->x = (LONG)floor(m.ox + (p->x + 0.5) * m.sx);
    p->y = (LONG)floor(m.oy + (p->y + 0.5) * m.sy);
    return TRUE;
}
BOOL ScreenToClient(HWND h, LPPOINT p)
{
    Win32Wnd *w = W(h);
    if (!w || !w->bw)
        return h == (HWND)&g_desktopTag;
    const ClientMap m = GetClientMap(w);
    p->x = (LONG)floor((p->x - m.ox) / m.sx);
    p->y = (LONG)floor((p->y - m.oy) / m.sy);
    return TRUE;
}
int MapWindowPoints(HWND from, HWND to, LPPOINT pts, UINT n)
{
    for (UINT i = 0; i < n; ++i)
    {
        if (from)
            ClientToScreen(from, &pts[i]);
        if (to)
            ScreenToClient(to, &pts[i]);
    }
    return 0;
}

// The 32-bit forms carry a window procedure as a 32-bit (Ptr32) handle: the engine's decompiled code converts the
// function pointer with BO1_P2I before passing it, and converts the old procedure back with BO1_I2P.
LONG GetWindowLongA(HWND h, int idx)
{
    if (idx == GWL_WNDPROC)
        return (LONG)Ptr32_Encode((const void *)GetWindowLongPtrA(h, idx));
    return (LONG)GetWindowLongPtrA(h, idx);
}
LONG SetWindowLongA(HWND h, int idx, LONG v)
{
    if (idx == GWL_WNDPROC)
        return (LONG)Ptr32_Encode((const void *)SetWindowLongPtrA(h, idx, (LONG_PTR)Ptr32_Decode((uint32_t)v)));
    return (LONG)SetWindowLongPtrA(h, idx, v);
}
LONG_PTR GetWindowLongPtrA(HWND h, int idx)
{
    Win32Wnd *w = W(h);
    if (!w)
        return 0;
    switch (idx)
    {
    case GWL_STYLE: return (LONG_PTR)(LONG)w->style;
    case GWL_EXSTYLE: return (LONG_PTR)(LONG)w->exStyle;
    case GWLP_USERDATA: return w->userData;
    case GWLP_WNDPROC: return (LONG_PTR)w->proc;
    case GWLP_HINSTANCE: return (LONG_PTR)w->instance;
    case GWLP_ID: return w->id;
    default: return 0;
    }
}
LONG_PTR SetWindowLongPtrA(HWND h, int idx, LONG_PTR v)
{
    Win32Wnd *w = W(h);
    if (!w)
        return 0;
    LONG_PTR old = GetWindowLongPtrA(h, idx);
    switch (idx)
    {
    case GWL_STYLE:
        // As on Windows, the frame follows a style change at the next SetWindowPos (the engine sends SWP_FRAMECHANGED
        // right after): whether the window is the engine's full-screen one is only known with its position, and a
        // window going full screen must not lose its title bar first.
        if ((DWORD)v != w->style)
            w->styleDirty = true;
        w->style = (DWORD)v;
        break;
    case GWL_EXSTYLE: w->exStyle = (DWORD)v; break;
    case GWLP_USERDATA: w->userData = v; break;
    case GWLP_WNDPROC: w->proc = (WNDPROC)v; break;
    case GWLP_ID: w->id = v; break;
    default: break;
    }
    return old;
}
BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT *p)
{
    memset(p, 0, sizeof(*p));
    p->length = sizeof(*p);
    p->showCmd = IsIconic(h) ? SW_SHOWMINIMIZED : SW_SHOWNORMAL;
    return GetWindowRect(h, &p->rcNormalPosition);
}
BOOL SetWindowPlacement(HWND h, const WINDOWPLACEMENT *p)
{
    const RECT &r = p->rcNormalPosition;
    SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
    ShowWindow(h, (int)p->showCmd);
    return TRUE;
}
BOOL SetWindowTextA(HWND h, LPCSTR s)
{
    Win32Wnd *w = W(h);
    if (!w)
        return FALSE;
    w->title = s ? s : "";
    if (w->bw)
        Bridge_SetTitle(w->bw, w->title.c_str());
    return TRUE;
}
int GetWindowTextA(HWND h, LPSTR buf, int n)
{
    Win32Wnd *w = W(h);
    if (!w || !buf || n <= 0)
        return 0;
    strncpy(buf, w->title.c_str(), (size_t)n - 1);
    buf[n - 1] = 0;
    return (int)strlen(buf);
}
int GetWindowTextLengthA(HWND h)
{
    Win32Wnd *w = W(h);
    return w ? (int)w->title.size() : 0;
}
HWND GetActiveWindow(void) { return g_appActive ? MainWindow() : nullptr; }
HWND SetActiveWindow(HWND h)
{
    HWND old = GetActiveWindow();
    SetForegroundWindow(h);
    return old;
}
HWND GetForegroundWindow(void)
{
    if (!g_appActive || !Bridge_IsAppActive())
        return nullptr;
    HWND mw = MainWindow();
    Win32Wnd *w = W(mw);
    if (w && w->bw && Bridge_IsMinimized(w->bw))
        return nullptr;
    return mw;
}
BOOL SetForegroundWindow(HWND h)
{
    Win32Wnd *w = W(h);
    if (!w || !w->bw)
        return FALSE;
    Bridge_ActivateApp();
    Bridge_FocusWindow(w->bw);
    g_focus = h;
    return TRUE;
}
HWND GetFocus(void) { return g_appActive ? (g_focus ? g_focus : MainWindow()) : nullptr; }
HWND SetFocus(HWND h)
{
    HWND old = GetFocus();
    Win32Wnd *w = W(h);
    if (w && w->bw)
        Bridge_FocusWindow(w->bw);
    g_focus = h;
    return old;
}
HWND GetDesktopWindow(void) { return (HWND)&g_desktopTag; }
HWND GetParent(HWND) { return nullptr; }
HWND FindWindowA(LPCSTR cls, LPCSTR title)
{
    for (Win32Wnd *w : g_windows)
    {
        if (w->destroyed)
            continue;
        if (cls && (uintptr_t)cls >= 0x10000 && Lower(cls) != Lower(w->className.c_str()))
            continue;
        if (title && w->title != title)
            continue;
        return (HWND)w;
    }
    return nullptr;
}
BOOL BringWindowToTop(HWND h) { return SetForegroundWindow(h); }
HWND GetCapture(void) { return g_captured ? MainWindow() : nullptr; }
HWND SetCapture(HWND h) { return h; }
BOOL ReleaseCapture(void) { return TRUE; }
BOOL InvalidateRect(HWND, const RECT *, BOOL) { return TRUE; }
BOOL ValidateRect(HWND, const RECT *) { return TRUE; }
HDC BeginPaint(HWND, LPPAINTSTRUCT ps)
{
    if (ps)
        memset(ps, 0, sizeof(*ps));
    return (HDC)&g_dcTag;
}
BOOL EndPaint(HWND, const PAINTSTRUCT *) { return TRUE; }
BOOL FlashWindow(HWND, BOOL) { return TRUE; }
BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp)
{
    std::vector<Win32Wnd *> copy;
    for (Win32Wnd *w : g_windows)
        copy.push_back(w);
    for (Win32Wnd *w : copy)
        if (!w->destroyed && !fn((HWND)w, lp))
            break;
    return TRUE;
}
BOOL EnumThreadWindows(DWORD, BOOL (*fn)(HWND, LPARAM), LPARAM lp) { return EnumWindows(fn, lp); }
DWORD GetWindowThreadProcessId(HWND, LPDWORD pid)
{
    if (pid)
        *pid = GetCurrentProcessId();
    return g_mainThreadId;
}
BOOL SetPropA(HWND h, LPCSTR name, HANDLE data)
{
    Win32Wnd *w = W(h);
    if (!w || !name)
        return FALSE;
    w->props[name] = data;
    return TRUE;
}
HANDLE GetPropA(HWND h, LPCSTR name)
{
    Win32Wnd *w = W(h);
    if (!w || !name)
        return nullptr;
    auto it = w->props.find(name);
    return it == w->props.end() ? nullptr : it->second;
}
HANDLE RemovePropA(HWND h, LPCSTR name)
{
    HANDLE v = GetPropA(h, name);
    Win32Wnd *w = W(h);
    if (w && name)
        w->props.erase(name);
    return v;
}
BOOL CloseWindow(HWND h) { return ShowWindow(h, SW_MINIMIZE) || TRUE; }

// ---------------------------------------------------------------- messages
LRESULT DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg)
    {
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE)
            Post(h, WM_CLOSE, 0, 0);
        else if ((wp & 0xFFF0) == SC_MINIMIZE)
            ShowWindow(h, SW_MINIMIZE);
        return 0;
    case WM_NCCREATE: return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: return TRUE;
    default: return 0;
    }
}
LRESULT CallWindowProcA(WNDPROC proc, HWND h, UINT msg, WPARAM wp, LPARAM lp) { return proc ? proc(h, msg, wp, lp) : 0; }

BOOL PeekMessageA(LPMSG out, HWND h, UINT mn, UINT mx, UINT remove)
{
    PumpIfMainThread(0);
    std::lock_guard<std::recursive_mutex> g(g_queueLock);
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
    {
        if (it->message == WM_NULL && it->hwnd)
        {
            continue;
        }
        if (!MatchMsg(*it, h, mn, mx))
            continue;
        *out = *it;
        if (remove & PM_REMOVE)
            g_queue.erase(it);
        return TRUE;
    }
    if (g_quitPosted && !h && (mn == 0 || (WM_QUIT >= mn && WM_QUIT <= mx)))
    {
        memset(out, 0, sizeof(*out));
        out->message = WM_QUIT;
        out->wParam = (WPARAM)g_quitCode;
        if (remove & PM_REMOVE)
            g_quitPosted = false;
        return TRUE;
    }
    return FALSE;
}
BOOL GetMessageA(LPMSG out, HWND h, UINT mn, UINT mx)
{
    for (;;)
    {
        if (PeekMessageA(out, h, mn, mx, PM_REMOVE))
            return out->message != WM_QUIT;
        PumpIfMainThread(10);
        if (GetCurrentThreadId() != g_mainThreadId)
            Sleep(1);
    }
}
BOOL TranslateMessage(const MSG *)
{
    // WM_CHAR is queued directly behind its key-down by the Cocoa translation (the same order TranslateMessage
    // produces on Windows).
    return TRUE;
}
LRESULT DispatchMessageA(const MSG *m)
{
    if (!m || m->message == WM_NULL)
        return 0;
    Win32Wnd *w = W(m->hwnd);
    if (!w || !w->proc)
        return 0;
    return w->proc(m->hwnd, m->message, m->wParam, m->lParam);
}
BOOL PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Post(h, msg, wp, lp);
    return TRUE;
}
LRESULT SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Win32Wnd *w = W(h);
    if (!w || !w->proc)
        return 0;
    return w->proc(h, msg, wp, lp);
}
BOOL PostThreadMessageA(DWORD, UINT msg, WPARAM wp, LPARAM lp)
{
    Post(nullptr, msg, wp, lp);
    return TRUE;
}
void PostQuitMessage(int code)
{
    g_quitPosted = true;
    g_quitCode = code;
}
BOOL WaitMessage(void)
{
    PumpIfMainThread(10);
    return TRUE;
}
DWORD MsgWaitForMultipleObjects(DWORD n, const HANDLE *handles, BOOL all, DWORD ms, DWORD)
{
    DWORD start = GetTickCount();
    for (;;)
    {
        if (n)
        {
            DWORD r = WaitForMultipleObjects(n, handles, all, 0);
            if (r != WAIT_TIMEOUT)
                return r;
        }
        PumpIfMainThread(0);
        {
            std::lock_guard<std::recursive_mutex> g(g_queueLock);
            if (!g_queue.empty())
                return WAIT_OBJECT_0 + n;
        }
        if (ms != INFINITE && GetTickCount() - start >= ms)
            return WAIT_TIMEOUT;
        Sleep(1);
    }
}
UINT RegisterWindowMessageA(LPCSTR name)
{
    std::string k = name ? name : "";
    auto it = g_registeredMessages.find(k);
    if (it != g_registeredMessages.end())
        return it->second;
    UINT id = g_nextRegisteredMessage++;
    g_registeredMessages[k] = id;
    return id;
}
DWORD GetMessageTime(void) { return g_lastMsgTime; }
LPARAM GetMessageExtraInfo(void) { return 0; }
UINT_PTR SetTimer(HWND, UINT_PTR id, UINT, void *) { return id ? id : 1; }
BOOL KillTimer(HWND, UINT_PTR) { return TRUE; }
HHOOK SetWindowsHookExA(int, HOOKPROC, HINSTANCE, DWORD) { return (HHOOK)&g_hookTag; }
BOOL UnhookWindowsHookEx(HHOOK) { return TRUE; }
LRESULT CallNextHookEx(HHOOK, int, WPARAM, LPARAM) { return 0; }

// ---------------------------------------------------------------- keyboard & mouse
SHORT GetAsyncKeyState(int vk)
{
    InitKeyMap();
    bool down = false;
    switch (vk)
    {
    case VK_LBUTTON: down = (g_buttons & MK_LBUTTON) != 0; break;
    case VK_RBUTTON: down = (g_buttons & MK_RBUTTON) != 0; break;
    case VK_MBUTTON: down = (g_buttons & MK_MBUTTON) != 0; break;
    case VK_XBUTTON1: down = (g_buttons & MK_XBUTTON1) != 0; break;
    case VK_XBUTTON2: down = (g_buttons & MK_XBUTTON2) != 0; break;
    case VK_SHIFT: down = Bridge_IsMacKeyDown(kVK_Shift) || Bridge_IsMacKeyDown(kVK_RightShift); break;
    case VK_LSHIFT: down = Bridge_IsMacKeyDown(kVK_Shift); break;
    case VK_RSHIFT: down = Bridge_IsMacKeyDown(kVK_RightShift); break;
    case VK_CONTROL: down = Bridge_IsMacKeyDown(kVK_Control) || Bridge_IsMacKeyDown(kVK_RightControl); break;
    case VK_LCONTROL: down = Bridge_IsMacKeyDown(kVK_Control); break;
    case VK_RCONTROL: down = Bridge_IsMacKeyDown(kVK_RightControl); break;
    case VK_MENU: down = Bridge_IsMacKeyDown(kVK_Option) || Bridge_IsMacKeyDown(kVK_RightOption); break;
    case VK_LMENU: down = Bridge_IsMacKeyDown(kVK_Option); break;
    case VK_RMENU: down = Bridge_IsMacKeyDown(kVK_RightOption); break;
    default:
        if (vk >= 0 && vk < 256)
        {
            for (int i = 0; i < 2; ++i)
                if (g_vkToMac[vk][i] != 0xFFFF && Bridge_IsMacKeyDown(g_vkToMac[vk][i]))
                    down = true;
        }
        break;
    }
    return down ? (SHORT)0x8000 : 0;
}
SHORT GetKeyState(int vk)
{
    if (vk == VK_CAPITAL)
        return (Bridge_CurrentModifiers() & BRIDGE_MOD_CAPSLOCK) ? 1 : 0;
    if (vk == VK_NUMLOCK)
        return 1; // Macs have no Num Lock: the keypad always types digits
    if (vk == VK_SCROLL)
        return 0;
    return GetAsyncKeyState(vk);
}
BOOL GetKeyboardState(PBYTE state)
{
    for (int i = 0; i < 256; ++i)
        state[i] = (GetKeyState(i) & 0x8000) ? 0x80 : 0;
    if (GetKeyState(VK_CAPITAL) & 1)
        state[VK_CAPITAL] |= 1;
    state[VK_NUMLOCK] |= 1;
    return TRUE;
}
UINT MapVirtualKeyA(UINT code, UINT type)
{
    switch (type)
    {
    case MAPVK_VK_TO_VSC: return (UINT)VkToScan((int)code);
    case MAPVK_VSC_TO_VK:
    case MAPVK_VSC_TO_VK_EX: return (UINT)ScanToVk((int)(code & 0xFF), (code & 0xE000) != 0);
    case MAPVK_VK_TO_CHAR: return VkToChar(code);
    default: return 0;
    }
}
int ToAscii(UINT vk, UINT, const BYTE *state, LPWORD out, UINT)
{
    UINT c = VkToChar(vk);
    if (!c)
        return 0;
    bool shift = state && (state[VK_SHIFT] & 0x80);
    bool caps = state && (state[VK_CAPITAL] & 1);
    if (c >= 'A' && c <= 'Z')
        c = (shift != caps) ? c : c + 32;
    else if (shift)
        c = ShiftedChar(c);
    *out = (WORD)c;
    return 1;
}
int ToUnicode(UINT vk, UINT scan, const BYTE *state, LPWSTR out, int n, UINT flags)
{
    WORD c;
    if (n < 1 || !ToAscii(vk, scan, state, &c, flags))
        return 0;
    out[0] = c;
    return 1;
}
HKL GetKeyboardLayout(DWORD) { return (HKL)(uintptr_t)0x04090409; }
int GetKeyNameTextA(LONG lp, LPSTR buf, int n)
{
    int scan = (lp >> 16) & 0xFF;
    bool ext = (lp >> 24) & 1;
    int vk = ScanToVk(scan, ext);
    const char *name = nullptr;
    char tmp[16];
    switch (vk)
    {
    case VK_ESCAPE: name = "Esc"; break;
    case VK_BACK: name = "Backspace"; break;
    case VK_TAB: name = "Tab"; break;
    case VK_RETURN: name = ext ? "Num Enter" : "Enter"; break;
    case VK_SPACE: name = "Space"; break;
    case VK_SHIFT: name = "Shift"; break;
    case VK_CONTROL: name = ext ? "Right Ctrl" : "Ctrl"; break;
    case VK_MENU: name = ext ? "Right Alt" : "Alt"; break;
    case VK_LWIN: name = "Command"; break;
    case VK_RWIN: name = "Right Command"; break;
    case VK_CAPITAL: name = "Caps Lock"; break;
    case VK_UP: name = "Up"; break;
    case VK_DOWN: name = "Down"; break;
    case VK_LEFT: name = "Left"; break;
    case VK_RIGHT: name = "Right"; break;
    case VK_HOME: name = "Home"; break;
    case VK_END: name = "End"; break;
    case VK_PRIOR: name = "Page Up"; break;
    case VK_NEXT: name = "Page Down"; break;
    case VK_INSERT: name = "Insert"; break;
    case VK_DELETE: name = "Delete"; break;
    default:
        if (vk >= VK_F1 && vk <= VK_F19)
        {
            snprintf(tmp, sizeof(tmp), "F%d", vk - VK_F1 + 1);
            name = tmp;
        }
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        {
            snprintf(tmp, sizeof(tmp), "Num %d", vk - VK_NUMPAD0);
            name = tmp;
        }
        else if (UINT c = VkToChar((UINT)vk))
        {
            tmp[0] = (char)c;
            tmp[1] = 0;
            name = tmp;
        }
        break;
    }
    if (!name || !buf || n <= 0)
        return 0;
    strncpy(buf, name, (size_t)n - 1);
    buf[n - 1] = 0;
    return (int)strlen(buf);
}
BOOL RegisterRawInputDevices(PCRAWINPUTDEVICE, UINT, UINT) { return TRUE; }
UINT GetRawInputData(HRAWINPUT, UINT, LPVOID, PUINT size, UINT)
{
    if (size)
        *size = 0;
    return (UINT)-1;
}
BOOL GetCursorPos(LPPOINT p)
{
    if (!p)
        return FALSE;
    if (!g_captured && g_bridgeReady)
        Bridge_GetCursorPos((int *)&g_vcurX, (int *)&g_vcurY);
    p->x = g_vcurX;
    p->y = g_vcurY;
    return TRUE;
}
BOOL SetCursorPos(int x, int y)
{
    g_vcurX = x;
    g_vcurY = y;
    if (g_captured)
        ClampVirtualCursor(); // as Windows keeps SetCursorPos on the desktop
    if (!g_captured && g_bridgeReady && Bridge_IsAppActive())
        Bridge_WarpCursor(x, y);
    return TRUE;
}
int ShowCursor(BOOL show)
{
    g_cursorCount += show ? 1 : -1;
    if (g_bridgeReady)
    {
        if (g_cursorCount >= 0 && !g_captured)
            SetCursorShown(true);
        else if (g_cursorCount < 0)
            SetCursorShown(false);
        UpdateCapture();
    }
    return g_cursorCount;
}
HCURSOR SetCursor(HCURSOR c)
{
    static HCURSOR current;
    HCURSOR old = current;
    current = c;
    return old;
}
HCURSOR GetCursor(void) { return (HCURSOR)&g_cursorTag; }
HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return (HCURSOR)&g_cursorTag; }
HICON LoadIconA(HINSTANCE, LPCSTR) { return (HICON)&g_iconTag; }
HANDLE LoadImageA(HINSTANCE, LPCSTR, UINT type, int, int, UINT) { return type == IMAGE_CURSOR ? (HANDLE)&g_cursorTag : (HANDLE)&g_iconTag; }
BOOL DestroyIcon(HICON) { return TRUE; }
BOOL ClipCursor(const RECT *) { return TRUE; }
BOOL GetClipCursor(LPRECT r)
{
    return GetWindowRect(nullptr, r);
}
BOOL TrackMouseEvent(LPTRACKMOUSEEVENT) { return TRUE; }
UINT GetDoubleClickTime(void) { return 500; }

// ---------------------------------------------------------------- clipboard
BOOL OpenClipboard(HWND) { return TRUE; }
BOOL CloseClipboard(void) { return TRUE; }
BOOL EmptyClipboard(void)
{
    Bridge_SetClipboardText("");
    return TRUE;
}
HANDLE GetClipboardData(UINT fmt)
{
    if (fmt != CF_TEXT && fmt != CF_OEMTEXT && fmt != CF_UNICODETEXT)
        return nullptr;
    free(g_clipboardText);
    g_clipboardText = Bridge_GetClipboardText();
    if (!g_clipboardText)
        return nullptr;
    if (fmt == CF_UNICODETEXT)
    {
        static std::vector<WCHAR> wide;
        int n = MultiByteToWideChar(CP_UTF8, 0, g_clipboardText, -1, nullptr, 0);
        wide.resize((size_t)n + 1);
        MultiByteToWideChar(CP_UTF8, 0, g_clipboardText, -1, wide.data(), n);
        return (HANDLE)wide.data();
    }
    return (HANDLE)g_clipboardText;
}
HANDLE SetClipboardData(UINT fmt, HANDLE mem)
{
    if (!mem)
        return nullptr;
    if (fmt == CF_UNICODETEXT)
    {
        char buf[8192];
        WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)mem, -1, buf, sizeof(buf), nullptr, nullptr);
        Bridge_SetClipboardText(buf);
    }
    else
        Bridge_SetClipboardText((const char *)mem);
    return mem;
}
BOOL IsClipboardFormatAvailable(UINT fmt)
{
    if (fmt != CF_TEXT && fmt != CF_OEMTEXT && fmt != CF_UNICODETEXT)
        return FALSE;
    char *t = Bridge_GetClipboardText();
    bool ok = t && *t;
    free(t);
    return ok;
}

// ---------------------------------------------------------------- dialogs
int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type)
{
    EnsureBridge();
    int buttons = 0;
    switch (type & 0xF)
    {
    case MB_OKCANCEL:
    case MB_RETRYCANCEL:
    case MB_ABORTRETRYIGNORE:
    case MB_CANCELTRYCONTINUE: buttons = 1; break;
    case MB_YESNOCANCEL: buttons = 3; break;
    case MB_YESNO: buttons = 4; break;
    default: buttons = 0; break;
    }
    int icon = (type & 0xF0) == MB_ICONHAND ? 2 : (type & 0xF0) == MB_ICONEXCLAMATION ? 1 : 0;
    fprintf(stderr, "[MessageBox] %s: %s\n", caption ? caption : "", text ? text : "");
    int r = Bridge_MessageBox(caption ? caption : "", text ? text : "", buttons, icon);
    switch (buttons)
    {
    case 1: return r == 0 ? IDOK : IDCANCEL;
    case 3: return r == 0 ? IDYES : r == 1 ? IDNO : IDCANCEL;
    case 4: return r == 0 ? IDYES : IDNO;
    default: return IDOK;
    }
}
int MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type)
{
    char t[4096], c[512];
    WideCharToMultiByte(CP_UTF8, 0, text ? text : L"", -1, t, sizeof(t), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, caption ? caption : L"", -1, c, sizeof(c), nullptr, nullptr);
    return MessageBoxA(h, t, c, type);
}
int MessageBoxExA(HWND h, LPCSTR text, LPCSTR caption, UINT type, WORD) { return MessageBoxA(h, text, caption, type); }
BOOL MessageBeep(UINT) { return TRUE; }

// ---------------------------------------------------------------- monitors / display
int GetSystemMetrics(int idx)
{
    EnsureBridge();
    BridgeRect full, work;
    Bridge_GetScreenRect(0, &full, &work, nullptr);
    switch (idx)
    {
    case SM_CXSCREEN:
    case SM_CXFULLSCREEN: return full.right - full.left;
    case SM_CYSCREEN:
    case SM_CYFULLSCREEN: return full.bottom - full.top;
    case SM_XVIRTUALSCREEN:
    case SM_YVIRTUALSCREEN:
    case SM_CXVIRTUALSCREEN:
    case SM_CYVIRTUALSCREEN:
    {
        int n = Bridge_GetScreenCount();
        BridgeRect u = full;
        for (int i = 1; i < n; ++i)
        {
            BridgeRect r;
            Bridge_GetScreenRect(i, &r, nullptr, nullptr);
            u.left = std::min(u.left, r.left);
            u.top = std::min(u.top, r.top);
            u.right = std::max(u.right, r.right);
            u.bottom = std::max(u.bottom, r.bottom);
        }
        if (idx == SM_XVIRTUALSCREEN) return u.left;
        if (idx == SM_YVIRTUALSCREEN) return u.top;
        if (idx == SM_CXVIRTUALSCREEN) return u.right - u.left;
        return u.bottom - u.top;
    }
    case SM_CYCAPTION: return (int)(kTitleBar / Bridge_UnitScale() + 0.5);
    case SM_CXBORDER:
    case SM_CYBORDER:
    case SM_CXFRAME:
    case SM_CYFRAME:
    case SM_CXDLGFRAME:
    case SM_CYDLGFRAME:
    case SM_CXPADDEDBORDER: return 0;
    case SM_CMONITORS: return Bridge_GetScreenCount();
    case SM_MOUSEPRESENT: return 1;
    case SM_REMOTESESSION: return 0;
    case SM_CXICON:
    case SM_CYICON: return 32;
    case SM_CXSMICON:
    case SM_CYSMICON: return 16;
    default: return 0;
    }
}
BOOL SystemParametersInfoA(UINT action, UINT, PVOID param, UINT)
{
    if (action == SPI_GETWORKAREA && param)
    {
        BridgeRect work;
        Bridge_GetScreenRect(0, nullptr, &work, nullptr);
        RECT *r = (RECT *)param;
        r->left = work.left;
        r->top = work.top;
        r->right = work.right;
        r->bottom = work.bottom;
        return TRUE;
    }
    if (action == SPI_GETSCREENSAVEACTIVE && param)
    {
        *(BOOL *)param = FALSE;
        return TRUE;
    }
    if (action == SPI_GETFOREGROUNDLOCKTIMEOUT && param)
    {
        *(DWORD *)param = 0;
        return TRUE;
    }
    return TRUE;
}
HMONITOR MonitorFromWindow(HWND h, DWORD)
{
    EnsureBridge();
    RECT r;
    if (!GetWindowRect(h, &r))
        return (HMONITOR)(intptr_t)1;
    return (HMONITOR)(intptr_t)(Bridge_ScreenForPoint((r.left + r.right) / 2, (r.top + r.bottom) / 2) + 1);
}
HMONITOR MonitorFromPoint(POINT pt, DWORD)
{
    EnsureBridge();
    return (HMONITOR)(intptr_t)(Bridge_ScreenForPoint(pt.x, pt.y) + 1);
}
HMONITOR MonitorFromRect(LPCRECT r, DWORD)
{
    EnsureBridge();
    return (HMONITOR)(intptr_t)(Bridge_ScreenForPoint((r->left + r->right) / 2, (r->top + r->bottom) / 2) + 1);
}
BOOL GetMonitorInfoA(HMONITOR m, LPMONITORINFO mi)
{
    EnsureBridge();
    int i = MonitorIndex(m);
    BridgeRect full, work;
    if (!Bridge_GetScreenRect(i, &full, &work, nullptr))
        return FALSE;
    mi->rcMonitor = {full.left, full.top, full.right, full.bottom};
    mi->rcWork = {work.left, work.top, work.right, work.bottom};
    mi->dwFlags = i == 0 ? MONITORINFOF_PRIMARY : 0;
    if (mi->cbSize >= sizeof(MONITORINFOEXA))
        snprintf(((MONITORINFOEXA *)mi)->szDevice, 32, "\\\\.\\DISPLAY%d", i + 1);
    return TRUE;
}
BOOL EnumDisplayMonitors(HDC, LPCRECT, MONITORENUMPROC fn, LPARAM lp)
{
    EnsureBridge();
    int n = Bridge_GetScreenCount();
    for (int i = 0; i < n; ++i)
    {
        BridgeRect full;
        Bridge_GetScreenRect(i, &full, nullptr, nullptr);
        RECT r = {full.left, full.top, full.right, full.bottom};
        if (!fn((HMONITOR)(intptr_t)(i + 1), (HDC)&g_dcTag, &r, lp))
            break;
    }
    return TRUE;
}
static int DeviceIndex(LPCSTR dev)
{
    if (!dev)
        return 0;
    const char *p = strstr(dev, "DISPLAY");
    return p ? std::max(0, atoi(p + 7) - 1) : 0;
}
BOOL EnumDisplaySettingsExA(LPCSTR dev, DWORD mode, DEVMODEA *dm, DWORD)
{
    EnsureBridge();
    int screen = DeviceIndex(dev);
    memset(dm, 0, sizeof(*dm));
    dm->dmSize = sizeof(*dm);
    dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    dm->dmBitsPerPel = 32;
    if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS)
    {
        BridgeRect full;
        double hz = 60;
        Bridge_GetScreenRect(screen, &full, nullptr, &hz);
        dm->dmPelsWidth = (DWORD)(full.right - full.left);
        dm->dmPelsHeight = (DWORD)(full.bottom - full.top);
        dm->dmDisplayFrequency = (DWORD)(hz + 0.5);
        dm->dmPosition.x = full.left;
        dm->dmPosition.y = full.top;
        return TRUE;
    }
    int w[128], h[128], r[128];
    int n = Bridge_GetScreenModes(screen, w, h, r, 128);
    if ((int)mode >= n)
        return FALSE;
    dm->dmPelsWidth = (DWORD)w[mode];
    dm->dmPelsHeight = (DWORD)h[mode];
    dm->dmDisplayFrequency = (DWORD)r[mode];
    return TRUE;
}
BOOL EnumDisplaySettingsA(LPCSTR dev, DWORD mode, DEVMODEA *dm) { return EnumDisplaySettingsExA(dev, mode, dm, 0); }
LONG ChangeDisplaySettingsA(DEVMODEA *, DWORD) { return DISP_CHANGE_SUCCESSFUL; }
LONG ChangeDisplaySettingsExA(LPCSTR, DEVMODEA *, HWND, DWORD, LPVOID) { return DISP_CHANGE_SUCCESSFUL; }
BOOL EnumDisplayDevicesA(LPCSTR, DWORD i, PDISPLAY_DEVICEA dd, DWORD)
{
    EnsureBridge();
    if ((int)i >= Bridge_GetScreenCount())
        return FALSE;
    memset(dd, 0, sizeof(*dd));
    dd->cb = sizeof(*dd);
    snprintf(dd->DeviceName, sizeof(dd->DeviceName), "\\\\.\\DISPLAY%u", i + 1);
    snprintf(dd->DeviceString, sizeof(dd->DeviceString), "Apple Display %u", i + 1);
    dd->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | (i == 0 ? DISPLAY_DEVICE_PRIMARY_DEVICE : 0);
    return TRUE;
}
BOOL SetProcessDPIAware(void) { return TRUE; }
BOOL IsProcessDPIAware(void) { return TRUE; }
BOOL SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT) { return TRUE; }
DPI_AWARENESS_CONTEXT SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT c) { return c; }
UINT GetDpiForWindow(HWND) { return 96; }
UINT GetDpiForSystem(void) { return 96; }

// ---------------------------------------------------------------- GDI (nothing is drawn through GDI)
HDC GetDC(HWND) { return (HDC)&g_dcTag; }
HDC GetWindowDC(HWND) { return (HDC)&g_dcTag; }
int ReleaseDC(HWND, HDC) { return 1; }
int GetDeviceCaps(HDC, int index)
{
    switch (index)
    {
    case HORZRES:
    case DESKTOPHORZRES: return GetSystemMetrics(SM_CXSCREEN);
    case VERTRES:
    case DESKTOPVERTRES: return GetSystemMetrics(SM_CYSCREEN);
    case BITSPIXEL: return 32;
    case LOGPIXELSX:
    case LOGPIXELSY: return 96;
    case VREFRESH:
    {
        double hz = 60;
        Bridge_GetScreenRect(0, nullptr, nullptr, &hz);
        return (int)(hz + 0.5);
    }
    default: return 0;
    }
}
BOOL SetDeviceGammaRamp(HDC, LPVOID) { return TRUE; }
BOOL GetDeviceGammaRamp(HDC, LPVOID ramp)
{
    WORD *r = (WORD *)ramp;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i)
            r[c * 256 + i] = (WORD)(i * 257);
    return TRUE;
}
HGDIOBJ GetStockObject(int) { return (HGDIOBJ)&g_dcTag; }
BOOL DeleteObject(HGDIOBJ) { return TRUE; }
HGDIOBJ SelectObject(HDC, HGDIOBJ h) { return h; }
HDC CreateCompatibleDC(HDC) { return (HDC)&g_dcTag; }
BOOL DeleteDC(HDC) { return TRUE; }
HBRUSH CreateSolidBrush(COLORREF) { return (HBRUSH)&g_dcTag; }
int FillRect(HDC, const RECT *, HBRUSH) { return 1; }
COLORREF SetTextColor(HDC, COLORREF c) { return c; }
COLORREF SetBkColor(HDC, COLORREF c) { return c; }
int SetBkMode(HDC, int m) { return m; }

// ---------------------------------------------------------------- shell
HINSTANCE ShellExecuteA(HWND, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, INT)
{
    (void)op;
    (void)dir;
    if (!file)
        return (HINSTANCE)(intptr_t)2;
    std::string target = file;
    if (params && *params && target.find("://") == std::string::npos)
    {
        // launching a program with parameters (e.g. relaunching the game)
        std::string cmd = std::string("\"") + file + "\" " + params;
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        if (CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            return (HINSTANCE)(intptr_t)33;
        return (HINSTANCE)(intptr_t)2;
    }
    Bridge_OpenURL(file);
    return (HINSTANCE)(intptr_t)33;
}
BOOL ShellExecuteExA(SHELLEXECUTEINFOA *info)
{
    HINSTANCE r = ShellExecuteA(info->hwnd, info->lpVerb, info->lpFile, info->lpParameters, info->lpDirectory, info->nShow);
    info->hInstApp = r;
    info->hProcess = nullptr;
    return (intptr_t)r > 32;
}

} // extern "C"
