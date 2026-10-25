#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dirt3_modloader_api.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

const ModloaderAPI* g_Api = nullptr;

// ----------------------------------------------------------------------------
// Offsets in neon::vehiclePhysics::Car (see REVERSING_NOTES_PHYSICS_VEHICLES.md)
// ----------------------------------------------------------------------------
static constexpr uintptr_t OFF_COLLISION_STATE = 0x021C; // int collision state / impact counter
static constexpr uintptr_t OFF_SUSPENSION_LOCK = 0x1620; // bool physics suspension lock flag
static constexpr uintptr_t OFF_WORLD_VEL       = 0x19E0; // Vector4 world velocity (m/s)
static constexpr uintptr_t OFF_ANGULAR_VEL     = 0x19F0; // Vector4 world angular velocity (rad/s)
static constexpr uintptr_t OFF_TRANSFORM_RIGHT = 0x1A00; // Vector4 orientation matrix right row (READ ONLY)
static constexpr uintptr_t OFF_TRANSFORM_UP    = 0x1A10; // Vector4 orientation matrix up row (READ ONLY)
static constexpr uintptr_t OFF_TRANSFORM_FWD   = 0x1A20; // Vector4 orientation matrix forward row (READ ONLY)
static constexpr uintptr_t OFF_TRANSFORM_POS   = 0x1A30; // Vector4 orientation matrix position row (READ ONLY)
static constexpr uintptr_t OFF_SPEED_MPS       = 0x2404; // float forward speed (m/s)

constexpr float PI = 3.14159265358979323846f;
constexpr float DEG2RAD = PI / 180.0f;
constexpr float STANDARD_G = 9.8100004f;

// ----------------------------------------------------------------------------
// 3D Vector & Quaternion Math Helpers
// ----------------------------------------------------------------------------
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }

    float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }

    Vec3 Cross(const Vec3& o) const {
        return {
            y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x
        };
    }

    float Len() const { return std::sqrt(x * x + y * y + z * z); }

    Vec3 Normalized(const Vec3& fallback = { 0.0f, 0.0f, 1.0f }) const {
        float l = Len();
        if (l < 1e-6f || !std::isfinite(l)) return fallback;
        return { x / l, y / l, z / l };
    }
};

// ----------------------------------------------------------------------------
// Configuration (loaded from mods/flyhack_mod.ini)
// ----------------------------------------------------------------------------
struct Config {
    int   keyToggle        = VK_F3;       // F3
    WORD  padToggle        = 0x0001;      // DPAD_UP

    int   keyForward       = 'W';
    int   keyBackward      = 'S';
    int   keyStrafeLeft    = 'A';
    int   keyStrafeRight   = 'D';
    int   keyUp            = VK_SPACE;
    int   keyDown          = VK_LCONTROL;
    int   keyBoost         = VK_LSHIFT;
    int   keySlow          = VK_LMENU;    // Alt

    int   keyYawLeft       = VK_LEFT;
    int   keyYawRight      = VK_RIGHT;
    int   keyPitchUp       = VK_DOWN;     // nose up
    int   keyPitchDown     = VK_UP;       // nose down
    int   keyRollLeft      = 'Q';
    int   keyRollRight     = 'E';
    int   keyLevel         = 'R';

    bool  enableNumpad     = true;

    float flySpeed         = 35.0f;       // m/s (~126 km/h)
    float boostMultiplier  = 2.5f;        // while holding Shift
    float verticalSpeed    = 22.0f;       // m/s
    float turnSpeed        = 90.0f;       // deg/s
    float rollSpeed        = 90.0f;       // deg/s

    bool  freezeWhenStill  = true;
    bool  disableCollision = true;        // Noclip mode
    bool  lockCamera       = true;        // Rigid camera follow
    float cameraDistance   = 5.5f;        // Distance behind car (m)
    float cameraHeight     = 1.8f;        // Height above car (m)
    bool  showConsole      = true;

    // Smooth FOV Changer
    bool  fovEnabled       = true;
    int   keyFovIncrease   = VK_OEM_6;    // ']'
    int   keyFovDecrease   = VK_OEM_4;    // '['
    int   keyFovReset      = VK_BACK;     // Backspace
    int   altKeyFovInc     = VK_PRIOR;    // PageUp
    int   altKeyFovDec     = VK_NEXT;     // PageDown
    int   altKeyFovReset   = VK_HOME;     // Home
    float fovMin           = 35.0f;
    float fovMax           = 135.0f;
    float fovDefault       = 65.0f;
    float fovAdjustSpeed   = 40.0f;       // deg/s
    float fovSmoothSpeed   = 12.0f;       // lerp factor
};

Config g_Cfg;

// FOV Runtime State
float g_TargetFovDeg = 65.0f;
float g_CurrentFovDeg = 65.0f;
bool  g_FovInitialized = false;

