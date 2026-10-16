// Cocoa implementation of the window/input/system bridge (see cocoa_bridge.h).
// The engine keeps its own main loop on the main thread; Bridge_PumpEvents drains the Cocoa event queue from it.

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include "cocoa_bridge.h"
#include "../win32compat/main_thread.h"
#include <stdlib.h>
#include <string.h>
#include <type_traits>

// AppKit objects belong to the main thread, but the engine also calls the bridge from its render thread, which
// creates, shows and sizes the game window. Such calls run on the main thread while the caller waits
// (main_thread.h: the main thread runs them from its Win32 waits and from the event pump).
template <typename F> static auto OnMain(F &&f) -> decltype(f())
{
    if (w32::IsMainThread())
        return f();
    if constexpr (std::is_void_v<decltype(f())>)
    {
        w32::RunOnMainThread([&] {
            @autoreleasepool
            {
                f();
            }
        });
    }
    else
    {
        decltype(f()) r{};
        w32::RunOnMainThread([&] {
            @autoreleasepool
            {
                r = f();
            }
        });
        return r;
    }
}

// wakes the main thread when it waits in the Cocoa run loop (Bridge_PumpEvents) instead of a Win32 wait
static void WakeMainRunLoop(void)
{
    dispatch_async_f(dispatch_get_main_queue(), nullptr, [](void *) { w32::ServiceMainThreadCalls(); });
}

static BridgeEventHandler g_handler;
static double g_unitScale = 1.0; // points per Win32 unit
static bool g_keyDown[256];
static int g_cursorHideCount;
static bool g_mouseCaptured;
static IOPMAssertionID g_sleepAssertion;
static bool g_sleepAssertionActive;
static double g_wheelAccum;

struct BridgeWindow
{
    NSWindow *window;
    NSView *view;
    void *userData;
    bool coversScreen;
};

static void Emit(BridgeEvent &ev)
{
    if (!ev.time)
        ev.time = [NSDate timeIntervalSinceReferenceDate];
    if (g_handler)
        g_handler(&ev);
}

static double PrimaryHeight()
{
    NSArray<NSScreen *> *screens = [NSScreen screens];
    return screens.count ? screens[0].frame.size.height : 0.0;
}

// Cocoa global rect (bottom-left origin, points) -> Win32 screen rect (top-left origin, units)
static BridgeRect ToWinRect(NSRect r)
{
    double H = PrimaryHeight();
    BridgeRect out;
    out.left = (int)lround(r.origin.x / g_unitScale);
    out.right = (int)lround((r.origin.x + r.size.width) / g_unitScale);
    out.top = (int)lround((H - (r.origin.y + r.size.height)) / g_unitScale);
    out.bottom = (int)lround((H - r.origin.y) / g_unitScale);
    return out;
}

static NSRect ToCocoaRect(int x, int y, int w, int h)
{
    double H = PrimaryHeight();
    double px = x * g_unitScale, py = y * g_unitScale, pw = w * g_unitScale, ph = h * g_unitScale;
    return NSMakeRect(px, H - (py + ph), pw, ph);
}

static uint32_t ModsFromFlags(NSEventModifierFlags f)
{
    uint32_t m = 0;
    if (f & NSEventModifierFlagShift)
        m |= BRIDGE_MOD_SHIFT;
    if (f & NSEventModifierFlagControl)
        m |= BRIDGE_MOD_CONTROL;
    if (f & NSEventModifierFlagOption)
        m |= BRIDGE_MOD_ALT;
    if (f & NSEventModifierFlagCommand)
        m |= BRIDGE_MOD_COMMAND;
    if (f & NSEventModifierFlagCapsLock)
        m |= BRIDGE_MOD_CAPSLOCK;
    return m;
}

// ------------------------------------------------------------------------------------------------ view

@interface BO1MetalView : NSView
@property(nonatomic, assign) BridgeWindow *owner;
@end

@implementation BO1MetalView
- (BOOL)wantsUpdateLayer
{
    return YES;
}
- (CALayer *)makeBackingLayer
{
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = nil; // set by the renderer
    layer.opaque = YES;
    layer.needsDisplayOnBoundsChange = NO;
    layer.presentsWithTransaction = NO;
    layer.magnificationFilter = kCAFilterLinear;
    return layer;
}
- (BOOL)acceptsFirstResponder
{
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    return YES;
}
- (BOOL)isOpaque
{
    return YES;
}
- (BOOL)canBecomeKeyView
{
    return YES;
}

