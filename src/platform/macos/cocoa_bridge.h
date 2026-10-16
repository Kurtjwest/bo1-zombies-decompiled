#pragma once
// C interface between the Win32 compatibility layer (C++, Windows types) and the Cocoa implementation
// (Objective-C++, cocoa_bridge.mm). Neither side sees the other's headers.
//
// Coordinates are in "Win32 screen units": origin at the top-left corner of the primary display, y down. By default
// one unit is one point (the backing scale factor only affects the Metal drawable); set BO1_HIDPI=1 to make one unit
// one physical pixel instead.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BridgeWindow BridgeWindow;

enum BridgeEventType
{
    BRIDGE_EV_KEY_DOWN = 1,
    BRIDGE_EV_KEY_UP,
    BRIDGE_EV_CHAR,
    BRIDGE_EV_MOUSE_MOVE,
    BRIDGE_EV_MOUSE_DOWN,
    BRIDGE_EV_MOUSE_UP,
    BRIDGE_EV_WHEEL,
    BRIDGE_EV_ACTIVATE,   // value: 1 = app became active, 0 = resigned
    BRIDGE_EV_MINIMIZE,   // value: 1 = minimized, 0 = restored
    BRIDGE_EV_RESIZE,     // x, y = new client size
    BRIDGE_EV_MOVE,       // x, y = new client origin (screen units)
    BRIDGE_EV_CLOSE,
    BRIDGE_EV_QUIT,       // Cmd-Q / app menu Quit
};

typedef struct BridgeEvent
{
    int type;
    BridgeWindow *window;
    void *userData;      // the HWND the window was created for
    uint16_t macKeyCode; // kVK_* for key events
    uint8_t isRepeat;
    uint32_t modifiers;  // BRIDGE_MOD_* bit set at the time of the event
    uint32_t ch;         // UTF-32 code point for BRIDGE_EV_CHAR
    int button;          // 0 left, 1 right, 2 middle, 3/4 extra
    int x, y;            // client coordinates (mouse) or size/position (resize/move)
    double dx, dy;       // raw mouse delta (points) or wheel delta
    int value;
    double time;         // seconds, event timestamp
} BridgeEvent;

enum
{
    BRIDGE_MOD_SHIFT = 1 << 0,
    BRIDGE_MOD_CONTROL = 1 << 1,
    BRIDGE_MOD_ALT = 1 << 2,
    BRIDGE_MOD_COMMAND = 1 << 3,
    BRIDGE_MOD_CAPSLOCK = 1 << 4,
};

typedef void (*BridgeEventHandler)(const BridgeEvent *ev);

typedef struct BridgeRect
{
    int left, top, right, bottom;
} BridgeRect;

// application
void Bridge_Init(BridgeEventHandler handler);
void Bridge_PumpEvents(int waitMs); // dispatches pending Cocoa events (waits up to waitMs for the first one)
int Bridge_IsAppActive(void);
void Bridge_ActivateApp(void);
double Bridge_UnitScale(void); // points per Win32 unit (1, or 1/backingScale with BO1_HIDPI)

// windows
BridgeWindow *Bridge_CreateWindow(const char *title, int x, int y, int clientW, int clientH, int borderless, int resizable, void *userData);
void Bridge_DestroyWindow(BridgeWindow *w);
void Bridge_ShowWindow(BridgeWindow *w, int show);
void Bridge_MinimizeWindow(BridgeWindow *w);
void Bridge_RestoreWindow(BridgeWindow *w);
int Bridge_IsMinimized(BridgeWindow *w);
int Bridge_IsVisible(BridgeWindow *w);
void Bridge_SetWindowStyle(BridgeWindow *w, int borderless, int resizable);
void Bridge_SetClientRect(BridgeWindow *w, int x, int y, int clientW, int clientH);
void Bridge_GetClientRect(BridgeWindow *w, BridgeRect *screenRect); // client area in screen units
void Bridge_GetFrameRect(BridgeWindow *w, BridgeRect *screenRect);  // including title bar
void Bridge_SetTitle(BridgeWindow *w, const char *title);
void Bridge_FocusWindow(BridgeWindow *w);
int Bridge_IsKeyWindow(BridgeWindow *w);
void Bridge_SetCoversScreen(BridgeWindow *w, int covers); // hide menu bar and Dock while a borderless window covers its screen
void *Bridge_GetMetalLayer(BridgeWindow *w);                // CAMetalLayer*
double Bridge_GetBackingScale(BridgeWindow *w);

// cursor / mouse
void Bridge_SetCursorVisible(int visible);
void Bridge_SetMouseCaptured(int captured); // decouple the cursor from mouse motion (relative mouse look)
void Bridge_WarpCursor(int screenX, int screenY);
void Bridge_GetCursorPos(int *screenX, int *screenY);
void Bridge_SetArrowCursor(int wait);

// keyboard state
int Bridge_IsMacKeyDown(uint16_t macKeyCode);
uint32_t Bridge_CurrentModifiers(void);

// displays
int Bridge_GetScreenCount(void);
int Bridge_GetScreenRect(int index, BridgeRect *full, BridgeRect *work, double *refreshHz); // screen units
int Bridge_ScreenForPoint(int x, int y);
int Bridge_GetScreenModes(int index, int *widths, int *heights, int *rates, int maxModes);

// system services
char *Bridge_GetClipboardText(void); // malloc'd UTF-8, or NULL
void Bridge_SetClipboardText(const char *utf8);
int Bridge_MessageBox(const char *title, const char *text, int buttons, int icon); // returns button index pressed
void Bridge_OpenURL(const char *url);
void Bridge_PreventDisplaySleep(int prevent);
const char *Bridge_BundleResourcePath(void);
const char *Bridge_AppSupportPath(void);                            // ~/Library/Application Support/BO1Zombies (created)
int Bridge_ChooseFolder(const char *message, char *out, int outSize); // folder picker; 1 if one was chosen

#ifdef __cplusplus
}
#endif
