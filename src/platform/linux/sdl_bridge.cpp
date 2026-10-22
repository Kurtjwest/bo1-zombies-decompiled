// SDL2 implementation of the window/input/system bridge (../common/platform_bridge.h) for the Linux build.
//
// SDL's video functions belong to the thread that initialized it, the main thread here (as AppKit's do on macOS),
// but the engine creates, shows and sizes its window from the render thread: those calls run on the main thread while
// the caller waits (main_thread.h; the main thread runs them from its Win32 waits and from the event pump). The event
// queue is drained on the main thread by Bridge_PumpEvents.
//
// Keys reach the Win32 layer as macOS virtual key codes (mac_keycodes.h), the codes its key map is written for: SDL's
// scan codes are positional (the key's place on a US keyboard), as macOS's virtual key codes and Windows' scan codes
// are, so each one names the same physical key on all three.
//
// Win32 screen units are SDL's screen coordinates (pixels on X11; on a scaled Wayland desktop the drawable of a window
// can be larger, see Bridge_GetBackingScale).

#include "../common/platform_bridge.h"
#include "../win32compat/mac_keycodes.h"
#include "../win32compat/main_thread.h"
#include "sdl_window.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

// SDL video calls from other threads run on the main thread while the caller waits.
template <typename F> static auto OnMain(F &&f) -> decltype(f())
{
    if (w32::IsMainThread())
        return f();
    if constexpr (std::is_void_v<decltype(f())>)
    {
        w32::RunOnMainThread([&] { f(); });
    }
    else
    {
        decltype(f()) r{};
        w32::RunOnMainThread([&] { r = f(); });
        return r;
    }
}

struct BridgeWindow
{
    SDL_Window *window = nullptr;
    Uint32 id = 0;
    void *userData = nullptr;
    SdlSurface surface;
    bool wantBorderless = false, wantResizable = false;
    bool wantFullScreen = false; // what the engine asked for
    bool inFullScreen = false;   // what the window is
    // the client rect the engine asked for while the window was in full screen, applied once it leaves
    bool haveWindowedRect = false;
    int windowedX = 0, windowedY = 0, windowedW = 0, windowedH = 0;
    // state other threads read every frame (the render thread asks whether the window is visible, minimized and
    // focused): kept current by the main thread, so those reads need not wait for it
    std::atomic<int> visible{0};
    std::atomic<int> minimized{0};
    std::atomic<int> keyWindow{0};
    std::atomic<int> fullScreen{0};
};

