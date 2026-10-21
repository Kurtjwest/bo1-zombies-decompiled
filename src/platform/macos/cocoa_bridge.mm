// Cocoa implementation of the window/input/system bridge (see ../common/platform_bridge.h).
// The engine keeps its own main loop on the main thread; Bridge_PumpEvents drains the Cocoa event queue from it.

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <objc/runtime.h>

#include "../common/platform_bridge.h"
#include "../win32compat/main_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <type_traits>
#include <vector>

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
static double g_wheelAccum;      // precise scrolling: points of the current swipe not turned into notches yet
static int g_wheelGestureNotches; // notches sent for the current swipe
static int g_wheelGestureDir;     // its direction (1, -1; 0 = none yet)
static double g_wheelLastTime;    // timestamp of the last precise scroll event
// The delta of the first mouse event after CGWarpMouseCursorPosition still contains the warp: AppKit measures it from
// where the cursor was at the previous event. The mouse is captured by warping the cursor into the window, so that
// first delta would throw the game's cursor (or the view) by the distance the cursor was moved; it is taken out
// again, as SDL does (Cocoa_HandleMouseEvent).
static bool g_warpPending;
static CGPoint g_warpTarget;        // CG global coordinates (top-left origin, points)
static NSPoint g_lastMoveLocation;  // +[NSEvent mouseLocation] at the last mouse move (bottom-left origin)
static bool g_haveLastMove;
static void NoteCursorWarp(CGPoint p)
{
    if (!g_haveLastMove)
    {
        g_lastMoveLocation = [NSEvent mouseLocation];
        g_haveLastMove = true;
    }
    g_warpTarget = p;
    g_warpPending = true;
}

struct BridgeWindow
{
    NSWindow *window;
    NSView *view;
    void *userData;
    bool coversScreen;
    // native full screen (a Space of its own): what the engine asked for, the state AppKit has the window in, and
    // the animated transition between the two (requests made during one are applied when it ends)
    bool wantFullScreen;
    bool inFullScreen;   // entered (or entering) full screen
    bool transitioning;
    int pendingFullScreen = -1;
    // the frame and style the engine asked for while the window was in full screen, applied once it leaves
    bool haveWindowedFrame;
    NSRect windowedContent;
    bool wantBorderless, wantResizable;
    bool fillScaling;    // the drawable stretches over the whole view (else it keeps its aspect ratio)
    int logicalW, logicalH; // the size the game draws at (the back buffer), in units
    // AppKit state other threads read every frame (the render thread asks whether the window is visible, minimized
    // and focused): kept current by the main thread, so those reads need not wait for it
    std::atomic<int> visible{0};
    std::atomic<int> minimized{0};
    std::atomic<int> keyWindow{0};
    std::atomic<int> fullScreen{0};
};

static std::vector<BridgeWindow *> g_bridgeWindows; // main thread only
static std::atomic<int> g_appActiveCache{1};

static void RefreshWindowState(BridgeWindow *w)
{
    if (!w || !w->window)
        return;
    w->visible.store(w->window.visible ? 1 : 0, std::memory_order_relaxed);
    w->minimized.store(w->window.miniaturized ? 1 : 0, std::memory_order_relaxed);
    w->keyWindow.store(w->window.isKeyWindow ? 1 : 0, std::memory_order_relaxed);
}

static void RefreshAllWindowState()
{
    g_appActiveCache.store([NSApp isActive] ? 1 : 0, std::memory_order_relaxed);
    for (BridgeWindow *w : g_bridgeWindows)
        RefreshWindowState(w);
}

