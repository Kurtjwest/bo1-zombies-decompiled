// XInput (include/xinput.h) on Apple's GameController framework: Xbox, PlayStation, Switch Pro and MFi controllers
// show up as XInput user indices 0-3, with rumble through Core Haptics where the controller supports it.
//
// Objective-C++ without the Win32 headers (Cocoa's BOOL is not Win32's), so the XInput structures are declared
// here again with the same layout.

#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace
{
struct XGamepad
{
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};
struct XState
{
    uint32_t dwPacketNumber;
    XGamepad Gamepad;
};
struct XVibration
{
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
};
struct XCapabilities
{
    uint8_t Type;
    uint8_t SubType;
    uint16_t Flags;
    XGamepad Gamepad;
    XVibration Vibration;
};
struct XBattery
{
    uint8_t BatteryType;
    uint8_t BatteryLevel;
};
static_assert(sizeof(XGamepad) == 12 && sizeof(XState) == 16 && sizeof(XCapabilities) == 20, "XInput layout");

enum : uint32_t
{
    kErrorSuccess = 0,
    kErrorBadArguments = 160,
    kErrorDeviceNotConnected = 1167,
    kErrorEmpty = 4306,
};
enum : uint16_t
{
    kDpadUp = 0x0001,
    kDpadDown = 0x0002,
    kDpadLeft = 0x0004,
    kDpadRight = 0x0008,
    kStart = 0x0010,
    kBack = 0x0020,
    kLeftThumb = 0x0040,
    kRightThumb = 0x0080,
    kLeftShoulder = 0x0100,
    kRightShoulder = 0x0200,
    kA = 0x1000,
    kB = 0x2000,
    kX = 0x4000,
    kY = 0x8000,
};

constexpr int kMaxUsers = 4;

// One rumble motor: a continuous haptic pattern whose intensity follows the XInput motor speed.
struct Motor
{
    CHHapticEngine *engine = nil;
    id<CHHapticAdvancedPatternPlayer> player = nil;
    float current = 0.0f;
    bool failed = false;
};

struct Slot
{
    GCController *controller = nil;
    XGamepad last = {};
    uint32_t packet = 0;
    Motor motors[2]; // 0 = left (low frequency), 1 = right (high frequency)
};

Slot g_slots[kMaxUsers];
bool g_enabled = true;
bool g_observing = false;
double g_lastRefresh = -1.0;
NSLock *g_lock = nil;

double Now() { return [NSProcessInfo processInfo].systemUptime; }

void StopMotor(Motor &m)
{
    if (m.player)
    {
        NSError *err = nil;
        [m.player stopAtTime:CHHapticTimeImmediate error:&err];
    }
    m.player = nil;
    if (m.engine)
        [m.engine stopWithCompletionHandler:nil];
    m.engine = nil;
    m.current = 0.0f;
    m.failed = false;
}

void ReleaseSlot(Slot &s)
{
    for (Motor &m : s.motors)
        StopMotor(m);
    s.controller = nil;
    s.last = {};
    ++s.packet;
}

void AssignControllers()
{
    NSArray<GCController *> *all = [GCController controllers];
    // drop controllers that went away
    for (Slot &s : g_slots)
        if (s.controller && ![all containsObject:s.controller])
            ReleaseSlot(s);
    // new controllers take the first free user index
    for (GCController *c in all)
    {
        if (!c.extendedGamepad)
            continue;
        bool known = false;
        for (Slot &s : g_slots)
            if (s.controller == c)
                known = true;
        if (known)
            continue;
        for (int i = 0; i < kMaxUsers; ++i)
        {
            if (!g_slots[i].controller)
            {
                g_slots[i].controller = c;
                g_slots[i].last = {};
                ++g_slots[i].packet;
                c.playerIndex = (GCControllerPlayerIndex)i;
                break;
            }
        }
    }
}

void EnsureObserving()
{
    if (g_observing)
        return;
    g_observing = true;
    g_lock = [[NSLock alloc] init];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification *) {
        [g_lock lock];
        g_lastRefresh = -1.0;
        [g_lock unlock];
    }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification *) {
        [g_lock lock];
        g_lastRefresh = -1.0;
        [g_lock unlock];
    }];
    // controllers keep reporting while another app is in front only if asked to; the engine decides itself what
    // to do with input while it is inactive (as with XInput on Windows)
    if (@available(macOS 11.3, *))
        GCController.shouldMonitorBackgroundEvents = YES;
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
}

void Refresh()
{
    EnsureObserving();
    double now = Now();
    if (g_lastRefresh >= 0.0 && now - g_lastRefresh < 0.5)
        return;
    g_lastRefresh = now;
    AssignControllers();
}

