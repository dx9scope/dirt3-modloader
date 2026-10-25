#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <string_view>
#include <initializer_list>

namespace modloader {

class Memory {
public:
    static uintptr_t GetBaseAddress();

    static inline uintptr_t Rva(uintptr_t idaAddress) {
        return GetBaseAddress() + (idaAddress - 0x00400000);
    }

    static bool Patch(uintptr_t address, const void* data, size_t size);
    static bool PatchBytes(uintptr_t address, std::initializer_list<uint8_t> bytes);
    static bool Nop(uintptr_t address, size_t count);

    template <typename T>
    static T Read(uintptr_t address) {
        T value{};
        Read(address, &value, sizeof(T));
        return value;
    }

    template <typename T>
    static bool Write(uintptr_t address, const T& value) {
        return Patch(address, &value, sizeof(T));
    }

    static bool Read(uintptr_t address, void* buffer, size_t size);

    // Scan for pattern within module
    static uintptr_t PatternScan(std::string_view pattern, std::string_view mask);
};

} // namespace modloader