namespace
{
BridgeEventHandler g_handler;
bool g_videoReady;
Uint32 g_wakeEvent = (Uint32)-1;
std::vector<BridgeWindow *> g_windows; // main thread only
std::atomic<int> g_appActive{0};
bool g_keyDown[256];
bool g_mouseCaptured;
bool g_cursorHidden;
double g_wheelAccum; // smooth scrolling: the part of a notch scrolled so far

void Emit(BridgeEvent &ev)
{
    if (!ev.time)
        ev.time = SDL_GetTicks64() / 1000.0;
    if (g_handler)
        g_handler(&ev);
}

BridgeWindow *FindWindow(Uint32 id)
{
    for (BridgeWindow *w : g_windows)
        if (w->id == id)
            return w;
    return nullptr;
}

BridgeWindow *FocusedWindow()
{
    for (BridgeWindow *w : g_windows)
        if (w->window && (SDL_GetWindowFlags(w->window) & SDL_WINDOW_INPUT_FOCUS))
            return w;
    return g_windows.empty() ? nullptr : g_windows.front();
}

void RefreshWindowState(BridgeWindow *w)
{
    if (!w || !w->window)
        return;
    const Uint32 f = SDL_GetWindowFlags(w->window);
    w->visible.store((f & SDL_WINDOW_SHOWN) ? 1 : 0, std::memory_order_relaxed);
    w->minimized.store((f & SDL_WINDOW_MINIMIZED) ? 1 : 0, std::memory_order_relaxed);
    w->keyWindow.store((f & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0, std::memory_order_relaxed);
}

// Text from the engine is UTF-8 when it comes from the file system and Windows-1252 / Latin-1 when it is the game's own
// (localized strings, console text, error messages); SDL wants UTF-8.
std::string EngineString(const char *s)
{
    if (!s)
        return "";
    const unsigned char *p = (const unsigned char *)s;
    bool utf8 = true;
    for (size_t i = 0; p[i] && utf8;)
    {
        const unsigned char c = p[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0)
            utf8 = false;
        for (int k = 1; k <= n && utf8; ++k)
            if ((p[i + k] & 0xC0) != 0x80)
                utf8 = false;
        i += (size_t)n + 1;
    }
    if (utf8)
        return s;
    std::string out;
    for (; *p; ++p)
    {
        if (*p < 0x80)
            out += (char)*p;
        else
        {
            out += (char)(0xC0 | (*p >> 6));
            out += (char)(0x80 | (*p & 0x3F));
        }
    }
    return out;
}

uint32_t ModsFromSdl(SDL_Keymod m)
{
    uint32_t r = 0;
    if (m & KMOD_SHIFT)
        r |= BRIDGE_MOD_SHIFT;
    if (m & KMOD_CTRL)
        r |= BRIDGE_MOD_CONTROL;
    if (m & KMOD_ALT)
        r |= BRIDGE_MOD_ALT;
    if (m & KMOD_GUI)
        r |= BRIDGE_MOD_COMMAND;
    if (m & KMOD_CAPS)
        r |= BRIDGE_MOD_CAPSLOCK;
    return r;
}

// SDL scan code -> macOS virtual key code (0xFFFF: no key of the Win32 layer's map)
uint16_t MacKeyForScancode(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
    {
        static const uint16_t letters[26] = {kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G,
                                             kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
                                             kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U,
                                             kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z};
        return letters[sc - SDL_SCANCODE_A];
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0)
    {
        static const uint16_t digits[10] = {kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5,
                                            kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9, kVK_ANSI_0};
        return digits[sc - SDL_SCANCODE_1];
    }
    switch (sc)
    {
    case SDL_SCANCODE_RETURN: return kVK_Return;
    case SDL_SCANCODE_ESCAPE: return kVK_Escape;
    case SDL_SCANCODE_BACKSPACE: return kVK_Delete;
    case SDL_SCANCODE_TAB: return kVK_Tab;
    case SDL_SCANCODE_SPACE: return kVK_Space;
    case SDL_SCANCODE_MINUS: return kVK_ANSI_Minus;
    case SDL_SCANCODE_EQUALS: return kVK_ANSI_Equal;
    case SDL_SCANCODE_LEFTBRACKET: return kVK_ANSI_LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return kVK_ANSI_RightBracket;
    case SDL_SCANCODE_BACKSLASH: return kVK_ANSI_Backslash;
    case SDL_SCANCODE_NONUSHASH: return kVK_ANSI_Backslash;
    case SDL_SCANCODE_SEMICOLON: return kVK_ANSI_Semicolon;
    case SDL_SCANCODE_APOSTROPHE: return kVK_ANSI_Quote;
    case SDL_SCANCODE_GRAVE: return kVK_ANSI_Grave;
    case SDL_SCANCODE_COMMA: return kVK_ANSI_Comma;
    case SDL_SCANCODE_PERIOD: return kVK_ANSI_Period;
    case SDL_SCANCODE_SLASH: return kVK_ANSI_Slash;
    case SDL_SCANCODE_CAPSLOCK: return kVK_CapsLock;
    case SDL_SCANCODE_F1: return kVK_F1;
    case SDL_SCANCODE_F2: return kVK_F2;
    case SDL_SCANCODE_F3: return kVK_F3;
    case SDL_SCANCODE_F4: return kVK_F4;
    case SDL_SCANCODE_F5: return kVK_F5;
    case SDL_SCANCODE_F6: return kVK_F6;
    case SDL_SCANCODE_F7: return kVK_F7;
    case SDL_SCANCODE_F8: return kVK_F8;
    case SDL_SCANCODE_F9: return kVK_F9;
    case SDL_SCANCODE_F10: return kVK_F10;
    case SDL_SCANCODE_F11: return kVK_F11;
    case SDL_SCANCODE_F12: return kVK_F12;
    // (Print Screen, Scroll Lock and Pause are F13-F15 on Apple's keyboards)
    case SDL_SCANCODE_PRINTSCREEN: return kVK_F13;
    case SDL_SCANCODE_SCROLLLOCK: return kVK_F14;
    case SDL_SCANCODE_PAUSE: return kVK_F15;
    case SDL_SCANCODE_F13: return kVK_F13;
    case SDL_SCANCODE_F14: return kVK_F14;
    case SDL_SCANCODE_F15: return kVK_F15;
    case SDL_SCANCODE_F16: return kVK_F16;
    case SDL_SCANCODE_F17: return kVK_F17;
    case SDL_SCANCODE_F18: return kVK_F18;
    case SDL_SCANCODE_F19: return kVK_F19;
    case SDL_SCANCODE_INSERT: return kVK_Help; // (the Insert key's place on Apple's keyboards; VK_INSERT)
    case SDL_SCANCODE_HOME: return kVK_Home;
    case SDL_SCANCODE_PAGEUP: return kVK_PageUp;
    case SDL_SCANCODE_DELETE: return kVK_ForwardDelete;
    case SDL_SCANCODE_END: return kVK_End;
    case SDL_SCANCODE_PAGEDOWN: return kVK_PageDown;
    case SDL_SCANCODE_RIGHT: return kVK_RightArrow;
    case SDL_SCANCODE_LEFT: return kVK_LeftArrow;
    case SDL_SCANCODE_DOWN: return kVK_DownArrow;
    case SDL_SCANCODE_UP: return kVK_UpArrow;
    case SDL_SCANCODE_NUMLOCKCLEAR: return kVK_ANSI_KeypadClear; // (VK_NUMLOCK)
    case SDL_SCANCODE_KP_DIVIDE: return kVK_ANSI_KeypadDivide;
    case SDL_SCANCODE_KP_MULTIPLY: return kVK_ANSI_KeypadMultiply;
    case SDL_SCANCODE_KP_MINUS: return kVK_ANSI_KeypadMinus;
    case SDL_SCANCODE_KP_PLUS: return kVK_ANSI_KeypadPlus;
    case SDL_SCANCODE_KP_ENTER: return kVK_ANSI_KeypadEnter;
    case SDL_SCANCODE_KP_1: return kVK_ANSI_Keypad1;
    case SDL_SCANCODE_KP_2: return kVK_ANSI_Keypad2;
    case SDL_SCANCODE_KP_3: return kVK_ANSI_Keypad3;
    case SDL_SCANCODE_KP_4: return kVK_ANSI_Keypad4;
    case SDL_SCANCODE_KP_5: return kVK_ANSI_Keypad5;
    case SDL_SCANCODE_KP_6: return kVK_ANSI_Keypad6;
    case SDL_SCANCODE_KP_7: return kVK_ANSI_Keypad7;
    case SDL_SCANCODE_KP_8: return kVK_ANSI_Keypad8;
    case SDL_SCANCODE_KP_9: return kVK_ANSI_Keypad9;
    case SDL_SCANCODE_KP_0: return kVK_ANSI_Keypad0;
    case SDL_SCANCODE_KP_PERIOD: return kVK_ANSI_KeypadDecimal;
    case SDL_SCANCODE_KP_EQUALS: return kVK_ANSI_KeypadEquals;
    case SDL_SCANCODE_LCTRL: return kVK_Control;
    case SDL_SCANCODE_LSHIFT: return kVK_Shift;
    case SDL_SCANCODE_LALT: return kVK_Option;
    case SDL_SCANCODE_LGUI: return kVK_Command;
    case SDL_SCANCODE_RCTRL: return kVK_RightControl;
    case SDL_SCANCODE_RSHIFT: return kVK_RightShift;
    case SDL_SCANCODE_RALT: return kVK_RightOption;
    case SDL_SCANCODE_RGUI: return kVK_RightCommand;
    case SDL_SCANCODE_MUTE: return kVK_Mute;
    case SDL_SCANCODE_VOLUMEUP: return kVK_VolumeUp;
    case SDL_SCANCODE_VOLUMEDOWN: return kVK_VolumeDown;
    default: return 0xFFFF;
    }
}

void EmitKey(BridgeWindow *w, uint16_t code, bool down, bool repeat, SDL_Keymod mods)
{
    BridgeEvent ev = {};
    ev.type = down ? BRIDGE_EV_KEY_DOWN : BRIDGE_EV_KEY_UP;
    ev.window = w;
    ev.userData = w ? w->userData : nullptr;
    ev.macKeyCode = code;
    ev.isRepeat = down && repeat;
    ev.modifiers = ModsFromSdl(mods);
    if (code < 256)
        g_keyDown[code] = down;
    Emit(ev);
}

void EmitChar(BridgeWindow *w, uint32_t cp, uint16_t code, bool repeat)
{
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_CHAR;
    ev.window = w;
    ev.userData = w ? w->userData : nullptr;
    ev.ch = cp;
    ev.macKeyCode = code;
    ev.isRepeat = repeat;
    Emit(ev);
}

// keys held when the game loses focus never get their key-up here: released (Windows does the same)
void ReleaseHeldKeys()
{
    for (int code = 0; code < 256; ++code)
    {
        if (!g_keyDown[code])
            continue;
        g_keyDown[code] = false;
        BridgeEvent up = {};
        up.type = BRIDGE_EV_KEY_UP;
        up.macKeyCode = (uint16_t)code;
        Emit(up);
    }
}

void SetActive(bool active)
{
    if ((g_appActive.load(std::memory_order_relaxed) != 0) == active)
        return;
    g_appActive.store(active ? 1 : 0, std::memory_order_relaxed);
    if (!active)
        ReleaseHeldKeys();
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_ACTIVATE;
    ev.value = active ? 1 : 0;
    Emit(ev);
}

void EmitFullScreen(BridgeWindow *w)
{
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_FULLSCREEN;
    ev.window = w;
    ev.userData = w->userData;
    ev.value = w->inFullScreen ? 1 : 0;
    Emit(ev);
}

void HandleWindowEvent(const SDL_WindowEvent &we)
{
    BridgeWindow *w = FindWindow(we.windowID);
    if (!w)
    {
        // One of the process's own other windows (the renderer's hidden ones for its shader compiler contexts): on the
        // console (KMSDRM) SDL gives every new window the keyboard, which is no reason to think the game inactive.
        if (we.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            SetActive(true);
        return;
    }
    RefreshWindowState(w);
    BridgeEvent ev = {};
    ev.window = w;
    ev.userData = w->userData;
    switch (we.event)
    {
    case SDL_WINDOWEVENT_CLOSE:
        ev.type = BRIDGE_EV_CLOSE;
        Emit(ev);
        break;
    case SDL_WINDOWEVENT_SIZE_CHANGED:
    {
        int cw = 0, ch = 0;
        SDL_GetWindowSize(w->window, &cw, &ch);
        ev.type = BRIDGE_EV_RESIZE;
        ev.x = cw;
        ev.y = ch;
        Emit(ev);
        break;
    }
    case SDL_WINDOWEVENT_MOVED:
        if (w->inFullScreen)
            break;
        ev.type = BRIDGE_EV_MOVE;
        ev.x = we.data1;
        ev.y = we.data2;
        Emit(ev);
        break;
    case SDL_WINDOWEVENT_MINIMIZED:
        ev.type = BRIDGE_EV_MINIMIZE;
        ev.value = 1;
        Emit(ev);
        break;
    case SDL_WINDOWEVENT_RESTORED:
        ev.type = BRIDGE_EV_MINIMIZE;
        ev.value = 0;
        Emit(ev);
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED: SetActive(true); break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        // (inactive unless the keyboard went to another window of the process, see above)
        if (!SDL_GetKeyboardFocus())
            SetActive(false);
        break;
    default: break;
    }
}

void HandleEvent(const SDL_Event &e)
{
    switch (e.type)
    {
    case SDL_QUIT:
    {
        BridgeEvent ev = {};
        ev.type = BRIDGE_EV_QUIT;
        Emit(ev);
        break;
    }
    case SDL_WINDOWEVENT: HandleWindowEvent(e.window); break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
        BridgeWindow *w = FindWindow(e.key.windowID);
        const uint16_t code = MacKeyForScancode(e.key.keysym.scancode);
        if (code == 0xFFFF)
            break;
        const bool down = e.type == SDL_KEYDOWN;
        const SDL_Keymod mods = (SDL_Keymod)e.key.keysym.mod;
        EmitKey(w, code, down, e.key.repeat != 0, mods);
        if (!down)
            break;
        // The characters Windows sends with WM_CHAR for keys that type no text: SDL's text input leaves out control
        // characters, so they are made here (the console's backspace, enter, tab, escape, and Ctrl+C/V/X/A).
        const SDL_Keycode sym = e.key.keysym.sym;
        uint32_t cp = 0;
        if (sym == SDLK_BACKSPACE)
            cp = 0x08;
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)
            cp = 0x0D;
        else if (sym == SDLK_TAB)
            cp = 0x09;
        else if (sym == SDLK_ESCAPE)
            cp = 0x1B;
        else if ((mods & KMOD_CTRL) && !(mods & KMOD_ALT) && sym >= SDLK_a && sym <= SDLK_z)
            cp = (uint32_t)(sym - SDLK_a + 1);
        if (cp)
            EmitChar(w, cp, code, e.key.repeat != 0);
        break;
    }
    case SDL_TEXTINPUT:
    {
        BridgeWindow *w = FindWindow(e.text.windowID);
        const unsigned char *p = (const unsigned char *)e.text.text;
        while (*p)
        {
            uint32_t cp = *p++;
            int extra = 0;
            if (cp >= 0xF0)
            {
                cp &= 0x07;
                extra = 3;
            }
            else if (cp >= 0xE0)
            {
                cp &= 0x0F;
                extra = 2;
            }
            else if (cp >= 0xC0)
            {
                cp &= 0x1F;
                extra = 1;
            }
            for (; extra && (*p & 0xC0) == 0x80; --extra)
                cp = (cp << 6) | (*p++ & 0x3F);
            if (cp >= 0x20 && cp != 0x7F)
                EmitChar(w, cp, 0, false);
        }
        break;
    }
    case SDL_MOUSEMOTION:
    {
        BridgeWindow *w = FindWindow(e.motion.windowID);
        BridgeEvent ev = {};
        ev.type = BRIDGE_EV_MOUSE_MOVE;
        ev.window = w;
        ev.userData = w ? w->userData : nullptr;
        ev.x = e.motion.x;
        ev.y = e.motion.y;
        ev.dx = e.motion.xrel;
        ev.dy = e.motion.yrel;
        ev.modifiers = ModsFromSdl(SDL_GetModState());
        Emit(ev);
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    {
        if (e.button.which == SDL_TOUCH_MOUSEID)
            break;
        BridgeWindow *w = FindWindow(e.button.windowID);
        int b;
        switch (e.button.button)
        {
        case SDL_BUTTON_LEFT: b = 0; break;
        case SDL_BUTTON_RIGHT: b = 1; break;
        case SDL_BUTTON_MIDDLE: b = 2; break;
        case SDL_BUTTON_X1: b = 3; break;
        case SDL_BUTTON_X2: b = 4; break;
        default: return;
        }
        BridgeEvent ev = {};
        ev.type = e.type == SDL_MOUSEBUTTONDOWN ? BRIDGE_EV_MOUSE_DOWN : BRIDGE_EV_MOUSE_UP;
        ev.window = w;
        ev.userData = w ? w->userData : nullptr;
        ev.x = e.button.x;
        ev.y = e.button.y;
        ev.button = b;
        ev.modifiers = ModsFromSdl(SDL_GetModState());
        Emit(ev);
        break;
    }
    case SDL_MOUSEWHEEL:
    {
        BridgeWindow *w = FindWindow(e.wheel.windowID);
        double d = e.wheel.preciseY;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            d = -d;
        // the game takes one wheel notch as one step (the next menu item, the next weapon): smooth scrolling (touch
        // pads, high-resolution wheels) is turned into whole notches
        if ((d > 0) != (g_wheelAccum > 0))
            g_wheelAccum = 0;
        g_wheelAccum += d;
        while (fabs(g_wheelAccum) >= 1.0)
        {
            const int dir = g_wheelAccum > 0 ? 1 : -1;
            g_wheelAccum -= dir;
            BridgeEvent ev = {};
            ev.type = BRIDGE_EV_WHEEL;
            ev.window = w;
            ev.userData = w ? w->userData : nullptr;
            ev.dy = dir;
            Emit(ev);
        }
        break;
    }
    default: break;
    }
}

void WakeMain()
{
    if (!g_videoReady || g_wakeEvent == (Uint32)-1)
        return;
    SDL_Event e;
    SDL_zero(e);
    e.type = g_wakeEvent;
    SDL_PushEvent(&e); // (thread-safe)
}

void ApplyFullScreen(BridgeWindow *w, bool on)
{
    if (!w->window || on == w->inFullScreen)
        return;
    if (on && !(SDL_GetWindowFlags(w->window) & SDL_WINDOW_SHOWN))
        return; // (applied when the window is shown, see Main_ShowWindow)
    if (on)
    {
        // the windowed rect comes back when the window leaves full screen
        if (!w->haveWindowedRect)
        {
            SDL_GetWindowPosition(w->window, &w->windowedX, &w->windowedY);
            SDL_GetWindowSize(w->window, &w->windowedW, &w->windowedH);
            w->haveWindowedRect = true;
        }
        // full screen at the desktop's resolution: the game draws at its own size and the renderer scales the
        // picture to the screen (the display's mode never changes, as in macOS's full screen)
        if (SDL_SetWindowFullscreen(w->window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
        {
            fprintf(stderr, "[sdl] full screen: %s\n", SDL_GetError());
            return;
        }
        w->inFullScreen = true;
    }
    else
    {
        SDL_SetWindowFullscreen(w->window, 0);
        w->inFullScreen = false;
        SDL_SetWindowBordered(w->window, w->wantBorderless ? SDL_FALSE : SDL_TRUE);
        SDL_SetWindowResizable(w->window, w->wantResizable ? SDL_TRUE : SDL_FALSE);
        if (w->haveWindowedRect)
        {
            SDL_SetWindowSize(w->window, std::max(w->windowedW, 1), std::max(w->windowedH, 1));
            SDL_SetWindowPosition(w->window, w->windowedX, w->windowedY);
            w->haveWindowedRect = false;
        }
    }
    w->fullScreen.store(w->inFullScreen ? 1 : 0, std::memory_order_relaxed);
    EmitFullScreen(w);
}

// ---- the bridge entry points that use SDL's video functions: on the main thread, whichever thread calls them
BridgeWindow *Main_CreateWindow(const char *title, int x, int y, int clientW, int clientH, int borderless, int resizable,
                                void *userData)
{
    if (!g_videoReady)
        return nullptr;
    if (clientW <= 0)
        clientW = 1280;
    if (clientH <= 0)
        clientH = 720;
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI;
    if (borderless)
        flags |= SDL_WINDOW_BORDERLESS;
    else if (resizable)
        flags |= SDL_WINDOW_RESIZABLE;
    const bool centred = x == (int)0x80000000 || y == (int)0x80000000;
    const std::string t = EngineString(title ? title : "BO1 Zombies");
    SDL_Window *win = SDL_CreateWindow(t.c_str(), centred ? SDL_WINDOWPOS_CENTERED : x, centred ? SDL_WINDOWPOS_CENTERED : y,
                                       clientW, clientH, flags);
    if (!win)
    {
        fprintf(stderr, "[sdl] cannot create the window: %s\n", SDL_GetError());
        return nullptr;
    }
    BridgeWindow *bw = new BridgeWindow();
    bw->window = win;
    bw->id = SDL_GetWindowID(win);
    bw->userData = userData;
    bw->surface.window = win;
    bw->wantBorderless = borderless != 0;
    bw->wantResizable = resizable != 0;
    g_windows.push_back(bw);
    RefreshWindowState(bw);
    return bw;
}

void Main_DestroyWindow(BridgeWindow *w)
{
    if (!w)
        return;
    g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), w), g_windows.end());
    if (w->window)
        SDL_DestroyWindow(w->window);
    // The struct itself stays allocated as a dead window (no SDL window, state cleared) rather than being freed:
    // other threads read the cached state through the pointer without waiting for the main thread (see the Cocoa
    // bridge). A window is only destroyed on vid_restart and quit.
    w->window = nullptr;
    w->surface.window = nullptr;
    w->visible.store(0, std::memory_order_relaxed);
    w->minimized.store(0, std::memory_order_relaxed);
    w->keyWindow.store(0, std::memory_order_relaxed);
}

void Main_ShowWindow(BridgeWindow *w, int show)
{
    if (!w || !w->window)
        return;
    if (show)
    {
        SDL_ShowWindow(w->window);
        SDL_RaiseWindow(w->window);
        // full screen asked for before the window was on screen (the engine creates its window hidden)
        if (w->wantFullScreen && !w->inFullScreen)
            ApplyFullScreen(w, true);
    }
    else
        SDL_HideWindow(w->window);
    RefreshWindowState(w);
}

void Main_SetWindowStyle(BridgeWindow *w, int borderless, int resizable)
{
    if (!w || !w->window)
        return;
    w->wantBorderless = borderless != 0;
    w->wantResizable = resizable != 0;
    if (w->inFullScreen)
        return; // (the style the window gets back when it leaves full screen)
    SDL_SetWindowBordered(w->window, borderless ? SDL_FALSE : SDL_TRUE);
    SDL_SetWindowResizable(w->window, (resizable && !borderless) ? SDL_TRUE : SDL_FALSE);
}

void Main_SetClientRect(BridgeWindow *w, int x, int y, int clientW, int clientH)
{
    if (!w || !w->window)
        return;
    if (w->inFullScreen)
    {
        // the full-screen window's rect is the screen's: the one asked for is the window's once it leaves
        w->windowedX = x;
        w->windowedY = y;
        w->windowedW = clientW;
        w->windowedH = clientH;
        w->haveWindowedRect = true;
        return;
    }
    SDL_SetWindowSize(w->window, std::max(clientW, 1), std::max(clientH, 1));
    if (x != (int)0x80000000 && y != (int)0x80000000)
        SDL_SetWindowPosition(w->window, x, y);
}

void Main_GetClientRect(BridgeWindow *w, BridgeRect *r)
{
    memset(r, 0, sizeof(*r));
    if (!w || !w->window)
        return;
    int x = 0, y = 0, cw = 0, ch = 0;
    SDL_GetWindowPosition(w->window, &x, &y);
    SDL_GetWindowSize(w->window, &cw, &ch);
    r->left = x;
    r->top = y;
    r->right = x + cw;
    r->bottom = y + ch;
}

void Main_GetFrameRect(BridgeWindow *w, BridgeRect *r)
{
    Main_GetClientRect(w, r);
    int top = 0, left = 0, bottom = 0, right = 0;
    if (w && w->window && SDL_GetWindowBordersSize(w->window, &top, &left, &bottom, &right) == 0)
    {
        r->left -= left;
        r->top -= top;
        r->right += right;
        r->bottom += bottom;
    }
}

int Main_GetScreenRect(int index, BridgeRect *full, BridgeRect *work, double *refreshHz)
{
    if (!g_videoReady || index < 0 || index >= SDL_GetNumVideoDisplays())
        return 0;
    SDL_Rect b;
    if (SDL_GetDisplayBounds(index, &b) != 0)
        return 0;
    if (full)
        *full = {b.x, b.y, b.x + b.w, b.y + b.h};
    if (work)
    {
        SDL_Rect u;
        if (SDL_GetDisplayUsableBounds(index, &u) != 0)
            u = b;
        *work = {u.x, u.y, u.x + u.w, u.y + u.h};
    }
    if (refreshHz)
    {
        SDL_DisplayMode m;
        *refreshHz = (SDL_GetDesktopDisplayMode(index, &m) == 0 && m.refresh_rate > 0) ? (double)m.refresh_rate : 60.0;
    }
    return 1;
}

int Main_GetScreenModes(int index, int *widths, int *heights, int *rates, int maxModes)
{
    if (!g_videoReady || index < 0 || index >= SDL_GetNumVideoDisplays() || maxModes <= 0)
        return 0;
    // The game draws at its own resolution whatever the display runs at (full screen never switches the display's
    // mode; the renderer scales the picture to the screen), so these are the resolutions it can draw at: the
    // display's modes and the usual sizes, up to the desktop's size.
    SDL_DisplayMode desk;
    if (SDL_GetDesktopDisplayMode(index, &desk) != 0)
        return 0;
    const int maxW = desk.w, maxH = desk.h;
    const int hz = desk.refresh_rate > 0 ? desk.refresh_rate : 60;
    std::vector<std::pair<int, int>> sizes;
    const int n = SDL_GetNumDisplayModes(index);
    for (int i = 0; i < n; ++i)
    {
        SDL_DisplayMode m;
        if (SDL_GetDisplayMode(index, i, &m) == 0)
            sizes.push_back({m.w, m.h});
    }
    static const int common[][2] = {{640, 480},   {720, 480},   {800, 600},   {1024, 640},  {1024, 768},  {1152, 864},
                                    {1280, 720},  {1280, 800},  {1280, 960},  {1280, 1024}, {1366, 768},  {1440, 900},
                                    {1600, 900},  {1600, 1000}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                                    {2048, 1152}, {2048, 1280}, {2304, 1440}, {2560, 1080}, {2560, 1440}, {2560, 1600},
                                    {2880, 1800}, {3200, 1800}, {3440, 1440}, {3840, 1600}, {3840, 2160}, {3840, 2400},
                                    {5120, 1440}, {5120, 2160}, {5120, 2880}};
    for (auto &c : common)
        sizes.push_back({c[0], c[1]});
    sizes.push_back({maxW, maxH});
    sizes.erase(std::remove_if(sizes.begin(), sizes.end(),
                               [&](const std::pair<int, int> &z) {
                                   return z.first < 640 || z.second < 480 || z.first > maxW || z.second > maxH;
                               }),
                sizes.end());
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    const size_t first = sizes.size() > (size_t)maxModes ? sizes.size() - (size_t)maxModes : 0;
    int count = 0;
    for (size_t i = first; i < sizes.size(); ++i, ++count)
    {
        widths[count] = sizes[i].first;
        heights[count] = sizes[i].second;
        rates[count] = hz;
    }
    return count;
}

void Main_SetMouseCaptured(int captured)
{
    if ((bool)captured == g_mouseCaptured)
        return;
    // a capture queued by the render thread can arrive after the game lost focus (which released it): ignored, or
    // the pointer would stay grabbed over the other program
    if (captured && !g_appActive.load(std::memory_order_relaxed))
        return;
    g_mouseCaptured = captured != 0;
    // relative mode: the pointer hidden and held in the window, the motion events carrying the raw deltas
    SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE);
    if (!captured)
        SDL_ShowCursor(g_cursorHidden ? SDL_DISABLE : SDL_ENABLE);
}