int16_t Axis(float v)
{
    if (v > 1.0f)
        v = 1.0f;
    if (v < -1.0f)
        v = -1.0f;
    return (int16_t)lrintf(v >= 0.0f ? v * 32767.0f : v * 32768.0f);
}

uint8_t Trigger(float v)
{
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    return (uint8_t)lrintf(v * 255.0f);
}

void ReadPad(GCExtendedGamepad *g, XGamepad &out)
{
    out = {};
    uint16_t b = 0;
    if (g.buttonA.pressed)
        b |= kA;
    if (g.buttonB.pressed)
        b |= kB;
    if (g.buttonX.pressed)
        b |= kX;
    if (g.buttonY.pressed)
        b |= kY;
    if (g.leftShoulder.pressed)
        b |= kLeftShoulder;
    if (g.rightShoulder.pressed)
        b |= kRightShoulder;
    if (g.dpad.up.pressed)
        b |= kDpadUp;
    if (g.dpad.down.pressed)
        b |= kDpadDown;
    if (g.dpad.left.pressed)
        b |= kDpadLeft;
    if (g.dpad.right.pressed)
        b |= kDpadRight;
    if (g.buttonMenu.pressed)
        b |= kStart;
    if (g.buttonOptions && g.buttonOptions.pressed)
        b |= kBack;
    if (g.leftThumbstickButton && g.leftThumbstickButton.pressed)
        b |= kLeftThumb;
    if (g.rightThumbstickButton && g.rightThumbstickButton.pressed)
        b |= kRightThumb;
    out.wButtons = b;
    out.bLeftTrigger = Trigger(g.leftTrigger.value);
    out.bRightTrigger = Trigger(g.rightTrigger.value);
    out.sThumbLX = Axis(g.leftThumbstick.xAxis.value);
    out.sThumbLY = Axis(g.leftThumbstick.yAxis.value);
    out.sThumbRX = Axis(g.rightThumbstick.xAxis.value);
    out.sThumbRY = Axis(g.rightThumbstick.yAxis.value);
}

bool StartMotor(GCController *c, Motor &m, int index)
{
    if (m.failed)
        return false;
    if (m.player)
        return true;
    if (@available(macOS 11.0, *))
    {
        GCDeviceHaptics *haptics = c.haptics;
        if (!haptics)
        {
            m.failed = true;
            return false;
        }
        NSSet<GCHapticsLocality> *localities = haptics.supportedLocalities;
        GCHapticsLocality loc = index == 0 ? GCHapticsLocalityLeftHandle : GCHapticsLocalityRightHandle;
        if (![localities containsObject:loc])
        {
            // single-actuator controllers: only the left (strong) motor drives the default locality
            if (index != 0)
            {
                m.failed = true;
                return false;
            }
            loc = GCHapticsLocalityDefault;
        }
        m.engine = [haptics createEngineWithLocality:loc];
        if (!m.engine)
        {
            m.failed = true;
            return false;
        }
        m.engine.playsHapticsOnly = YES;
        m.engine.autoShutdownEnabled = NO;
        NSError *err = nil;
        if (![m.engine startAndReturnError:&err])
        {
            m.engine = nil;
            m.failed = true;
            return false;
        }
        CHHapticEventParameter *intensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
        CHHapticEventParameter *sharpness = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                                                           value:index == 0 ? 0.2f : 0.8f];
        // a long continuous event; restarted if it ever runs out
        CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                         parameters:@[ intensity, sharpness ]
                                                       relativeTime:0
                                                           duration:3600.0];
        CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[ ev ] parameters:@[] error:&err];
        if (!pattern)
        {
            StopMotor(m);
            m.failed = true;
            return false;
        }
        m.player = [m.engine createAdvancedPlayerWithPattern:pattern error:&err];
        if (!m.player)
        {
            StopMotor(m);
            m.failed = true;
            return false;
        }
        m.player.loopEnabled = YES;
        m.current = -1.0f; // force the first intensity update
        return true;
    }
    m.failed = true;
    return false;
}

void SetMotor(GCController *c, Motor &m, int index, float level)
{
    if (level <= 0.001f)
    {
        if (m.player && m.current > 0.0f)
        {
            NSError *err = nil;
            [m.player pauseAtTime:CHHapticTimeImmediate error:&err];
            m.current = 0.0f;
        }
        return;
    }
    if (!StartMotor(c, m, index))
        return;
    if (fabsf(level - m.current) < 0.02f)
        return;
    NSError *err = nil;
    CHHapticDynamicParameter *p = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                                                                                  value:level
                                                                           relativeTime:0];
    [m.player sendParameters:@[ p ] atTime:CHHapticTimeImmediate error:&err];
    if (m.current <= 0.0f)
    {
        if (m.current < 0.0f)
            [m.player startAtTime:CHHapticTimeImmediate error:&err];
        else
            [m.player resumeAtTime:CHHapticTimeImmediate error:&err];
    }
    m.current = level;
}
} // namespace

