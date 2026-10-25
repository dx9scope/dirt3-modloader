#include "plugin_loader_module.hpp"
#include "logger.hpp"
#include "hook_manager.hpp"
#include "memory.hpp"

#include <algorithm>

namespace modloader {

void* PluginLoaderModule::origCarUpdate_ = nullptr;
typedef int (__thiscall* PFN_CarUpdate)(void* thisPtr, float dt);

PluginLoaderModule::PluginLoaderModule() {
    char currentDir[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, currentDir);
    gameDirStr_ = currentDir;
    modsDirStr_ = (std::filesystem::path(currentDir) / "mods").string();

    InitApiStruct();
}

PluginLoaderModule& PluginLoaderModule::Instance() {
    static PluginLoaderModule instance;
    return instance;
}

void PluginLoaderModule::InitApiStruct() {
    api_.structSize = sizeof(ModloaderAPI);
    api_.apiVersion = DIRT3_MODLOADER_API_VERSION;
    api_.gameBaseAddress = Memory::GetBaseAddress();

    api_.LogInfo = [](const char* tag, const char* message) {
        Logger::Instance().Log(LogLevel::Info, tag, message);
    };
    api_.LogWarn = [](const char* tag, const char* message) {
        Logger::Instance().Log(LogLevel::Warn, tag, message);
    };
    api_.LogError = [](const char* tag, const char* message) {
        Logger::Instance().Log(LogLevel::Error, tag, message);
    };

    api_.Rva = [](uintptr_t staticAddr) {
        return Memory::Rva(staticAddr);
    };
    api_.PatchMemory = [](uintptr_t addr, const void* data, size_t size) {
        return Memory::Patch(addr, data, size);
    };
    api_.CreateHook = [](const char* name, uintptr_t target, void* detour, void** original) {
        return HookManager::Instance().CreateHook(name, target, detour, original);
    };

    api_.RegisterFrameTick = [](PFN_FrameTickCallback callback) {
        return PluginLoaderModule::Instance().RegisterFrameTick(callback);
    };
    api_.RegisterCarPhysicsStep = [](PFN_CarPhysicsCallback callback) {
        return PluginLoaderModule::Instance().RegisterCarPhysicsStep(callback);
    };

    api_.GetPlayerCar = []() {
        return PluginLoaderModule::GetPlayerCar();
    };

    api_.GetGameDirectory = []() {
        return PluginLoaderModule::Instance().GetGameDirectory().c_str();
    };
    api_.GetModsDirectory = []() {
        return PluginLoaderModule::Instance().GetModsDirectory().c_str();
    };
}

bool PluginLoaderModule::Initialize() {
    // 1. Install physics hook on neon::vehiclePhysics::Car::update (0x008007B0)
    uintptr_t carUpdateAddr = Memory::Rva(0x008007B0);
    MOD_LOG_INFO("PluginLoader", "Installing core Car::update hook at 0x{:08X}...", carUpdateAddr);

    bool hooked = HookManager::Instance().CreateHook(
        "neon::vehiclePhysics::Car::update",
        carUpdateAddr,
        reinterpret_cast<void*>(HookedCarUpdate),
        &origCarUpdate_
    );

    if (!hooked) {
        MOD_LOG_ERROR("PluginLoader", "Failed to hook Car::update!");
    } else {
        MOD_LOG_INFO("PluginLoader", "Core vehicle physics hook installed successfully");
    }

    // 2. Scan and load all plugins from mods/ directory
    ScanAndLoadPlugins();

    return true;
}

void PluginLoaderModule::Shutdown() {
    origCarUpdate_ = nullptr;
    frameCallbacks_.clear();
    physicsCallbacks_.clear();

    // Free loaded plugins in reverse order
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        if (it->handle) {
            FreeLibrary(it->handle);
            it->handle = nullptr;
        }
    }
    plugins_.clear();
}

bool PluginLoaderModule::RegisterFrameTick(PFN_FrameTickCallback callback) {
    if (!callback) return false;
    frameCallbacks_.push_back(callback);
    return true;
}

bool PluginLoaderModule::RegisterCarPhysicsStep(PFN_CarPhysicsCallback callback) {
    if (!callback) return false;
    physicsCallbacks_.push_back(callback);
    return true;
}

void PluginLoaderModule::OnFrameTick(float deltaTime) {
    for (auto cb : frameCallbacks_) {
        if (cb) {
            cb(deltaTime);
        }
    }
}

void PluginLoaderModule::OnCarPhysicsStep(void* carPtr, float deltaTime) {
    for (auto cb : physicsCallbacks_) {
        if (cb) {
            cb(carPtr, deltaTime);
        }
    }
}

int __fastcall PluginLoaderModule::HookedCarUpdate(void* thisPtr, void* edx, float dt) {
    PFN_CarUpdate orig = reinterpret_cast<PFN_CarUpdate>(origCarUpdate_);
    int result = orig(thisPtr, dt);

    Instance().OnCarPhysicsStep(thisPtr, dt);

    return result;
}

