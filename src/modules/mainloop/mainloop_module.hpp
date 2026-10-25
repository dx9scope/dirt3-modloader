#pragma once

#include "module.hpp"

namespace modloader {

class MainLoopModule : public IModule {
public:
    const char* GetName() const override { return "MainLoopModule"; }
    const char* GetDescription() const override { return "Hooks CMR6::UpdateTask::tick for per-frame updates"; }

    bool Initialize() override;
    void Shutdown() override;

private:
    static void* origTick_;
    static char __fastcall HookedTick(void* thisPtr, void* edx);
};

} // namespace modloader
