#include "module_manager.hpp"
#include "logger.hpp"

namespace modloader {

ModuleManager& ModuleManager::Instance() {
    static ModuleManager instance;
    return instance;
}

void ModuleManager::RegisterModule(std::shared_ptr<IModule> module) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!module) return;

    MOD_LOG_INFO("ModuleManager", "Registering module: '{}' (v{})", module->GetName(), module->GetVersion());
    modules_.push_back(module);
}

bool ModuleManager::InitializeAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    MOD_LOG_INFO("ModuleManager", "Initializing {} registered module(s)...", modules_.size());

    for (auto& module : modules_) {
        MOD_LOG_INFO("ModuleManager", "Initializing module: '{}'...", module->GetName());
        if (!module->Initialize()) {
            MOD_LOG_ERROR("ModuleManager", "Module '{}' failed to initialize!", module->GetName());
        } else {
            MOD_LOG_INFO("ModuleManager", "Module '{}' initialized successfully", module->GetName());
        }
    }

    initialized_ = true;
    return true;
}

void ModuleManager::ShutdownAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return;

    MOD_LOG_INFO("ModuleManager", "Shutting down modules...");

    // Shutdown in reverse order
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        MOD_LOG_INFO("ModuleManager", "Shutting down module: '{}'...", (*it)->GetName());
        (*it)->Shutdown();
    }

    modules_.clear();
    initialized_ = false;
}

void ModuleManager::DispatchFrameTick(float deltaTime) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& module : modules_) {
        module->OnFrameTick(deltaTime);
    }
}

bool ModuleManager::DispatchResolveFilePath(const std::string& originalPath, std::string& outPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& module : modules_) {
        if (module->OnResolveFilePath(originalPath, outPath)) {
            return true;
        }
    }
    return false;
}

} // namespace modloader