std::string ExecutableDir()
{
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return ".";
    buf[n] = 0;
    std::string p = buf;
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash ? slash : 1);
}

bool MakeDirs(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); ++i)
    {
        if (i < path.size() && path[i] != '/')
            continue;
        const std::string part = path.substr(0, i);
        struct stat st;
        if ((stat)(part.c_str(), &st) == 0)
            continue;
        if (mkdir(part.c_str(), 0755) != 0)
            return false;
    }
    return true;
}
} // namespace

extern "C" {

void Bridge_Init(BridgeEventHandler handler)
{
    g_handler = handler;
    // the engine handles closing its window (WM_CLOSE) and quits on its own
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    // a game: the screen saver stays off while it runs only when the engine asks (Bridge_PreventDisplaySleep)
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    // full screen at the desktop's resolution stays on screen when the game loses focus (Alt+Tab to another program
    // minimizes it otherwise)
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    // mouse look: relative motion from the raw device, without the desktop's pointer acceleration
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
    SDL_SetHint(SDL_HINT_APP_NAME, "BO1 Zombies");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        fprintf(stderr, "[sdl] no display: %s\n", SDL_GetError());
        return;
    }
    g_videoReady = true;
    g_wakeEvent = SDL_RegisterEvents(1);
    {
        SDL_version v;
        SDL_GetVersion(&v);
        const char *video = SDL_GetCurrentVideoDriver();
        fprintf(stderr, "[sdl] SDL %d.%d.%d, %s video\n", v.major, v.minor, v.patch, video ? video : "?");
    }
    // controllers (XInput, sdl_gamepad.cpp) and sound (sdl_audio.cpp): on this thread, whose event loop pumps them;
    // the game runs without either when the system has none. The controllers' input counts whichever window SDL thinks
    // has the keyboard (on the console, KMSDRM, it can think none has: SDL then drops every button press); the engine
    // reads the controllers only while its window is in front anyway (IN_Frame).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "[sdl] no game controller support: %s\n", SDL_GetError());
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        fprintf(stderr, "[sdl] no audio: %s\n", SDL_GetError());
    // the framebuffer of the game window: 8-bit color, no depth (the renderer draws into its own textures and copies
    // the back buffer to the window at Present); the context itself is the renderer's (gl_rhi.cpp)
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    w32::AddMainThreadWaker(WakeMain);
    g_appActive.store(1, std::memory_order_relaxed); // (until a window tells otherwise)
}

