#include "database_injector_module.hpp"
#include "logger.hpp"
#include "hook_manager.hpp"
#include "memory.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "../outbreak/outbreak_module.hpp"

namespace modloader {

static void* s_OrigSub573530 = nullptr;
static void* s_OrigResolveString = nullptr;

typedef int (__thiscall* PFN_Sub573530)(void* thisPtr);
typedef TableDescriptor* (__cdecl* PFN_GetTable)(const char* name);

typedef void* (__cdecl* PFN_ResolveString)(void* outStr, void* langMgr, const char* key);
typedef int (__thiscall* PFN_AssignString)(void* thisPtr, const char* src);

static PFN_AssignString s_AssignString = nullptr;

// Trim helper functions
static inline std::string Trim(const std::string& str) {
    auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();
    return (start < end) ? std::string(start, end) : std::string();
}

static inline std::string StripQuotes(const std::string& str) {
    if (str.size() >= 2 && ((str.front() == '"' && str.back() == '"') || 
                           (str.front() == '\'' && str.back() == '\''))) {
        return str.substr(1, str.size() - 2);
    }
    return str;
}

static inline bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

static int __fastcall HookedSub573530(void* thisPtr, void* /*edx*/) {
    PFN_Sub573530 orig = reinterpret_cast<PFN_Sub573530>(s_OrigSub573530);
    int result = orig(thisPtr);

    // Call dynamic database injection right after game database load finishes
    DatabaseInjectorModule::Instance().OnDatabaseLoaded();

    return result;
}

static void* __cdecl HookedResolveString(void* outStr, void* langMgr, const char* key) {
    if (key != nullptr && outStr != nullptr) {
        std::string customVal;
        if (DatabaseInjectorModule::Instance().LookupCustomString(key, customVal)) {
            // Replicate game's internal string assignment (sub_D00D50):
            // Initial pointer must be null before calling sub_D1C520
            *reinterpret_cast<void**>(outStr) = nullptr;
            if (s_AssignString) {
                s_AssignString(outStr, customVal.c_str());
            }
            return outStr;
        }
    }

    PFN_ResolveString orig = reinterpret_cast<PFN_ResolveString>(s_OrigResolveString);
    return orig(outStr, langMgr, key);
}

void DatabaseInjectorModule::RegisterCustomString(std::string key, std::string value) {
    std::lock_guard<std::mutex> lock(m_StringsMutex);
    std::string lowerKey = key;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
    m_CustomStrings[lowerKey] = value;
    MOD_LOG_INFO("Database", "Registered custom string: '{}' -> '{}'", key, value);
}

bool DatabaseInjectorModule::LookupCustomString(const std::string& key, std::string& outValue) const {
    std::lock_guard<std::mutex> lock(m_StringsMutex);
    std::string lowerKey = key;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

    // 1. Direct match
    auto it = m_CustomStrings.find(lowerKey);
    if (it != m_CustomStrings.end()) {
        outValue = it->second;
        return true;
    }

    // 2. If key starts with "db_", check without "db_"
    if (lowerKey.rfind("db_", 0) == 0) {
        std::string stripped = lowerKey.substr(3);
        it = m_CustomStrings.find(stripped);
        if (it != m_CustomStrings.end()) {
            outValue = it->second;
            return true;
        }
    }

    // 3. If key doesn't start with "db_", check with "db_"
    std::string prefixed = "db_" + lowerKey;
    it = m_CustomStrings.find(prefixed);
    if (it != m_CustomStrings.end()) {
        outValue = it->second;
        return true;
    }

    return false;
}

static void CloneTrackModelConditions(int srcTrackModelId, int newTrackModelId) {
    PFN_GetTable GetTable = reinterpret_cast<PFN_GetTable>(Memory::Rva(0x004014F0));
    TableDescriptor* condTable = GetTable("track_model_conditions");
    if (!condTable || !condTable->first_chunk) return;

    int tmidColOffset = -1;
    int idColOffset = -1;
    uint32_t offsetAcc = 0;
    for (uint32_t c = 0; c < condTable->num_columns; ++c) {
        if (EqualsIgnoreCase(condTable->columns[c].name, "track_model_id")) {
            tmidColOffset = static_cast<int>(offsetAcc);
        }
        if (condTable->columns[c].key_type == 1 || EqualsIgnoreCase(condTable->columns[c].name, "id")) {
            idColOffset = static_cast<int>(offsetAcc);
        }
        offsetAcc += condTable->columns[c].size;
    }

    if (tmidColOffset == -1) return;

    std::vector<std::vector<uint8_t>> matchingRows;
    int maxExistingId = 10000;

    for (ChunkNode* chunk = condTable->first_chunk; chunk != nullptr; chunk = chunk->next) {
        if (!chunk->row_data) continue;
        for (uint32_t r = 0; r < chunk->row_count; ++r) {
            uint8_t* rowPtr = chunk->row_data + r * condTable->row_stride;
            if (idColOffset != -1) {
                int curId = *reinterpret_cast<int32_t*>(rowPtr + 4 + idColOffset);
                if (curId > maxExistingId) maxExistingId = curId;
            }
            int curTmid = *reinterpret_cast<int32_t*>(rowPtr + 4 + tmidColOffset);
            if (curTmid == srcTrackModelId) {
                std::vector<uint8_t> rowBytes(rowPtr, rowPtr + condTable->row_stride);
                matchingRows.push_back(rowBytes);
            }
        }
    }

    if (matchingRows.empty()) {
        MOD_LOG_WARN("Database", "  [CLONE] No conditions found for source track_model_id {}", srcTrackModelId);
        return;
    }

    uint32_t rowCount = static_cast<uint32_t>(matchingRows.size());
    uint32_t totalBytes = rowCount * condTable->row_stride;
    uint8_t* condBuffer = new uint8_t[totalBytes]();

    for (uint32_t i = 0; i < rowCount; ++i) {
        uint8_t* dstRow = condBuffer + i * condTable->row_stride;
        memcpy(dstRow, matchingRows[i].data(), condTable->row_stride);

        // Update track_model_id to new ID
        *reinterpret_cast<int32_t*>(dstRow + 4 + tmidColOffset) = newTrackModelId;

        // Assign a unique id if primary key column exists
        if (idColOffset != -1) {
            *reinterpret_cast<int32_t*>(dstRow + 4 + idColOffset) = maxExistingId + 1 + i;
        }
    }

    ChunkNode* condChunk = new ChunkNode();
    condChunk->row_data = condBuffer;
    condChunk->byte_size = totalBytes;
    condChunk->extension_id = 0;
    condChunk->flags = 0;
    condChunk->next = nullptr;
    condChunk->row_count = rowCount;

    if (!condTable->first_chunk) {
        condTable->first_chunk = condChunk;
    } else {
        ChunkNode* curr = condTable->first_chunk;
        while (curr->next) curr = curr->next;
        curr->next = condChunk;
    }
    condTable->chunk_count++;

    MOD_LOG_INFO("Database", "  [CLONE] Automatically cloned {} condition records for track_model_id {}",
        rowCount, newTrackModelId);
}

bool DatabaseInjectorModule::Initialize() {
    uintptr_t targetAddr = Memory::Rva(0x00573530);
    MOD_LOG_INFO("Database", "Installing database load completion hook at 0x{:08X}...", targetAddr);

    bool hooked = HookManager::Instance().CreateHook(
        "Database::InitComplete",
        targetAddr,
        reinterpret_cast<void*>(HookedSub573530),
        &s_OrigSub573530
    );

    if (!hooked) {
        MOD_LOG_ERROR("Database", "Failed to hook database initialization (sub_573530)!");
        return false;
    }

    MOD_LOG_INFO("Database", "Database load completion hook successfully installed");

    s_AssignString = reinterpret_cast<PFN_AssignString>(Memory::Rva(0x00D1C520));

    uintptr_t resolveStrAddr = Memory::Rva(0x00D00D50);
    MOD_LOG_INFO("Database", "Installing string localization hook at 0x{:08X}...", resolveStrAddr);
    bool hookStr = HookManager::Instance().CreateHook(
        "Language::ResolveString",
        resolveStrAddr,
        reinterpret_cast<void*>(HookedResolveString),
        &s_OrigResolveString
    );
    if (!hookStr) {
        MOD_LOG_WARN("Database", "Failed to hook string localization (sub_D00D50)!");
    } else {
        MOD_LOG_INFO("Database", "String localization hook successfully installed");
    }

    return true;
}

void DatabaseInjectorModule::Shutdown() {
}

void DatabaseInjectorModule::OnDatabaseLoaded() {
    if (m_Injected.exchange(true)) {
        return;
    }

    MOD_LOG_INFO("Database", "=================================================");
    MOD_LOG_INFO("Database", "   DiRT 3 Dynamic Database Injector Triggered    ");
    MOD_LOG_INFO("Database", "=================================================");

    std::filesystem::path modsDir = "mods";
    if (std::filesystem::exists(modsDir)) {
        std::vector<std::filesystem::path> iniFiles;
        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(modsDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".ini") {
                    iniFiles.push_back(entry.path());
                }
            }
        } catch (const std::exception& ex) {
            MOD_LOG_ERROR("Database", "Error searching for INI files: {}", ex.what());
        }

