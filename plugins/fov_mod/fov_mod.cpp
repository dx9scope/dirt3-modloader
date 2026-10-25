#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dirt3_modloader_api.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

const ModloaderAPI* g_Api = nullptr;

constexpr float PI = 3.14159265358979323846f;
constexpr float DEG2RAD = PI / 180.0f;
constexpr float RAD2DEG = 180.0f / PI;

struct FovConfig {
    int keyIncrease = VK_OEM_6;  // ']'
    int keyDecrease = VK_OEM_4;  // '['
    int keyReset    = VK_BACK;   // Backspace
    int altKeyIncrease = VK_PRIOR; // PageUp
    int altKeyDecrease = VK_NEXT;  // PageDown
    int altKeyReset    = VK_HOME;  // Home

    float minFov = 35.0f;
    float maxFov = 135.0f;
    float defaultFov = 65.0f;
    float adjustSpeed = 40.0f;   // degrees per second held
    float smoothSpeed = 12.0f;   // lerp factor per second
    bool enableOverlay = true;
};

FovConfig g_Cfg;

std::atomic<bool> g_FovActive{true};
float g_BaseFovRad = 0.0f;
float g_TargetFovDeg = 65.0f;
float g_CurrentFovDeg = 65.0f;
bool g_InitializedFromGame = false;

typedef void (__thiscall* PFN_ChaseCamUpdate)(void* thisPtr, float* pDt, void* pState);
PFN_ChaseCamUpdate g_OrigChaseCamUpdate = nullptr;

bool IsValidUserPtr(uintptr_t ptr) {
    return ptr >= 0x10000 && ptr < 0x7FFE0000;
}

int ParseKey(const std::string& val) {
    if (val.empty()) return 0;
    if (val.rfind("0x", 0) == 0 || val.rfind("0X", 0) == 0) {
        return static_cast<int>(std::strtol(val.c_str(), nullptr, 16));
    }
    if (val.size() == 1) {
        char c = static_cast<char>(std::toupper(val[0]));
        return c;
    }
    if (val == "PAGEUP" || val == "PRIOR") return VK_PRIOR;
    if (val == "PAGEDOWN" || val == "NEXT") return VK_NEXT;
    if (val == "HOME") return VK_HOME;
    if (val == "END") return VK_END;
    if (val == "BACKSPACE" || val == "BACK") return VK_BACK;
    if (val == "LBRACKET" || val == "[") return VK_OEM_4;
    if (val == "RBRACKET" || val == "]") return VK_OEM_6;
    if (val == "MINUS" || val == "-") return VK_OEM_MINUS;
    if (val == "PLUS" || val == "=") return VK_OEM_PLUS;
    if (val == "F7") return VK_F7;
    if (val == "F8") return VK_F8;
    if (val == "F9") return VK_F9;
    return 0;
}

void LoadConfig() {
    std::ifstream f("mods/fov_mod.ini");
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        size_t comment = line.find(';');
        if (comment != std::string::npos) line = line.substr(0, comment);
        comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        auto Trim = [](std::string& s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            s = s.substr(i);
        };
        Trim(key);
        Trim(val);
        if (key.empty() || val.empty()) continue;

        try {
            if (key == "KeyIncrease")         g_Cfg.keyIncrease = ParseKey(val);
            else if (key == "KeyDecrease")    g_Cfg.keyDecrease = ParseKey(val);
            else if (key == "KeyReset")       g_Cfg.keyReset = ParseKey(val);
            else if (key == "MinFov")         g_Cfg.minFov = std::stof(val);
            else if (key == "MaxFov")         g_Cfg.maxFov = std::stof(val);
            else if (key == "DefaultFov")     g_Cfg.defaultFov = std::stof(val);
            else if (key == "AdjustSpeed")    g_Cfg.adjustSpeed = std::stof(val);
            else if (key == "SmoothSpeed")    g_Cfg.smoothSpeed = std::stof(val);
        } catch (...) {}
    }

    g_TargetFovDeg = g_Cfg.defaultFov;
    g_CurrentFovDeg = g_Cfg.defaultFov;
}