- (void)emitMouse:(NSEvent *)e type:(int)type button:(int)button
{
    BridgeEvent ev = {};
    ev.type = type;
    ev.window = self.owner;
    ev.userData = self.owner ? self.owner->userData : nullptr;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    ev.x = (int)lround(p.x / g_unitScale);
    ev.y = (int)lround((self.bounds.size.height - p.y) / g_unitScale);
    ev.dx = e.deltaX;
    ev.dy = e.deltaY;
    ev.button = button;
    ev.modifiers = ModsFromFlags(e.modifierFlags);
    ev.time = e.timestamp;
    Emit(ev);
}
- (void)mouseMoved:(NSEvent *)e
{
    [self emitMouse:e type:BRIDGE_EV_MOUSE_MOVE button:0];
}
- (void)mouseDragged:(NSEvent *)e
{
    [self emitMouse:e type:BRIDGE_EV_MOUSE_MOVE button:0];
}
- (void)rightMouseDragged:(NSEvent *)e
{
    [self emitMouse:e type:BRIDGE_EV_MOUSE_MOVE button:0];
}
- (void)otherMouseDragged:(NSEvent *)e
{
    [self emitMouse:e type:BRIDGE_EV_MOUSE_MOVE button:0];
}
- (void)mouseDown:(NSEvent *)e
{
    // Control-click is the Mac convention for a right click on one-button mice/trackpads
    int b = (e.modifierFlags & NSEventModifierFlagControl) && !g_mouseCaptured ? 1 : 0;
    [self emitMouse:e type:BRIDGE_EV_MOUSE_DOWN button:b];
}
- (void)mouseUp:(NSEvent *)e
{
    int b = (e.modifierFlags & NSEventModifierFlagControl) && !g_mouseCaptured ? 1 : 0;
    [self emitMouse:e type:BRIDGE_EV_MOUSE_UP button:b];
}
- (void)rightMouseDown:(NSEvent *)e
{
    [self emitMouse:e type:BRIDGE_EV_MOUSE_DOWN button:1];
}
- (void)rightMouseUp:(NSEvent *)e
{
    [self emitMouse:e type:BRIDGE_EV_MOUSE_UP button:1];
}
- (void)otherMouseDown:(NSEvent *)e
{
    int b = (int)e.buttonNumber; // 2 = middle, 3/4 = side buttons
    [self emitMouse:e type:BRIDGE_EV_MOUSE_DOWN button:b > 4 ? 4 : b];
}
- (void)otherMouseUp:(NSEvent *)e
{
    int b = (int)e.buttonNumber;
    [self emitMouse:e type:BRIDGE_EV_MOUSE_UP button:b > 4 ? 4 : b];
}
- (void)scrollWheel:(NSEvent *)e
{
    double d = e.scrollingDeltaY;
    if (e.hasPreciseScrollingDeltas)
    {
        // trackpads: accumulate pixel deltas into wheel notches
        g_wheelAccum += d;
        const double step = 16.0;
        while (fabs(g_wheelAccum) >= step)
        {
            BridgeEvent ev = {};
            ev.type = BRIDGE_EV_WHEEL;
            ev.window = self.owner;
            ev.userData = self.owner ? self.owner->userData : nullptr;
            ev.dy = g_wheelAccum > 0 ? 1.0 : -1.0;
            ev.time = e.timestamp;
            Emit(ev);
            g_wheelAccum -= g_wheelAccum > 0 ? step : -step;
        }
        return;
    }
    if (d == 0)
        return;
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_WHEEL;
    ev.window = self.owner;
    ev.userData = self.owner ? self.owner->userData : nullptr;
    ev.dy = d > 0 ? 1.0 : -1.0;
    ev.time = e.timestamp;
    Emit(ev);
}

