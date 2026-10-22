// XInput (win32compat/include/xinput.h) on SDL2's game controller API for the Linux build: Xbox, PlayStation, Switch
// Pro, Steam and the other controllers SDL knows show up as XInput user indices 0-3, with rumble where the controller
// has it. The Linux counterpart of macos/gamepad_mac.mm.
//
// A controller SDL has no layout for is a plain joystick to it and is left out: the log names it, and a layout line
// for it (SDL's format, as made by the SDL2 Gamepad Tool or found in the community's gamecontrollerdb.txt) in
// gamecontrollerdb.txt in the game folder, or in the SDL_GAMECONTROLLERCONFIG environment variable, makes it usable.
//
// Written without the Win32 headers, like the macOS file: the XInput structures are declared here again with the
// same layout.

#include "../win32compat/w32_platform.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

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

struct Slot
{
    SDL_GameController *controller = nullptr;
    SDL_JoystickID id = -1;
    XGamepad last = {};
    uint32_t packet = 0;
    uint16_t rumble[2] = {0, 0};
    Uint32 rumbleSent = 0; // SDL_GetTicks of the last rumble request (they run for a limited time)
};

std::mutex g_lock;
Slot g_slots[kMaxUsers];
bool g_enabled = true;
bool g_ready = false, g_failed = false;
Uint32 g_lastRefresh = 0;

bool Ready()
{
    if (g_ready)
        return true;
    if (g_failed)
        return false;
    // (brought up by the bridge on the main thread, whose event loop pumps the controllers' state)
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER) && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
    {
        fprintf(stderr, "[input] no controller support: %s\n", SDL_GetError());
        g_failed = true;
        return false;
    }
    // layouts of the user's own for controllers SDL does not know (see the top)
    const char *dir = w32::GameDirectory();
    if (dir && *dir)
    {
        const std::string db = std::string(dir) + "/gamecontrollerdb.txt";
        if (FILE *f = fopen(db.c_str(), "rb"))
        {
            fclose(f);
            const int n = SDL_GameControllerAddMappingsFromFile(db.c_str());
            if (n < 0)
                fprintf(stderr, "[input] %s: %s\n", db.c_str(), SDL_GetError());
            else
                fprintf(stderr, "[input] %d controller layouts from %s\n", n, db.c_str());
        }
    }
    g_ready = true;
    return true;
}

// the log's line about each controller or joystick, once
std::set<SDL_JoystickID> g_logged;
bool g_loggedNone = false;

void LogDevice(int index, SDL_JoystickID id)
{
    if (!g_logged.insert(id).second)
        return;
    char guid[64] = "";
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
    if (SDL_IsGameController(index))
    {
        const char *name = SDL_GameControllerNameForIndex(index);
        fprintf(stderr, "[input] controller: %s\n", name ? name : "?");
    }
    else
    {
        const char *name = SDL_JoystickNameForIndex(index);
        fprintf(stderr,
                "[input] %s (%s): a joystick SDL has no controller layout for, left out (a layout line for it in "
                "gamecontrollerdb.txt in the game folder makes it usable)\n",
                name ? name : "joystick", guid);
    }
}

void ReleaseSlot(Slot &s)
{
    if (s.controller)
        SDL_GameControllerClose(s.controller);
    s.controller = nullptr;
    s.id = -1;
    s.last = {};
    s.rumble[0] = s.rumble[1] = 0;
    ++s.packet;
}

// controllers keep their slot while connected; new ones take the first free slot, in SDL's device order
void AssignControllers()
{
    for (Slot &s : g_slots)
        if (s.controller && !SDL_GameControllerGetAttached(s.controller))
            ReleaseSlot(s);
    const int n = SDL_NumJoysticks();
    if (n <= 0 && !g_loggedNone)
    {
        // (once: a controller plugged in later is logged when it appears)
        g_loggedNone = true;
        fprintf(stderr, "[input] no controllers found%s\n",
                n < 0 ? "" : " (they are read through /dev/input/event*, which the user must be able to read)");
    }
    for (int i = 0; i < n; ++i)
    {
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        LogDevice(i, id);
        if (!SDL_IsGameController(i))
            continue;
        bool have = false;
        for (const Slot &s : g_slots)
            have |= s.controller && s.id == id;
        if (have)
            continue;
        for (Slot &s : g_slots)
        {
            if (s.controller)
                continue;
            s.controller = SDL_GameControllerOpen(i);
            if (s.controller)
            {
                s.id = id;
                s.last = {};
                ++s.packet;
            }
            else
                fprintf(stderr, "[input] cannot open the controller: %s\n", SDL_GetError());
            break;
        }
    }
}

void Refresh()
{
    if (!Ready())
        return;
    const Uint32 now = SDL_GetTicks();
    if (g_lastRefresh && now - g_lastRefresh < 500)
        return;
    g_lastRefresh = now;
    AssignControllers();
}

int16_t Stick(Sint16 v, bool invert)
{
    if (!invert)
        return v;
    // SDL's stick y is down-positive, XInput's up-positive
    const int r = -(int)v;
    return (int16_t)std::clamp(r, -32768, 32767);
}

uint8_t Trigger(Sint16 v) { return (uint8_t)std::clamp((int)v * 255 / 32767, 0, 255); }

