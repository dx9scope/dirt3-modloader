#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dirt3_modloader_api.h"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>

namespace {

const ModloaderAPI* g_Api = nullptr;

// neon::vehiclePhysics::Car field offsets
static constexpr uintptr_t OFF_SPEED_MPS    = 0x2404; // float (m/s)
static constexpr uintptr_t OFF_RPM          = 0x1C80; // float (rad/s)
static constexpr uintptr_t OFF_MAX_RPM      = 0x1C18; // float (rad/s)
static constexpr uintptr_t OFF_GEAR         = 0x1D58; // int (10=R, 0=N, 1..9=1..9)
static constexpr uintptr_t OFF_THROTTLE     = 0x1CB0; // float [0.0, 1.0]
static constexpr uintptr_t OFF_STEER        = 0x1308; // float [-1.0, 1.0]
static constexpr uintptr_t OFF_BRAKE        = 0x1B64; // float [0.0, 1.0]
static constexpr uintptr_t OFF_CLUTCH       = 0x130C; // float [0.0, 1.0]
static constexpr uintptr_t OFF_ACCEL_X      = 0x02A0; // float
static constexpr uintptr_t OFF_ACCEL_Z      = 0x02A8; // float
static constexpr uintptr_t OFF_POS_X        = 0x1A30; // float (world position X)
static constexpr uintptr_t OFF_POS_Y        = 0x1A34; // float (world position Y)
static constexpr uintptr_t OFF_POS_Z        = 0x1A38; // float (world position Z)

struct TelemetryData {
    bool inVehicle{ false };
    float speedKmh{ 0.0f };
    float speedMph{ 0.0f };
    float speedMps{ 0.0f };
    float rpm{ 0.0f };
    float maxRpm{ 8500.0f };
    int gear{ 0 };
    std::string gearName{ "N" };
    float throttle{ 0.0f };
    float brake{ 0.0f };
    float steer{ 0.0f };
    float clutch{ 0.0f };
    float gLat{ 0.0f };
    float gLong{ 0.0f };
    float posX{ 0.0f };
    float posY{ 0.0f };
    float posZ{ 0.0f };
};

std::mutex g_Mutex;
TelemetryData g_CurrentData;
void* g_PlayerCarPtr = nullptr;
float g_TimeSinceLastPhysics = 0.0f;
float g_HudTimer = 0.0f;
bool g_HudEnabled = true;
bool g_ConsoleInitialized = false;

struct RawCarPhysics {
    float speedMps{ 0.0f };
    float rpm{ 0.0f };
    float maxRpm{ 8500.0f };
    int gear{ 1 };
    float throttle{ 0.0f };
    float steer{ 0.0f };
    float brake{ 0.0f };
    float clutch{ 0.0f };
    float accelX{ 0.0f };
    float accelZ{ 0.0f };
    float posX{ 0.0f };
    float posY{ 0.0f };
    float posZ{ 0.0f };
};

static inline bool IsValidUserPtr(uintptr_t ptr) {
    return (ptr >= 0x00010000 && ptr < 0x7FFE0000);
}

bool ReadCarPhysicsRaw(uintptr_t car, RawCarPhysics* out) {
    if (!IsValidUserPtr(car)) return false;
    __try {
        out->speedMps = *reinterpret_cast<const float*>(car + OFF_SPEED_MPS);
        out->rpm      = *reinterpret_cast<const float*>(car + OFF_RPM);
        out->maxRpm   = *reinterpret_cast<const float*>(car + OFF_MAX_RPM);
        out->gear     = *reinterpret_cast<const int*>(car + OFF_GEAR);
        out->throttle = *reinterpret_cast<const float*>(car + OFF_THROTTLE);
        out->steer    = *reinterpret_cast<const float*>(car + OFF_STEER);
        out->brake    = *reinterpret_cast<const float*>(car + OFF_BRAKE);
        out->clutch   = *reinterpret_cast<const float*>(car + OFF_CLUTCH);
        out->accelX   = *reinterpret_cast<const float*>(car + OFF_ACCEL_X);
        out->accelZ   = *reinterpret_cast<const float*>(car + OFF_ACCEL_Z);
        out->posX     = *reinterpret_cast<const float*>(car + OFF_POS_X);
        out->posY     = *reinterpret_cast<const float*>(car + OFF_POS_Y);
        out->posZ     = *reinterpret_cast<const float*>(car + OFF_POS_Z);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string MakeBar(float val, int width, char fill = '=', char empty = '-') {
    val = std::clamp(val, 0.0f, 1.0f);
    int filled = static_cast<int>(val * width);
    std::string s;
    s.reserve(width + 2);
    s += '[';
    for (int i = 0; i < width; ++i) {
        s += (i < filled) ? fill : empty;
    }
    s += ']';
    return s;
}

std::string MakeRpmBar(float rpm, float maxRpm, int width) {
    float ratio = (maxRpm > 0.0f) ? std::clamp(rpm / maxRpm, 0.0f, 1.0f) : 0.0f;
    int filled = static_cast<int>(ratio * width);
    std::string s;
    s.reserve(width + 2);
    s += '[';
    for (int i = 0; i < width; ++i) {
        if (i < filled) {
            s += (i >= width - 3) ? '!' : '|';
        } else {
            s += '.';
        }
    }
    s += ']';
    return s;
}

std::string MakeSteerBar(float steer, int halfWidth) {
    steer = std::clamp(steer, -1.0f, 1.0f);
    std::string s = "<";
    int leftFilled = (steer < 0.0f) ? static_cast<int>(-steer * halfWidth) : 0;
    int rightFilled = (steer > 0.0f) ? static_cast<int>(steer * halfWidth) : 0;
    for (int i = halfWidth; i > 0; --i) {
        s += (i <= leftFilled) ? '=' : '.';
    }
    s += '|';
    for (int i = 1; i <= halfWidth; ++i) {
        s += (i <= rightFilled) ? '=' : '.';
    }
    s += '>';
    return s;
}

std::string PadRight(std::string_view text, size_t totalWidth) {
    std::string s(text);
    if (s.size() < totalWidth) {
        s.append(totalWidth - s.size(), ' ');
    }
    return s;
}

void RenderConsoleHud(const TelemetryData& t) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hConsole || hConsole == INVALID_HANDLE_VALUE) return;

    if (!g_ConsoleInitialized) {
        CONSOLE_CURSOR_INFO cursorInfo;
        GetConsoleCursorInfo(hConsole, &cursorInfo);
        cursorInfo.bVisible = FALSE;
        SetConsoleCursorInfo(hConsole, &cursorInfo);
        g_ConsoleInitialized = true;
    }

    constexpr size_t LINE_WIDTH = 79;
    std::ostringstream ss;

    auto AddLine = [&](std::string_view text) {
        ss << PadRight(text, LINE_WIDTH) << "\r\n";
    };

    AddLine("===============================================================================");
    AddLine("                    DiRT 3 REAL-TIME TELEMETRY DASHBOARD                      ");
    AddLine("===============================================================================");

    if (!t.inVehicle) {
        AddLine("  STATUS: [ WAITING FOR VEHICLE / IN MENU OR LOADING SCREEN ]                 ");
        AddLine("  Hint: Enter a race or Battersea Free-Run to view live telemetry.             ");
        AddLine("  Hotkey: [F9] Toggle HUD On/Off                                               ");
        AddLine("-------------------------------------------------------------------------------");
        for (int i = 0; i < 11; ++i) {
            AddLine("");
        }
        AddLine("===============================================================================");
    } else {
        // Row 1: Speed & Gear
        {
            std::ostringstream r1;
            r1 << "  SPEED: " << std::setw(3) << static_cast<int>(std::round(t.speedKmh)) << " km/h ("
               << std::setw(3) << static_cast<int>(std::round(t.speedMph)) << " mph)   "
               << "|  GEAR: [ " << t.gearName << " ]   "
               << "|  HOTKEY: [F9] Toggle HUD";
            AddLine(r1.str());
        }

        // Row 2: RPM Bar
        {
            std::ostringstream r2;
            r2 << "  RPM:   " << std::setw(5) << static_cast<int>(std::round(t.rpm))
               << " / " << static_cast<int>(std::round(t.maxRpm)) << "  "
               << MakeRpmBar(t.rpm, t.maxRpm, 30)
               << " " << std::setw(3) << static_cast<int>((t.rpm / t.maxRpm) * 100.0f) << "%";
            AddLine(r2.str());
        }

        AddLine("-------------------------------------------------------------------------------");

        // Row 3: Pedals & Inputs Header
        AddLine("  DRIVER INPUTS & CONTROLS:");

        // Row 4: Throttle
        {
            std::ostringstream r;
            r << "    THROTTLE: " << MakeBar(t.throttle, 24, '=', '-')
              << " " << std::setw(3) << static_cast<int>(t.throttle * 100.0f) << "%";
            AddLine(r.str());
        }

        // Row 5: Brake
        {
            std::ostringstream r;
            r << "    BRAKE:    " << MakeBar(t.brake, 24, '#', '-')
              << " " << std::setw(3) << static_cast<int>(t.brake * 100.0f) << "%";
            AddLine(r.str());
        }

        // Row 6: Clutch
        {
            std::ostringstream r;
            r << "    CLUTCH:   " << MakeBar(t.clutch, 24, '=', '-')
              << " " << std::setw(3) << static_cast<int>(t.clutch * 100.0f) << "%";
            AddLine(r.str());
        }

        // Row 7: Steer
        {
            std::ostringstream r;
            int steerPct = static_cast<int>(std::abs(t.steer) * 100.0f);
            const char* dir = (t.steer < -0.02f) ? "L" : (t.steer > 0.02f) ? "R" : "C";
            r << "    STEER:    " << MakeSteerBar(t.steer, 11)
              << " " << std::setw(3) << steerPct << "% [" << dir << "]";
            AddLine(r.str());
        }

        AddLine("-------------------------------------------------------------------------------");

        // Row 8: Dynamics & Forces
        AddLine("  VEHICLE DYNAMICS & G-FORCES:");

        // Row 9: G-forces
        {
            std::ostringstream r;
            r << std::fixed << std::setprecision(2);
            const char* latDir = (t.gLat > 0.05f) ? "Right" : (t.gLat < -0.05f) ? "Left " : "Cent ";
            const char* lonDir = (t.gLong > 0.05f) ? "Accel" : (t.gLong < -0.05f) ? "Brake" : "Coast";
            r << "    LATERAL G:      " << std::setw(5) << t.gLat << " G (" << latDir << ")   "
              << "| LONGITUDINAL G: " << std::setw(5) << t.gLong << " G (" << lonDir << ")";
            AddLine(r.str());
        }

        // Row 10: World Coordinates
        {
            std::ostringstream r;
            r << std::fixed << std::setprecision(2)
              << "    POSITION (XYZ): X=" << std::setw(8) << t.posX
              << "  Y=" << std::setw(7) << t.posY
              << "  Z=" << std::setw(8) << t.posZ;
            AddLine(r.str());
        }

        AddLine("===============================================================================");
    }

    std::string out = ss.str();
    COORD coord = { 0, 0 };
    SetConsoleCursorPosition(hConsole, coord);

    DWORD written = 0;
    WriteConsoleA(hConsole, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
}

void OnCarPhysics(void* carPtr, float dt) {
    if (!carPtr || !g_Api) return;
    uintptr_t car = reinterpret_cast<uintptr_t>(carPtr);
    if (!IsValidUserPtr(car)) return;

    void* playerCar = g_Api->GetPlayerCar();
    if (playerCar) {
        if (carPtr != playerCar) return;
        g_PlayerCarPtr = playerCar;
    } else {
        if (!g_PlayerCarPtr) g_PlayerCarPtr = carPtr;
        if (carPtr != g_PlayerCarPtr) return;
    }

    RawCarPhysics raw;
    if (!ReadCarPhysicsRaw(car, &raw)) return;

    TelemetryData data;
    data.inVehicle = true;
    data.speedMps = raw.speedMps;
    data.speedKmh = data.speedMps * 3.6f;
    data.speedMph = data.speedMps * 2.236936f;

    constexpr float RADS_TO_RPM = 9.54929658551f;
    data.rpm = std::max(0.0f, raw.rpm * RADS_TO_RPM);
    data.maxRpm = raw.maxRpm * RADS_TO_RPM;
    if (data.maxRpm < 2000.0f) {
        data.maxRpm = 8500.0f;
    }

    data.gear = raw.gear;
    if (raw.gear == 10) {
        data.gear = -1;
        data.gearName = "R";
    } else if (raw.gear == 0) {
        data.gear = 0;
        data.gearName = "N";
    } else if (raw.gear >= 1 && raw.gear <= 9) {
        data.gear = raw.gear;
        data.gearName = std::to_string(raw.gear);
    } else {
        data.gear = raw.gear;
        data.gearName = "?";
    }

    data.throttle = std::clamp(raw.throttle, 0.0f, 1.0f);
    data.steer = std::clamp(-raw.steer, -1.0f, 1.0f);
    data.brake = std::clamp(raw.brake, 0.0f, 1.0f);
    data.clutch = std::clamp(raw.clutch, 0.0f, 1.0f);

    constexpr float K_INV_G = 1.0f / 9.80665f;
    data.gLat = raw.accelX * K_INV_G;
    data.gLong = raw.accelZ * K_INV_G;

    data.posX = raw.posX;
    data.posY = raw.posY;
    data.posZ = raw.posZ;

    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_CurrentData = data;
        g_TimeSinceLastPhysics = 0.0f;
    }
}

void OnFrame(float dt) {
    // F9 hotkey debounce to toggle HUD
    static bool f9WasDown = false;
    bool f9IsDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9IsDown && !f9WasDown) {
        g_HudEnabled = !g_HudEnabled;
        if (!g_HudEnabled && g_Api) {
            g_Api->LogInfo("Telemetry", "Live HUD hidden (press F9 to restore)");
        }
    }
    f9WasDown = f9IsDown;

    g_TimeSinceLastPhysics += dt;
    if (g_TimeSinceLastPhysics > 0.5f) {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_CurrentData.inVehicle = false;
        g_PlayerCarPtr = nullptr;
    }

    if (!g_HudEnabled) return;

    g_HudTimer += dt;
    if (g_HudTimer >= 0.033f) {
        g_HudTimer = 0.0f;
        TelemetryData snap;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            snap = g_CurrentData;
        }
        RenderConsoleHud(snap);
    }
}

} // namespace

extern "C" __declspec(dllexport) bool ModInit(const ModloaderAPI* api) {
    if (!api || api->apiVersion != DIRT3_MODLOADER_API_VERSION) {
        return false;
    }

    g_Api = api;
    g_Api->LogInfo("TelemetryMod", "Telemetry Plugin loaded successfully!");

    g_Api->RegisterCarPhysicsStep(OnCarPhysics);
    g_Api->RegisterFrameTick(OnFrame);

    return true;
}
