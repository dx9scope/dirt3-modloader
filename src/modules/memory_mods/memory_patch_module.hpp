#pragma once

#include "module.hpp"
#include "memory.hpp"
#include <vector>
#include <functional>

namespace modloader {

struct MemoryPatch {
    std::string name;
    uintptr_t address;
    std::vector<uint8_t> patchBytes;
    std::vector<uint8_t> originalBytes;
    bool applied{ false };
};

class MemoryPatchModule : public IModule {
public:
    const char* GetName() const override { return "MemoryPatchModule"; }
    const char* GetDescription() const override { return "Manages in-memory game modifications and memory patches"; }

    bool Initialize() override;
    void Shutdown() override;
    void OnFrameTick(float deltaTime) override;

    // Register a memory patch
    bool RegisterPatch(const std::string& name, uintptr_t address, std::initializer_list<uint8_t> patchBytes);
    bool ApplyPatch(const std::string& name);
    bool RevertPatch(const std::string& name);

    // Register a per-frame memory callback (e.g. for dynamic physics adjustments)
    void RegisterFrameCallback(std::string name, std::function<void(float)> callback);

private:
    std::vector<MemoryPatch> patches_;
    std::vector<std::pair<std::string, std::function<void(float)>>> frameCallbacks_;
};

} // namespace modloader