- (void)emitKey:(NSEvent *)e down:(bool)down
{
    uint16_t code = e.keyCode;
    BridgeEvent ev = {};
    ev.type = down ? BRIDGE_EV_KEY_DOWN : BRIDGE_EV_KEY_UP;
    ev.window = self.owner;
    ev.userData = self.owner ? self.owner->userData : nullptr;
    ev.macKeyCode = code;
    ev.isRepeat = down && e.isARepeat;
    ev.modifiers = ModsFromFlags(e.modifierFlags);
    ev.time = e.timestamp;
    if (code < 256)
        g_keyDown[code] = down;
    Emit(ev);
}
- (void)keyDown:(NSEvent *)e
{
    [self emitKey:e down:true];
    // characters typed (the engine's console and text fields)
    NSString *chars = e.characters;
    const bool command = (e.modifierFlags & NSEventModifierFlagCommand) != 0;
    if (command)
    {
        // Cmd-V/C/X/A behave like the Windows Ctrl shortcuts in text fields
        NSString *base = e.charactersIgnoringModifiers.lowercaseString;
        unichar c = base.length ? [base characterAtIndex:0] : 0;
        if (c == 'v' || c == 'c' || c == 'x' || c == 'a')
        {
            BridgeEvent ev = {};
            ev.type = BRIDGE_EV_CHAR;
            ev.window = self.owner;
            ev.userData = self.owner ? self.owner->userData : nullptr;
            ev.ch = (uint32_t)(c - 'a' + 1);
            ev.macKeyCode = e.keyCode;
            ev.time = e.timestamp;
            Emit(ev);
        }
        return;
    }
    for (NSUInteger i = 0; i < chars.length; ++i)
    {
        unichar c = [chars characterAtIndex:i];
        uint32_t cp = c;
        if (CFStringIsSurrogateHighCharacter(c) && i + 1 < chars.length)
        {
            unichar lo = [chars characterAtIndex:i + 1];
            cp = CFStringGetLongCharacterForSurrogatePair(c, lo);
            ++i;
        }
        if (cp >= 0xF700 && cp <= 0xF8FF)
            continue; // function/arrow keys
        if (cp == 0x7F)
            cp = 0x08; // Delete -> Backspace
        if (cp == 0x03)
            cp = 0x0D; // keypad Enter
        BridgeEvent ev = {};
        ev.type = BRIDGE_EV_CHAR;
        ev.window = self.owner;
        ev.userData = self.owner ? self.owner->userData : nullptr;
        ev.ch = cp;
        ev.macKeyCode = e.keyCode;
        ev.isRepeat = e.isARepeat;
        ev.time = e.timestamp;
        Emit(ev);
    }
}
- (void)keyUp:(NSEvent *)e
{
    [self emitKey:e down:false];
}
- (void)flagsChanged:(NSEvent *)e
{
    uint16_t code = e.keyCode;
    NSEventModifierFlags f = e.modifierFlags;
    bool down;
    switch (code)
    {
    case kVK_Shift:
    case kVK_RightShift: down = (f & NSEventModifierFlagShift) != 0; break;
    case kVK_Control:
    case kVK_RightControl: down = (f & NSEventModifierFlagControl) != 0; break;
    case kVK_Option:
    case kVK_RightOption: down = (f & NSEventModifierFlagOption) != 0; break;
    case kVK_Command:
    case kVK_RightCommand: down = (f & NSEventModifierFlagCommand) != 0; break;
    case kVK_CapsLock: down = (f & NSEventModifierFlagCapsLock) != 0; break;
    default: return;
    }
    // with both keys of a pair held, releasing one keeps the flag set: use our own state for the other key
    if (code < 256 && g_keyDown[code] == down && code != kVK_CapsLock)
        down = !down;
    BridgeEvent ev = {};
    ev.type = down ? BRIDGE_EV_KEY_DOWN : BRIDGE_EV_KEY_UP;
    ev.window = self.owner;
    ev.userData = self.owner ? self.owner->userData : nullptr;
    ev.macKeyCode = code;
    ev.modifiers = ModsFromFlags(f);
    ev.time = e.timestamp;
    if (code < 256)
        g_keyDown[code] = down;
    Emit(ev);
}
// swallow the system beep for unhandled keys
- (void)doCommandBySelector:(SEL)selector
{
}
@end

// ------------------------------------------------------------------------------------------------ window

@interface BO1Window : NSWindow
@end
@implementation BO1Window
- (BOOL)canBecomeKeyWindow
{
    return YES;
}
- (BOOL)canBecomeMainWindow
{
    return YES;
}
@end

@interface BO1WindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) BridgeWindow *owner;
@end

@implementation BO1WindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_CLOSE;
    ev.window = self.owner;
    ev.userData = self.owner->userData;
    Emit(ev);
    return NO; // the engine decides
}
- (void)windowDidResize:(NSNotification *)n
{
    NSRect r = [self.owner->view bounds];
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_RESIZE;
    ev.window = self.owner;
    ev.userData = self.owner->userData;
    ev.x = (int)lround(r.size.width / g_unitScale);
    ev.y = (int)lround(r.size.height / g_unitScale);
    Emit(ev);
}
- (void)windowDidMove:(NSNotification *)n
{
    BridgeRect rc;
    Bridge_GetClientRect(self.owner, &rc);
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_MOVE;
    ev.window = self.owner;
    ev.userData = self.owner->userData;
    ev.x = rc.left;
    ev.y = rc.top;
    Emit(ev);
}
- (void)windowDidMiniaturize:(NSNotification *)n
{
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_MINIMIZE;
    ev.window = self.owner;
    ev.userData = self.owner->userData;
    ev.value = 1;
    Emit(ev);
}
- (void)windowDidDeminiaturize:(NSNotification *)n
{
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_MINIMIZE;
    ev.window = self.owner;
    ev.userData = self.owner->userData;
    ev.value = 0;
    Emit(ev);
}
- (void)windowDidChangeBackingProperties:(NSNotification *)n
{
    [self windowDidResize:n];
}
@end