void ReadPad(SDL_GameController *g, XGamepad &out)
{
    out = {};
    struct Map
    {
        SDL_GameControllerButton b;
        uint16_t x;
    };
    static const Map kMap[] = {
        {SDL_CONTROLLER_BUTTON_A, kA},
        {SDL_CONTROLLER_BUTTON_B, kB},
        {SDL_CONTROLLER_BUTTON_X, kX},
        {SDL_CONTROLLER_BUTTON_Y, kY},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kLeftShoulder},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kRightShoulder},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, kDpadUp},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDpadDown},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kDpadLeft},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kDpadRight},
        {SDL_CONTROLLER_BUTTON_START, kStart},
        {SDL_CONTROLLER_BUTTON_BACK, kBack},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, kLeftThumb},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kRightThumb},
    };
    uint16_t b = 0;
    for (const Map &m : kMap)
        if (SDL_GameControllerGetButton(g, m.b))
            b |= m.x;
    out.wButtons = b;
    out.bLeftTrigger = Trigger(SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    out.bRightTrigger = Trigger(SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    out.sThumbLX = Stick(SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTX), false);
    out.sThumbLY = Stick(SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY), true);
    out.sThumbRX = Stick(SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_RIGHTX), false);
    out.sThumbRY = Stick(SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_RIGHTY), true);
}

// XInput motors run until told otherwise, SDL's rumble for a duration: requests are renewed while a motor runs
void SendRumble(Slot &s, uint16_t left, uint16_t right)
{
    const Uint32 now = SDL_GetTicks();
    const bool changed = left != s.rumble[0] || right != s.rumble[1];
    const bool running = left || right;
    if (!changed && (!running || now - s.rumbleSent < 500))
        return;
    s.rumble[0] = left;
    s.rumble[1] = right;
    s.rumbleSent = now;
    SDL_GameControllerRumble(s.controller, left, right, running ? 1000 : 0);
}
} // namespace

extern "C" {

uint32_t XInputGetState(uint32_t user, XState *state)
{
    if (user >= kMaxUsers || !state)
        return kErrorBadArguments;
    std::lock_guard<std::mutex> lock(g_lock);
    Refresh();
    Slot &s = g_slots[user];
    if (!s.controller)
    {
        memset(state, 0, sizeof(*state));
        return kErrorDeviceNotConnected;
    }
    XGamepad pad = {};
    if (g_enabled)
        ReadPad(s.controller, pad);
    if (memcmp(&pad, &s.last, sizeof(pad)) != 0)
    {
        s.last = pad;
        ++s.packet;
    }
    state->dwPacketNumber = s.packet;
    state->Gamepad = pad;
    return kErrorSuccess;
}

uint32_t XInputSetState(uint32_t user, XVibration *vib)
{
    if (user >= kMaxUsers || !vib)
        return kErrorBadArguments;
    std::lock_guard<std::mutex> lock(g_lock);
    Refresh();
    Slot &s = g_slots[user];
    if (!s.controller)
        return kErrorDeviceNotConnected;
    SendRumble(s, g_enabled ? vib->wLeftMotorSpeed : 0, g_enabled ? vib->wRightMotorSpeed : 0);
    return kErrorSuccess;
}

uint32_t XInputGetCapabilities(uint32_t user, uint32_t flags, XCapabilities *caps)
{
    (void)flags;
    if (user >= kMaxUsers || !caps)
        return kErrorBadArguments;
    std::lock_guard<std::mutex> lock(g_lock);
    Refresh();
    Slot &s = g_slots[user];
    if (!s.controller)
        return kErrorDeviceNotConnected;
    memset(caps, 0, sizeof(*caps));
    caps->Type = 0x01;    // XINPUT_DEVTYPE_GAMEPAD
    caps->SubType = 0x01; // XINPUT_DEVSUBTYPE_GAMEPAD
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = 0xFF;
    caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = (int16_t)0xFFC0;
    caps->Gamepad.sThumbLY = (int16_t)0xFFC0;
    caps->Gamepad.sThumbRX = (int16_t)0xFFC0;
    caps->Gamepad.sThumbRY = (int16_t)0xFFC0;
    bool rumble = true;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    rumble = SDL_GameControllerHasRumble(s.controller) == SDL_TRUE;
#endif
    caps->Vibration.wLeftMotorSpeed = rumble ? 0xFFFF : 0;
    caps->Vibration.wRightMotorSpeed = rumble ? 0xFFFF : 0;
    return kErrorSuccess;
}

uint32_t XInputGetBatteryInformation(uint32_t user, uint8_t devType, XBattery *info)
{
    (void)devType;
    if (user >= kMaxUsers || !info)
        return kErrorBadArguments;
    std::lock_guard<std::mutex> lock(g_lock);
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
    switch (SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(s.controller)))
    {
    case SDL_JOYSTICK_POWER_EMPTY:
        info->BatteryType = 0x02;
        info->BatteryLevel = 0;
        break;
    case SDL_JOYSTICK_POWER_LOW:
        info->BatteryType = 0x02;
        info->BatteryLevel = 1;
        break;
    case SDL_JOYSTICK_POWER_MEDIUM:
        info->BatteryType = 0x02;
        info->BatteryLevel = 2;
        break;
    case SDL_JOYSTICK_POWER_FULL:
        info->BatteryType = 0x02;
        info->BatteryLevel = 3;
        break;
    default:
        break;
    }
    return kErrorSuccess;
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
    std::lock_guard<std::mutex> lock(g_lock);
    g_enabled = enable != 0;
    if (!g_enabled && g_ready)
        for (Slot &s : g_slots)
            if (s.controller)
                SendRumble(s, 0, 0);
}

} // extern "C"
