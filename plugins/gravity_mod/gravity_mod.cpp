#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dirt3_modloader_api.h"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <fstream>
#include <cmath>

namespace {

const ModloaderAPI* g_Api = nullptr;

// Exact DiRT 3 neon::vehiclePhysics::Car world coordinates & velocity
// (+0x19E0 = Vector4 world_velocity: X, Y, Z, W)
// (+0x1A30 = Vector4 world_position: X, Y, Z, W)
static constexpr uintptr_t OFF_WORLD_VEL_Y = 0x19E4; // float (world vertical velocity in m/s, +Y is UP)
static constexpr uintptr_t OFF_WORLD_POS_Y = 0x1A34; // float (world vertical position / height in meters)

constexpr float STANDARD_G = 9.8100004f;

struct GravityPreset {
    const char* name;
    float scale;
};

static const GravityPreset PRESETS[] = {
    { "Normal (Earth 1.0G)",   1.0f },
    { "Moon (Low-G 0.2G)",     0.2f },
    { "Mars (0.5G)",           0.5f },
    { "Zero-G (Weightless)",   0.0f },
    { "Heavy (Jupiter 2.0G)",  2.0f },
    { "Inverted (-1.0G)",     -1.0f }
};
constexpr size_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

float g_CurrentScale = 1.0f;
size_t g_CurrentPresetIndex = 0;
float g_StepSize = 0.1f;

int g_KeyCyclePreset = VK_F6;
int g_KeyIncrease    = VK_PRIOR; // Page Up
int g_KeyDecrease    = VK_NEXT;  // Page Down
int g_KeyReset       = VK_HOME;  // Home

void SetGravityScale(float scale, const char* label = nullptr) {
    // Clamp to sensible range [-5.0G, 10.0G]
    scale = std::clamp(scale, -5.0f, 10.0f);
    g_CurrentScale = scale;

    if (g_Api) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "=======================================================";
        g_Api->LogInfo("GravityMod", ss.str().c_str());

        std::ostringstream msg;
        msg << std::fixed << std::setprecision(2);
        msg << ">> GRAVITY SET TO: " << scale << "G (" << (scale * STANDARD_G) << " m/s^2)";
        if (label && label[0]) {
            msg << " [" << label << "]";
        }
        g_Api->LogInfo("GravityMod", msg.str().c_str());

        std::ostringstream hint;
        hint << ">> Hotkeys: [F6] Presets | [PgUp/PgDn] +/-" << g_StepSize << "G | [Home] Reset 1.0G";
        g_Api->LogInfo("GravityMod", hint.str().c_str());
        g_Api->LogInfo("GravityMod", "=======================================================");
    }
}

void CyclePreset() {
    g_CurrentPresetIndex = (g_CurrentPresetIndex + 1) % PRESET_COUNT;
    SetGravityScale(PRESETS[g_CurrentPresetIndex].scale, PRESETS[g_CurrentPresetIndex].name);
}

void IncreaseGravity() {
    float newScale = std::round((g_CurrentScale + g_StepSize) * 10.0f) / 10.0f;
    SetGravityScale(newScale, "Custom");
}

void DecreaseGravity() {
    float newScale = std::round((g_CurrentScale - g_StepSize) * 10.0f) / 10.0f;
    SetGravityScale(newScale, "Custom");
}

void ResetGravity() {
    g_CurrentPresetIndex = 0;
    SetGravityScale(1.0f, PRESETS[0].name);
}

static inline bool IsValidUserPtr(uintptr_t ptr) {
    return (ptr >= 0x00010000 && ptr < 0x7FFE0000);
}