// ----------------------------------------------------------------------------
// Flight Controller State
// ----------------------------------------------------------------------------
std::atomic<bool>  g_FlyActive{ false };
std::atomic<bool>  g_NeedInitFlight{ false };
std::atomic<void*> g_PlayerCar{ nullptr };
std::atomic<float> g_TimeSincePhysics{ 10.0f };
std::atomic<float> g_CurrentSpeedKmh{ 0.0f };

struct InputState {
    float moveForward  = 0.0f; // [-1.0 .. +1.0]
    float moveStrafe   = 0.0f; // [-1.0 .. +1.0]
    float moveVertical = 0.0f; // [-1.0 .. +1.0]
    float rotYaw       = 0.0f; // [-1.0 .. +1.0]
    float rotPitch     = 0.0f; // [-1.0 .. +1.0]
    float rotRoll      = 0.0f; // [-1.0 .. +1.0]
    bool  boost        = false;
    bool  slow         = false;
    bool  level        = false;
};

std::atomic<InputState> g_Input{};

// ----------------------------------------------------------------------------
// Dynamic XInput Loader
// ----------------------------------------------------------------------------
struct XInputGamepadLite {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT thumbLX, thumbLY, thumbRX, thumbRY;
};
struct XInputStateLite {
    DWORD dwPacketNumber;
    XInputGamepadLite Gamepad;
};
typedef DWORD (WINAPI* PFN_XInputGetState)(DWORD, XInputStateLite*);
PFN_XInputGetState g_XInputGetState = nullptr;

void LoadXInput() {
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (const char* name : dlls) {
        HMODULE h = LoadLibraryA(name);
        if (h) {
            g_XInputGetState = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(h, "XInputGetState"));
            if (g_XInputGetState) return;
        }
    }
}

// ----------------------------------------------------------------------------
// Memory & Validation Helpers
// ----------------------------------------------------------------------------
static inline bool IsValidUserPtr(uintptr_t ptr) {
    return (ptr >= 0x00010000 && ptr < 0x7FFE0000);
}

void Log(const char* msg) {
    if (g_Api) g_Api->LogInfo("FlyhackMod", msg);
}

bool IsPlayerCar(void* carPtr) {
    void* player = g_Api->GetPlayerCar();
    if (player) {
        g_PlayerCar.store(player);
        return carPtr == player;
    }
    void* expected = nullptr;
    if (g_PlayerCar.compare_exchange_strong(expected, carPtr)) return true;
    return g_PlayerCar.load() == carPtr;
}