// ------------------------------------------------------------------------------------------------ app

@interface BO1AppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation BO1AppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_QUIT;
    Emit(ev);
    return NSTerminateCancel; // the engine shuts down on its own
}
- (void)applicationDidBecomeActive:(NSNotification *)n
{
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_ACTIVATE;
    ev.value = 1;
    Emit(ev);
}
- (void)applicationDidResignActive:(NSNotification *)n
{
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_ACTIVATE;
    ev.value = 0;
    Emit(ev);
    if (g_mouseCaptured)
        CGAssociateMouseAndMouseCursorPosition(true);
}
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app
{
    return YES;
}
@end

static BO1AppDelegate *g_appDelegate;

static void BuildMenu()
{
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] init];
    NSString *name = [[NSProcessInfo processInfo] processName];
    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    [bar addItem:windowItem];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    windowItem.submenu = windowMenu;
    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:windowMenu];
}

extern "C" {

void Bridge_Init(BridgeEventHandler handler)
{
    g_handler = handler;
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        const char *hidpi = getenv("BO1_HIDPI");
        if (hidpi && atoi(hidpi))
        {
            NSScreen *main = [NSScreen mainScreen];
            g_unitScale = main ? 1.0 / main.backingScaleFactor : 1.0;
        }
        w32::AddMainThreadWaker(WakeMainRunLoop);
        g_appDelegate = [[BO1AppDelegate alloc] init];
        [NSApp setDelegate:g_appDelegate];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        BuildMenu();
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void Bridge_PumpEvents(int waitMs)
{
    w32::ServiceMainThreadCalls();
    @autoreleasepool
    {
        NSDate *until = waitMs > 0 ? [NSDate dateWithTimeIntervalSinceNow:waitMs / 1000.0] : [NSDate distantPast];
        for (;;)
        {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!e)
                break;
            until = [NSDate distantPast];
            [NSApp sendEvent:e];
        }
        [NSApp updateWindows];
    }
    w32::ServiceMainThreadCalls();
}

static int Main_IsAppActive(void) { return [NSApp isActive] ? 1 : 0; }
static void Main_ActivateApp(void) { [NSApp activateIgnoringOtherApps:YES]; }
double Bridge_UnitScale(void) { return g_unitScale; }

static BridgeWindow *Main_CreateWindow(const char *title, int x, int y, int clientW, int clientH, int borderless, int resizable, void *userData)
{
    @autoreleasepool
    {
        BridgeWindow *bw = new BridgeWindow();
        bw->userData = userData;
        NSWindowStyleMask style = borderless ? NSWindowStyleMaskBorderless
                                             : (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable);
        if (resizable && !borderless)
            style |= NSWindowStyleMaskResizable;
        if (clientW <= 0)
            clientW = 1280;
        if (clientH <= 0)
            clientH = 720;
        NSRect content = ToCocoaRect(x, y, clientW, clientH);
        BO1Window *win = [[BO1Window alloc] initWithContentRect:content styleMask:style backing:NSBackingStoreBuffered defer:NO];
        win.releasedWhenClosed = NO;
        win.title = [NSString stringWithUTF8String:title ? title : "BO1 Zombies"];
        win.acceptsMouseMovedEvents = YES;
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone | NSWindowCollectionBehaviorManaged;
        win.backgroundColor = [NSColor blackColor];
        BO1MetalView *view = [[BO1MetalView alloc] initWithFrame:NSMakeRect(0, 0, content.size.width, content.size.height)];
        view.owner = bw;
        view.wantsLayer = YES;
        view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        win.contentView = view;
        BO1WindowDelegate *del = [[BO1WindowDelegate alloc] init];
        del.owner = bw;
        win.delegate = del;
        objc_setAssociatedObject(win, "bo1delegate", del, OBJC_ASSOCIATION_RETAIN);
        [win makeFirstResponder:view];
        bw->window = win;
        bw->view = view;
        if (x == (int)0x80000000 || y == (int)0x80000000)
            [win center];
        return bw;
    }
}

static void Main_DestroyWindow(BridgeWindow *w)
{
    if (!w)
        return;
    @autoreleasepool
    {
        if (w->coversScreen)
            Bridge_SetCoversScreen(w, 0);
        w->window.delegate = nil;
        [w->window orderOut:nil];
        [w->window close];
        w->window = nil;
        w->view = nil;
    }
    delete w;
}

static void Main_ShowWindow(BridgeWindow *w, int show)
{
    if (!w)
        return;
    if (show)
    {
        [w->window makeKeyAndOrderFront:nil];
        [w->window makeFirstResponder:w->view];
        [NSApp activateIgnoringOtherApps:YES];
    }
    else
        [w->window orderOut:nil];
}
static void Main_MinimizeWindow(BridgeWindow *w)
{
    if (w)
        [w->window miniaturize:nil];
}
static void Main_RestoreWindow(BridgeWindow *w)
{
    if (w && w->window.miniaturized)
        [w->window deminiaturize:nil];
}
static int Main_IsMinimized(BridgeWindow *w) { return w && w->window.miniaturized ? 1 : 0; }
static int Main_IsVisible(BridgeWindow *w) { return w && w->window.visible ? 1 : 0; }

static void Main_SetWindowStyle(BridgeWindow *w, int borderless, int resizable)
{
    if (!w)
        return;
    NSRect content = [w->window contentRectForFrameRect:w->window.frame];
    NSWindowStyleMask style = borderless ? NSWindowStyleMaskBorderless
                                         : (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable);
    if (resizable && !borderless)
        style |= NSWindowStyleMaskResizable;
    if (w->window.styleMask != style)
    {
        w->window.styleMask = style;
        [w->window setFrame:[w->window frameRectForContentRect:content] display:YES];
        [w->window makeFirstResponder:w->view];
    }
}

static void Main_SetClientRect(BridgeWindow *w, int x, int y, int clientW, int clientH)
{
    if (!w)
        return;
    NSRect content = ToCocoaRect(x, y, clientW, clientH);
    NSRect frame = [w->window frameRectForContentRect:content];
    [w->window setFrame:frame display:YES];
}

static void Main_GetClientRect(BridgeWindow *w, BridgeRect *r)
{
    if (!w)
    {
        memset(r, 0, sizeof(*r));
        return;
    }
    NSRect content = [w->window contentRectForFrameRect:w->window.frame];
    *r = ToWinRect(content);
}

static void Main_GetFrameRect(BridgeWindow *w, BridgeRect *r)
{
    if (!w)
    {
        memset(r, 0, sizeof(*r));
        return;
    }
    *r = ToWinRect(w->window.frame);
}

static void Main_SetTitle(BridgeWindow *w, const char *title)
{
    if (w && title)
        w->window.title = [NSString stringWithUTF8String:title];
}

static void Main_FocusWindow(BridgeWindow *w)
{
    if (!w)
        return;
    [w->window makeKeyAndOrderFront:nil];
    [w->window makeFirstResponder:w->view];
}

static int Main_IsKeyWindow(BridgeWindow *w) { return w && w->window.isKeyWindow && [NSApp isActive] ? 1 : 0; }

static void Main_SetCoversScreen(BridgeWindow *w, int covers)
{
    if (!w || (bool)covers == w->coversScreen)
        return;
    w->coversScreen = covers != 0;
    if (covers)
    {
        [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
        w->window.level = NSNormalWindowLevel;
    }
    else
    {
        [NSApp setPresentationOptions:NSApplicationPresentationDefault];
    }
}

static void *Main_GetMetalLayer(BridgeWindow *w) { return w ? (__bridge void *)w->view.layer : nullptr; }
static double Main_GetBackingScale(BridgeWindow *w) { return w ? w->window.backingScaleFactor : 1.0; }

static void Main_SetCursorVisible(int visible)
{
    if (visible)
    {
        while (g_cursorHideCount > 0)
        {
            [NSCursor unhide];
            --g_cursorHideCount;
        }
    }
    else if (g_cursorHideCount == 0)
    {
        [NSCursor hide];
        ++g_cursorHideCount;
    }
}

static void Main_SetMouseCaptured(int captured)
{
    if ((bool)captured == g_mouseCaptured)
        return;
    g_mouseCaptured = captured != 0;
    CGAssociateMouseAndMouseCursorPosition(captured ? false : true);
}

static void Main_WarpCursor(int x, int y)
{
    CGPoint p = CGPointMake(x * g_unitScale, y * g_unitScale);
    CGWarpMouseCursorPosition(p);
    if (!g_mouseCaptured)
        CGAssociateMouseAndMouseCursorPosition(true);
}

static void Main_GetCursorPos(int *x, int *y)
{
    NSPoint p = [NSEvent mouseLocation];
    double H = PrimaryHeight();
    *x = (int)lround(p.x / g_unitScale);
    *y = (int)lround((H - p.y) / g_unitScale);
}

static void Main_SetArrowCursor(int wait)
{
    (void)wait;
    [[NSCursor arrowCursor] set];
}

int Bridge_IsMacKeyDown(uint16_t code) { return code < 256 && g_keyDown[code] ? 1 : 0; }
static uint32_t Main_CurrentModifiers(void) { return ModsFromFlags([NSEvent modifierFlags]); }

static int Main_GetScreenCount(void) { return (int)[NSScreen screens].count; }

static int Main_GetScreenRect(int index, BridgeRect *full, BridgeRect *work, double *refreshHz)
{
    NSArray<NSScreen *> *screens = [NSScreen screens];
    if (index < 0 || index >= (int)screens.count)
        return 0;
    NSScreen *s = screens[index];
    if (full)
        *full = ToWinRect(s.frame);
    if (work)
        *work = ToWinRect(s.visibleFrame);
    if (refreshHz)
    {
        double hz = 60.0;
        if (@available(macOS 12.0, *))
            hz = (double)s.maximumFramesPerSecond;
        *refreshHz = hz > 0 ? hz : 60.0;
    }
    return 1;
}

static int Main_ScreenForPoint(int x, int y)
{
    NSArray<NSScreen *> *screens = [NSScreen screens];
    for (NSUInteger i = 0; i < screens.count; ++i)
    {
        BridgeRect r = ToWinRect(screens[i].frame);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return (int)i;
    }
    return 0;
}

static int Main_GetScreenModes(int index, int *widths, int *heights, int *rates, int maxModes)
{
    NSArray<NSScreen *> *screens = [NSScreen screens];
    if (index < 0 || index >= (int)screens.count)
        return 0;
    NSScreen *s = screens[index];
    // Windowed/borderless rendering at any size up to the screen is possible; offer the common 16:9/16:10/4:3 sizes
    // that fit, plus the screen itself.
    BridgeRect full = ToWinRect(s.frame);
    int sw = full.right - full.left, sh = full.bottom - full.top;
    double hz = 60.0;
    if (@available(macOS 12.0, *))
        hz = s.maximumFramesPerSecond > 0 ? (double)s.maximumFramesPerSecond : 60.0;
    static const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 800}, {1280, 960}, {1280, 1024}, {1366, 768}, {1440, 900}, {1600, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200}, {2048, 1152}, {2560, 1440}, {2560, 1600}, {2880, 1800}, {3024, 1964}, {3456, 2234}, {3840, 2160}, {5120, 2880}};
    int n = 0;
    for (auto &sz : sizes)
    {
        if (sz[0] <= sw && sz[1] <= sh && n < maxModes)
        {
            widths[n] = sz[0];
            heights[n] = sz[1];
            rates[n] = (int)lround(hz);
            ++n;
        }
    }
    bool haveNative = false;
    for (int i = 0; i < n; ++i)
        if (widths[i] == sw && heights[i] == sh)
            haveNative = true;
    if (!haveNative && n < maxModes)
    {
        widths[n] = sw;
        heights[n] = sh;
        rates[n] = (int)lround(hz);
        ++n;
    }
    return n;
}