void OnCarPhysics(void* carPtr, float dt) {
    if (!carPtr || dt <= 0.0f || dt > 0.1f) return;
    uintptr_t car = reinterpret_cast<uintptr_t>(carPtr);
    if (!IsValidUserPtr(car)) return;

    // Apply vertical physics delta when scale is not Earth normal (1.0G)
    // In Earth gravity, downward acceleration is -9.81 m/s^2.
    // Under scale 's', target downward acceleration is -s * 9.81 m/s^2.
    // The compensation acceleration is: deltaA = (1.0 - s) * 9.81 m/s^2.
    // e.g. scale = 0.0 (Zero-G): deltaA = +9.81 m/s^2 (cancels Earth gravity completely)
    // e.g. scale = 0.2 (Moon):   deltaA = +7.848 m/s^2 (car falls at only 1.96 m/s^2)
    // e.g. scale = 2.0 (Heavy):  deltaA = -9.81 m/s^2 (double gravity downward)
    // e.g. scale = -1.0 (Invert): deltaA = +19.62 m/s^2 (car accelerates up)
    if (std::abs(g_CurrentScale - 1.0f) > 0.001f) {
        __try {
            float* pWorldVelY = reinterpret_cast<float*>(car + OFF_WORLD_VEL_Y);
            float* pWorldPosY = reinterpret_cast<float*>(car + OFF_WORLD_POS_Y);

            float deltaA = (1.0f - g_CurrentScale) * STANDARD_G;
            float deltaVy = deltaA * dt;

            *pWorldVelY += deltaVy;
            *pWorldPosY += deltaVy * dt * 0.5f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

void OnFrame(float dt) {
    // Key debouncing
    static bool f6WasDown = false;
    static bool pgUpWasDown = false;
    static bool pgDnWasDown = false;
    static bool homeWasDown = false;

    // Alternate hotkeys: '[' and ']'
    static bool bracketOpenWasDown = false;
    static bool bracketCloseWasDown = false;

    bool f6IsDown = (GetAsyncKeyState(g_KeyCyclePreset) & 0x8000) != 0;
    bool pgUpIsDown = (GetAsyncKeyState(g_KeyIncrease) & 0x8000) != 0;
    bool pgDnIsDown = (GetAsyncKeyState(g_KeyDecrease) & 0x8000) != 0;
    bool homeIsDown = (GetAsyncKeyState(g_KeyReset) & 0x8000) != 0;

    bool bracketOpenIsDown = (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0;  // '['
    bool bracketCloseIsDown = (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0; // ']'

    if (f6IsDown && !f6WasDown) {
        CyclePreset();
    }
    if ((pgUpIsDown && !pgUpWasDown) || (bracketCloseIsDown && !bracketCloseWasDown)) {
        IncreaseGravity();
    }
    if ((pgDnIsDown && !pgDnWasDown) || (bracketOpenIsDown && !bracketOpenWasDown)) {
        DecreaseGravity();
    }
    if (homeIsDown && !homeWasDown) {
        ResetGravity();
    }

    f6WasDown = f6IsDown;
    pgUpWasDown = pgUpIsDown;
    pgDnWasDown = pgDnIsDown;
    homeWasDown = homeIsDown;
    bracketOpenWasDown = bracketOpenIsDown;
    bracketCloseWasDown = bracketCloseIsDown;
}

void LoadConfig() {
    if (!g_Api) return;

    std::string configPath = std::string(g_Api->GetModsDirectory()) + "\\gravity_mod.ini";
    std::ifstream file(configPath);
    if (!file.is_open()) {
        g_Api->LogInfo("GravityMod", "gravity_mod.ini not found, using default configuration");
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        val.erase(0, val.find_first_not_of(" \t\r\n"));
        val.erase(val.find_last_not_of(" \t\r\n") + 1);

        try {
            if (key == "DefaultScale") {
                g_CurrentScale = std::stof(val);
            } else if (key == "StepSize") {
                g_StepSize = std::stof(val);
            }
        } catch (...) {}
    }
}

} // namespace

extern "C" __declspec(dllexport) bool ModInit(const ModloaderAPI* api) {
    if (!api || api->apiVersion != DIRT3_MODLOADER_API_VERSION) {
        return false;
    }

    g_Api = api;
    g_Api->LogInfo("GravityMod", "Gravity Mod v1.1.0 initializing...");

    LoadConfig();

    // Register engine callbacks
    g_Api->RegisterCarPhysicsStep(OnCarPhysics);
    g_Api->RegisterFrameTick(OnFrame);

    // Apply initial scale if configured
    if (std::abs(g_CurrentScale - 1.0f) > 0.001f) {
        SetGravityScale(g_CurrentScale, "Config");
    } else {
        g_Api->LogInfo("GravityMod", "Gravity active at 1.00G (Earth Normal). Press [F6] to cycle presets!");
    }

    return true;
}
