#pragma once

#include "module.hpp"
#include "dirt3_modloader_api.h"
#include <vector>
#include <string>
#include <filesystem>
#include <windows.h>

namespace modloader {

struct LoadedPlugin {
    std::wstring fileName;
    std::filesystem::path filePath;
    HMODULE handle{ nullptr };
    PFN_ModInit initFn{ nullptr };
    bool isNativePlugin{ false };
};

class PluginLoaderModule : public IModule {
public:
    static PluginLoaderModule& Instance();

    const char* GetName() const override { return "PluginLoader"; }
    bool Initialize() override;
    void Shutdown() override;
    void OnFrameTick(float deltaTime) override;

    // Dispatch car physics step to registered plugin callbacks
    void OnCarPhysicsStep(void* carPtr, float deltaTime);

    // Registration APIs exposed via ModloaderAPI
    bool RegisterFrameTick(PFN_FrameTickCallback callback);
    bool RegisterCarPhysicsStep(PFN_CarPhysicsCallback callback);

    // Helpers
    static void* GetPlayerCar();
    const std::string& GetGameDirectory() const { return gameDirStr_; }
    const std::string& GetModsDirectory() const { return modsDirStr_; }

    const ModloaderAPI& GetApi() const { return api_; }

private:
    PluginLoaderModule();
    ~PluginLoaderModule() override = default;

    void ScanAndLoadPlugins();
    void LoadPluginFile(const std::filesystem::path& path);
    void InitApiStruct();

    // Hook for neon::vehiclePhysics::Car::update
    static int __fastcall HookedCarUpdate(void* thisPtr, void* edx, float dt);
    static void* origCarUpdate_;

    std::vector<LoadedPlugin> plugins_;
    std::vector<PFN_FrameTickCallback> frameCallbacks_;
    std::vector<PFN_CarPhysicsCallback> physicsCallbacks_;

    std::string gameDirStr_;
    std::string modsDirStr_;
    ModloaderAPI api_{};
};

} // namespace modloader
