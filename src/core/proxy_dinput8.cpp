#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <string>
#include <atomic>
#include "logger.hpp"

std::atomic<bool> g_DirectInputCreated{ false };

typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

static PFN_DirectInput8Create g_OrigDirectInput8Create = nullptr;
static HMODULE g_RealDInput8 = nullptr;

static bool LoadOriginalDInput8() {
    if (g_OrigDirectInput8Create) return true;

    wchar_t sysDir[MAX_PATH];
    UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        MOD_LOG_ERROR("ProxyDInput8", "Failed to get system directory!");
        return false;
    }

    std::wstring realDllPath = std::wstring(sysDir) + L"\\dinput8.dll";
    g_RealDInput8 = LoadLibraryExW(realDllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_RealDInput8) {
        g_RealDInput8 = LoadLibraryW(realDllPath.c_str());
    }

    if (!g_RealDInput8) {
        MOD_LOG_ERROR("ProxyDInput8", "Failed to load real dinput8.dll from System32! error={}", GetLastError());
        return false;
    }

    g_OrigDirectInput8Create = reinterpret_cast<PFN_DirectInput8Create>(
        GetProcAddress(g_RealDInput8, "DirectInput8Create")
    );

    if (!g_OrigDirectInput8Create) {
        MOD_LOG_ERROR("ProxyDInput8", "Failed to find DirectInput8Create in real dinput8.dll!");
        return false;
    }

    MOD_LOG_INFO("ProxyDInput8", "Successfully chained real DirectInput8Create (Module=0x{:p}, Func=0x{:p})", 
        (void*)g_RealDInput8, (void*)g_OrigDirectInput8Create);
    return true;
}

extern "C" HRESULT WINAPI DirectInput8Create_Proxy(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID* ppvOut,
    LPUNKNOWN punkOuter
) {
    g_DirectInputCreated.store(true);
    MOD_LOG_INFO("ProxyDInput8", "DirectInput8Create_Proxy called (hinst=0x{:p}, ver=0x{:X})", (void*)hinst, dwVersion);

    if (!g_OrigDirectInput8Create) {
        if (!LoadOriginalDInput8()) {
            return E_FAIL;
        }
    }

    HRESULT hr = g_OrigDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    MOD_LOG_INFO("ProxyDInput8", "Original DirectInput8Create returned 0x{:08X}", (unsigned long)hr);
    return hr;
}
