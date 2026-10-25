#include "vfs_module.hpp"
#include "logger.hpp"
#include "hook_manager.hpp"
#include "module_manager.hpp"
#include <algorithm>
#include <filesystem>

namespace modloader {

void* VfsModule::origCreateFileA_ = nullptr;
void* VfsModule::origGetFileAttributesA_ = nullptr;
void* VfsModule::origCreateFileW_ = nullptr;
void* VfsModule::origGetFileAttributesW_ = nullptr;

typedef HANDLE(WINAPI* PFN_CreateFileA)(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
);

typedef DWORD(WINAPI* PFN_GetFileAttributesA)(LPCSTR lpFileName);

typedef HANDLE(WINAPI* PFN_CreateFileW)(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
);

typedef DWORD(WINAPI* PFN_GetFileAttributesW)(LPCWSTR lpFileName);

static std::string NormalizePath(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    for (char c : path) {
        if (c == '\\') result.push_back('/');
        else result.push_back((char)::tolower(c));
    }
    return result;
}

bool VfsModule::Initialize() {
    MOD_LOG_INFO("VfsModule", "Initializing Universal Loose File Redirection...");

    // Create mods/ directory if needed
    std::error_code ec;
    std::filesystem::create_directories("mods", ec);

    RescanMods();

    // Hook CreateFileA
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        MOD_LOG_ERROR("VfsModule", "Failed to get kernel32.dll handle!");
        return false;
    }

    void* pCreateFileA = (void*)GetProcAddress(hKernel32, "CreateFileA");
    void* pGetFileAttributesA = (void*)GetProcAddress(hKernel32, "GetFileAttributesA");
    void* pCreateFileW = (void*)GetProcAddress(hKernel32, "CreateFileW");
    void* pGetFileAttributesW = (void*)GetProcAddress(hKernel32, "GetFileAttributesW");

    if (!pCreateFileA || !pGetFileAttributesA) {
        MOD_LOG_ERROR("VfsModule", "Failed to resolve Win32 file APIs!");
        return false;
    }

    bool hooked1 = HookManager::Instance().CreateHook(
        "CreateFileA",
        pCreateFileA,
        reinterpret_cast<void*>(HookedCreateFileA),
        &origCreateFileA_
    );

    bool hooked2 = HookManager::Instance().CreateHook(
        "GetFileAttributesA",
        pGetFileAttributesA,
        reinterpret_cast<void*>(HookedGetFileAttributesA),
        &origGetFileAttributesA_
    );

    if (pCreateFileW) {
        HookManager::Instance().CreateHook(
            "CreateFileW",
            pCreateFileW,
            reinterpret_cast<void*>(HookedCreateFileW),
            &origCreateFileW_
        );
    }

    if (pGetFileAttributesW) {
        HookManager::Instance().CreateHook(
            "GetFileAttributesW",
            pGetFileAttributesW,
            reinterpret_cast<void*>(HookedGetFileAttributesW),
            &origGetFileAttributesW_
        );
    }

    if (!hooked1 || !hooked2) {
        MOD_LOG_ERROR("VfsModule", "Failed to install Win32 file hooks!");
        return false;
    }

    MOD_LOG_INFO("VfsModule", "Universal File Redirection is ACTIVE (Hooks: CreateFileA/W & GetFileAttributesA/W)");
    return true;
}

void VfsModule::Shutdown() {
    activeModDirs_.clear();
    fileRedirectMap_.clear();
}

void VfsModule::RescanMods() {
    activeModDirs_.clear();
    fileRedirectMap_.clear();

    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists("mods", ec)) {
        return;
    }

    // Collect mod directories (e.g. mods/autism)
    for (const auto& entry : fs::directory_iterator("mods", ec)) {
        if (entry.is_directory(ec)) {
            std::string modPath = entry.path().string();
            activeModDirs_.push_back(modPath);
            MOD_LOG_INFO("VfsModule", "Found mod folder: {}", modPath);
        }
    }

    // Index all loose files inside mod directories
    for (const auto& modDir : activeModDirs_) {
        for (const auto& fileEntry : fs::recursive_directory_iterator(modDir, ec)) {
            if (fileEntry.is_regular_file(ec)) {
                auto relPath = fs::relative(fileEntry.path(), modDir, ec);
                std::string relNormalized = NormalizePath(relPath.string());
                
                // Native Windows path for CreateFileA
                std::string redirectPath = fileEntry.path().string();

                fileRedirectMap_[relNormalized] = redirectPath;
                MOD_LOG_DEBUG("VfsModule", "Mapped asset: '{}' -> '{}'", relNormalized, redirectPath);
            }
        }
    }

    MOD_LOG_INFO("VfsModule", "Indexed {} redirected file(s) across {} mod(s)", fileRedirectMap_.size(), activeModDirs_.size());
}

