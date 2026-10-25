#include "hook_manager.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <MinHook.h>

namespace modloader {

HookManager& HookManager::Instance() {
    static HookManager instance;
    return instance;
}

HookManager::~HookManager() {
    Shutdown();
}

bool HookManager::Initialize() {
    if (initialized_) return true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        MOD_LOG_ERROR("HookManager", "MH_Initialize failed with code {}", (int)status);
        return false;
    }

    initialized_ = true;
    MOD_LOG_INFO("HookManager", "MinHook initialized successfully");
    return true;
}

void HookManager::Shutdown() {
    if (!initialized_) return;

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    initialized_ = false;
    MOD_LOG_INFO("HookManager", "MinHook shutdown complete");
}

bool HookManager::CreateHook(std::string_view name, void* target, void* detour, void** original) {
    if (!initialized_) {
        MOD_LOG_ERROR("HookManager", "Cannot create hook '{}': HookManager not initialized", name);
        return false;
    }

    MH_STATUS status = MH_UNKNOWN;
    // Retry up to 30 times (3 seconds total) if memory is not yet executable (waiting for DRM/unpack)
    for (int attempt = 0; attempt < 30; ++attempt) {
        status = MH_CreateHook(target, detour, original);
        if (status != MH_ERROR_NOT_EXECUTABLE) {
            break;
        }
        Sleep(100);
    }

    if (status != MH_OK) {
        MOD_LOG_ERROR("HookManager", "Failed to create hook '{}' at 0x{:p}, status={}", name, target, (int)status);
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        MOD_LOG_ERROR("HookManager", "Failed to enable hook '{}' at 0x{:p}, status={}", name, target, (int)status);
        return false;
    }

    MOD_LOG_INFO("HookManager", "Hook '{}' successfully installed at 0x{:p}", name, target);
    return true;
}

bool HookManager::EnableHook(void* target) {
    return MH_EnableHook(target) == MH_OK;
}

bool HookManager::DisableHook(void* target) {
    return MH_DisableHook(target) == MH_OK;
}

} // namespace modloader