static inline bool IsValidUserPtr(uintptr_t ptr) {
    return (ptr >= 0x00010000 && ptr < 0x7FFE0000);
}

void* PluginLoaderModule::GetPlayerCar() {
    __try {
        uintptr_t appPtrAddr = Memory::Rva(0x00FF6900);
        if (!IsValidUserPtr(appPtrAddr)) return nullptr;

        uintptr_t app = *reinterpret_cast<uintptr_t*>(appPtrAddr);
        if (IsValidUserPtr(app)) {
            uintptr_t vm = *reinterpret_cast<uintptr_t*>(app + 4280);
            if (IsValidUserPtr(vm)) {
                uintptr_t vecBegin = *reinterpret_cast<uintptr_t*>(vm + 104);
                uintptr_t vecEnd = *reinterpret_cast<uintptr_t*>(vm + 108);
                if (IsValidUserPtr(vecBegin) && vecEnd > vecBegin && (vecEnd - vecBegin) < 10000) {
                    uintptr_t pVehicle = *reinterpret_cast<uintptr_t*>(vecBegin);
                    if (IsValidUserPtr(pVehicle)) {
                        uintptr_t pCar = *reinterpret_cast<uintptr_t*>(pVehicle + 48);
                        if (IsValidUserPtr(pCar)) {
                            return reinterpret_cast<void*>(pCar);
                        }
                    }
                }
            }
        }

        // Fallback: active cars table
        uintptr_t countAddr = Memory::Rva(0x01029CFC);
        if (IsValidUserPtr(countAddr)) {
            int count = *reinterpret_cast<int*>(countAddr);
            if (count > 0 && count < 64) {
                uintptr_t pCar = *reinterpret_cast<uintptr_t*>(Memory::Rva(0x01029C70));
                if (IsValidUserPtr(pCar)) {
                    return reinterpret_cast<void*>(pCar);
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }

    return nullptr;
}

void PluginLoaderModule::ScanAndLoadPlugins() {
    namespace fs = std::filesystem;
    fs::path modsPath = "mods";

    if (!fs::exists(modsPath) || !fs::is_directory(modsPath)) {
        MOD_LOG_INFO("PluginLoader", "Directory 'mods' does not exist. Creating it...");
        fs::create_directories(modsPath);
        return;
    }

    MOD_LOG_INFO("PluginLoader", "Scanning for plugins in '{}'...", modsPath.string());

    std::vector<fs::path> candidateFiles;

    // Scan mods/ directly
    for (const auto& entry : fs::directory_iterator(modsPath)) {
        if (entry.is_regular_file()) {
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".dll" || ext == ".asi") {
                candidateFiles.push_back(entry.path());
            }
        } else if (entry.is_directory()) {
            // Also scan immediate subdirectories (e.g. mods/gravity_mod/gravity_mod.dll)
            for (const auto& subEntry : fs::directory_iterator(entry.path())) {
                if (subEntry.is_regular_file()) {
                    auto ext = subEntry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    if (ext == ".dll" || ext == ".asi") {
                        candidateFiles.push_back(subEntry.path());
                    }
                }
            }
        }
    }

    for (const auto& filePath : candidateFiles) {
        LoadPluginFile(filePath);
    }

    MOD_LOG_INFO("PluginLoader", "Plugin scanning complete. Total loaded: {}", plugins_.size());
}

void PluginLoaderModule::LoadPluginFile(const std::filesystem::path& path) {
    std::string filename = path.filename().string();
    std::string lowerFilename = filename;
    std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    // Ignore self and dinput8
    if (lowerFilename == "dinput8.dll") {
        return;
    }

    MOD_LOG_INFO("PluginLoader", "Loading plugin: '{}'...", path.string());

    HMODULE hMod = LoadLibraryW(path.wstring().c_str());
    if (!hMod) {
        DWORD err = GetLastError();
        MOD_LOG_ERROR("PluginLoader", "Failed to load '{}' (Error: 0x{:08X})", path.string(), err);
        return;
    }

    LoadedPlugin lp;
    lp.fileName = path.filename().wstring();
    lp.filePath = path;
    lp.handle = hMod;

    PFN_ModInit initFn = reinterpret_cast<PFN_ModInit>(GetProcAddress(hMod, "ModInit"));
    if (initFn) {
        lp.initFn = initFn;
        lp.isNativePlugin = true;
        MOD_LOG_INFO("PluginLoader", "Found ModInit in '{}', initializing native plugin...", filename);

        bool ok = initFn(&api_);
        if (ok) {
            MOD_LOG_INFO("PluginLoader", "Plugin '{}' initialized successfully!", filename);
        } else {
            MOD_LOG_WARN("PluginLoader", "Plugin '{}' reported initialization failure!", filename);
        }
    } else {
        lp.isNativePlugin = false;
        MOD_LOG_INFO("PluginLoader", "Loaded external library/ASI '{}' (no ModInit export)", filename);
    }

    plugins_.push_back(lp);
}

} // namespace modloader