// Text from the engine is UTF-8 when it comes from the file system and Windows-1252 / Latin-1 when it is the game's own
// (localized strings, console text, error messages). stringWithUTF8String: returns nil for the latter, and AppKit
// raises on a nil window title, pasteboard string or panel message, so this always returns a string.
static NSString *EngineString(const char *s)
{
    if (!s)
        return @"";
    NSString *r = [NSString stringWithUTF8String:s];
    if (!r)
        r = [NSString stringWithCString:s encoding:NSWindowsCP1252StringEncoding];
    if (!r)
        r = [NSString stringWithCString:s encoding:NSISOLatin1StringEncoding];
    return r ? r : @"";
}

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
    layer.contentsGravity = kCAGravityResizeAspect;
    layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack); // around a drawable kept at its aspect ratio
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
    if (type == BRIDGE_EV_MOUSE_MOVE)
    {
        const NSPoint loc = [NSEvent mouseLocation];
        if (g_warpPending && g_haveLastMove)
        {
            ev.dx += g_lastMoveLocation.x - g_warpTarget.x;
            ev.dy += (PrimaryHeight() - g_lastMoveLocation.y) - g_warpTarget.y;
        }
        g_warpPending = false;
        g_lastMoveLocation = loc;
        g_haveLastMove = true;
    }
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
    // natural scrolling (the macOS default) inverts the deltas; Windows reports the wheel's physical direction
    if (e.isDirectionInvertedFromDevice)
        d = -d;
    if (e.hasPreciseScrollingDeltas)
    {
        // Trackpads and the Magic Mouse send a stream of point deltas, and then keep scrolling on their own (the
        // momentum phase) after the fingers lift. The game takes one wheel notch as one step (the next menu item
        // on the front end's screens, the next weapon), so the momentum is dropped and the deltas are turned into
        // notches: the first one early in a swipe, the following ones only as the swipe goes on.
        if (e.momentumPhase != NSEventPhaseNone)
            return;
        const bool gestureOver = e.phase == NSEventPhaseEnded || e.phase == NSEventPhaseCancelled;
        // (devices with smooth scrolling but no gesture phases: a pause starts a new swipe)
        const bool paused = e.timestamp - g_wheelLastTime > 0.25;
        g_wheelLastTime = e.timestamp;
        if (gestureOver || paused || e.phase == NSEventPhaseBegan || e.phase == NSEventPhaseMayBegin)
        {
            g_wheelAccum = 0;
            g_wheelGestureNotches = 0;
            g_wheelGestureDir = 0;
        }
        if (gestureOver || d == 0)
            return;
        const int dir = d > 0 ? 1 : -1;
        if (dir != g_wheelGestureDir)
        {
            // a new swipe, or the same one turned around
            g_wheelAccum = 0;
            g_wheelGestureNotches = 0;
            g_wheelGestureDir = dir;
        }
        g_wheelAccum += fabs(d);
        for (;;)
        {
            const double step = g_wheelGestureNotches == 0 ? 6.0 : 48.0;
            if (g_wheelAccum < step)
                break;
            g_wheelAccum -= step;
            ++g_wheelGestureNotches;
            BridgeEvent ev = {};
            ev.type = BRIDGE_EV_WHEEL;
            ev.window = self.owner;
            ev.userData = self.owner ? self.owner->userData : nullptr;
            ev.dy = dir;
            ev.time = e.timestamp;
            Emit(ev);
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
    // Each side of a modifier pair has its own device-dependent flag (IOLLEvent.h NX_DEVICE*KEYMASK), so the state of
    // the key that changed is read directly; deriving it from our own key table instead inverted it for good once a
    // press or release went to another app (Cmd-Tab, Cmd-H). Without device flags (some remote or synthetic
    // events), the device-independent flag stands in.
    NSEventModifierFlags mine = 0, other = 0, indep = 0;
    switch (code)
    {
    case kVK_Shift: mine = 0x2, other = 0x4, indep = NSEventModifierFlagShift; break;
    case kVK_RightShift: mine = 0x4, other = 0x2, indep = NSEventModifierFlagShift; break;
    case kVK_Control: mine = 0x1, other = 0x2000, indep = NSEventModifierFlagControl; break;
    case kVK_RightControl: mine = 0x2000, other = 0x1, indep = NSEventModifierFlagControl; break;
    case kVK_Option: mine = 0x20, other = 0x40, indep = NSEventModifierFlagOption; break;
    case kVK_RightOption: mine = 0x40, other = 0x20, indep = NSEventModifierFlagOption; break;
    case kVK_Command: mine = 0x8, other = 0x10, indep = NSEventModifierFlagCommand; break;
    case kVK_RightCommand: mine = 0x10, other = 0x8, indep = NSEventModifierFlagCommand; break;
    case kVK_CapsLock:
    {
        // Windows sends a press and a release for each Caps Lock key press, whichever way the lock goes
        for (int i = 0; i < 2; ++i)
        {
            BridgeEvent ev = {};
            ev.type = i == 0 ? BRIDGE_EV_KEY_DOWN : BRIDGE_EV_KEY_UP;
            ev.window = self.owner;
            ev.userData = self.owner ? self.owner->userData : nullptr;
            ev.macKeyCode = code;
            ev.modifiers = ModsFromFlags(f);
            ev.time = e.timestamp;
            Emit(ev);
        }
        g_keyDown[code] = false;
        return;
    }
    default: return;
    }
    const bool down = (f & (mine | other)) ? (f & mine) != 0 : (f & indep) != 0;
    if (code < 256 && g_keyDown[code] == down)
        return; // no change for this key (the other key of the pair changed)
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

// ---- native full screen --------------------------------------------------------------------------------------------
// The engine's full-screen and borderless modes, and the window's green button / Ctrl-Cmd-F, put the game window in a
// Space of its own. AppKit animates the change; requests that arrive during the animation are applied once it ends.
// The game keeps drawing at its own resolution: the Metal drawable keeps the back buffer's size and the layer scales
// it to the view (see SetContentScaling and the Win32 layer's client mapping).
static NSWindowStyleMask WindowedStyleMask(bool borderless, bool resizable)
{
    NSWindowStyleMask style = borderless ? NSWindowStyleMaskBorderless
                                         : (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable);
    if (resizable && !borderless)
        style |= NSWindowStyleMaskResizable;
    return style;
}

// The drawable over the view: stretched over all of it for the engine's own full screen (it draws for the screen's
// shape, as for a monitor showing any mode), else at its own aspect ratio with black bars around it. A window showing
// the engine's full screen outside of full screen (the player took it out) keeps the screen's shape when resized.
static void ApplyContentGravity(BridgeWindow *w)
{
    CALayer *layer = w->view.layer;
    if (!layer)
        return;
    NSString *g = w->fillScaling ? kCAGravityResize : kCAGravityResizeAspect;
    if (![layer.contentsGravity isEqualToString:g])
    {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.contentsGravity = g;
        [CATransaction commit];
    }
    NSWindow *win = w->window;
    if (!win || w->inFullScreen || w->transitioning)
        return;
    NSScreen *screen = win.screen ? win.screen : [NSScreen mainScreen];
    if (w->fillScaling && screen && screen.frame.size.height > 0)
        win.contentAspectRatio = screen.frame.size;
    else
        win.contentResizeIncrements = NSMakeSize(1.0, 1.0); // (clears the aspect ratio)
}

static void EmitFullScreen(BridgeWindow *w, bool on)
{
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_FULLSCREEN;
    ev.window = w;
    ev.userData = w->userData;
    ev.value = on ? 1 : 0;
    Emit(ev);
}

static void EnterFullScreenNow(BridgeWindow *w)
{
    NSWindow *win = w->window;
    if (!win || !win.visible)
        return; // (entered when the window is shown, see Main_ShowWindow)
    // only a full-screen capable window toggles, and a borderless one only with a title bar (hidden in full screen):
    // it gets one for the duration, and its own style back when it leaves
    win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary | NSWindowCollectionBehaviorManaged;
    const NSWindowStyleMask fsStyle = WindowedStyleMask(false, true);
    if ((win.styleMask & fsStyle) != fsStyle)
    {
        const NSRect content = [win contentRectForFrameRect:win.frame];
        win.styleMask = fsStyle;
        [win setFrame:[win frameRectForContentRect:content] display:NO];
        [win makeFirstResponder:w->view];
    }
    w->transitioning = true;
    [win toggleFullScreen:nil];
}

static void ExitFullScreenNow(BridgeWindow *w)
{
    if (!w->window)
        return;
    w->transitioning = true;
    [w->window toggleFullScreen:nil];
}

// the animation has ended (or failed): take up a request made meanwhile, or restore the windowed frame and style
static void FullScreenSettled(BridgeWindow *w)
{
    w->transitioning = false;
    w->fullScreen.store(w->inFullScreen ? 1 : 0, std::memory_order_relaxed);
    const int pending = w->pendingFullScreen;
    w->pendingFullScreen = -1;
    if (!w->inFullScreen)
    {
        NSWindow *win = w->window;
        const NSWindowStyleMask style = WindowedStyleMask(w->wantBorderless, w->wantResizable);
        NSRect content = w->haveWindowedFrame ? w->windowedContent : [win contentRectForFrameRect:win.frame];
        if (win.styleMask != style)
            win.styleMask = style;
        [win setFrame:[win frameRectForContentRect:content] display:YES];
        [win makeFirstResponder:w->view];
        w->haveWindowedFrame = false;
    }
    ApplyContentGravity(w);
    EmitFullScreen(w, w->inFullScreen);
    if (pending >= 0 && (pending != 0) != w->inFullScreen)
    {
        // (not from inside AppKit's completion of the last transition)
        dispatch_async(dispatch_get_main_queue(), ^{
          if (!w->window || w->transitioning || (pending != 0) == w->inFullScreen)
              return;
          if (pending)
              EnterFullScreenNow(w);
          else
              ExitFullScreenNow(w);
        });
    }
}

// full screen the engine asked for while AppKit could not do it (the app in the background, the window minimized):
// tried again when that changes
static void RetryFullScreen(BridgeWindow *w)
{
    dispatch_async(dispatch_get_main_queue(), ^{
      NSWindow *win = w->window;
      if (win && win.visible && !win.miniaturized && w->wantFullScreen && !w->inFullScreen && !w->transitioning)
          EnterFullScreenNow(w);
    });
}

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
- (void)windowDidBecomeKey:(NSNotification *)n
{
    RefreshWindowState(self.owner);
}
- (void)windowDidResignKey:(NSNotification *)n
{
    RefreshWindowState(self.owner);
}
- (void)windowDidMiniaturize:(NSNotification *)n
{
    RefreshWindowState(self.owner);
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_MINIMIZE;
    ev.window = self.owner;
    ev.userData = self.owner->userData;
    ev.value = 1;
    Emit(ev);
}
- (void)windowDidDeminiaturize:(NSNotification *)n
{
    RefreshWindowState(self.owner);
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_MINIMIZE;
    ev.window = self.owner;
    ev.userData = self.owner->userData;
    ev.value = 0;
    Emit(ev);
    RetryFullScreen(self.owner);
}
- (void)windowDidChangeBackingProperties:(NSNotification *)n
{
    [self windowDidResize:n];
}
- (void)windowWillEnterFullScreen:(NSNotification *)n
{
    self.owner->transitioning = true;
    self.owner->inFullScreen = true;
    self.owner->fullScreen.store(1, std::memory_order_relaxed);
}
- (void)windowDidEnterFullScreen:(NSNotification *)n
{
    self.owner->inFullScreen = true;
    FullScreenSettled(self.owner);
}
- (void)windowDidFailToEnterFullScreen:(NSWindow *)window
{
    self.owner->inFullScreen = false;
    FullScreenSettled(self.owner);
}
- (void)windowWillExitFullScreen:(NSNotification *)n
{
    self.owner->transitioning = true;
}
- (void)windowDidExitFullScreen:(NSNotification *)n
{
    BridgeWindow *w = self.owner;
    w->inFullScreen = false;
    if (w->wantFullScreen && w->pendingFullScreen < 0)
    {
        // the player left the engine's full screen (green button, Ctrl-Cmd-F): the game stays in a window that fits
        // the screen, of the shape the game draws for (the screen's when it is stretched, else its own), until the
        // engine asks for full screen again (vid_restart) or the player goes back to it
        w->wantFullScreen = false;
        w->wantBorderless = false;
        w->wantResizable = true;
        NSScreen *screen = w->window.screen ? w->window.screen : [NSScreen mainScreen];
        NSRect visible = NSInsetRect(screen.visibleFrame, 24, 24);
        const NSRect cur = [w->window contentRectForFrameRect:w->window.frame];
        const CGFloat aspect = w->fillScaling && screen.frame.size.height > 0 ? screen.frame.size.width / screen.frame.size.height
                               : w->logicalW > 0 && w->logicalH > 0           ? (CGFloat)w->logicalW / w->logicalH
                               : cur.size.height > 0                          ? cur.size.width / cur.size.height
                                                                              : 16.0 / 9.0;
        const CGFloat titleBar = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 100, 100)
                                                         styleMask:WindowedStyleMask(false, true)].size.height - 100;
        CGFloat cw = visible.size.width, ch = visible.size.height - titleBar;
        if (cw / ch > aspect)
            cw = ch * aspect;
        else
            ch = cw / aspect;
        w->windowedContent = NSMakeRect(NSMidX(visible) - cw / 2, NSMidY(visible) - (ch + titleBar) / 2, cw, ch);
        w->haveWindowedFrame = true;
    }
    FullScreenSettled(w);
}
- (void)windowDidFailToExitFullScreen:(NSWindow *)window
{
    self.owner->inFullScreen = true;
    FullScreenSettled(self.owner);
}
@end