void Bridge_PumpEvents(int waitMs)
{
    w32::ServiceMainThreadCalls();
    if (!g_videoReady)
        return;
    SDL_Event e;
    bool got = waitMs > 0 ? SDL_WaitEventTimeout(&e, waitMs) != 0 : SDL_PollEvent(&e) != 0;
    while (got)
    {
        if (e.type != g_wakeEvent)
            HandleEvent(e);
        got = SDL_PollEvent(&e) != 0;
    }
    for (BridgeWindow *w : g_windows)
        RefreshWindowState(w);
    w32::ServiceMainThreadCalls();
}

int Bridge_IsAppActive(void) { return g_appActive.load(std::memory_order_relaxed); }
void Bridge_ActivateApp(void)
{
    OnMain([&] {
        if (BridgeWindow *w = FocusedWindow())
            if (w->window)
                SDL_RaiseWindow(w->window);
    });
}
double Bridge_UnitScale(void) { return 1.0; }

BridgeWindow *Bridge_CreateWindow(const char *title, int x, int y, int clientW, int clientH, int borderless, int resizable, void *userData)
{
    return OnMain([&] { return Main_CreateWindow(title, x, y, clientW, clientH, borderless, resizable, userData); });
}
void Bridge_DestroyWindow(BridgeWindow *w) { OnMain([&] { Main_DestroyWindow(w); }); }
void Bridge_ShowWindow(BridgeWindow *w, int show) { OnMain([&] { Main_ShowWindow(w, show); }); }
void Bridge_MinimizeWindow(BridgeWindow *w)
{
    OnMain([&] {
        if (w && w->window)
        {
            SDL_MinimizeWindow(w->window);
            RefreshWindowState(w);
        }
    });
}
void Bridge_RestoreWindow(BridgeWindow *w)
{
    OnMain([&] {
        if (w && w->window && (SDL_GetWindowFlags(w->window) & SDL_WINDOW_MINIMIZED))
            SDL_RestoreWindow(w->window);
        RefreshWindowState(w);
    });
}
// other threads read the state the main thread keeps current (RefreshWindowState) instead of waiting for it
int Bridge_IsMinimized(BridgeWindow *w)
{
    if (w && w32::IsMainThread())
        RefreshWindowState(w);
    return w ? w->minimized.load(std::memory_order_relaxed) : 0;
}
int Bridge_IsVisible(BridgeWindow *w)
{
    if (w && w32::IsMainThread())
        RefreshWindowState(w);
    return w ? w->visible.load(std::memory_order_relaxed) : 0;
}
void Bridge_SetWindowStyle(BridgeWindow *w, int borderless, int resizable) { OnMain([&] { Main_SetWindowStyle(w, borderless, resizable); }); }
void Bridge_SetClientRect(BridgeWindow *w, int x, int y, int clientW, int clientH)
{
    OnMain([&] { Main_SetClientRect(w, x, y, clientW, clientH); });
}
void Bridge_GetClientRect(BridgeWindow *w, BridgeRect *r) { OnMain([&] { Main_GetClientRect(w, r); }); }
void Bridge_GetFrameRect(BridgeWindow *w, BridgeRect *r) { OnMain([&] { Main_GetFrameRect(w, r); }); }
void Bridge_SetTitle(BridgeWindow *w, const char *title)
{
    if (!title)
        return;
    const std::string t = EngineString(title);
    OnMain([&] {
        if (w && w->window)
            SDL_SetWindowTitle(w->window, t.c_str());
    });
}
void Bridge_FocusWindow(BridgeWindow *w)
{
    OnMain([&] {
        if (w && w->window)
        {
            SDL_RaiseWindow(w->window);
            RefreshWindowState(w);
        }
    });
}
int Bridge_IsKeyWindow(BridgeWindow *w)
{
    if (w && w32::IsMainThread())
        RefreshWindowState(w);
    return w && w->keyWindow.load(std::memory_order_relaxed) && g_appActive.load(std::memory_order_relaxed) ? 1 : 0;
}
// (a borderless window over its screen needs nothing more: the window manager puts it over the panels)
void Bridge_SetCoversScreen(BridgeWindow *, int) {}
void Bridge_SetFullScreen(BridgeWindow *w, int fullScreen)
{
    OnMain([&] {
        if (!w)
            return;
        w->wantFullScreen = fullScreen != 0;
        ApplyFullScreen(w, fullScreen != 0);
    });
}
int Bridge_IsFullScreen(BridgeWindow *w)
{
    if (!w)
        return 0;
    // (in full screen, or asked to be: the engine's window goes full screen when it is first shown)
    return w->fullScreen.load(std::memory_order_relaxed);
}
void Bridge_SetContentScaling(BridgeWindow *w, int logicalW, int logicalH, int fill)
{
    if (!w)
        return;
    w->surface.logicalW.store(logicalW, std::memory_order_relaxed);
    w->surface.logicalH.store(logicalH, std::memory_order_relaxed);
    w->surface.fill.store(fill ? 1 : 0, std::memory_order_relaxed);
}
void *Bridge_GetMetalLayer(BridgeWindow *w) { return w && w->window ? (void *)&w->surface : nullptr; }
double Bridge_GetBackingScale(BridgeWindow *w)
{
    return OnMain([&] {
        if (!w || !w->window)
            return 1.0;
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(w->window, &ww, &wh);
        SDL_GL_GetDrawableSize(w->window, &dw, &dh);
        return ww > 0 && dw > 0 ? (double)dw / ww : 1.0;
    });
}

