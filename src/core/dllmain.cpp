#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#include <filesystem>

#include "logger.hpp"
#include "hook_manager.hpp"
#include "module_manager.hpp"

#include "../modules/vfs/vfs_module.hpp"
#include "../modules/mainloop/mainloop_module.hpp"
#include "../modules/memory_mods/memory_patch_module.hpp"
#include "../modules/plugin_loader/plugin_loader_module.hpp"
#include "../modules/database_injector/database_injector_module.hpp"
#include "../modules/outbreak/outbreak_module.hpp"

static LONG WINAPI UnhandledCrashHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    MOD_LOG_ERROR("CRASH", "=== FATAL UNHANDLED EXCEPTION 0x{:08X} ===", pExceptionInfo->ExceptionRecord->ExceptionCode);
    MOD_LOG_ERROR("CRASH", "Fault Address: 0x{:08X}", (uintptr_t)pExceptionInfo->ExceptionRecord->ExceptionAddress);
    MOD_LOG_ERROR("CRASH", "EAX=0x{:08X} EBX=0x{:08X} ECX=0x{:08X} EDX=0x{:08X}",
        pExceptionInfo->ContextRecord->Eax, pExceptionInfo->ContextRecord->Ebx,
        pExceptionInfo->ContextRecord->Ecx, pExceptionInfo->ContextRecord->Edx);
    MOD_LOG_ERROR("CRASH", "ESI=0x{:08X} EDI=0x{:08X} EBP=0x{:08X} ESP=0x{:08X}",
        pExceptionInfo->ContextRecord->Esi, pExceptionInfo->ContextRecord->Edi,
        pExceptionInfo->ContextRecord->Ebp, pExceptionInfo->ContextRecord->Esp);
    return EXCEPTION_CONTINUE_SEARCH;
}

#include "memory.hpp"

extern std::atomic<bool> g_DirectInputCreated;

static bool IsMemoryExecutable(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT) {
            DWORD p = mbi.Protect & 0xFF;
            if (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || 
                p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY) {
                return true;
            }
        }
    }
    return false;
}

static DWORD WINAPI ModloaderThread(LPVOID lpParam) {
    SetUnhandledExceptionFilter(UnhandledCrashHandler);

    bool openConsole = true;
    if (std::filesystem::exists("disable_console.txt")) {
        openConsole = false;
    }

    modloader::Logger::Instance().Init(openConsole);
    MOD_LOG_INFO("Core", "=================================================");
    MOD_LOG_INFO("Core", "   DiRT 3 Modloader v1.0.0 (Telemetry & Mods)   ");
    MOD_LOG_INFO("Core", "=================================================");
    MOD_LOG_INFO("Core", "Game Executable Base Address: 0x{:08X}", modloader::Memory::GetBaseAddress());

    // Initialize MinHook
    if (!modloader::HookManager::Instance().Initialize()) {
        MOD_LOG_ERROR("Core", "Failed to initialize HookManager!");
        return 1;
    }

    auto& mm = modloader::ModuleManager::Instance();

    // 1. Initialize VFS first so that early asset loads are redirected
    auto vfs = std::make_shared<modloader::VfsModule>();
    mm.RegisterModule(vfs);
    vfs->Initialize();

    // 2. Wait for game executable (.text section) to be unpacked by Steam DRM
    uintptr_t checkAddr = modloader::Memory::Rva(0x00584AE0);
    MOD_LOG_INFO("Core", "Waiting for game executable to unpack (.text at 0x{:08X} to become executable)...", checkAddr);
    int waitedMs = 0;
    while (waitedMs < 30000) {
        if (IsMemoryExecutable(checkAddr) || g_DirectInputCreated.load()) {
            break;
        }
        Sleep(100);
        waitedMs += 100;
    }

    // Give a brief moment for static tables
    Sleep(300);

    if (IsMemoryExecutable(checkAddr)) {
        MOD_LOG_INFO("Core", "Game executable is unpacked and ready (waited {} ms)!", waitedMs);
    } else {
        MOD_LOG_WARN("Core", "Timeout waiting for .text memory at 0x{:08X} to become executable, attempting hooks anyway...", checkAddr);
    }

    // 3. Register and initialize in-game memory hooks and plugin loader
    auto mainLoop = std::make_shared<modloader::MainLoopModule>();
    auto memPatch = std::make_shared<modloader::MemoryPatchModule>();
    auto pluginLoader = std::shared_ptr<modloader::IModule>(&modloader::PluginLoaderModule::Instance(), [](auto*){});
    auto dbInjector = std::shared_ptr<modloader::IModule>(&modloader::DatabaseInjectorModule::Instance(), [](auto*){});
    auto outbreakMod = std::shared_ptr<modloader::IModule>(&modloader::OutbreakModule::Instance(), [](auto*){});

    mm.RegisterModule(mainLoop);
    mm.RegisterModule(memPatch);
    mm.RegisterModule(pluginLoader);
    mm.RegisterModule(dbInjector);
    mm.RegisterModule(outbreakMod);

    mainLoop->Initialize();
    memPatch->Initialize();
    pluginLoader->Initialize();
    dbInjector->Initialize();
    outbreakMod->Initialize();

    MOD_LOG_INFO("Core", "Modloader initialized! All systems and plugins active.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, ModloaderThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        modloader::ModuleManager::Instance().ShutdownAll();
        modloader::HookManager::Instance().Shutdown();
        modloader::Logger::Instance().Shutdown();
        break;
    }
    return TRUE;
}