// ------------------------------------------------------------------------------------------------ app

@interface BO1AppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation BO1AppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    // a logout, restart or shutdown ends the process now (cancelling it would stop the user's logout); Cmd-Q and the
    // Dock's Quit become a quit request that the engine handles on its own
    NSAppleEventDescriptor *ae = [[NSAppleEventManager sharedAppleEventManager] currentAppleEvent];
    NSAppleEventDescriptor *why = ae ? [ae attributeDescriptorForKeyword:'why?'] : nil;
    if (why)
    {
        const OSType reason = why.typeCodeValue;
        if (reason == 'logo' || reason == 'rlgo' || reason == 'rrst' || reason == 'rest' || reason == 'rsdn' ||
            reason == 'shut')
        {
            // NSTerminateNow would call exit(), running the static destructors while the render, sound and worker
            // threads still use those objects; Windows simply ends the process at logoff, and so does this
            // (TerminateProcess in kernel32_misc.cpp does the same)
            fflush(nullptr);
            _Exit(0);
        }
    }
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_QUIT;
    Emit(ev);
    return NSTerminateCancel; // the engine shuts down on its own
}
- (void)applicationDidBecomeActive:(NSNotification *)n
{
    RefreshAllWindowState();
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_ACTIVATE;
    ev.value = 1;
    Emit(ev);
    for (BridgeWindow *w : g_bridgeWindows)
        RetryFullScreen(w);
}
- (void)applicationDidResignActive:(NSNotification *)n
{
    RefreshAllWindowState();
    // keys held when the app loses focus never get their key-up here: release them (Windows does the same)
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
    // the system's full-screen command (and its Ctrl-Cmd-F / Globe-F shortcuts) for the game window
    NSMenuItem *fullScreen = [windowMenu addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fullScreen.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
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
        // a game: no App Nap and no timer coalescing while it runs, which would stretch the 1 ms sleeps and timed
        // waits its threads pace themselves with (the system may still sleep when idle)
        static id<NSObject> activity;
        activity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
                              reason:@"Game"];
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
            // -[NSApplication sendEvent:] swallows key-ups while Command is held: the letter of a Cmd chord would
            // stay down in the engine (W after Cmd-W keeps running)
            if (e.type == NSEventTypeKeyUp && (e.modifierFlags & NSEventModifierFlagCommand) && NSApp.keyWindow)
                [NSApp.keyWindow sendEvent:e];
        }
        [NSApp updateWindows];
        RefreshAllWindowState();
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
        // no window state kept by macOS (~/Library/Saved Application State): the game keeps its own window settings,
        // in its game folder
        win.restorable = NO;
        win.title = EngineString(title ? title : "BO1 Zombies");
        win.acceptsMouseMovedEvents = YES;
        // the green button / Ctrl-Cmd-F take the window to a full-screen Space (the drawable keeps its size)
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary | NSWindowCollectionBehaviorManaged;
        win.backgroundColor = [NSColor blackColor];
        bw->wantBorderless = borderless != 0;
        bw->wantResizable = resizable != 0;
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
        ApplyContentGravity(bw);
        if (x == (int)0x80000000 || y == (int)0x80000000)
            [win center];
        g_bridgeWindows.push_back(bw);
        RefreshWindowState(bw);
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
    g_bridgeWindows.erase(std::remove(g_bridgeWindows.begin(), g_bridgeWindows.end(), w), g_bridgeWindows.end());
    // The struct itself stays allocated as a dead window (no NSWindow, state cleared) rather than being freed:
    // other threads read the cached state through the pointer without waiting for the main thread, and one that
    // fetched it just before the Win32 layer detached it may still ask whether it is visible or minimized.
    // A window is only destroyed on vid_restart and quit, so this costs a few bytes each time.
    w->visible.store(0, std::memory_order_relaxed);
    w->minimized.store(0, std::memory_order_relaxed);
    w->keyWindow.store(0, std::memory_order_relaxed);
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
        // full screen asked for before the window was on screen (the engine creates its window hidden)
        if (w->wantFullScreen && !w->inFullScreen && !w->transitioning)
            dispatch_async(dispatch_get_main_queue(), ^{
              if (w->window && w->wantFullScreen && !w->inFullScreen && !w->transitioning)
                  EnterFullScreenNow(w);
            });
    }
    else
        [w->window orderOut:nil];
    RefreshWindowState(w);
}
static void Main_MinimizeWindow(BridgeWindow *w)
{
    if (w)
    {
        [w->window miniaturize:nil];
        RefreshWindowState(w);
    }
}
static void Main_RestoreWindow(BridgeWindow *w)
{
    if (w && w->window.miniaturized)
        [w->window deminiaturize:nil];
    RefreshWindowState(w);
}
static int Main_IsMinimized(BridgeWindow *w) { return w && w->window.miniaturized ? 1 : 0; }
static int Main_IsVisible(BridgeWindow *w) { return w && w->window.visible ? 1 : 0; }