static char *Main_GetClipboardText(void)
{
    @autoreleasepool
    {
        NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        if (!s)
            return nullptr;
        return strdup(s.UTF8String);
    }
}

static void Main_SetClipboardText(const char *utf8)
{
    @autoreleasepool
    {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        if (utf8)
            [pb setString:[NSString stringWithUTF8String:utf8] forType:NSPasteboardTypeString];
    }
}

static int RunAlert(NSString *title, NSString *text, int buttons, int icon)
{
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = title;
    a.informativeText = text;
    a.alertStyle = icon == 2 ? NSAlertStyleCritical : icon == 1 ? NSAlertStyleWarning : NSAlertStyleInformational;
    // buttons: 0 OK, 1 OK/Cancel, 4 Yes/No, 3 Yes/No/Cancel
    switch (buttons)
    {
    case 1:
        [a addButtonWithTitle:@"OK"];
        [a addButtonWithTitle:@"Cancel"];
        break;
    case 3:
        [a addButtonWithTitle:@"Yes"];
        [a addButtonWithTitle:@"No"];
        [a addButtonWithTitle:@"Cancel"];
        break;
    case 4:
        [a addButtonWithTitle:@"Yes"];
        [a addButtonWithTitle:@"No"];
        break;
    default: [a addButtonWithTitle:@"OK"]; break;
    }
    bool hadCapture = g_mouseCaptured;
    Bridge_SetMouseCaptured(0);
    Bridge_SetCursorVisible(1);
    NSModalResponse r = [a runModal];
    if (hadCapture)
        Bridge_SetMouseCaptured(1);
    return (int)(r - NSAlertFirstButtonReturn);
}