// ----------------------------------------------------------------------------
// Noclip Subsystem (Collision & Geometry Filtering)
// ----------------------------------------------------------------------------
void SetCarNoclip(uintptr_t car, bool noclip) {
    if (!IsValidUserPtr(car)) return;

    __try {
        // 1. Suspension lock flag: prevents wheel raycasts catching scenery/ground
        *reinterpret_cast<bool*>(car + OFF_SUSPENSION_LOCK) = noclip;

        // 2. Clear collision impact event counter / damage impulses
        *reinterpret_cast<int*>(car + OFF_COLLISION_STATE) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ----------------------------------------------------------------------------
// Camera Lock Subsystem
// ----------------------------------------------------------------------------
uintptr_t GetPlayerVehicle() {
    if (!g_Api) return 0;
    __try {
        uintptr_t base = g_Api->gameBaseAddress;
        uintptr_t appPtrAddr = base + (0x00FF6900 - 0x00400000);
        if (!IsValidUserPtr(appPtrAddr)) return 0;
        uintptr_t app = *reinterpret_cast<uintptr_t*>(appPtrAddr);
        if (!IsValidUserPtr(app)) return 0;
        uintptr_t vm = *reinterpret_cast<uintptr_t*>(app + 4280);
        if (!IsValidUserPtr(vm)) return 0;
        uintptr_t vecBegin = *reinterpret_cast<uintptr_t*>(vm + 104);
        uintptr_t vecEnd = *reinterpret_cast<uintptr_t*>(vm + 108);
        if (!IsValidUserPtr(vecBegin) || vecEnd <= vecBegin) return 0;
        uintptr_t pVehicle = *reinterpret_cast<uintptr_t*>(vecBegin);
        if (!IsValidUserPtr(pVehicle)) return 0;
        return pVehicle;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Hooked ChaseCameraView::update (0x00B31120)
// Uses the engine's authentic native chase camera reset (sub_B13250)
// Flag +589 triggers sub_B13250 which zeroes drift angles (+1956),
// zeroes turn lag (+2032..2040), sets snapToCar (+588 = 1), and updates view matrices cleanly via sub_B15450.
typedef void (__thiscall* PFN_ChaseCamUpdate)(void* thisPtr, float* pDt, void* pState);
PFN_ChaseCamUpdate g_OrigChaseCamUpdate = nullptr;

void __fastcall HookedChaseCamUpdate(void* thisPtr, void* edx, float* pDt, void* pState) {
    __try {
        if (g_FlyActive.load() && g_Cfg.lockCamera && pState) {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pState) + 588) = 1;
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pState) + 589) = 1;
        }

        if (g_OrigChaseCamUpdate) {
            g_OrigChaseCamUpdate(thisPtr, pDt, pState);
        }

        if (g_Cfg.fovEnabled && pState) {
            uintptr_t stateAddr = reinterpret_cast<uintptr_t>(pState);
            if (IsValidUserPtr(stateAddr)) {
                float* pFov = reinterpret_cast<float*>(stateAddr + 0x54);
                if (!g_FovInitialized && *pFov > 0.2f && *pFov < 3.0f) {
                    float vanillaDeg = *pFov * (180.0f / 3.14159265f);
                    if (g_Cfg.fovDefault <= 0.0f) {
                        g_TargetFovDeg = vanillaDeg;
                        g_CurrentFovDeg = vanillaDeg;
                    }
                    g_FovInitialized = true;
                }

                constexpr float DEG2RAD = 3.14159265f / 180.0f;
                *pFov = g_CurrentFovDeg * DEG2RAD;

                if (IsValidUserPtr(reinterpret_cast<uintptr_t>(thisPtr))) {
                    *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(thisPtr) + 0x138) = g_CurrentFovDeg * DEG2RAD;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Request clean camera reset when flight ends
void ResetCameraOnFlightEnd() {
    __try {
        uintptr_t pVehicle = GetPlayerVehicle();
        if (!IsValidUserPtr(pVehicle)) return;
        uintptr_t pViewManager = *reinterpret_cast<uintptr_t*>(pVehicle + 9444);
        if (!IsValidUserPtr(pViewManager)) return;

        uintptr_t viewArray = *reinterpret_cast<uintptr_t*>(pViewManager);
        int activeIdx = *reinterpret_cast<int*>(pViewManager + 320);
        if (!IsValidUserPtr(viewArray) || activeIdx < 0) return;

        uintptr_t pViewState = viewArray + (640 * activeIdx) + 16;
        if (IsValidUserPtr(pViewState)) {
            *reinterpret_cast<uint8_t*>(pViewState + 589) = 1; // Request reset
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ----------------------------------------------------------------------------
// Flight Physics Simulation (runs on physics thread once per substep)
// ----------------------------------------------------------------------------
void OnCarPhysics(void* carPtr, float dt) {
    if (!carPtr || !g_Api || dt <= 0.0f || dt > 0.1f) return;
    uintptr_t car = reinterpret_cast<uintptr_t>(carPtr);
    if (!IsValidUserPtr(car)) return;
    if (!IsPlayerCar(carPtr)) return;

    g_TimeSincePhysics.store(0.0f);

    if (!g_FlyActive.load()) return;

    __try {
        float* vel = reinterpret_cast<float*>(car + OFF_WORLD_VEL);
        float* angVel = reinterpret_cast<float*>(car + OFF_ANGULAR_VEL);

        // Read orientation vectors from car (READ-ONLY, never written directly)
        const float* rRaw = reinterpret_cast<const float*>(car + OFF_TRANSFORM_RIGHT);
        const float* uRaw = reinterpret_cast<const float*>(car + OFF_TRANSFORM_UP);
        const float* fRaw = reinterpret_cast<const float*>(car + OFF_TRANSFORM_FWD);

        Vec3 right(rRaw[0], rRaw[1], rRaw[2]);
        Vec3 up(uRaw[0], uRaw[1], uRaw[2]);
        Vec3 fwd(fRaw[0], fRaw[1], fRaw[2]);

        right = right.Normalized({ 1.0f, 0.0f, 0.0f });
        up    = up.Normalized({ 0.0f, 1.0f, 0.0f });
        fwd   = fwd.Normalized({ 0.0f, 0.0f, 1.0f });

        // Maintain Noclip mode active on every physics substep
        if (g_Cfg.disableCollision) {
            SetCarNoclip(car, true);
        }

        // Initial activation frame: smooth hover freeze without jumping
        if (g_NeedInitFlight.exchange(false)) {
            vel[0] = 0.0f;
            vel[1] = STANDARD_G * dt; // Perfect neutral hover balance against gravity
            vel[2] = 0.0f;
            vel[3] = 0.0f;
            angVel[0] = 0.0f;
            angVel[1] = 0.0f;
            angVel[2] = 0.0f;
            angVel[3] = 0.0f;
            *reinterpret_cast<float*>(car + OFF_SPEED_MPS) = 0.0f;
            g_CurrentSpeedKmh.store(0.0f);
            return;
        }

        InputState in = g_Input.load();

        // --------------------------------------------------------------------
        // 1. Flight Linear Movement
        // --------------------------------------------------------------------
        Vec3 moveDir(0.0f, 0.0f, 0.0f);
        if (std::abs(in.moveForward) > 0.001f)  moveDir = moveDir + (fwd * in.moveForward);
        if (std::abs(in.moveStrafe) > 0.001f)   moveDir = moveDir + (right * in.moveStrafe);
        if (std::abs(in.moveVertical) > 0.001f) moveDir = moveDir + (Vec3(0.0f, 1.0f, 0.0f) * in.moveVertical);

        float speed = g_Cfg.flySpeed;
        if (in.boost) speed *= g_Cfg.boostMultiplier;
        if (in.slow)  speed *= 0.35f;

        Vec3 targetVel(0.0f, 0.0f, 0.0f);
        if (moveDir.Len() > 0.001f) {
            targetVel = moveDir.Normalized() * speed;
        }

        // Flight Linear Movement & Noclip
        // When noclip is active: advance position directly (passes cleanly through solid collision meshes)
        // and apply half velocity so total speed matches targetVel without double-speeding.
        if (g_Cfg.disableCollision && moveDir.Len() > 0.001f) {
            float* pos = reinterpret_cast<float*>(car + OFF_TRANSFORM_POS);
            pos[0] += targetVel.x * dt * 0.5f;
            pos[1] += targetVel.y * dt * 0.5f;
            pos[2] += targetVel.z * dt * 0.5f;

            vel[0] = targetVel.x * 0.5f;
            vel[1] = targetVel.y * 0.5f + (STANDARD_G * dt);
            vel[2] = targetVel.z * 0.5f;
        } else {
            vel[0] = targetVel.x;
            vel[1] = targetVel.y + (STANDARD_G * dt);
            vel[2] = targetVel.z;
        }
        vel[3] = 0.0f;

        *reinterpret_cast<float*>(car + OFF_SPEED_MPS) = targetVel.Len();
        g_CurrentSpeedKmh.store(targetVel.Len() * 3.6f);

        // --------------------------------------------------------------------
        // 2. Flight Angular Movement (Rotation via angular velocity)
        // --------------------------------------------------------------------
        Vec3 targetAngVel(0.0f, 0.0f, 0.0f);

        if (in.level) {
            // Level horizon: compute tilt error between current Up and world Up (0, 1, 0)
            Vec3 worldUp(0.0f, 1.0f, 0.0f);
            Vec3 tiltAxis = up.Cross(worldUp);
            float tiltAmount = tiltAxis.Len();
            if (tiltAmount > 0.005f) {
                targetAngVel = targetAngVel + (tiltAxis.Normalized() * std::min(tiltAmount * 6.0f, 5.0f));
            }
        } else {
            float turnRad = g_Cfg.turnSpeed * DEG2RAD;
            float rollRad = g_Cfg.rollSpeed * DEG2RAD;

            // Yaw: turn left / right around local Up (INVERTED according to user request)
            if (std::abs(in.rotYaw) > 0.001f) {
                targetAngVel = targetAngVel + (up * (in.rotYaw * turnRad));
            }

            // Pitch: tilt nose down / up around local Right
            if (std::abs(in.rotPitch) > 0.001f) {
                targetAngVel = targetAngVel + (right * (in.rotPitch * turnRad));
            }

            // Roll: bank left / right around local Forward
            if (std::abs(in.rotRoll) > 0.001f) {
                targetAngVel = targetAngVel + (fwd * (in.rotRoll * rollRad));
            }
        }

        // Overwrite angular velocity (engine integrates this cleanly into orientation)
        angVel[0] = targetAngVel.x;
        angVel[1] = targetAngVel.y;
        angVel[2] = targetAngVel.z;
        angVel[3] = 0.0f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ----------------------------------------------------------------------------
// Console Status HUD
// ----------------------------------------------------------------------------
void RenderConsoleStatus(bool active) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hConsole || hConsole == INVALID_HANDLE_VALUE) return;

    std::string line;
    if (active) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1)
           << "  [FLYHACK: ON] Spd: " << std::setw(5) << g_CurrentSpeedKmh.load() << " km/h"
           << " | Noclip: ON | CamLock: " << (g_Cfg.lockCamera ? "ON" : "OFF")
           << " | WASD: Fly | Arrows: Turn";
        line = ss.str();
    } else {
        line = "  [FLYHACK: OFF] Press [F3] to activate flight mode                             ";
    }
    if (line.size() < 79) line.append(79 - line.size(), ' ');

    CONSOLE_SCREEN_BUFFER_INFO info{};
    COORD prev = { 0, 0 };
    if (GetConsoleScreenBufferInfo(hConsole, &info)) prev = info.dwCursorPosition;

    COORD pos = { 0, 19 };
    SetConsoleCursorPosition(hConsole, pos);
    DWORD written = 0;
    WriteConsoleA(hConsole, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    SetConsoleCursorPosition(hConsole, prev);
}

// ----------------------------------------------------------------------------
// Frame Tick (Input Gathering, Toggle Logic, Camera Updates)
// ----------------------------------------------------------------------------
void OnFrame(float dt) {
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    bool windowActive = (fgPid == GetCurrentProcessId());

    auto Key = [](int vk) -> bool {
        return (vk != 0) && ((GetAsyncKeyState(vk) & 0x8000) != 0);
    };

    // Smooth FOV Changer tick (active globally whenever DiRT 3 window is focused)
    if (g_Cfg.fovEnabled && windowActive) {
        bool inc = Key(g_Cfg.keyFovIncrease) || Key(g_Cfg.altKeyFovInc);
        bool dec = Key(g_Cfg.keyFovDecrease) || Key(g_Cfg.altKeyFovDec);
        bool rst = Key(g_Cfg.keyFovReset)    || Key(g_Cfg.altKeyFovReset);

        if (inc) g_TargetFovDeg += g_Cfg.fovAdjustSpeed * dt;
        if (dec) g_TargetFovDeg -= g_Cfg.fovAdjustSpeed * dt;
        if (rst) g_TargetFovDeg = g_Cfg.fovDefault;

        g_TargetFovDeg = std::clamp(g_TargetFovDeg, g_Cfg.fovMin, g_Cfg.fovMax);
    }

    if (g_Cfg.fovEnabled) {
        float blend = 1.0f - std::exp(-g_Cfg.fovSmoothSpeed * dt);
        g_CurrentFovDeg += (g_TargetFovDeg - g_CurrentFovDeg) * blend;
    }

    static bool toggleWasDown = false;
    bool toggleKey = (g_Cfg.keyToggle != 0) && Key(g_Cfg.keyToggle);

    bool padToggle = false;
    XInputStateLite padState{};
    bool hasPad = false;
    if (g_XInputGetState && g_Cfg.padToggle != 0) {
        for (DWORD i = 0; i < 4; ++i) {
            if (g_XInputGetState(i, &padState) == ERROR_SUCCESS) {
                hasPad = true;
                if (padState.Gamepad.wButtons & g_Cfg.padToggle) padToggle = true;
                break;
            }
        }
    }

    bool toggleIsDown = windowActive && (toggleKey || padToggle);
    if (toggleIsDown && !toggleWasDown) {
        bool newState = !g_FlyActive.load();
        g_FlyActive.store(newState);

        void* player = g_PlayerCar.load();
        if (!player && g_Api) {
            player = g_Api->GetPlayerCar();
            if (player) g_PlayerCar.store(player);
        }
        uintptr_t car = player ? reinterpret_cast<uintptr_t>(player) : 0;

        if (newState) {
            g_NeedInitFlight.store(true);
            if (IsValidUserPtr(car) && g_Cfg.disableCollision) {
                SetCarNoclip(car, true);
            }
            Log(">> [FLYHACK] ACTIVATED (WASD to fly, Arrows to turn, Noclip: ON, CamLock: ON)");
        } else {
            Log(">> [FLYHACK] DEACTIVATED (Normal physics and collisions restored)");
            if (IsValidUserPtr(car)) {
                // Restore normal collisions & suspension
                SetCarNoclip(car, false);

                // Release gently
                float* vel = reinterpret_cast<float*>(car + OFF_WORLD_VEL);
                vel[0] = 0.0f; vel[1] = -1.0f; vel[2] = 0.0f;
            }
            // Reset camera back to standard gameplay smoothing
            ResetCameraOnFlightEnd();
        }
        if (g_Cfg.showConsole) {
            RenderConsoleStatus(newState);
        }
    }
    toggleWasDown = toggleIsDown;

    float since = g_TimeSincePhysics.load() + dt;
    g_TimeSincePhysics.store(since);
    if (since > 0.5f) {
        g_PlayerCar.store(nullptr);
        g_FlyActive.store(false);
    }

    if (!g_FlyActive.load()) return;

    // Direct camera follow frame update: ensure native snap flags remain set for next camera update
    if (g_Cfg.lockCamera) {
        uintptr_t pVehicle = GetPlayerVehicle();
        if (IsValidUserPtr(pVehicle)) {
            uintptr_t pViewManager = *reinterpret_cast<uintptr_t*>(pVehicle + 9444);
            if (IsValidUserPtr(pViewManager)) {
                uintptr_t viewArray = *reinterpret_cast<uintptr_t*>(pViewManager);
                int activeIdx = *reinterpret_cast<int*>(pViewManager + 320);
                if (IsValidUserPtr(viewArray) && activeIdx >= 0) {
                    uintptr_t pViewState = viewArray + (640 * activeIdx) + 16;
                    if (IsValidUserPtr(pViewState)) {
                        *reinterpret_cast<uint8_t*>(pViewState + 588) = 1;
                        *reinterpret_cast<uint8_t*>(pViewState + 589) = 1;
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // Gather 6-DOF Input
    // ------------------------------------------------------------------------
    InputState in{};

    if (windowActive) {
        auto Key = [](int vk) -> bool {
            return (vk != 0) && ((GetAsyncKeyState(vk) & 0x8000) != 0);
        };

        // Movement
        if (Key(g_Cfg.keyForward))          in.moveForward += 1.0f;
        if (Key(g_Cfg.keyBackward))         in.moveForward -= 1.0f;
        if (Key(g_Cfg.keyStrafeRight))      in.moveStrafe += 1.0f;
        if (Key(g_Cfg.keyStrafeLeft))       in.moveStrafe -= 1.0f;
        if (Key(g_Cfg.keyUp))               in.moveVertical += 1.0f;
        if (Key(g_Cfg.keyDown) || Key('C')) in.moveVertical -= 1.0f;

        // Rotation: Yaw, Pitch, Roll
        if (Key(g_Cfg.keyYawRight))   in.rotYaw -= 1.0f; // Turn right
        if (Key(g_Cfg.keyYawLeft))    in.rotYaw += 1.0f; // Turn left
        if (Key(g_Cfg.keyPitchDown))  in.rotPitch += 1.0f; // Nose down
        if (Key(g_Cfg.keyPitchUp))    in.rotPitch -= 1.0f; // Nose up
        if (Key(g_Cfg.keyRollRight))  in.rotRoll -= 1.0f; // Bank right
        if (Key(g_Cfg.keyRollLeft))   in.rotRoll += 1.0f; // Bank left
        if (Key(g_Cfg.keyLevel))      in.level = true;

        // Numpad alternate rotation keys
        if (g_Cfg.enableNumpad) {
            if (Key(VK_NUMPAD6)) in.rotYaw -= 1.0f;
            if (Key(VK_NUMPAD4)) in.rotYaw += 1.0f;
            if (Key(VK_NUMPAD8)) in.rotPitch += 1.0f;
            if (Key(VK_NUMPAD2)) in.rotPitch -= 1.0f;
            if (Key(VK_NUMPAD9)) in.rotRoll -= 1.0f;
            if (Key(VK_NUMPAD7)) in.rotRoll += 1.0f;
            if (Key(VK_NUMPAD5)) in.level = true;
        }

        in.boost = Key(g_Cfg.keyBoost);
        in.slow  = Key(g_Cfg.keySlow);

        // Analog Gamepad Integration
        if (hasPad) {
            constexpr SHORT DEADZONE = 7000;
            auto ApplyDeadzone = [](SHORT v) -> float {
                if (std::abs(v) < DEADZONE) return 0.0f;
                return static_cast<float>(v) / 32767.0f;
            };

            float lx = ApplyDeadzone(padState.Gamepad.thumbLX);
            float ly = ApplyDeadzone(padState.Gamepad.thumbLY);
            float rx = ApplyDeadzone(padState.Gamepad.thumbRX);
            float ry = ApplyDeadzone(padState.Gamepad.thumbRY);

            in.moveStrafe  += lx;
            in.moveForward += ly;

            in.rotYaw   -= rx;
            in.rotPitch += ry;

            // Triggers: RT = Up, LT = Down
            if (padState.Gamepad.bRightTrigger > 30) {
                in.moveVertical += (padState.Gamepad.bRightTrigger - 30) / 225.0f;
            }
            if (padState.Gamepad.bLeftTrigger > 30) {
                in.moveVertical -= (padState.Gamepad.bLeftTrigger - 30) / 225.0f;
            }

            // Bumpers: Roll
            if (padState.Gamepad.wButtons & 0x0100) in.rotRoll += 1.0f; // LB
            if (padState.Gamepad.wButtons & 0x0200) in.rotRoll -= 1.0f; // RB

            // Face buttons
            if (padState.Gamepad.wButtons & 0x1000) in.boost = true; // A button
            if (padState.Gamepad.wButtons & 0x2000) in.slow  = true; // B button
            if (padState.Gamepad.wButtons & 0x4000) in.level = true; // X button
        }
    }

    in.moveForward  = std::clamp(in.moveForward, -1.0f, 1.0f);
    in.moveStrafe   = std::clamp(in.moveStrafe, -1.0f, 1.0f);
    in.moveVertical = std::clamp(in.moveVertical, -1.0f, 1.0f);
    in.rotYaw       = std::clamp(in.rotYaw, -1.0f, 1.0f);
    in.rotPitch     = std::clamp(in.rotPitch, -1.0f, 1.0f);
    in.rotRoll      = std::clamp(in.rotRoll, -1.0f, 1.0f);

    g_Input.store(in);

    static float hudTimer = 0.0f;
    hudTimer += dt;
    if (g_Cfg.showConsole && hudTimer >= 0.1f) {
        hudTimer = 0.0f;
        RenderConsoleStatus(true);
    }
}

// ----------------------------------------------------------------------------
// Config Parser
// ----------------------------------------------------------------------------
int ParseKey(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (v.empty() || v == "NONE") return 0;

    struct Named { const char* name; int vk; };
    static const Named names[] = {
        { "LSHIFT", VK_LSHIFT }, { "RSHIFT", VK_RSHIFT }, { "SHIFT", VK_SHIFT },
        { "LCTRL", VK_LCONTROL }, { "RCTRL", VK_RCONTROL }, { "CTRL", VK_CONTROL },
        { "LALT", VK_LMENU }, { "RALT", VK_RMENU }, { "ALT", VK_MENU },
        { "SPACE", VK_SPACE }, { "TAB", VK_TAB }, { "CAPSLOCK", VK_CAPITAL },
        { "ENTER", VK_RETURN }, { "BACKSPACE", VK_BACK }, { "BACK", VK_BACK },
        { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT }, { "UP", VK_UP }, { "DOWN", VK_DOWN },
        { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME }, { "END", VK_END },
        { "PGUP", VK_PRIOR }, { "PGDN", VK_NEXT }, { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
        { "LBRACKET", VK_OEM_4 }, { "RBRACKET", VK_OEM_6 }, { "[", VK_OEM_4 }, { "]", VK_OEM_6 },
        { "MINUS", VK_OEM_MINUS }, { "PLUS", VK_OEM_PLUS }, { "-", VK_OEM_MINUS }, { "=", VK_OEM_PLUS },
        { "NUM0", VK_NUMPAD0 }, { "NUM1", VK_NUMPAD1 }, { "NUM2", VK_NUMPAD2 },
        { "NUM3", VK_NUMPAD3 }, { "NUM4", VK_NUMPAD4 }, { "NUM5", VK_NUMPAD5 },
        { "NUM6", VK_NUMPAD6 }, { "NUM7", VK_NUMPAD7 }, { "NUM8", VK_NUMPAD8 },
        { "NUM9", VK_NUMPAD9 },
    };
    for (const auto& n : names) {
        if (v == n.name) return n.vk;
    }
    if (v.size() == 1 && std::isalnum(static_cast<unsigned char>(v[0]))) return v[0];
    if (v == "[") return VK_OEM_4;
    if (v == "]") return VK_OEM_6;
    if (v.size() >= 2 && v[0] == 'F' && std::isdigit(static_cast<unsigned char>(v[1]))) {
        int f = std::stoi(v.substr(1));
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    return std::stoi(v, nullptr, 0);
}

WORD ParsePadButton(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    struct Named { const char* name; WORD mask; };
    static const Named names[] = {
        { "NONE", 0x0000 }, { "DPAD_UP", 0x0001 }, { "DPAD_DOWN", 0x0002 },
        { "DPAD_LEFT", 0x0004 }, { "DPAD_RIGHT", 0x0008 }, { "START", 0x0010 },
        { "BACK", 0x0020 }, { "L3", 0x0040 }, { "R3", 0x0080 }, { "LB", 0x0100 },
        { "RB", 0x0200 }, { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 },
    };
    for (const auto& n : names) {
        if (v == n.name) return n.mask;
    }
    return static_cast<WORD>(std::stoi(v, nullptr, 0));
}

bool ParseBool(const std::string& v) {
    return v == "1" || _stricmp(v.c_str(), "true") == 0 || _stricmp(v.c_str(), "yes") == 0;
}

void LoadConfig() {
    std::string path = std::string(g_Api->GetModsDirectory()) + "\\flyhack_mod.ini";
    std::ifstream file(path);
    if (!file.is_open()) {
        Log("flyhack_mod.ini not found, using default configuration");
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        auto cpos = val.find(';');
        if (cpos != std::string::npos) val = val.substr(0, cpos);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t\r\n") + 1);

        try {
            if      (key == "ToggleKey")              g_Cfg.keyToggle = ParseKey(val);
            else if (key == "GamepadToggle")          g_Cfg.padToggle = ParsePadButton(val);
            else if (key == "KeyForward")             g_Cfg.keyForward = ParseKey(val);
            else if (key == "KeyBackward")            g_Cfg.keyBackward = ParseKey(val);
            else if (key == "KeyStrafeLeft")          g_Cfg.keyStrafeLeft = ParseKey(val);
            else if (key == "KeyStrafeRight")         g_Cfg.keyStrafeRight = ParseKey(val);
            else if (key == "KeyUp")                  g_Cfg.keyUp = ParseKey(val);
            else if (key == "KeyDown")                g_Cfg.keyDown = ParseKey(val);
            else if (key == "KeyBoost")               g_Cfg.keyBoost = ParseKey(val);
            else if (key == "KeySlow")                g_Cfg.keySlow = ParseKey(val);
            else if (key == "KeyYawLeft")             g_Cfg.keyYawLeft = ParseKey(val);
            else if (key == "KeyYawRight")            g_Cfg.keyYawRight = ParseKey(val);
            else if (key == "KeyPitchUp")             g_Cfg.keyPitchUp = ParseKey(val);
            else if (key == "KeyPitchDown")           g_Cfg.keyPitchDown = ParseKey(val);
            else if (key == "KeyRollLeft")            g_Cfg.keyRollLeft = ParseKey(val);
            else if (key == "KeyRollRight")           g_Cfg.keyRollRight = ParseKey(val);
            else if (key == "KeyLevel")               g_Cfg.keyLevel = ParseKey(val);
            else if (key == "EnableNumpadRotation")   g_Cfg.enableNumpad = ParseBool(val);
            else if (key == "FlySpeed")               g_Cfg.flySpeed = std::max(1.0f, std::stof(val));
            else if (key == "BoostMultiplier")        g_Cfg.boostMultiplier = std::max(1.0f, std::stof(val));
            else if (key == "VerticalSpeed")          g_Cfg.verticalSpeed = std::max(1.0f, std::stof(val));
            else if (key == "TurnSpeed")              g_Cfg.turnSpeed = std::max(1.0f, std::stof(val));
            else if (key == "RollSpeed")              g_Cfg.rollSpeed = std::max(1.0f, std::stof(val));
            else if (key == "FreezePhysicsWhenStill") g_Cfg.freezeWhenStill = ParseBool(val);
            else if (key == "DisableCollisions")      g_Cfg.disableCollision = ParseBool(val);
            else if (key == "LockCamera")             g_Cfg.lockCamera = ParseBool(val);
            else if (key == "CameraDistance")         g_Cfg.cameraDistance = std::max(1.0f, std::stof(val));
            else if (key == "CameraHeight")           g_Cfg.cameraHeight = std::max(0.1f, std::stof(val));
            else if (key == "ShowConsoleStatus")      g_Cfg.showConsole = ParseBool(val);
            else if (key == "FovEnabled")             g_Cfg.fovEnabled = ParseBool(val);
            else if (key == "KeyFovIncrease")         g_Cfg.keyFovIncrease = ParseKey(val);
            else if (key == "KeyFovDecrease")         g_Cfg.keyFovDecrease = ParseKey(val);
            else if (key == "KeyFovReset")            g_Cfg.keyFovReset = ParseKey(val);
            else if (key == "AltKeyFovIncrease")      g_Cfg.altKeyFovInc = ParseKey(val);
            else if (key == "AltKeyFovDecrease")      g_Cfg.altKeyFovDec = ParseKey(val);
            else if (key == "AltKeyFovReset")         g_Cfg.altKeyFovReset = ParseKey(val);
            else if (key == "FovMin")                 g_Cfg.fovMin = std::stof(val);
            else if (key == "FovMax")                 g_Cfg.fovMax = std::stof(val);
            else if (key == "FovDefault")             g_Cfg.fovDefault = std::stof(val);
            else if (key == "FovAdjustSpeed")         g_Cfg.fovAdjustSpeed = std::stof(val);
            else if (key == "FovSmoothSpeed")         g_Cfg.fovSmoothSpeed = std::stof(val);
        } catch (...) {
            std::string err = "Invalid value for '" + key + "': " + val;
            Log(err.c_str());
        }
    }

    g_TargetFovDeg = g_Cfg.fovDefault;
    g_CurrentFovDeg = g_Cfg.fovDefault;
}

} // namespace

extern "C" __declspec(dllexport) bool ModInit(const ModloaderAPI* api) {
    if (!api || api->apiVersion != DIRT3_MODLOADER_API_VERSION) {
        return false;
    }

    g_Api = api;
    Log("Flyhack Mod v1.6.0 initializing (with Smooth FOV Changer)...");

    LoadConfig();
    LoadXInput();

    g_Api->RegisterCarPhysicsStep(OnCarPhysics);
    g_Api->RegisterFrameTick(OnFrame);

    // Install ChaseCameraView::update hook (0x00B31120) for camera follow & smooth FOV
    uintptr_t targetChaseCam = g_Api->Rva(0x00B31120);
    bool hookOk = g_Api->CreateHook(
        "ChaseCamera::update",
        targetChaseCam,
        reinterpret_cast<void*>(HookedChaseCamUpdate),
        reinterpret_cast<void**>(&g_OrigChaseCamUpdate)
    );
    if (hookOk) {
        Log("Chase camera & smooth FOV hook successfully installed at 0x00B31120");
    } else {
        Log("Warning: Could not hook ChaseCamera::update at 0x00B31120!");
    }

    std::ostringstream ss;
    ss << "Ready: Toggle [F3], WASD to fly, Arrows to turn, Space/Ctrl for Up/Down, Noclip: ON, CamLock: ON"
       << (g_XInputGetState ? " [XInput active]" : "")
       << " | FOV: '[' / ']' or PageUp/PageDown, Reset: Backspace";
    Log(ss.str().c_str());

    return true;
}
