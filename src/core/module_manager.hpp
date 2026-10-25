#pragma once

#include "module.hpp"
#include <vector>
#include <memory>
#include <mutex>
#include <string>

namespace modloader {

class ModuleManager {
public:
    static ModuleManager& Instance();

    void RegisterModule(std::shared_ptr<IModule> module);

    bool InitializeAll();
    void ShutdownAll();

    void DispatchFrameTick(float deltaTime);
    bool DispatchResolveFilePath(const std::string& originalPath, std::string& outPath);

    const std::vector<std::shared_ptr<IModule>>& GetModules() const { return modules_; }

private:
    ModuleManager() = default;
    ~ModuleManager() = default;

    std::mutex mutex_;
    std::vector<std::shared_ptr<IModule>> modules_;
    bool initialized_{ false };
};

} // namespace modloader