// cursor changes from other threads (the render thread shows and hides the cursor every frame) are queued for the
// main thread without waiting; later calls from the same thread run after them
void Bridge_SetCursorVisible(int visible)
{
    w32::PostToMainThread([visible] {
        g_cursorHidden = !visible;
        if (g_videoReady && !g_mouseCaptured)
            SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
    });
}
void Bridge_SetMouseCaptured(int captured)
{
    w32::PostToMainThread([captured] {
        if (g_videoReady)
            Main_SetMouseCaptured(captured);
    });
}
void Bridge_WarpCursor(int x, int y)
{
    w32::PostToMainThread([x, y] {
        if (g_videoReady)
            SDL_WarpMouseGlobal(x, y);
    });
}
void Bridge_GetCursorPos(int *x, int *y)
{
    int gx = 0, gy = 0;
    if (g_videoReady)
        SDL_GetGlobalMouseState(&gx, &gy); // (thread-safe on X11 and Wayland)
    *x = gx;
    *y = gy;
}
void Bridge_SetArrowCursor(int)
{
    w32::PostToMainThread([] {
        if (!g_videoReady)
            return;
        static SDL_Cursor *arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        if (arrow)
            SDL_SetCursor(arrow);
    });
}

int Bridge_IsMacKeyDown(uint16_t code) { return code < 256 && g_keyDown[code] ? 1 : 0; }
uint32_t Bridge_CurrentModifiers(void) { return g_videoReady ? ModsFromSdl(SDL_GetModState()) : 0; }