static void Main_SetWindowStyle(BridgeWindow *w, int borderless, int resizable)
{
    if (!w)
        return;
    w->wantBorderless = borderless != 0;
    w->wantResizable = resizable != 0;
    if (w->inFullScreen || w->transitioning)
        return; // the style the window gets back when it leaves full screen
    NSRect content = [w->window contentRectForFrameRect:w->window.frame];
    NSWindowStyleMask style = WindowedStyleMask(borderless != 0, resizable != 0);
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
    if (w->inFullScreen || w->transitioning)
    {
        // AppKit owns the frame of a full-screen window: the one asked for is the window's once it leaves
        w->windowedContent = content;
        w->haveWindowedFrame = true;
        return;
    }
    NSRect frame = [w->window frameRectForContentRect:content];
    [w->window setFrame:frame display:YES];
}

static void Main_SetFullScreen(BridgeWindow *w, int fullScreen)
{
    if (!w || !w->window)
        return;
    w->wantFullScreen = fullScreen != 0;
    if (w->transitioning)
    {
        w->pendingFullScreen = fullScreen ? 1 : 0;
        return;
    }
    if (fullScreen && !w->inFullScreen)
        EnterFullScreenNow(w);
    else if (!fullScreen && w->inFullScreen)
        ExitFullScreenNow(w);
}

