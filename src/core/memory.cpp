#include "memory.hpp"
#include "logger.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

namespace modloader {

uintptr_t Memory::GetBaseAddress() {
    static uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    return base;
}

bool Memory::Patch(uintptr_t address, const void* data, size_t size) {
    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        MOD_LOG_ERROR("Memory", "Failed to VirtualProtect at 0x{:08X}, error={}", address, GetLastError());
        return false;
    }

    std::memcpy((void*)address, data, size);

    VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)address, size);
    return true;
}

bool Memory::PatchBytes(uintptr_t address, std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> vec(bytes);
    return Patch(address, vec.data(), vec.size());
}

bool Memory::Nop(uintptr_t address, size_t count) {
    std::vector<uint8_t> nops(count, 0x90);
    return Patch(address, nops.data(), nops.size());
}

bool Memory::Read(uintptr_t address, void* buffer, size_t size) {
    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READ, &oldProtect)) {
        return false;
    }

    std::memcpy(buffer, (const void*)address, size);
    VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
    return true;
}

uintptr_t Memory::PatternScan(std::string_view pattern, std::string_view mask) {
    MODULEINFO moduleInfo{};
    HMODULE hMod = GetModuleHandleA(nullptr);
    if (!GetModuleInformation(GetCurrentProcess(), hMod, &moduleInfo, sizeof(moduleInfo))) {
        return 0;
    }

    auto base = reinterpret_cast<const uint8_t*>(moduleInfo.lpBaseOfDll);
    size_t size = moduleInfo.SizeOfImage;

    const auto pat = reinterpret_cast<const uint8_t*>(pattern.data());
    const size_t patLen = pattern.size();

    for (size_t i = 0; i <= size - patLen; ++i) {
        bool found = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] != '?' && pat[j] != base[i + j]) {
                found = false;
                break;
            }
        }
        if (found) {
            return reinterpret_cast<uintptr_t>(&base[i]);
        }
    }

    return 0;
}

} // namespace modloader
