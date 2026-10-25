#include "mainloop_module.hpp"
#include "logger.hpp"
#include "hook_manager.hpp"
#include "module_manager.hpp"
#include "memory.hpp"
#include <chrono>

namespace modloader {

void* MainLoopModule::origTick_ = nullptr;

typedef int (__cdecl* PFN_FrameSync)();

bool MainLoopModule::Initialize() {
    uintptr_t targetAddr = Memory::Rva(0x00584AE0);
    MOD_LOG_INFO("MainLoop", "Installing frame sync hook at 0x{:08X} (Base: 0x{:08X})...", 
        targetAddr, Memory::GetBaseAddress());

    bool hooked = HookManager::Instance().CreateHook(
        "Application::frameSync",
        targetAddr,
        reinterpret_cast<void*>(HookedTick),
        &origTick_
    );

    if (!hooked) {
        MOD_LOG_ERROR("MainLoop", "Failed to hook frameSync!");
        return false;
    }

    MOD_LOG_INFO("MainLoop", "Main loop frame hook successfully installed");
    return true;
}

void MainLoopModule::Shutdown() {
}

char __fastcall MainLoopModule::HookedTick(void* thisPtr, void* edx) {
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastTime).count();
    lastTime = now;

    static int tickCount = 0;
    if (tickCount++ < 5) {
        MOD_LOG_INFO("MainLoop", "Tick #{} (dt={:.4f})", tickCount, dt);
    }

    // Dispatch frame tick to all active modules
    ModuleManager::Instance().DispatchFrameTick(dt);

    PFN_FrameSync orig = reinterpret_cast<PFN_FrameSync>(origTick_);
    return (char)orig();
}

} // namespace modloader