int Bridge_GetScreenCount(void)
{
    return OnMain([&] { return g_videoReady ? std::max(SDL_GetNumVideoDisplays(), 0) : 0; });
}
int Bridge_GetScreenRect(int index, BridgeRect *full, BridgeRect *work, double *refreshHz)
{
    return OnMain([&] { return Main_GetScreenRect(index, full, work, refreshHz); });
}
int Bridge_ScreenForPoint(int x, int y)
{
    return OnMain([&] {
        const int n = g_videoReady ? SDL_GetNumVideoDisplays() : 0;
        for (int i = 0; i < n; ++i)
        {
            SDL_Rect b;
            if (SDL_GetDisplayBounds(i, &b) == 0 && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
                return i;
        }
        return 0;
    });
}
int Bridge_GetScreenModes(int index, int *widths, int *heights, int *rates, int maxModes)
{
    return OnMain([&] { return Main_GetScreenModes(index, widths, heights, rates, maxModes); });
}

char *Bridge_GetClipboardText(void)
{
    return OnMain([&]() -> char * {
        if (!g_videoReady || !SDL_HasClipboardText())
            return nullptr;
        char *s = SDL_GetClipboardText();
        if (!s)
            return nullptr;
        char *r = strdup(s); // (the caller frees it with free())
        SDL_free(s);
        return r;
    });
}
void Bridge_SetClipboardText(const char *utf8)
{
    const std::string t = EngineString(utf8 ? utf8 : "");
    OnMain([&] {
        if (g_videoReady)
            SDL_SetClipboardText(t.c_str());
    });
}

int Bridge_MessageBox(const char *title, const char *text, int buttons, int icon)
{
    const std::string t = EngineString(title), m = EngineString(text);
    // buttons: 0 OK, 1 OK/Cancel, 4 Yes/No, 3 Yes/No/Cancel; returns the index of the button pressed
    static const char *labels[4][3] = {{"OK"}, {"OK", "Cancel"}, {"Yes", "No", "Cancel"}, {"Yes", "No"}};
    const int set = buttons == 1 ? 1 : buttons == 3 ? 2 : buttons == 4 ? 3 : 0;
    const int count = set == 0 ? 1 : set == 2 ? 3 : 2;
    SDL_MessageBoxButtonData b[3];
    for (int i = 0; i < count; ++i)
    {
        b[i].flags = 0;
        if (i == 0)
            b[i].flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        if (i == count - 1)
            b[i].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        b[i].buttonid = i;
        b[i].text = labels[set][i];
    }
    SDL_MessageBoxData data = {};
    data.flags = icon == 2 ? SDL_MESSAGEBOX_ERROR : icon == 1 ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION;
    data.title = t.c_str();
    data.message = m.c_str();
    data.numbuttons = count;
    data.buttons = b;
    // (from any thread: Win32 shows one from any thread, and a fatal error on another thread happens while the main
    // thread waits for it; SDL's X11 and Wayland message boxes open their own connection to the display)
    if (g_mouseCaptured && w32::IsMainThread())
        Main_SetMouseCaptured(0);
    int pressed = 0;
    if (!g_videoReady || SDL_ShowMessageBox(&data, &pressed) != 0)
    {
        fprintf(stderr, "\n*** %s ***\n%s\n", t.c_str(), m.c_str());
        return 0;
    }
    if (pressed < 0) // closed without a button: the escape button's answer
        pressed = count - 1;
    return pressed;
}

void Bridge_OpenURL(const char *url)
{
    if (!url || !*url)
        return;
    // a path (a folder to show in the file manager) or a URL
    const std::string u = url[0] == '/' ? std::string("file://") + url : std::string(url);
    OnMain([&] { SDL_OpenURL(u.c_str()); });
}

void Bridge_PreventDisplaySleep(int prevent)
{
    w32::PostToMainThread([prevent] {
        if (!g_videoReady)
            return;
        if (prevent)
            SDL_DisableScreenSaver();
        else
            SDL_EnableScreenSaver();
    });
}

const char *Bridge_AppSupportPath(void)
{
    // the XDG data folder: $XDG_DATA_HOME/BO1Zombies, ~/.local/share/BO1Zombies by default (created)
    static char path[PATH_MAX];
    if (!path[0])
    {
        std::string base;
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0] == '/')
            base = xdg;
        else
        {
            const char *home = getenv("HOME");
            base = std::string(home && *home ? home : ".") + "/.local/share";
        }
        const std::string dir = base + "/BO1Zombies";
        MakeDirs(dir);
        snprintf(path, sizeof(path), "%s", dir.c_str());
    }
    return path;
}

const char *Bridge_BundleResourcePath(void)
{
    // the game's own files are in gamedata/ next to the executable (cmake/linux.cmake)
    static char path[PATH_MAX];
    if (!path[0])
        snprintf(path, sizeof(path), "%s", ExecutableDir().c_str());
    return path;
}

} // extern "C"