static void Main_SetContentScaling(BridgeWindow *w, int logicalW, int logicalH, int fill)
{
    if (!w)
        return;
    w->fillScaling = fill != 0;
    w->logicalW = logicalW;
    w->logicalH = logicalH;
    ApplyContentGravity(w);
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
        w->window.title = EngineString(title);
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
    // a capture queued by the render thread can arrive after the app lost focus (which released it): ignore it,
    // or the pointer would stay detached over the other app
    if (captured && ![NSApp isActive])
        return;
    g_mouseCaptured = captured != 0;
    if (captured)
    {
        // clicks go to whatever is under the (hidden, frozen) cursor: put it in the middle of the game window first
        NSWindow *win = NSApp.keyWindow ? NSApp.keyWindow : (g_bridgeWindows.empty() ? nil : g_bridgeWindows.front()->window);
        NSArray<NSScreen *> *screens = [NSScreen screens];
        if (win && screens.count)
        {
            const NSRect content = [win contentRectForFrameRect:win.frame];
            const CGFloat primaryHeight = screens[0].frame.size.height;
            const CGPoint centre = CGPointMake(NSMidX(content), primaryHeight - NSMidY(content));
            NoteCursorWarp(centre);
            CGWarpMouseCursorPosition(centre);
        }
    }
    CGAssociateMouseAndMouseCursorPosition(captured ? false : true);
}

