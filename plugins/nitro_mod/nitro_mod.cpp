#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dirt3_modloader_api.h"
#include "overlay.hpp"

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

// neon::vehiclePhysics::Car field offsets (see REVERSING_NOTES_PHYSICS_VEHICLES.md)
static constexpr uintptr_t OFF_WORLD_VEL = 0x19E0; // Vector4 world velocity (m/s)
static constexpr uintptr_t OFF_FORWARD   = 0x1A20; // Vector4 orientation matrix forward row
static constexpr uintptr_t OFF_SPEED_MPS = 0x2404; // float forward speed (m/s)
static constexpr uintptr_t OFF_THROTTLE  = 0x1CB0; // float [0..1]

// ----------------------------------------------------------------------------
// Configuration (overridable via mods/nitro_mod.ini)
// ----------------------------------------------------------------------------
struct Config {
    int   keyBoost        = VK_LSHIFT;   // keyboard key to hold
    WORD  padButtonMask   = 0x0040;      // XINPUT_GAMEPAD_LEFT_THUMB (L3)
    float acceleration    = 14.0f;       // extra acceleration while boosting (m/s^2), ~1.4G
    float maxSpeedKmh     = 300.0f;      // boost fades out towards this speed (0 = no cap)
    float capacity        = 100.0f;      // tank size (units)
    float burnRate        = 25.0f;       // units per second while boosting (100/25 = 4s)
    float regenRate       = 10.0f;       // units per second while not boosting
    float regenDelay      = 1.5f;        // seconds after boost before regen starts
    float minToActivate   = 10.0f;       // min tank to start a new boost (prevents stutter)
    bool  infinite        = false;       // never drain the tank
    bool  invertDirection = false;       // manual override if forward vector is flipped
    bool  showHud         = true;        // draw nitro bar in modloader console
    bool  showOverlay     = true;        // draw nitro bar on game screen via DirectX overlay
    bool  speedVignette   = true;        // screen-edge speed vignette & motion streaks when boosting
    float hudX            = -1.0f;       // on-screen gauge X position (-1 = auto bottom-right)
    float hudY            = -1.0f;       // on-screen gauge Y position (-1 = auto bottom-right)
    float hudScale        = 1.0f;        // size multiplier for on-screen gauge
};

Config g_Cfg;

// ----------------------------------------------------------------------------
// Shared state (frame thread <-> physics worker threads)
// ----------------------------------------------------------------------------
std::atomic<bool>  g_BoostHeld{ false };
std::atomic<bool>  g_Boosting{ false };
std::atomic<float> g_Tank{ 100.0f };
std::atomic<float> g_SpeedKmh{ 0.0f };
std::atomic<float> g_TimeSincePhysics{ 10.0f };
std::atomic<void*> g_PlayerCar{ nullptr };

// Physics-thread-only state (player car is updated by a single job)
float g_RegenCooldown = 0.0f;
bool  g_Latched = false;           // boost engaged (until key released or tank empty)

// Automatic forward-vector sign calibration
float g_CalibSum = 0.0f;
float g_CalibTime = 0.0f;
float g_DirSign = 1.0f;
bool  g_Calibrated = false;

