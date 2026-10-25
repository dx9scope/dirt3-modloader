#pragma once

#include <cstdint>
#include <string_view>

namespace modloader {

class HookManager {
public:
    static HookManager& Instance();

    bool Initialize();
    void Shutdown();

    bool CreateHook(std::string_view name, void* target, void* detour, void** original);

    inline bool CreateHook(std::string_view name, uintptr_t target, void* detour, void** original) {
        return CreateHook(name, reinterpret_cast<void*>(target), detour, original);
    }

    bool EnableHook(void* target);
    bool DisableHook(void* target);

    inline bool EnableHook(uintptr_t target) {
        return EnableHook(reinterpret_cast<void*>(target));
    }

    inline bool DisableHook(uintptr_t target) {
        return DisableHook(reinterpret_cast<void*>(target));
    }

private:
    HookManager() = default;
    ~HookManager();

    bool initialized_{ false };
};

} // namespace modloader