        for (const auto& iniPath : iniFiles) {
            ProcessIniFile(iniPath);
        }
    } else {
        MOD_LOG_INFO("Database", "No 'mods' folder found, skipping INI search.");
    }

    // Always ensure Outbreak class 'ALL' is registered and injected into database
    OutbreakModule::Instance().OnDatabaseLoaded();

    MOD_LOG_INFO("Database", "Database injection completed successfully!");
}

void DatabaseInjectorModule::ProcessIniFile(const std::filesystem::path& iniPath) {
    std::ifstream file(iniPath);
    if (!file.is_open()) {
        return;
    }

    PFN_GetTable GetTable = reinterpret_cast<PFN_GetTable>(Memory::Rva(0x004014F0));

    std::vector<IniRowEntry> entries;
    IniRowEntry currentEntry;
    bool inValidSection = false;
    bool inStringsSection = false;

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            if (inValidSection && !currentEntry.tableName.empty()) {
                entries.push_back(currentEntry);
                currentEntry.fields.clear();
            }

            std::string header = trimmed.substr(1, trimmed.size() - 2);
            std::string tableName;
            std::string label;

            size_t colonPos = header.find(':');
            if (colonPos != std::string::npos) {
                tableName = Trim(header.substr(0, colonPos));
                label = Trim(header.substr(colonPos + 1));
            } else {
                tableName = Trim(header);
                label = "";
            }

            if (EqualsIgnoreCase(tableName, "strings") || 
                EqualsIgnoreCase(tableName, "localization") || 
                EqualsIgnoreCase(tableName, "language")) {
                inStringsSection = true;
                inValidSection = false;
                currentEntry.tableName.clear();
                continue;
            }

            inStringsSection = false;

            // Check if tableName corresponds to a valid table in the game database
            TableDescriptor* table = GetTable(tableName.c_str());
            if (table != nullptr) {
                inValidSection = true;
                currentEntry.tableName = tableName;
                currentEntry.sectionLabel = label;
            } else {
                inValidSection = false;
                currentEntry.tableName.clear();
            }
            continue;
        }

        if (inStringsSection) {
            size_t eqPos = trimmed.find('=');
            if (eqPos != std::string::npos) {
                std::string key = Trim(trimmed.substr(0, eqPos));
                std::string val = StripQuotes(Trim(trimmed.substr(eqPos + 1)));
                if (!key.empty()) {
                    RegisterCustomString(key, val);
                    if (key.rfind("db_", 0) != 0) {
                        RegisterCustomString("db_" + key, val);
                    }
                }
            }
        } else if (inValidSection) {
            size_t eqPos = trimmed.find('=');
            if (eqPos != std::string::npos) {
                std::string key = Trim(trimmed.substr(0, eqPos));
                std::string val = StripQuotes(Trim(trimmed.substr(eqPos + 1)));
                if (!key.empty()) {
                    currentEntry.fields[key] = val;
                }
            }
        }
    }

    if (inValidSection && !currentEntry.tableName.empty()) {
        entries.push_back(currentEntry);
    }

    if (!entries.empty()) {
        MOD_LOG_INFO("Database", "File '{}': found {} database entries to process", 
            iniPath.string(), entries.size());
        ApplyDatabaseEntries(entries);
    }
}