static void Main_WarpCursor(int x, int y)
{
    CGPoint p = CGPointMake(x * g_unitScale, y * g_unitScale);
    NoteCursorWarp(p);
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
    if (index < 0 || index >= (int)screens.count || maxModes <= 0)
        return 0;
    NSScreen *s = screens[index];
    // The game draws at its own resolution whatever the display runs at (the layer scales the drawable to the screen;
    // full screen never switches the display's mode), so these are the resolutions it can draw at: the display's own
    // modes (System Settings' "looks like" sizes, in points and in the pixels macOS renders them at), the usual
    // sizes, and the screen's size in points and in pixels, up to the screen's pixels (the backing store of the mode
    // it runs, or the panel's native resolution when that is larger).
    const BridgeRect full = ToWinRect(s.frame);
    const int unitW = full.right - full.left, unitH = full.bottom - full.top;
    const CGFloat scale = s.backingScaleFactor > 0 ? s.backingScaleFactor : 1.0;
    int maxW = (int)lround(s.frame.size.width * scale), maxH = (int)lround(s.frame.size.height * scale);
    int hz = 60;
    if (@available(macOS 12.0, *))
        hz = s.maximumFramesPerSecond > 0 ? (int)s.maximumFramesPerSecond : 60;
    std::vector<std::pair<int, int>> sizes;
    const CGDirectDisplayID display = [s.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    NSDictionary *options = @{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes : @YES};
    if (CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, (__bridge CFDictionaryRef)options))
    {
        constexpr uint32_t kNativeModeFlag = 0x02000000; // kDisplayModeNativeFlag (IOGraphicsTypes.h)
        for (CFIndex i = 0; i < CFArrayGetCount(modes); ++i)
        {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            if (!CGDisplayModeIsUsableForDesktopGUI(m))
                continue;
            const int pw = (int)CGDisplayModeGetPixelWidth(m), ph = (int)CGDisplayModeGetPixelHeight(m);
            if ((CGDisplayModeGetIOFlags(m) & kNativeModeFlag) && pw * ph > maxW * maxH)
            {
                maxW = pw;
                maxH = ph;
            }
            sizes.push_back({(int)CGDisplayModeGetWidth(m), (int)CGDisplayModeGetHeight(m)});
            sizes.push_back({pw, ph});
        }
        CFRelease(modes);
    }
    static const int common[][2] = {{640, 480},   {720, 480},   {800, 600},   {1024, 640},  {1024, 768},  {1152, 864},
                                    {1280, 720},  {1280, 800},  {1280, 960},  {1280, 1024}, {1366, 768},  {1440, 900},
                                    {1600, 900},  {1600, 1000}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                                    {2048, 1152}, {2048, 1280}, {2304, 1440}, {2560, 1440}, {2560, 1600}, {2880, 1800},
                                    {3024, 1964}, {3200, 1800}, {3456, 2234}, {3840, 2160}, {3840, 2400}, {5120, 2880}};
    for (auto &c : common)
        sizes.push_back({c[0], c[1]});
    sizes.push_back({unitW, unitH});
    sizes.push_back({maxW, maxH});
    sizes.erase(std::remove_if(sizes.begin(), sizes.end(),
                               [&](const std::pair<int, int> &z) {
                                   return z.first < 640 || z.second < 480 || z.first > maxW || z.second > maxH;
                               }),
                sizes.end());
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    // (too many for the caller: the smallest go)
    const size_t first = sizes.size() > (size_t)maxModes ? sizes.size() - (size_t)maxModes : 0;
    int n = 0;
    for (size_t i = first; i < sizes.size(); ++i, ++n)
    {
        widths[n] = sizes[i].first;
        heights[n] = sizes[i].second;
        rates[n] = hz;
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
            [pb setString:EngineString(utf8) forType:NSPasteboardTypeString];
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
        NSString *t = EngineString(title);
        NSString *m = EngineString(text);
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
        NSString *s = EngineString(url);
        if (!s.length)
            return;
        // a path (a folder to show in Finder) or a URL
        NSURL *u = [s hasPrefix:@"/"] ? [NSURL fileURLWithPath:s] : [NSURL URLWithString:s];
        if (!u)
            u = [NSURL fileURLWithPath:s];
        if (u)
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
        strncpy(path, p ? p.fileSystemRepresentation : ".", sizeof(path) - 1);
    }
    return path;
}