// ----------------------------------------------------------------------------
// XInput (loaded dynamically so no extra link dependency is needed)
// ----------------------------------------------------------------------------
struct XInputGamepadLite {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
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

bool IsPadBoostPressed() {
    if (!g_XInputGetState || g_Cfg.padButtonMask == 0) return false;
    for (DWORD i = 0; i < 4; ++i) {
        XInputStateLite st{};
        if (g_XInputGetState(i, &st) == ERROR_SUCCESS) {
            if (st.Gamepad.wButtons & g_Cfg.padButtonMask) return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static inline bool IsValidUserPtr(uintptr_t ptr) {
    return (ptr >= 0x00010000 && ptr < 0x7FFE0000);
}

void Log(const std::string& msg) {
    if (g_Api) g_Api->LogInfo("NitroMod", msg.c_str());
}

bool IsPlayerCar(void* carPtr) {
    void* player = g_Api->GetPlayerCar();
    if (player) {
        g_PlayerCar.store(player);
        return carPtr == player;
    }
    // Fallback: lock onto the first car we see (same approach as telemetry mod)
    void* expected = nullptr;
    if (g_PlayerCar.compare_exchange_strong(expected, carPtr)) return true;
    return g_PlayerCar.load() == carPtr;
}

void UpdateCalibration(const float vel[3], const float fwd[3], float speedMps, float throttle, float dt) {
    if (g_Calibrated || g_Cfg.invertDirection) return;
    float vlen = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    // Only sample when the car is clearly driving forwards under throttle
    if (throttle < 0.5f || vlen < 8.0f || std::abs(speedMps) < 8.0f) return;

    float d = (vel[0] * fwd[0] + vel[1] * fwd[1] + vel[2] * fwd[2]) / vlen;
    g_CalibSum += d * dt;
    g_CalibTime += dt;
    if (g_CalibTime >= 1.5f) {
        float avg = g_CalibSum / g_CalibTime;
        g_DirSign = (avg < -0.3f) ? -1.0f : 1.0f;
        g_Calibrated = true;
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2)
           << "Forward vector calibrated (avg dot=" << avg << ") -> direction sign " << g_DirSign;
        Log(ss.str());
    }
}

// ----------------------------------------------------------------------------
// Physics step (runs on physics worker threads, once per car per substep)
// ----------------------------------------------------------------------------
void OnCarPhysics(void* carPtr, float dt) {
    if (!carPtr || !g_Api || dt <= 0.0f || dt > 0.1f) return;
    uintptr_t car = reinterpret_cast<uintptr_t>(carPtr);
    if (!IsValidUserPtr(car)) return;
    if (!IsPlayerCar(carPtr)) return;

    g_TimeSincePhysics.store(0.0f);

    __try {
        float* vel = reinterpret_cast<float*>(car + OFF_WORLD_VEL);
        const float* fwdRaw = reinterpret_cast<const float*>(car + OFF_FORWARD);
        float speedMps = *reinterpret_cast<const float*>(car + OFF_SPEED_MPS);
        float throttle = *reinterpret_cast<const float*>(car + OFF_THROTTLE);

        float fwd[3] = { fwdRaw[0], fwdRaw[1], fwdRaw[2] };
        float flen = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (flen < 0.5f || flen > 2.0f || !std::isfinite(flen)) return; // garbage / not a car
        fwd[0] /= flen; fwd[1] /= flen; fwd[2] /= flen;

        UpdateCalibration(vel, fwd, speedMps, throttle, dt);

        float speedKmh = std::abs(speedMps) * 3.6f;
        g_SpeedKmh.store(speedKmh);

        // --- Tank / activation logic ---
        float tank = g_Tank.load();
        bool held = g_BoostHeld.load();

        if (!held) {
            g_Latched = false;
        } else if (!g_Latched && (tank >= g_Cfg.minToActivate || g_Cfg.infinite)) {
            g_Latched = true;
        }
        if (!g_Cfg.infinite && tank <= 0.0f) {
            g_Latched = false;
        }

        bool boosting = g_Latched;

        if (boosting) {
            if (!g_Cfg.infinite) {
                tank = std::max(0.0f, tank - g_Cfg.burnRate * dt);
            }
            g_RegenCooldown = g_Cfg.regenDelay;

            // Fade boost out as we approach the speed cap
            float scale = 1.0f;
            if (g_Cfg.maxSpeedKmh > 0.0f) {
                float fadeStart = g_Cfg.maxSpeedKmh * 0.85f;
                if (speedKmh >= g_Cfg.maxSpeedKmh) {
                    scale = 0.0f;
                } else if (speedKmh > fadeStart) {
                    scale = (g_Cfg.maxSpeedKmh - speedKmh) / (g_Cfg.maxSpeedKmh - fadeStart);
                }
            }

            float sign = g_Cfg.invertDirection ? -1.0f : g_DirSign;
            float dv = g_Cfg.acceleration * scale * dt * sign;
            vel[0] += fwd[0] * dv;
            vel[1] += fwd[1] * dv;
            vel[2] += fwd[2] * dv;
        } else {
            if (g_RegenCooldown > 0.0f) {
                g_RegenCooldown -= dt;
            } else {
                tank = std::min(g_Cfg.capacity, tank + g_Cfg.regenRate * dt);
            }
        }

        g_Tank.store(tank);
        g_Boosting.store(boosting);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ----------------------------------------------------------------------------
// Console HUD (drawn below the telemetry dashboard)
// ----------------------------------------------------------------------------
void RenderHud() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hConsole || hConsole == INVALID_HANDLE_VALUE) return;

    float tank = g_Tank.load();
    float ratio = (g_Cfg.capacity > 0.0f) ? std::clamp(tank / g_Cfg.capacity, 0.0f, 1.0f) : 0.0f;
    constexpr int WIDTH = 40;
    int filled = static_cast<int>(ratio * WIDTH);

    std::string bar = "[";
    for (int i = 0; i < WIDTH; ++i) bar += (i < filled) ? '#' : '.';
    bar += "]";

    std::ostringstream ss;
    ss << "  NITRO: " << bar << " " << std::setw(3) << static_cast<int>(ratio * 100.0f) << "%  "
       << (g_Boosting.load() ? ">>> BOOST <<<" : (g_Cfg.infinite ? "(infinite)   " : "             "));
    std::string line = ss.str();
    if (line.size() < 79) line.append(79 - line.size(), ' ');

    CONSOLE_SCREEN_BUFFER_INFO info{};
    COORD prev = { 0, 0 };
    if (GetConsoleScreenBufferInfo(hConsole, &info)) prev = info.dwCursorPosition;

    COORD pos = { 0, 17 };
    SetConsoleCursorPosition(hConsole, pos);
    DWORD written = 0;
    WriteConsoleA(hConsole, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    SetConsoleCursorPosition(hConsole, prev);
}

// ----------------------------------------------------------------------------
// Frame tick (main thread): input polling + HUD
// ----------------------------------------------------------------------------
void OnFrame(float dt) {
    bool keyDown = g_Cfg.keyBoost != 0 && (GetAsyncKeyState(g_Cfg.keyBoost) & 0x8000) != 0;
    bool padDown = IsPadBoostPressed();
    bool held = keyDown || padDown;

    // Ignore input when the game window isn't focused
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    if (fgPid != GetCurrentProcessId()) held = false;

    g_BoostHeld.store(held);

    float since = g_TimeSincePhysics.load() + dt;
    g_TimeSincePhysics.store(since);
    if (since > 0.5f) {
        // Left the race / back in menu: refill and forget the car
        g_PlayerCar.store(nullptr);
        g_Tank.store(g_Cfg.capacity);
        g_Boosting.store(false);
    }

    static float hudTimer = 0.0f;
    hudTimer += dt;
    if (g_Cfg.showHud && since <= 0.5f && hudTimer >= 0.05f) {
        hudTimer = 0.0f;
        RenderHud();
    }
}

// ----------------------------------------------------------------------------
// On-Screen DirectX Overlay Rendering
// ----------------------------------------------------------------------------
constexpr uint32_t ARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8)  |
           static_cast<uint32_t>(b);
}

static const uint8_t* GetGlyphRows(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    switch (c) {
    case '0': { static const uint8_t g[] = { 0b111, 0b101, 0b101, 0b101, 0b111 }; return g; }
    case '1': { static const uint8_t g[] = { 0b010, 0b110, 0b010, 0b010, 0b111 }; return g; }
    case '2': { static const uint8_t g[] = { 0b111, 0b001, 0b111, 0b100, 0b111 }; return g; }
    case '3': { static const uint8_t g[] = { 0b111, 0b001, 0b111, 0b001, 0b111 }; return g; }
    case '4': { static const uint8_t g[] = { 0b101, 0b101, 0b111, 0b001, 0b001 }; return g; }
    case '5': { static const uint8_t g[] = { 0b111, 0b100, 0b111, 0b001, 0b111 }; return g; }
    case '6': { static const uint8_t g[] = { 0b111, 0b100, 0b111, 0b101, 0b111 }; return g; }
    case '7': { static const uint8_t g[] = { 0b111, 0b001, 0b010, 0b010, 0b010 }; return g; }
    case '8': { static const uint8_t g[] = { 0b111, 0b101, 0b111, 0b101, 0b111 }; return g; }
    case '9': { static const uint8_t g[] = { 0b111, 0b101, 0b111, 0b001, 0b111 }; return g; }
    case '%': { static const uint8_t g[] = { 0b101, 0b001, 0b010, 0b100, 0b101 }; return g; }
    case 'A': { static const uint8_t g[] = { 0b111, 0b101, 0b111, 0b101, 0b101 }; return g; }
    case 'B': { static const uint8_t g[] = { 0b110, 0b101, 0b110, 0b101, 0b110 }; return g; }
    case 'C': { static const uint8_t g[] = { 0b111, 0b100, 0b100, 0b100, 0b111 }; return g; }
    case 'D': { static const uint8_t g[] = { 0b110, 0b101, 0b101, 0b101, 0b110 }; return g; }
    case 'E': { static const uint8_t g[] = { 0b111, 0b100, 0b110, 0b100, 0b111 }; return g; }
    case 'F': { static const uint8_t g[] = { 0b111, 0b100, 0b110, 0b100, 0b100 }; return g; }
    case 'G': { static const uint8_t g[] = { 0b111, 0b100, 0b101, 0b101, 0b111 }; return g; }
    case 'H': { static const uint8_t g[] = { 0b101, 0b101, 0b111, 0b101, 0b101 }; return g; }
    case 'I': { static const uint8_t g[] = { 0b111, 0b010, 0b010, 0b010, 0b111 }; return g; }
    case 'K': { static const uint8_t g[] = { 0b101, 0b110, 0b100, 0b110, 0b101 }; return g; }
    case 'L': { static const uint8_t g[] = { 0b100, 0b100, 0b100, 0b100, 0b111 }; return g; }
    case 'M': { static const uint8_t g[] = { 0b101, 0b111, 0b101, 0b101, 0b101 }; return g; }
    case 'N': { static const uint8_t g[] = { 0b111, 0b101, 0b101, 0b101, 0b101 }; return g; }
    case 'O': { static const uint8_t g[] = { 0b111, 0b101, 0b101, 0b101, 0b111 }; return g; }
    case 'P': { static const uint8_t g[] = { 0b111, 0b101, 0b111, 0b100, 0b100 }; return g; }
    case 'R': { static const uint8_t g[] = { 0b110, 0b101, 0b110, 0b101, 0b101 }; return g; }
    case 'S': { static const uint8_t g[] = { 0b111, 0b100, 0b111, 0b001, 0b111 }; return g; }
    case 'T': { static const uint8_t g[] = { 0b111, 0b010, 0b010, 0b010, 0b010 }; return g; }
    case 'U': { static const uint8_t g[] = { 0b101, 0b101, 0b101, 0b101, 0b111 }; return g; }
    case 'V': { static const uint8_t g[] = { 0b101, 0b101, 0b101, 0b101, 0b010 }; return g; }
    case 'W': { static const uint8_t g[] = { 0b101, 0b101, 0b101, 0b111, 0b101 }; return g; }
    case 'X': { static const uint8_t g[] = { 0b101, 0b101, 0b010, 0b101, 0b101 }; return g; }
    case 'Y': { static const uint8_t g[] = { 0b101, 0b101, 0b111, 0b010, 0b010 }; return g; }
    case 'Z': { static const uint8_t g[] = { 0b111, 0b001, 0b010, 0b100, 0b111 }; return g; }
    case '>': { static const uint8_t g[] = { 0b100, 0b010, 0b001, 0b010, 0b100 }; return g; }
    case '<': { static const uint8_t g[] = { 0b001, 0b010, 0b100, 0b010, 0b001 }; return g; }
    case '-': { static const uint8_t g[] = { 0b000, 0b000, 0b111, 0b000, 0b000 }; return g; }
    case '.': { static const uint8_t g[] = { 0b000, 0b000, 0b000, 0b000, 0b010 }; return g; }
    case '!': { static const uint8_t g[] = { 0b010, 0b010, 0b010, 0b000, 0b010 }; return g; }
    default: return nullptr;
    }
}

float DrawText(overlay::DrawList& list, const char* str, float x, float y, float px, uint32_t color) {
    float curX = x;
    while (*str) {
        char ch = *str++;
        if (ch == ' ') {
            curX += px * 3.0f;
            continue;
        }
        const uint8_t* rows = GetGlyphRows(ch);
        if (rows) {
            for (int r = 0; r < 5; ++r) {
                uint8_t mask = rows[r];
                for (int c = 0; c < 3; ++c) {
                    if (mask & (1 << (2 - c))) {
                        list.Rect(curX + c * px, y + r * px, px, px, color);
                    }
                }
            }
        }
        curX += px * 4.0f;
    }
    return curX - x;
}

void OnDrawOverlay(overlay::DrawList& list, float screenW, float screenH, float dt) {
    static float s_VisualBoost = 0.0f;
    static float s_AnimTime = 0.0f;
    s_AnimTime += dt;

    bool boosting = g_Boosting.load();
    if (boosting) {
        s_VisualBoost = std::min(1.0f, s_VisualBoost + dt * 8.0f);
    } else {
        s_VisualBoost = std::max(0.0f, s_VisualBoost - dt * 3.5f);
    }

    // ------------------------------------------------------------------------
    // Speed Vignette & Motion Streaks around screen edges
    // ------------------------------------------------------------------------
    if (g_Cfg.speedVignette && s_VisualBoost > 0.005f) {
        float edgeW = screenW * 0.16f;
        float edgeH = screenH * 0.16f;
        uint8_t aEdge = static_cast<uint8_t>(std::clamp(s_VisualBoost * 120.0f, 0.0f, 255.0f));
        uint8_t aDark = static_cast<uint8_t>(std::clamp(s_VisualBoost * 150.0f, 0.0f, 255.0f));

        // Left border
        list.RectH(0.0f, 0.0f, edgeW, screenH, ARGB(aEdge, 0, 160, 240), ARGB(0, 0, 160, 240));
        list.RectH(0.0f, 0.0f, edgeW * 0.45f, screenH, ARGB(aDark, 0, 15, 35), ARGB(0, 0, 15, 35));

        // Right border
        list.RectH(screenW - edgeW, 0.0f, edgeW, screenH, ARGB(0, 0, 160, 240), ARGB(aEdge, 0, 160, 240));
        list.RectH(screenW - edgeW * 0.45f, 0.0f, edgeW * 0.45f, screenH, ARGB(0, 0, 15, 35), ARGB(aDark, 0, 15, 35));

        // Top border
        list.RectV(0.0f, 0.0f, screenW, edgeH, ARGB(static_cast<uint8_t>(aEdge * 0.7f), 0, 140, 220), ARGB(0, 0, 140, 220));

        // Bottom border
        list.RectV(0.0f, screenH - edgeH, screenW, edgeH, ARGB(0, 0, 140, 220), ARGB(static_cast<uint8_t>(aEdge * 0.7f), 0, 140, 220));

        // Speed streaks streaming along screen edges
        float speedRatio = std::clamp(g_SpeedKmh.load() / 100.0f, 0.6f, 2.5f);
        for (int i = 0; i < 14; ++i) {
            float yPhase = std::fmod(i * 0.0714f + s_AnimTime * 0.3f * speedRatio, 1.0f);
            float y = yPhase * screenH;
            float streakLen = (50.0f + 140.0f * (0.5f + 0.5f * std::sin(i * 3.7f + s_AnimTime * 10.0f)));
            float alphaStr = s_VisualBoost * (0.35f + 0.45f * (0.5f + 0.5f * std::cos(i * 5.3f + s_AnimTime * 14.0f)));
            uint8_t a = static_cast<uint8_t>(std::clamp(alphaStr * 255.0f, 0.0f, 255.0f));
            if (a > 10) {
                list.RectH(0.0f, y, streakLen, 2.0f, ARGB(a, 200, 245, 255), ARGB(0, 0, 180, 255));
                list.RectH(screenW - streakLen, y, streakLen, 2.0f, ARGB(0, 0, 180, 255), ARGB(a, 200, 245, 255));
            }
        }
    }

    // ------------------------------------------------------------------------
    // On-Screen Nitro HUD Gauge
    // ------------------------------------------------------------------------
    // Only render HUD when active in a race (physics running within last 0.5s)
    if (!g_Cfg.showOverlay || g_TimeSincePhysics.load() > 0.5f) return;

    float scale = std::clamp(g_Cfg.hudScale, 0.5f, 3.0f);
    float barW = 200.0f * scale;
    float barH = 14.0f * scale;
    float pad = 40.0f * scale;

    float x = (g_Cfg.hudX >= 0.0f) ? g_Cfg.hudX : (screenW - barW - pad);
    float y = (g_Cfg.hudY >= 0.0f) ? g_Cfg.hudY : (screenH - barH - pad);

    float bgX = x - 6.0f * scale;
    float bgY = y - 18.0f * scale;
    float bgW = barW + 12.0f * scale;
    float bgH = barH + 24.0f * scale;

    // Outer backing box
    list.Rect(bgX, bgY, bgW, bgH, ARGB(190, 10, 14, 20));
    list.Frame(bgX, bgY, bgW, bgH, 1.0f * scale, ARGB(210, 55, 70, 90));

    // Outer pulsing glow when boosting
    if (s_VisualBoost > 0.05f) {
        uint8_t aGlow = static_cast<uint8_t>(std::clamp(s_VisualBoost * 140.0f, 0.0f, 255.0f));
        list.Frame(bgX - 2.0f, bgY - 2.0f, bgW + 4.0f, bgH + 4.0f, 1.5f * scale, ARGB(aGlow, 0, 210, 255));
    }

    // Header labels
    float fontPx = 2.0f * scale;
    if (boosting) {
        float pulse = 0.5f + 0.5f * std::sin(s_AnimTime * 15.0f);
        uint8_t gCol = static_cast<uint8_t>(180 + pulse * 75);
        DrawText(list, ">> BOOST <<", bgX + 6.0f * scale, bgY + 4.0f * scale, fontPx, ARGB(255, 255, gCol, 60));
    } else {
        DrawText(list, "NITRO", bgX + 6.0f * scale, bgY + 4.0f * scale, fontPx, ARGB(240, 0, 210, 255));
    }

    float tank = g_Tank.load();
    float ratio = (g_Cfg.capacity > 0.0f) ? std::clamp(tank / g_Cfg.capacity, 0.0f, 1.0f) : 0.0f;
    char pctBuf[16];
    snprintf(pctBuf, sizeof(pctBuf), "%d%%", static_cast<int>(std::round(ratio * 100.0f)));
    DrawText(list, pctBuf, bgX + bgW - 34.0f * scale, bgY + 4.0f * scale, fontPx, ARGB(240, 220, 230, 240));

    // Inner slot background
    list.Rect(x, y, barW, barH, ARGB(230, 16, 20, 26));
    list.Frame(x, y, barW, barH, 1.0f, ARGB(180, 35, 45, 58));

    // Gauge fill bar
    float fillW = barW * ratio;
    if (fillW > 1.5f) {
        float innerW = fillW - 2.0f;
        float innerH = barH - 2.0f;
        if (boosting) {
            float pulse = 0.5f + 0.5f * std::sin(s_AnimTime * 20.0f);
            uint32_t colL = ARGB(255, 0, 200, 255);
            uint32_t colR = ARGB(255, static_cast<uint8_t>(100 + pulse * 155), 240, 255);
            list.RectH(x + 1.0f, y + 1.0f, innerW, innerH, colL, colR);
            // Specular gloss strip
            list.Rect(x + 1.0f, y + 1.0f, innerW, 2.0f * scale, ARGB(200, 220, 250, 255));
        } else if (tank < g_Cfg.minToActivate) {
            list.RectH(x + 1.0f, y + 1.0f, innerW, innerH, ARGB(240, 210, 60, 40), ARGB(240, 160, 30, 20));
        } else {
            list.RectH(x + 1.0f, y + 1.0f, innerW, innerH, ARGB(240, 0, 190, 255), ARGB(240, 0, 110, 210));
            list.Rect(x + 1.0f, y + 1.0f, innerW, 2.0f * scale, ARGB(150, 180, 235, 255));
        }
    }

    // Segment divider ticks (at 20%, 40%, 60%, 80%)
    for (int seg = 1; seg <= 4; ++seg) {
        float tx = x + (barW * seg / 5.0f);
        list.Rect(tx, y, 1.0f, barH, ARGB(210, 12, 16, 22));
    }
}

// ----------------------------------------------------------------------------
// Config loading
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
        { "ENTER", VK_RETURN }, { "BACKSPACE", VK_BACK },
        { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "END", VK_END },
        { "NUM0", VK_NUMPAD0 }, { "NUM1", VK_NUMPAD1 }, { "NUM2", VK_NUMPAD2 },
        { "NUM3", VK_NUMPAD3 }, { "NUM4", VK_NUMPAD4 }, { "NUM5", VK_NUMPAD5 },
        { "NUM6", VK_NUMPAD6 }, { "NUM7", VK_NUMPAD7 }, { "NUM8", VK_NUMPAD8 },
        { "NUM9", VK_NUMPAD9 }, { "MOUSE4", VK_XBUTTON1 }, { "MOUSE5", VK_XBUTTON2 },
    };
    for (const auto& n : names) {
        if (v == n.name) return n.vk;
    }
    if (v.size() == 1 && (std::isalnum(static_cast<unsigned char>(v[0])))) return v[0];
    if (v.size() >= 2 && v[0] == 'F' && std::isdigit(static_cast<unsigned char>(v[1]))) {
        int f = std::stoi(v.substr(1));
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    return std::stoi(v, nullptr, 0); // raw virtual-key code, e.g. 0xA0
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
    std::string path = std::string(g_Api->GetModsDirectory()) + "\\nitro_mod.ini";
    std::ifstream file(path);
    if (!file.is_open()) {
        Log("nitro_mod.ini not found, using defaults");
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
            if      (key == "Key")             g_Cfg.keyBoost = ParseKey(val);
            else if (key == "GamepadButton")   g_Cfg.padButtonMask = ParsePadButton(val);
            else if (key == "Acceleration")    g_Cfg.acceleration = std::clamp(std::stof(val), 0.0f, 100.0f);
            else if (key == "MaxSpeedKmh")     g_Cfg.maxSpeedKmh = std::max(0.0f, std::stof(val));
            else if (key == "Capacity")        g_Cfg.capacity = std::max(1.0f, std::stof(val));
            else if (key == "BurnRate")        g_Cfg.burnRate = std::max(0.0f, std::stof(val));
            else if (key == "RegenRate")       g_Cfg.regenRate = std::max(0.0f, std::stof(val));
            else if (key == "RegenDelay")      g_Cfg.regenDelay = std::max(0.0f, std::stof(val));
            else if (key == "MinToActivate")   g_Cfg.minToActivate = std::max(0.0f, std::stof(val));
            else if (key == "Infinite")        g_Cfg.infinite = ParseBool(val);
            else if (key == "InvertDirection") g_Cfg.invertDirection = ParseBool(val);
            else if (key == "ShowHud")         g_Cfg.showHud = ParseBool(val);
            else if (key == "ShowOverlay")     g_Cfg.showOverlay = ParseBool(val);
            else if (key == "SpeedVignette")   g_Cfg.speedVignette = ParseBool(val);
            else if (key == "HudX")            g_Cfg.hudX = std::stof(val);
            else if (key == "HudY")            g_Cfg.hudY = std::stof(val);
            else if (key == "HudScale")        g_Cfg.hudScale = std::clamp(std::stof(val), 0.5f, 3.0f);
        } catch (...) {
            Log("Invalid value for '" + key + "': " + val);
        }
    }
    g_Cfg.minToActivate = std::min(g_Cfg.minToActivate, g_Cfg.capacity);
}

} // namespace

extern "C" __declspec(dllexport) bool ModInit(const ModloaderAPI* api) {
    if (!api || api->apiVersion != DIRT3_MODLOADER_API_VERSION) {
        return false;
    }

    g_Api = api;
    Log("Nitro Mod v1.0.0 initializing...");

    LoadConfig();
    LoadXInput();
    g_Tank.store(g_Cfg.capacity);

    g_Api->RegisterCarPhysicsStep(OnCarPhysics);
    g_Api->RegisterFrameTick(OnFrame);

    if (g_Cfg.showOverlay || g_Cfg.speedVignette) {
        overlay::Start(api, OnDrawOverlay);
        Log("DirectX overlay initialized for nitro HUD & speed effects");
    }

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1)
       << "Ready: key VK=0x" << std::hex << g_Cfg.keyBoost << std::dec
       << ", pad mask=0x" << std::hex << g_Cfg.padButtonMask << std::dec
       << ", accel=" << g_Cfg.acceleration << " m/s^2"
       << ", tank=" << g_Cfg.capacity << " (burn " << g_Cfg.burnRate << "/s, regen " << g_Cfg.regenRate << "/s)"
       << (g_XInputGetState ? "" : " [XInput unavailable]");
    Log(ss.str());
    return true;
}