void __fastcall HookedChaseCamUpdate(void* thisPtr, void* edx, float* pDt, void* pState) {
    __try {
        if (g_OrigChaseCamUpdate) {
            g_OrigChaseCamUpdate(thisPtr, pDt, pState);
        }

        if (g_FovActive.load() && pState) {
            uintptr_t stateAddr = reinterpret_cast<uintptr_t>(pState);
            if (IsValidUserPtr(stateAddr)) {
                float* pFov = reinterpret_cast<float*>(stateAddr + 0x54);
                
                // First time capture vanilla base FOV
                if (!g_InitializedFromGame && *pFov > 0.2f && *pFov < 3.0f) {
                    g_BaseFovRad = *pFov;
                    float vanillaDeg = g_BaseFovRad * RAD2DEG;
                    if (g_Cfg.defaultFov <= 0.0f) {
                        g_TargetFovDeg = vanillaDeg;
                        g_CurrentFovDeg = vanillaDeg;
                    }
                    g_InitializedFromGame = true;
                }

                // Apply current smoothly interpolated FOV
                *pFov = g_CurrentFovDeg * DEG2RAD;

                // Also update ChaseCamera internal target FOV at thisPtr + 0x138
                if (IsValidUserPtr(reinterpret_cast<uintptr_t>(thisPtr))) {
                    *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(thisPtr) + 0x138) = g_CurrentFovDeg * DEG2RAD;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void OnFrame(float dt) {
    if (dt <= 0.0f || dt > 0.2f) dt = 0.016f;

    DWORD fgPid = 0;
    HWND hForeground = GetForegroundWindow();
    if (hForeground) GetWindowThreadProcessId(hForeground, &fgPid);
    bool windowActive = (fgPid == GetCurrentProcessId());

    if (windowActive) {
        auto KeyDown = [](int vk) -> bool {
            return (vk != 0) && ((GetAsyncKeyState(vk) & 0x8000) != 0);
        };

        bool inc = KeyDown(g_Cfg.keyIncrease) || KeyDown(g_Cfg.altKeyIncrease);
        bool dec = KeyDown(g_Cfg.keyDecrease) || KeyDown(g_Cfg.altKeyDecrease);
        bool rst = KeyDown(g_Cfg.keyReset)    || KeyDown(g_Cfg.altKeyReset);

        if (inc) {
            g_TargetFovDeg += g_Cfg.adjustSpeed * dt;
        }
        if (dec) {
            g_TargetFovDeg -= g_Cfg.adjustSpeed * dt;
        }
        if (rst) {
            g_TargetFovDeg = g_Cfg.defaultFov;
        }

        g_TargetFovDeg = std::clamp(g_TargetFovDeg, g_Cfg.minFov, g_Cfg.maxFov);
    }

    // Smooth exponential decay interpolation (lerp)
    float blend = 1.0f - std::exp(-g_Cfg.smoothSpeed * dt);
    g_CurrentFovDeg += (g_TargetFovDeg - g_CurrentFovDeg) * blend;
}

} // namespace

extern "C" __declspec(dllexport) bool ModInit(const ModloaderAPI* api) {
    if (!api || api->apiVersion != DIRT3_MODLOADER_API_VERSION) {
        return false;
    }

    g_Api = api;
    g_Api->LogInfo("FovMod", "Smooth FOV Changer v1.0.0 initializing...");

    LoadConfig();

    g_Api->RegisterFrameTick(OnFrame);

    uintptr_t targetChaseCam = g_Api->Rva(0x00B31120);
    bool hookOk = g_Api->CreateHook(
        "ChaseCamera::update_fov",
        targetChaseCam,
        reinterpret_cast<void*>(HookedChaseCamUpdate),
        reinterpret_cast<void**>(&g_OrigChaseCamUpdate)
    );

    if (hookOk) {
        std::ostringstream ss;
        ss << "Ready: Smooth FOV Changer active! Keys: '[' / ']' or PageUp/PageDown, Reset: Backspace. Range: "
           << g_Cfg.minFov << " - " << g_Cfg.maxFov << " deg.";
        g_Api->LogInfo("FovMod", ss.str().c_str());
    } else {
        g_Api->LogError("FovMod", "Failed to hook ChaseCamera::update at 0x00B31120!");
        return false;
    }

    return true;
}