// A message box from another thread. Win32 shows one from any thread; AppKit only runs alerts on the main thread,
// which may be the very thread the caller is waiting for (Sys_Error suspends the other threads, and a fatal error on
// the database thread happens while the main thread waits for the load). CFUserNotification alerts are shown by the
// system on behalf of the process and can be run from any thread.
static int RunAlertFromThread(NSString *title, NSString *text, int buttons, int icon)
{
    // the cursor may be hidden and decoupled from the mouse (mouse look): give it back so the alert can be clicked
    CGAssociateMouseAndMouseCursorPosition(true);
    CGDisplayShowCursor(kCGDirectMainDisplay);
    const CFOptionFlags level = icon == 2   ? kCFUserNotificationStopAlertLevel
                                : icon == 1 ? kCFUserNotificationCautionAlertLevel
                                            : kCFUserNotificationNoteAlertLevel;
    CFStringRef def = CFSTR("OK"), alt = nullptr, other = nullptr;
    switch (buttons)
    {
    case 1: alt = CFSTR("Cancel"); break;
    case 3:
        def = CFSTR("Yes");
        alt = CFSTR("No");
        other = CFSTR("Cancel");
        break;
    case 4:
        def = CFSTR("Yes");
        alt = CFSTR("No");
        break;
    default: break;
    }
    CFOptionFlags response = kCFUserNotificationDefaultResponse;
    CFUserNotificationDisplayAlert(0, level, nullptr, nullptr, nullptr, (__bridge CFStringRef)title,
                                   (__bridge CFStringRef)text, def, alt, other, &response);
    switch (response & 3)
    {
    case kCFUserNotificationAlternateResponse: return 1;
    case kCFUserNotificationOtherResponse: return 2;
    case kCFUserNotificationCancelResponse: return buttons == 1 || buttons == 3 ? (buttons == 1 ? 1 : 2) : 0;
    default: return 0;
    }
}