bool VfsModule::OnResolveFilePath(const std::string& originalPath, std::string& outPath) {
    std::string normalized = NormalizePath(originalPath);

    // Don't redirect files already inside mods/
    if (normalized.find("/mods/") != std::string::npos) {
        return false;
    }

    for (const auto& [relPath, redirectPath] : fileRedirectMap_) {
        if (normalized.ends_with(relPath)) {
            outPath = redirectPath;
            return true;
        }
    }
    return false;
}

HANDLE WINAPI VfsModule::HookedCreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
) {
    PFN_CreateFileA orig = reinterpret_cast<PFN_CreateFileA>(origCreateFileA_);

    if (lpFileName && reinterpret_cast<uintptr_t>(lpFileName) > 0x10000) {
        std::string redirected;
        if (ModuleManager::Instance().DispatchResolveFilePath(lpFileName, redirected)) {
            MOD_LOG_INFO("VFS", "CreateFileA >>> REDIRECTED: '{}' -> '{}'", lpFileName, redirected);
            return orig(redirected.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
        } else if (strstr(lpFileName, "tracks") || strstr(lpFileName, "battersea")) {
            MOD_LOG_INFO("VFS", "CreateFileA (passthrough): '{}'", lpFileName);
        }
    }

    return orig(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

DWORD WINAPI VfsModule::HookedGetFileAttributesA(LPCSTR lpFileName) {
    PFN_GetFileAttributesA orig = reinterpret_cast<PFN_GetFileAttributesA>(origGetFileAttributesA_);

    if (lpFileName && reinterpret_cast<uintptr_t>(lpFileName) > 0x10000) {
        std::string redirected;
        if (ModuleManager::Instance().DispatchResolveFilePath(lpFileName, redirected)) {
            MOD_LOG_INFO("VFS", "GetFileAttributesA >>> REDIRECTED: '{}' -> '{}'", lpFileName, redirected);
            return orig(redirected.c_str());
        } else if (strstr(lpFileName, "tracks") || strstr(lpFileName, "battersea")) {
            MOD_LOG_INFO("VFS", "GetFileAttributesA (passthrough): '{}'", lpFileName);
        }
    }

    return orig(lpFileName);
}

HANDLE WINAPI VfsModule::HookedCreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
) {
    PFN_CreateFileW orig = reinterpret_cast<PFN_CreateFileW>(origCreateFileW_);

    if (lpFileName && reinterpret_cast<uintptr_t>(lpFileName) > 0x10000) {
        int len = WideCharToMultiByte(CP_UTF8, 0, lpFileName, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string mb(len - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, lpFileName, -1, mb.data(), len, nullptr, nullptr);
            std::string redirected;
            if (ModuleManager::Instance().DispatchResolveFilePath(mb, redirected)) {
                MOD_LOG_INFO("VFS", "CreateFileW >>> REDIRECTED: '{}' -> '{}'", mb, redirected);
                int wlen = MultiByteToWideChar(CP_UTF8, 0, redirected.c_str(), -1, nullptr, 0);
                if (wlen > 0) {
                    std::wstring wb(wlen - 1, 0);
                    MultiByteToWideChar(CP_UTF8, 0, redirected.c_str(), -1, wb.data(), wlen);
                    return orig(wb.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
                }
            }
        }
    }

    return orig(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

DWORD WINAPI VfsModule::HookedGetFileAttributesW(LPCWSTR lpFileName) {
    PFN_GetFileAttributesW orig = reinterpret_cast<PFN_GetFileAttributesW>(origGetFileAttributesW_);

    if (lpFileName && reinterpret_cast<uintptr_t>(lpFileName) > 0x10000) {
        int len = WideCharToMultiByte(CP_UTF8, 0, lpFileName, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string mb(len - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, lpFileName, -1, mb.data(), len, nullptr, nullptr);
            std::string redirected;
            if (ModuleManager::Instance().DispatchResolveFilePath(mb, redirected)) {
                MOD_LOG_INFO("VFS", "GetFileAttributesW >>> REDIRECTED: '{}' -> '{}'", mb, redirected);
                int wlen = MultiByteToWideChar(CP_UTF8, 0, redirected.c_str(), -1, nullptr, 0);
                if (wlen > 0) {
                    std::wstring wb(wlen - 1, 0);
                    MultiByteToWideChar(CP_UTF8, 0, redirected.c_str(), -1, wb.data(), wlen);
                    return orig(wb.c_str());
                }
            }
        }
    }

    return orig(lpFileName);
}

} // namespace modloader