void DatabaseInjectorModule::ApplyDatabaseEntries(const std::vector<IniRowEntry>& entries) {
    PFN_GetTable GetTable = reinterpret_cast<PFN_GetTable>(Memory::Rva(0x004014F0));

    // Group entries by table name
    std::map<std::string, std::vector<IniRowEntry>> grouped;
    for (const auto& entry : entries) {
        grouped[entry.tableName].push_back(entry);
    }

    for (const auto& [tableName, rowList] : grouped) {
        TableDescriptor* table = GetTable(tableName.c_str());
        if (!table) {
            MOD_LOG_WARN("Database", "Table '{}' not found in database schema, skipping.", tableName);
            continue;
        }

        MOD_LOG_INFO("Database", "Processing table '{}' (Columns: {}, Stride: {} bytes, TableIdx: {}):",
            tableName, table->num_columns, table->row_stride, table->table_index);

        for (uint32_t c = 0; c < table->num_columns; ++c) {
            const auto& col = table->columns[c];
            const char* typeStr = "unknown";
            if (col.type == 1) typeStr = "bool";
            else if (col.type == 2) typeStr = "int";
            else if (col.type == 3) typeStr = "string";
            else if (col.type == 4) typeStr = "float";

            MOD_LOG_INFO("Database", "    Col {:2d}: {:<28} type={:<6} size={:<2d} PK={}", 
                c, col.name, typeStr, col.size, col.key_type == 1 ? "YES" : "no");
        }

        // Find primary key column (key_type == 1, or named "id")
        int pkColIdx = -1;
        uint32_t pkOffset = 0;
        uint32_t runningOffset = 0;
        for (uint32_t c = 0; c < table->num_columns; ++c) {
            if (table->columns[c].key_type == 1 || EqualsIgnoreCase(table->columns[c].name, "id")) {
                pkColIdx = static_cast<int>(c);
                pkOffset = runningOffset;
                break;
            }
            runningOffset += table->columns[c].size;
        }

        std::vector<IniRowEntry> insertRows;

        for (const auto& iniRow : rowList) {
            uint8_t* existingRow = nullptr;

            // Register custom display_name if provided
            auto itDisp = iniRow.fields.find("display_name");
            auto itNameId = iniRow.fields.find("name_string_id");
            if (itDisp != iniRow.fields.end() && itNameId != iniRow.fields.end()) {
                RegisterCustomString(itNameId->second, itDisp->second);
                RegisterCustomString("db_" + itNameId->second, itDisp->second);
            }

            // Check if primary key is specified and matches an existing row
            if (pkColIdx != -1) {
                const auto& pkCol = table->columns[pkColIdx];
                for (const auto& [key, valStr] : iniRow.fields) {
                    if (EqualsIgnoreCase(key, pkCol.name)) {
                        // Scan existing chunks for matching PK
                        for (ChunkNode* chunk = table->first_chunk; chunk != nullptr; chunk = chunk->next) {
                            if (!chunk->row_data) continue;
                            for (uint32_t r = 0; r < chunk->row_count; ++r) {
                                uint8_t* rowPtr = chunk->row_data + r * table->row_stride;
                                uint8_t* valPtr = rowPtr + 4 + pkOffset;

                                if (pkCol.type == 2) { // int32
                                    try {
                                        int targetId = std::stoi(valStr, nullptr, 0);
                                        if (*reinterpret_cast<int32_t*>(valPtr) == targetId) {
                                            existingRow = rowPtr;
                                            break;
                                        }
                                    } catch (...) {}
                                } else if (pkCol.type == 3) { // string
                                    if (EqualsIgnoreCase(reinterpret_cast<char*>(valPtr), valStr)) {
                                        existingRow = rowPtr;
                                        break;
                                    }
                                }
                            }
                            if (existingRow) break;
                        }
                        break;
                    }
                }
            }

            if (existingRow != nullptr) {
                // UPDATE existing row in memory
                uint32_t colOffset = 0;
                for (uint32_t c = 0; c < table->num_columns; ++c) {
                    const auto& col = table->columns[c];
                    for (const auto& [key, valStr] : iniRow.fields) {
                        if (EqualsIgnoreCase(key, col.name)) {
                            uint8_t* fieldPtr = existingRow + 4 + colOffset;
                            try {
                                if (col.type == 1) { // bool
                                    bool b = (valStr == "1" || EqualsIgnoreCase(valStr, "true") || EqualsIgnoreCase(valStr, "yes"));
                                    *fieldPtr = b ? 1 : 0;
                                } else if (col.type == 2) { // int32
                                    *reinterpret_cast<int32_t*>(fieldPtr) = std::stoi(valStr, nullptr, 0);
                                } else if (col.type == 3) { // string
                                    strncpy_s(reinterpret_cast<char*>(fieldPtr), col.size, valStr.c_str(), _TRUNCATE);
                                } else if (col.type == 4) { // float
                                    *reinterpret_cast<float*>(fieldPtr) = std::stof(valStr);
                                }
                            } catch (const std::exception& ex) {
                                MOD_LOG_WARN("Database", "Failed to parse field '{}={}' on table '{}': {}", key, valStr, tableName, ex.what());
                            }
                            break;
                        }
                    }
                    colOffset += col.size;
                }
                MOD_LOG_INFO("Database", "  [UPDATE] Updated existing row in table '{}'", tableName);
            } else {
                // Row does not exist yet -> schedule for INSERT
                insertRows.push_back(iniRow);
            }
        }

        // Perform INSERT for all new rows
        if (!insertRows.empty()) {
            uint32_t rowCount = static_cast<uint32_t>(insertRows.size());
            uint32_t totalBytes = rowCount * table->row_stride;
            uint8_t* buffer = new uint8_t[totalBytes]();

            for (uint32_t r = 0; r < rowCount; ++r) {
                uint8_t* row = buffer + r * table->row_stride;

                // 4-byte header: 'I', 'T', 'M', table_index
                row[0] = 'I';
                row[1] = 'T';
                row[2] = 'M';
                row[3] = static_cast<uint8_t>(table->table_index);

                const auto& iniRow = insertRows[r];

                // Check if clone_from_id or clone_from is specified
                std::string cloneFromStr;
                for (const auto& [k, v] : iniRow.fields) {
                    if (EqualsIgnoreCase(k, "clone_from_id") || EqualsIgnoreCase(k, "clone_from") || EqualsIgnoreCase(k, "base_id")) {
                        cloneFromStr = v;
                        break;
                    }
                }

                if (!cloneFromStr.empty() && pkColIdx != -1) {
                    const auto& pkCol = table->columns[pkColIdx];
                    uint8_t* sourceRow = nullptr;

                    for (ChunkNode* chunk = table->first_chunk; chunk != nullptr; chunk = chunk->next) {
                        if (!chunk->row_data) continue;
                        for (uint32_t cr = 0; cr < chunk->row_count; ++cr) {
                            uint8_t* candRow = chunk->row_data + cr * table->row_stride;
                            uint8_t* candValPtr = candRow + 4 + pkOffset;

                            if (pkCol.type == 2) { // int32 PK
                                try {
                                    int targetId = std::stoi(cloneFromStr, nullptr, 0);
                                    if (*reinterpret_cast<int32_t*>(candValPtr) == targetId) {
                                        sourceRow = candRow;
                                        break;
                                    }
                                } catch (...) {}
                            } else if (pkCol.type == 3) { // string PK
                                if (EqualsIgnoreCase(reinterpret_cast<char*>(candValPtr), cloneFromStr)) {
                                    sourceRow = candRow;
                                    break;
                                }
                            }
                        }
                        if (sourceRow) break;
                    }

                    if (sourceRow) {
                        // Copy all columns from source row
                        memcpy(row + 4, sourceRow + 4, table->row_stride - 4);
                        MOD_LOG_INFO("Database", "  [CLONE] Cloned base data from row '{}' into new row in table '{}'",
                            cloneFromStr, tableName);

                        // If table is track_model, also clone conditions
                        if (EqualsIgnoreCase(tableName, "track_model")) {
                            try {
                                int srcTrackModelId = std::stoi(cloneFromStr, nullptr, 0);
                                int newTrackModelId = 0;
                                auto itId = iniRow.fields.find("id");
                                if (itId != iniRow.fields.end()) {
                                    newTrackModelId = std::stoi(itId->second, nullptr, 0);
                                }
                                if (srcTrackModelId != 0 && newTrackModelId != 0) {
                                    CloneTrackModelConditions(srcTrackModelId, newTrackModelId);
                                }
                            } catch (...) {}
                        }
                    } else {
                        MOD_LOG_WARN("Database", "  [CLONE] Source row '{}' not found in table '{}'!", cloneFromStr, tableName);
                    }
                }

                // Apply user-specified column overrides over top
                uint32_t colOffset = 0;
                for (uint32_t c = 0; c < table->num_columns; ++c) {
                    const auto& col = table->columns[c];
                    uint8_t* fieldPtr = row + 4 + colOffset;

                    for (const auto& [key, valStr] : iniRow.fields) {
                        // Skip modloader meta-keys
                        if (EqualsIgnoreCase(key, "clone_from_id") || 
                            EqualsIgnoreCase(key, "clone_from") || 
                            EqualsIgnoreCase(key, "base_id") || 
                            EqualsIgnoreCase(key, "display_name")) {
                            continue;
                        }

                        if (EqualsIgnoreCase(key, col.name)) {
                            try {
                                if (col.type == 1) { // bool
                                    bool b = (valStr == "1" || EqualsIgnoreCase(valStr, "true") || EqualsIgnoreCase(valStr, "yes"));
                                    *fieldPtr = b ? 1 : 0;
                                } else if (col.type == 2) { // int32
                                    *reinterpret_cast<int32_t*>(fieldPtr) = std::stoi(valStr, nullptr, 0);
                                } else if (col.type == 3) { // string
                                    strncpy_s(reinterpret_cast<char*>(fieldPtr), col.size, valStr.c_str(), _TRUNCATE);
                                } else if (col.type == 4) { // float
                                    *reinterpret_cast<float*>(fieldPtr) = std::stof(valStr);
                                }
                            } catch (const std::exception& ex) {
                                MOD_LOG_WARN("Database", "Failed to parse field '{}={}' on table '{}': {}", key, valStr, tableName, ex.what());
                            }
                            break;
                        }
                    }
                    colOffset += col.size;
                }
            }

            ChunkNode* newChunk = new ChunkNode();
            newChunk->row_data = buffer;
            newChunk->byte_size = totalBytes;
            newChunk->extension_id = 0; // 0 = base game (unlocked)
            newChunk->flags = 0;
            newChunk->next = nullptr;
            newChunk->row_count = rowCount;

            // Link into table
            if (!table->first_chunk) {
                table->first_chunk = newChunk;
            } else {
                ChunkNode* curr = table->first_chunk;
                while (curr->next) {
                    curr = curr->next;
                }
                curr->next = newChunk;
            }
            table->chunk_count++;

            MOD_LOG_INFO("Database", "  [INSERT] Injected {} new rows into table '{}' (Chunk: {} bytes)", 
                rowCount, tableName, totalBytes);
        }
    }
}

} // namespace modloader