int Bridge_MessageBox(const char *title, const char *text, int buttons, int icon)
{
    int result = 0;
    @autoreleasepool
    {
        NSString *t = [NSString stringWithUTF8String:title ? title : ""];
        NSString *m = [NSString stringWithUTF8String:text ? text : ""];
        if (!t)
            t = @"";
        if (!m)
            m = [NSString stringWithCString:text ? text : "" encoding:NSISOLatin1StringEncoding];
        if (!m)
            m = @"";
        if ([NSThread isMainThread])
            result = RunAlert(t, m, buttons, icon);
        else
            result = RunAlertFromThread(t, m, buttons, icon);
    }
    return result;
}

static void Main_OpenURL(const char *url)
{
    if (!url)
        return;
    @autoreleasepool
    {
        NSString *s = [NSString stringWithUTF8String:url];
        NSURL *u = [NSURL URLWithString:s];
        if (!u)
            u = [NSURL fileURLWithPath:s];
        [[NSWorkspace sharedWorkspace] openURL:u];
    }
}

void Bridge_PreventDisplaySleep(int prevent)
{
    if (prevent && !g_sleepAssertionActive)
    {
        if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, CFSTR("BO1 Zombies is running"), &g_sleepAssertion) == kIOReturnSuccess)
            g_sleepAssertionActive = true;
    }
    else if (!prevent && g_sleepAssertionActive)
    {
        IOPMAssertionRelease(g_sleepAssertion);
        g_sleepAssertionActive = false;
    }
}

int Bridge_ChooseFolder(const char *message, char *out, int outSize)
{
    __block int ok = 0;
    void (^run)(void) = ^{
        @autoreleasepool
        {
            [NSApp activateIgnoringOtherApps:YES];
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            panel.canChooseDirectories = YES;
            panel.canChooseFiles = NO;
            panel.allowsMultipleSelection = NO;
            panel.canCreateDirectories = NO;
            panel.prompt = @"Choose";
            if (message)
                panel.message = [NSString stringWithUTF8String:message];
            if ([panel runModal] == NSModalResponseOK && panel.URLs.count)
            {
                const char *p = panel.URLs[0].path.fileSystemRepresentation;
                if (p && out && outSize > 0)
                {
                    strncpy(out, p, (size_t)outSize - 1);
                    out[outSize - 1] = 0;
                    ok = 1;
                }
            }
        }
    };
    // (not dispatch_sync: the main thread may be blocked in a Win32 wait, where it runs queued calls instead)
    OnMain([&] { run(); });
    return ok;
}

const char *Bridge_AppSupportPath(void)
{
    static char path[4096];
    @autoreleasepool
    {
        NSArray<NSURL *> *urls = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask];
        NSString *base = urls.count ? urls[0].path : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"];
        NSString *dir = [base stringByAppendingPathComponent:@"BO1Zombies"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        strncpy(path, dir.fileSystemRepresentation, sizeof(path) - 1);
    }
    return path;
}

