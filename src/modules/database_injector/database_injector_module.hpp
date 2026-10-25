#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <filesystem>
#include <mutex>

#include "module.hpp"

namespace modloader {

#pragma pack(push, 1)
struct ColumnDescriptor {
    char name[32];          // +0x00: column name null-terminated
    uint32_t type;          // +0x20: 1=bool, 2=int32, 3=string, 4=float
    uint32_t size;          // +0x24: byte size (1 for bool, 4 for int/float, N for string)
    uint32_t key_type;      // +0x28: 1=primary, 2=foreign, 0=none
    uint32_t foreign_table; // +0x2C: if key_type==2, pointer to foreign TableDescriptor
    uint32_t foreign_col;   // +0x30: if key_type==2, pointer to foreign ColumnDescriptor
};
static_assert(sizeof(ColumnDescriptor) == 52, "ColumnDescriptor must be 52 bytes");

struct ChunkNode {
    uint8_t* row_data;      // +0x00: raw row bytes
    uint32_t byte_size;     // +0x04: row_count * row_stride
    uint32_t extension_id;  // +0x08: e.g. 999
    uint32_t flags;         // +0x0C: 0
    ChunkNode* next;        // +0x10: next chunk in linked list
    uint32_t row_count;     // +0x14: number of rows in this chunk
};
static_assert(sizeof(ChunkNode) == 24, "ChunkNode must be 24 bytes");

struct TableDescriptor {
    void* allocator;           // +0x00
    char name[32];             // +0x04
    uint32_t num_columns;      // +0x24 (+36 bytes from start)
    ColumnDescriptor* columns; // +0x28 (+40 bytes)
    uint32_t row_stride;       // +0x2C (+44 bytes)
    ChunkNode* first_chunk;    // +0x30 (+48 bytes)
    uint32_t chunk_count;      // +0x34 (+52 bytes)
    uint32_t table_index;      // +0x38 (+56 bytes)
};
#pragma pack(pop)

struct IniRowEntry {
    std::string tableName;
    std::string sectionLabel;
    std::map<std::string, std::string> fields;
};

class DatabaseInjectorModule : public IModule {
public:
    static DatabaseInjectorModule& Instance() {
        static DatabaseInjectorModule instance;
        return instance;
    }

    const char* GetName() const override { return "DatabaseInjector"; }
    bool Initialize() override;
    void Shutdown() override;

    void OnDatabaseLoaded();

    void RegisterCustomString(std::string key, std::string value);
    bool LookupCustomString(const std::string& key, std::string& outValue) const;

private:
    DatabaseInjectorModule() = default;
    ~DatabaseInjectorModule() override = default;

    void ProcessIniFile(const std::filesystem::path& iniPath);
    void ApplyDatabaseEntries(const std::vector<IniRowEntry>& entries);

    std::atomic<bool> m_Injected{false};
    std::map<std::string, std::string> m_CustomStrings;
    mutable std::mutex m_StringsMutex;
};

} // namespace modloader
