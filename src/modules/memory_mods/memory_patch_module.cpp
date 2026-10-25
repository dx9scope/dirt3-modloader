#include "memory_patch_module.hpp"
#include "logger.hpp"

namespace modloader {

bool MemoryPatchModule::Initialize() {
    MOD_LOG_INFO("MemoryPatchModule", "Initializing In-Memory Modding Subsystem...");
    // Future in-memory game patches can be registered and applied here!
    return true;
}

void MemoryPatchModule::Shutdown() {
    MOD_LOG_INFO("MemoryPatchModule", "Reverting all active memory patches...");
    for (auto& patch : patches_) {
        if (patch.applied) {
            Memory::Patch(patch.address, patch.originalBytes.data(), patch.originalBytes.size());
            patch.applied = false;
        }
    }
    patches_.clear();
    frameCallbacks_.clear();
}

void MemoryPatchModule::OnFrameTick(float deltaTime) {
    // Execute all registered per-frame memory modifier callbacks
    for (const auto& [name, callback] : frameCallbacks_) {
        if (callback) {
            callback(deltaTime);
        }
    }
}

bool MemoryPatchModule::RegisterPatch(const std::string& name, uintptr_t address, std::initializer_list<uint8_t> patchBytes) {
    MemoryPatch patch;
    patch.name = name;
    patch.address = address;
    patch.patchBytes = patchBytes;
    patch.originalBytes.resize(patch.patchBytes.size());

    if (!Memory::Read(address, patch.originalBytes.data(), patch.originalBytes.size())) {
        MOD_LOG_ERROR("MemoryPatchModule", "Failed to read original bytes for patch '{}' at 0x{:08X}", name, address);
        return false;
    }

    patches_.push_back(std::move(patch));
    MOD_LOG_INFO("MemoryPatchModule", "Registered patch '{}' at 0x{:08X} ({} bytes)", name, address, patchBytes.size());
    return true;
}

bool MemoryPatchModule::ApplyPatch(const std::string& name) {
    for (auto& patch : patches_) {
        if (patch.name == name) {
            if (patch.applied) return true;
            if (Memory::Patch(patch.address, patch.patchBytes.data(), patch.patchBytes.size())) {
                patch.applied = true;
                MOD_LOG_INFO("MemoryPatchModule", "Applied patch '{}'", name);
                return true;
            } else {
                MOD_LOG_ERROR("MemoryPatchModule", "Failed to apply patch '{}'", name);
                return false;
            }
        }
    }
    MOD_LOG_WARN("MemoryPatchModule", "Patch '{}' not found", name);
    return false;
}

bool MemoryPatchModule::RevertPatch(const std::string& name) {
    for (auto& patch : patches_) {
        if (patch.name == name) {
            if (!patch.applied) return true;
            if (Memory::Patch(patch.address, patch.originalBytes.data(), patch.originalBytes.size())) {
                patch.applied = false;
                MOD_LOG_INFO("MemoryPatchModule", "Reverted patch '{}'", name);
                return true;
            } else {
                MOD_LOG_ERROR("MemoryPatchModule", "Failed to revert patch '{}'", name);
                return false;
            }
        }
    }
    return false;
}

void MemoryPatchModule::RegisterFrameCallback(std::string name, std::function<void(float)> callback) {
    MOD_LOG_INFO("MemoryPatchModule", "Registered frame memory callback '{}'", name);
    frameCallbacks_.emplace_back(std::move(name), std::move(callback));
}

} // namespace modloader