const char *Bridge_BundleResourcePath(void)
{
    static char path[4096];
    @autoreleasepool
    {
        NSString *p = [[NSBundle mainBundle] resourcePath];
        strncpy(path, p ? p.UTF8String : ".", sizeof(path) - 1);
    }
    return path;
}

// ---- the bridge entry points that use AppKit: on the main thread, whichever thread calls them
int Bridge_IsAppActive(void) { return OnMain([&] { return Main_IsAppActive(); }); }
void Bridge_ActivateApp(void) { OnMain([&] { Main_ActivateApp(); }); }
BridgeWindow *Bridge_CreateWindow(const char *title, int x, int y, int clientW, int clientH, int borderless, int resizable, void *userData)
{
    return OnMain([&] { return Main_CreateWindow(title, x, y, clientW, clientH, borderless, resizable, userData); });
}
void Bridge_DestroyWindow(BridgeWindow *w) { OnMain([&] { Main_DestroyWindow(w); }); }
void Bridge_ShowWindow(BridgeWindow *w, int show) { OnMain([&] { Main_ShowWindow(w, show); }); }
void Bridge_MinimizeWindow(BridgeWindow *w) { OnMain([&] { Main_MinimizeWindow(w); }); }
void Bridge_RestoreWindow(BridgeWindow *w) { OnMain([&] { Main_RestoreWindow(w); }); }
int Bridge_IsMinimized(BridgeWindow *w) { return OnMain([&] { return Main_IsMinimized(w); }); }
int Bridge_IsVisible(BridgeWindow *w) { return OnMain([&] { return Main_IsVisible(w); }); }
void Bridge_SetWindowStyle(BridgeWindow *w, int borderless, int resizable) { OnMain([&] { Main_SetWindowStyle(w, borderless, resizable); }); }
void Bridge_SetClientRect(BridgeWindow *w, int x, int y, int clientW, int clientH) { OnMain([&] { Main_SetClientRect(w, x, y, clientW, clientH); }); }
void Bridge_GetClientRect(BridgeWindow *w, BridgeRect *r) { OnMain([&] { Main_GetClientRect(w, r); }); }
void Bridge_GetFrameRect(BridgeWindow *w, BridgeRect *r) { OnMain([&] { Main_GetFrameRect(w, r); }); }
void Bridge_SetTitle(BridgeWindow *w, const char *title) { OnMain([&] { Main_SetTitle(w, title); }); }
void Bridge_FocusWindow(BridgeWindow *w) { OnMain([&] { Main_FocusWindow(w); }); }
int Bridge_IsKeyWindow(BridgeWindow *w) { return OnMain([&] { return Main_IsKeyWindow(w); }); }
void Bridge_SetCoversScreen(BridgeWindow *w, int covers) { OnMain([&] { Main_SetCoversScreen(w, covers); }); }
void *Bridge_GetMetalLayer(BridgeWindow *w) { return OnMain([&] { return Main_GetMetalLayer(w); }); }
double Bridge_GetBackingScale(BridgeWindow *w) { return OnMain([&] { return Main_GetBackingScale(w); }); }
void Bridge_SetCursorVisible(int visible) { OnMain([&] { Main_SetCursorVisible(visible); }); }
void Bridge_SetMouseCaptured(int captured) { OnMain([&] { Main_SetMouseCaptured(captured); }); }
void Bridge_WarpCursor(int x, int y) { OnMain([&] { Main_WarpCursor(x, y); }); }
void Bridge_GetCursorPos(int *x, int *y) { OnMain([&] { Main_GetCursorPos(x, y); }); }
void Bridge_SetArrowCursor(int wait) { OnMain([&] { Main_SetArrowCursor(wait); }); }
uint32_t Bridge_CurrentModifiers(void) { return OnMain([&] { return Main_CurrentModifiers(); }); }
int Bridge_GetScreenCount(void) { return OnMain([&] { return Main_GetScreenCount(); }); }
int Bridge_GetScreenRect(int index, BridgeRect *full, BridgeRect *work, double *refreshHz)
{
    return OnMain([&] { return Main_GetScreenRect(index, full, work, refreshHz); });
}
int Bridge_ScreenForPoint(int x, int y) { return OnMain([&] { return Main_ScreenForPoint(x, y); }); }
int Bridge_GetScreenModes(int index, int *widths, int *heights, int *rates, int maxModes)
{
    return OnMain([&] { return Main_GetScreenModes(index, widths, heights, rates, maxModes); });
}
char *Bridge_GetClipboardText(void) { return OnMain([&] { return Main_GetClipboardText(); }); }
void Bridge_SetClipboardText(const char *utf8) { OnMain([&] { Main_SetClipboardText(utf8); }); }
void Bridge_OpenURL(const char *url) { OnMain([&] { Main_OpenURL(url); }); }

} // extern "C"