// ---- the bridge entry points that use AppKit: on the main thread, whichever thread calls them
int Bridge_IsAppActive(void)
{
    if (!w32::IsMainThread())
        return g_appActiveCache.load(std::memory_order_relaxed);
    const int active = Main_IsAppActive();
    g_appActiveCache.store(active, std::memory_order_relaxed);
    return active;
}
void Bridge_ActivateApp(void) { OnMain([&] { Main_ActivateApp(); }); }
BridgeWindow *Bridge_CreateWindow(const char *title, int x, int y, int clientW, int clientH, int borderless, int resizable, void *userData)
{
    return OnMain([&] { return Main_CreateWindow(title, x, y, clientW, clientH, borderless, resizable, userData); });
}
void Bridge_DestroyWindow(BridgeWindow *w) { OnMain([&] { Main_DestroyWindow(w); }); }
void Bridge_ShowWindow(BridgeWindow *w, int show) { OnMain([&] { Main_ShowWindow(w, show); }); }
void Bridge_MinimizeWindow(BridgeWindow *w) { OnMain([&] { Main_MinimizeWindow(w); }); }
void Bridge_RestoreWindow(BridgeWindow *w) { OnMain([&] { Main_RestoreWindow(w); }); }
// other threads read the state the main thread keeps current (RefreshWindowState) instead of waiting for it
int Bridge_IsMinimized(BridgeWindow *w)
{
    if (!w32::IsMainThread())
        return w ? w->minimized.load(std::memory_order_relaxed) : 0;
    RefreshWindowState(w);
    return Main_IsMinimized(w);
}
int Bridge_IsVisible(BridgeWindow *w)
{
    if (!w32::IsMainThread())
        return w ? w->visible.load(std::memory_order_relaxed) : 0;
    RefreshWindowState(w);
    return Main_IsVisible(w);
}
void Bridge_SetWindowStyle(BridgeWindow *w, int borderless, int resizable) { OnMain([&] { Main_SetWindowStyle(w, borderless, resizable); }); }
void Bridge_SetClientRect(BridgeWindow *w, int x, int y, int clientW, int clientH) { OnMain([&] { Main_SetClientRect(w, x, y, clientW, clientH); }); }
void Bridge_GetClientRect(BridgeWindow *w, BridgeRect *r) { OnMain([&] { Main_GetClientRect(w, r); }); }
void Bridge_GetFrameRect(BridgeWindow *w, BridgeRect *r) { OnMain([&] { Main_GetFrameRect(w, r); }); }
void Bridge_SetTitle(BridgeWindow *w, const char *title) { OnMain([&] { Main_SetTitle(w, title); }); }
void Bridge_FocusWindow(BridgeWindow *w) { OnMain([&] { Main_FocusWindow(w); }); }
int Bridge_IsKeyWindow(BridgeWindow *w)
{
    if (!w32::IsMainThread())
        return w && w->keyWindow.load(std::memory_order_relaxed) && g_appActiveCache.load(std::memory_order_relaxed) ? 1 : 0;
    RefreshWindowState(w);
    return Main_IsKeyWindow(w);
}
void Bridge_SetCoversScreen(BridgeWindow *w, int covers) { OnMain([&] { Main_SetCoversScreen(w, covers); }); }
void Bridge_SetFullScreen(BridgeWindow *w, int fullScreen) { OnMain([&] { Main_SetFullScreen(w, fullScreen); }); }
int Bridge_IsFullScreen(BridgeWindow *w) { return w ? w->fullScreen.load(std::memory_order_relaxed) : 0; }
void Bridge_SetContentScaling(BridgeWindow *w, int logicalW, int logicalH, int fill)
{
    OnMain([&] { Main_SetContentScaling(w, logicalW, logicalH, fill); });
}
void *Bridge_GetMetalLayer(BridgeWindow *w) { return OnMain([&] { return Main_GetMetalLayer(w); }); }
double Bridge_GetBackingScale(BridgeWindow *w) { return OnMain([&] { return Main_GetBackingScale(w); }); }
// cursor changes from other threads (the render thread shows and hides the cursor every frame) are queued for the
// main thread without waiting; later calls from the same thread run after them
void Bridge_SetCursorVisible(int visible)
{
    w32::PostToMainThread([visible] {
        @autoreleasepool
        {
            Main_SetCursorVisible(visible);
        }
    });
}
void Bridge_SetMouseCaptured(int captured)
{
    w32::PostToMainThread([captured] { Main_SetMouseCaptured(captured); });
}
void Bridge_WarpCursor(int x, int y)
{
    w32::PostToMainThread([x, y] { Main_WarpCursor(x, y); });
}
void Bridge_GetCursorPos(int *x, int *y) { OnMain([&] { Main_GetCursorPos(x, y); }); }
void Bridge_SetArrowCursor(int wait)
{
    w32::PostToMainThread([wait] {
        @autoreleasepool
        {
            Main_SetArrowCursor(wait);
        }
    });
}
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
