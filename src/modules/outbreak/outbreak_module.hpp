#pragma once

#include "module.hpp"
#include <cstdint>

namespace modloader {

class OutbreakModule : public IModule {
public:
    static OutbreakModule& Instance() {
        static OutbreakModule instance;
        return instance;
    }

    const char* GetName() const override { return "OutbreakMod"; }
    const char* GetVersion() const override { return "2.0.0"; }

    bool Initialize() override;
    void Shutdown() override;

    void OnDatabaseLoaded();

private:
    OutbreakModule() = default;
    ~OutbreakModule() override = default;

    static void ApplyVehicleUnlocks();
};

} // namespace modloader