extern "C" {

uint32_t XInputGetState(uint32_t user, XState *state)
{
    if (user >= kMaxUsers || !state)
        return kErrorBadArguments;
    @autoreleasepool
    {
        Refresh();
        Slot &s = g_slots[user];
        if (!s.controller || !s.controller.extendedGamepad)
        {
            memset(state, 0, sizeof(*state));
            return kErrorDeviceNotConnected;
        }
        XGamepad pad = {};
        if (g_enabled)
            ReadPad(s.controller.extendedGamepad, pad);
        if (memcmp(&pad, &s.last, sizeof(pad)) != 0)
        {
            s.last = pad;
            ++s.packet;
        }
        state->dwPacketNumber = s.packet;
        state->Gamepad = pad;
        return kErrorSuccess;
    }
}

uint32_t XInputSetState(uint32_t user, XVibration *vib)
{
    if (user >= kMaxUsers || !vib)
        return kErrorBadArguments;
    @autoreleasepool
    {
        Refresh();
        Slot &s = g_slots[user];
        if (!s.controller)
            return kErrorDeviceNotConnected;
        const float left = g_enabled ? vib->wLeftMotorSpeed / 65535.0f : 0.0f;
        const float right = g_enabled ? vib->wRightMotorSpeed / 65535.0f : 0.0f;
        SetMotor(s.controller, s.motors[0], 0, left);
        SetMotor(s.controller, s.motors[1], 1, right);
        return kErrorSuccess;
    }
}

uint32_t XInputGetCapabilities(uint32_t user, uint32_t flags, XCapabilities *caps)
{
    (void)flags;
    if (user >= kMaxUsers || !caps)
        return kErrorBadArguments;
    @autoreleasepool
    {
        Refresh();
        Slot &s = g_slots[user];
        if (!s.controller || !s.controller.extendedGamepad)
            return kErrorDeviceNotConnected;
        memset(caps, 0, sizeof(*caps));
        caps->Type = 0x01;    // XINPUT_DEVTYPE_GAMEPAD
        caps->SubType = 0x01; // XINPUT_DEVSUBTYPE_GAMEPAD
        caps->Flags = 0;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = 0xFF;
        caps->Gamepad.bRightTrigger = 0xFF;
        caps->Gamepad.sThumbLX = (int16_t)0xFFC0;
        caps->Gamepad.sThumbLY = (int16_t)0xFFC0;
        caps->Gamepad.sThumbRX = (int16_t)0xFFC0;
        caps->Gamepad.sThumbRY = (int16_t)0xFFC0;
        bool rumble = false;
        if (@available(macOS 11.0, *))
            rumble = s.controller.haptics != nil;
        caps->Vibration.wLeftMotorSpeed = rumble ? 0xFFFF : 0;
        caps->Vibration.wRightMotorSpeed = rumble ? 0xFFFF : 0;
        return kErrorSuccess;
    }
}

uint32_t XInputGetBatteryInformation(uint32_t user, uint8_t devType, XBattery *info)
{
    (void)devType;
    if (user >= kMaxUsers || !info)
        return kErrorBadArguments;
    @autoreleasepool
    {
        Refresh();
        Slot &s = g_slots[user];
        if (!s.controller)
        {
            info->BatteryType = 0x00; // BATTERY_TYPE_DISCONNECTED
            info->BatteryLevel = 0;
            return kErrorDeviceNotConnected;
        }
        info->BatteryType = 0x01; // BATTERY_TYPE_WIRED
        info->BatteryLevel = 0x03;
        if (@available(macOS 11.0, *))
        {
            GCDeviceBattery *bat = s.controller.battery;
            if (bat && bat.batteryState != GCDeviceBatteryStateUnknown && bat.batteryState != GCDeviceBatteryStateCharging &&
                bat.batteryState != GCDeviceBatteryStateFull)
            {
                info->BatteryType = 0x02; // BATTERY_TYPE_ALKALINE (battery powered)
                float l = bat.batteryLevel;
                info->BatteryLevel = l < 0.1f ? 0 : l < 0.4f ? 1 : l < 0.7f ? 2 : 3;
            }
        }
        return kErrorSuccess;
    }
}

uint32_t XInputGetKeystroke(uint32_t user, uint32_t reserved, void *keystroke)
{
    (void)reserved;
    (void)keystroke;
    if (user >= kMaxUsers && user != 0xFF)
        return kErrorBadArguments;
    return kErrorEmpty;
}

void XInputEnable(int enable)
{
    @autoreleasepool
    {
        g_enabled = enable != 0;
        if (!g_enabled)
            for (Slot &s : g_slots)
                for (int i = 0; i < 2; ++i)
                    if (s.controller)
                        SetMotor(s.controller, s.motors[i], i, 0.0f);
    }
}

} // extern "C"
