#pragma once

#include <string>
#include <string_view>

namespace modloader {

class IModule {
public:
    virtual ~IModule() = default;

    virtual const char* GetName() const = 0;
    virtual const char* GetDescription() const { return ""; }
    virtual const char* GetVersion() const { return "1.0.0"; }

    // Lifecycle
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;

    // Engine Hooks
    virtual void OnFrameTick(float deltaTime) {}

    // VFS Hook: returns true if path was overridden/redirected
    virtual bool OnResolveFilePath(const std::string& originalPath, std::string& outPath) {
        return false;
    }
};

} // namespace modloader
