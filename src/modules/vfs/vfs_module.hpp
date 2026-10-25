#pragma once

#include "module.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <windows.h>

namespace modloader {

class VfsModule : public IModule {
public:
    const char* GetName() const override { return "VfsModule"; }
    const char* GetDescription() const override { return "Universal loose file redirection for DiRT 3 (mods/ folder)"; }

    bool Initialize() override;
    void Shutdown() override;

    bool OnResolveFilePath(const std::string& originalPath, std::string& outPath) override;

    void RescanMods();

private:
    std::vector<std::string> activeModDirs_;
    std::unordered_map<std::string, std::string> fileRedirectMap_;

    static void* origCreateFileA_;
    static void* origGetFileAttributesA_;
    static void* origCreateFileW_;
    static void* origGetFileAttributesW_;

    static HANDLE WINAPI HookedCreateFileA(
        LPCSTR lpFileName,
        DWORD dwDesiredAccess,
        DWORD dwShareMode,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        DWORD dwCreationDisposition,
        DWORD dwFlagsAndAttributes,
        HANDLE hTemplateFile
    );

    static DWORD WINAPI HookedGetFileAttributesA(LPCSTR lpFileName);

    static HANDLE WINAPI HookedCreateFileW(
        LPCWSTR lpFileName,
        DWORD dwDesiredAccess,
        DWORD dwShareMode,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        DWORD dwCreationDisposition,
        DWORD dwFlagsAndAttributes,
        HANDLE hTemplateFile
    );

    static DWORD WINAPI HookedGetFileAttributesW(LPCWSTR lpFileName);
};

} // namespace modloader
