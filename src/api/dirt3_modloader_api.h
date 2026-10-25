#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIRT3_MODLOADER_API_VERSION 1

typedef void (*PFN_LogCallback)(const char* tag, const char* message);
typedef void (*PFN_FrameTickCallback)(float deltaTime);
typedef void (*PFN_CarPhysicsCallback)(void* carPtr, float deltaTime);

struct ModloaderAPI {
    uint32_t structSize;
    uint32_t apiVersion;

    // Base address of dirt3_game.exe in current process (accounting for ASLR)
    uintptr_t gameBaseAddress;

    // Logging functions
    void (*LogInfo)(const char* tag, const char* message);
    void (*LogWarn)(const char* tag, const char* message);
    void (*LogError)(const char* tag, const char* message);

    // Memory manipulation helpers
    uintptr_t (*Rva)(uintptr_t staticAddr);
    bool (*PatchMemory)(uintptr_t addr, const void* data, size_t size);
    bool (*CreateHook)(const char* name, uintptr_t target, void* detour, void** original);

    // Engine Event Callbacks
    bool (*RegisterFrameTick)(PFN_FrameTickCallback callback);
    bool (*RegisterCarPhysicsStep)(PFN_CarPhysicsCallback callback);

    // Vehicle helpers
    void* (*GetPlayerCar)();

    // Directories
    const char* (*GetGameDirectory)();
    const char* (*GetModsDirectory)();
};

typedef struct ModloaderAPI ModloaderAPI;

// Exported function required from mod plugins in mods/*.dll
// Return true on successful initialization.
typedef bool (*PFN_ModInit)(const ModloaderAPI* api);

#ifdef __cplusplus
}
#endif
