#include "outbreak_module.hpp"
#include "logger.hpp"
#include "hook_manager.hpp"
#include "memory.hpp"
#include "../database_injector/database_injector_module.hpp"

#include <cstring>
#include <algorithm>
#include <vector>
#include <map>
#include <set>

namespace modloader {

typedef TableDescriptor* (__cdecl* PFN_GetTable)(const char* name);

static inline bool EqualsIgnoreCase(const char* a, const char* b) {
    return _stricmp(a, b) == 0;
}

bool OutbreakModule::Initialize() {
    MOD_LOG_INFO("OutbreakMod", "Initializing Outbreak All-Cars Module (ACAT Architecture)...");

    // 1. Patch disable_online check in ScreenVehicleSelect (0x00503AD4)
    // 0x00503AD4: 75 1A (jnz short loc_503AF0) -> EB 1A (jmp short loc_503AF0)
    // This bypasses the disable_online == 0 filter so DLC & bonus cars are visible
    uintptr_t patchAddr = Memory::Rva(0x00503AD4);
    uint8_t patchBytes[] = { 0xEB, 0x1A };
    if (Memory::Patch(patchAddr, patchBytes, sizeof(patchBytes))) {
        MOD_LOG_INFO("OutbreakMod", "Patched 0x{:08X} (disable_online check bypassed) -> All DLC/bonus cars selectable", patchAddr);
    } else {
        MOD_LOG_WARN("OutbreakMod", "Failed to patch disable_online check at 0x{:08X}", patchAddr);
    }

    // 2. Pre-register string localizations for "ANY"
    DatabaseInjectorModule::Instance().RegisterCustomString("vehicle_any", "ANY");
    DatabaseInjectorModule::Instance().RegisterCustomString("db_vehicle_any", "ANY");
    DatabaseInjectorModule::Instance().RegisterCustomString("single_race_vehicle_group_22", "ANY");
    DatabaseInjectorModule::Instance().RegisterCustomString("db_single_race_vehicle_group_22", "ANY");

    return true;
}

void OutbreakModule::Shutdown() {
}

void OutbreakModule::OnDatabaseLoaded() {
    ApplyVehicleUnlocks();
}

void OutbreakModule::ApplyVehicleUnlocks() {
    PFN_GetTable GetTable = reinterpret_cast<PFN_GetTable>(Memory::Rva(0x004014F0));
    if (!GetTable) {
        MOD_LOG_ERROR("OutbreakMod", "GetTable function (0x004014F0) not available!");
        return;
    }

    MOD_LOG_INFO("OutbreakMod", "=========================================================");
    MOD_LOG_INFO("OutbreakMod", "   Applying ACAT Vehicle Class Restrictions & Unlocks   ");
    MOD_LOG_INFO("OutbreakMod", "=========================================================");

    // =========================================================================
    // 1. UNLOCK ALL 16 CLASSES IN 'net_vehicle_class_restrictions'
    // =========================================================================
    // In vanilla DiRT 3, Outbreak (race_type=12) only allows class 31 (Gymkhana).
    // Adding all 16 valid multiplayer classes unlocks the "Vehicle Class"
    // option in Outbreak lobby so ANY car can be selected via its class!
    TableDescriptor* restrTable = GetTable("net_vehicle_class_restrictions");
    if (restrTable && restrTable->first_chunk) {
        int rIdOffset = -1;
        int rRaceTypeOffset = -1;
        int rVehClassOffset = -1;
        uint32_t runningOffset = 0;

        for (uint32_t c = 0; c < restrTable->num_columns; ++c) {
            const auto& col = restrTable->columns[c];
            if (EqualsIgnoreCase(col.name, "id") || col.key_type == 1) {
                rIdOffset = static_cast<int>(runningOffset);
            } else if (EqualsIgnoreCase(col.name, "net_race_type_id") || EqualsIgnoreCase(col.name, "race_type") || EqualsIgnoreCase(col.name, "race_type_id")) {
                rRaceTypeOffset = static_cast<int>(runningOffset);
            } else if (EqualsIgnoreCase(col.name, "net_vehicle_class_id") || EqualsIgnoreCase(col.name, "vehicle_class") || EqualsIgnoreCase(col.name, "vehicle_class_id")) {
                rVehClassOffset = static_cast<int>(runningOffset);
            }
            runningOffset += col.size;
        }

        if (rRaceTypeOffset != -1 && rVehClassOffset != -1) {
            uint8_t* templateRestr = nullptr;
            std::map<int, std::vector<int>> existingClassesByRaceType;
            int maxRestrId = 1000;

            for (ChunkNode* ch = restrTable->first_chunk; ch != nullptr; ch = ch->next) {
                if (!ch->row_data) continue;
                for (uint32_t r = 0; r < ch->row_count; ++r) {
                    uint8_t* rowPtr = ch->row_data + r * restrTable->row_stride;
                    if (rIdOffset != -1) {
                        int curId = *reinterpret_cast<int32_t*>(rowPtr + 4 + rIdOffset);
                        if (curId > maxRestrId) maxRestrId = curId;
                    }
                    int rt = *reinterpret_cast<int32_t*>(rowPtr + 4 + rRaceTypeOffset);
                    int vc = *reinterpret_cast<int32_t*>(rowPtr + 4 + rVehClassOffset);
                    existingClassesByRaceType[rt].push_back(vc);
                    if (rt == 12 && !templateRestr) {
                        templateRestr = rowPtr;
                    }
                }
            }

            if (!templateRestr && restrTable->first_chunk->row_data) {
                templateRestr = restrTable->first_chunk->row_data;
            }

            // All 16 legitimate multiplayer vehicle classes from ACAT:
            // 16: WRC Rally, 17: Rallycross, 18: Open Trailblazer, 19: Super Buggies
            // 20: Raid, 21: Stadium Trucks, 22: Open Rally, 23: 90s Rally, 24: 80s Rally
            // 26: Group B Rally, 27: 70s Rally, 28: 60s Rally, 29: Classic Trailblazer
            // 30: S2000 Rally, 31: Gymkhana, 34: Modern Classic Trailblazer
            const int targetClasses[] = {
                16, 17, 18, 19, 20, 21, 22, 23, 24, 26, 27, 28, 29, 30, 31, 34
            };

            // Target multiplayer and party race types:
            // 12 = Outbreak, 10 = Invasion, 11 = Outpost, 13 = Transporter, 14 = Cat and Mouse, 15 = Joyride
            // 2, 3, 4, 9, 16 = standard multiplayer disciplines
            const int targetRaceTypes[] = { 2, 3, 4, 9, 10, 11, 12, 13, 14, 15, 16 };

            std::vector<std::pair<int, int>> entriesToAdd;
            for (int rt : targetRaceTypes) {
                const auto& existing = existingClassesByRaceType[rt];
                for (int tc : targetClasses) {
                    if (std::find(existing.begin(), existing.end(), tc) == existing.end()) {
                        entriesToAdd.push_back({ rt, tc });
                    }
                }
            }

            if (!entriesToAdd.empty() && templateRestr) {
                uint32_t addCount = static_cast<uint32_t>(entriesToAdd.size());
                uint32_t addBytes = addCount * restrTable->row_stride;
                uint8_t* addBuffer = new uint8_t[addBytes]();

                for (uint32_t i = 0; i < addCount; ++i) {
                    uint8_t* row = addBuffer + i * restrTable->row_stride;
                    memcpy(row, templateRestr, restrTable->row_stride);

                    if (rIdOffset != -1) {
                        *reinterpret_cast<int32_t*>(row + 4 + rIdOffset) = ++maxRestrId;
                    }
                    *reinterpret_cast<int32_t*>(row + 4 + rRaceTypeOffset) = entriesToAdd[i].first;
                    *reinterpret_cast<int32_t*>(row + 4 + rVehClassOffset) = entriesToAdd[i].second;
                }

                ChunkNode* newChunk = new ChunkNode();
                newChunk->row_data = addBuffer;
                newChunk->byte_size = addBytes;
                newChunk->extension_id = 0;
                newChunk->flags = 0;
                newChunk->next = nullptr;
                newChunk->row_count = addCount;

                ChunkNode* cTail = restrTable->first_chunk;
                while (cTail->next) cTail = cTail->next;
                cTail->next = newChunk;
                restrTable->chunk_count++;

                MOD_LOG_INFO("OutbreakMod", "Injected {} missing restriction entries into 'net_vehicle_class_restrictions'.", addCount);
                MOD_LOG_INFO("OutbreakMod", "Outbreak (race_type=12) now has all {} classes selectable in lobby!",
                    existingClassesByRaceType[12].size() + (sizeof(targetClasses)/sizeof(targetClasses[0]) - existingClassesByRaceType[12].size()));
            } else {
                MOD_LOG_INFO("OutbreakMod", "All vehicle classes already present in 'net_vehicle_class_restrictions'.");
            }
        }
    } else {
        MOD_LOG_WARN("OutbreakMod", "Table 'net_vehicle_class_restrictions' not found or empty.");
    }

    // =========================================================================
    // 2. ADD 'vehicle_any' (id=22) TO 'single_race_vehicle_groups'
    // =========================================================================
    TableDescriptor* groupsTable = GetTable("single_race_vehicle_groups");
    bool hasGroup22 = false;
    if (groupsTable && groupsTable->first_chunk) {
        int gIdOffset = -1;
        int gNameOffset = -1;
        uint32_t gNameSize = 0;
        int gOrderOffset = -1;
        uint32_t runningOffset = 0;

        for (uint32_t c = 0; c < groupsTable->num_columns; ++c) {
            const auto& col = groupsTable->columns[c];
            if (EqualsIgnoreCase(col.name, "id") || col.key_type == 1) {
                gIdOffset = static_cast<int>(runningOffset);
            } else if (EqualsIgnoreCase(col.name, "name") || EqualsIgnoreCase(col.name, "vehicle_group")) {
                gNameOffset = static_cast<int>(runningOffset);
                gNameSize = col.size;
            } else if (EqualsIgnoreCase(col.name, "order_index") || EqualsIgnoreCase(col.name, "order")) {
                gOrderOffset = static_cast<int>(runningOffset);
            }
            runningOffset += col.size;
        }

        uint8_t* templateGroup = nullptr;
        for (ChunkNode* ch = groupsTable->first_chunk; ch != nullptr; ch = ch->next) {
            if (!ch->row_data) continue;
            for (uint32_t r = 0; r < ch->row_count; ++r) {
                uint8_t* rowPtr = ch->row_data + r * groupsTable->row_stride;
                if (!templateGroup) templateGroup = rowPtr;
                if (gIdOffset != -1) {
                    int curId = *reinterpret_cast<int32_t*>(rowPtr + 4 + gIdOffset);
                    if (curId == 22) {
                        hasGroup22 = true;
                        break;
                    }
                }
                if (gNameOffset != -1) {
                    const char* curName = reinterpret_cast<const char*>(rowPtr + 4 + gNameOffset);
                    if (EqualsIgnoreCase(curName, "vehicle_any")) {
                        hasGroup22 = true;
                        break;
                    }
                }
            }
            if (hasGroup22) break;
        }

        if (!hasGroup22 && templateGroup && gIdOffset != -1) {
            uint8_t* newGroupBuf = new uint8_t[groupsTable->row_stride]();
            memcpy(newGroupBuf, templateGroup, groupsTable->row_stride);

            *reinterpret_cast<int32_t*>(newGroupBuf + 4 + gIdOffset) = 22;
            if (gNameOffset != -1 && gNameSize > 0) {
                strncpy_s(reinterpret_cast<char*>(newGroupBuf + 4 + gNameOffset), gNameSize, "vehicle_any", _TRUNCATE);
            }
            if (gOrderOffset != -1) {
                *reinterpret_cast<int32_t*>(newGroupBuf + 4 + gOrderOffset) = 0;
            }

            ChunkNode* newChunk = new ChunkNode();
            newChunk->row_data = newGroupBuf;
            newChunk->byte_size = groupsTable->row_stride;
            newChunk->extension_id = 0;
            newChunk->flags = 0;
            newChunk->next = nullptr;
            newChunk->row_count = 1;

            ChunkNode* cTail = groupsTable->first_chunk;
            while (cTail->next) cTail = cTail->next;
            cTail->next = newChunk;
            groupsTable->chunk_count++;

            hasGroup22 = true;
            MOD_LOG_INFO("OutbreakMod", "Injected group 'vehicle_any' (id=22) into 'single_race_vehicle_groups'!");
        } else if (hasGroup22) {
            MOD_LOG_INFO("OutbreakMod", "Group 'vehicle_any' (id=22) already exists in 'single_race_vehicle_groups'.");
        }
    }

    // =========================================================================
    // 3. MAP GROUP 22 TO ALL CLASSES IN 'single_race_vehicle_class'
    // =========================================================================
    TableDescriptor* srvcTable = GetTable("single_race_vehicle_class");
    if (srvcTable && srvcTable->first_chunk) {
        int sIdOffset = -1;
        int sGroupOffset = -1;
        int sClassOffset = -1;
        uint32_t runningOffset = 0;

        for (uint32_t c = 0; c < srvcTable->num_columns; ++c) {
            const auto& col = srvcTable->columns[c];
            if (EqualsIgnoreCase(col.name, "id") || col.key_type == 1) {
                sIdOffset = static_cast<int>(runningOffset);
            } else if (EqualsIgnoreCase(col.name, "single_race_vehicle_group_id") || 
                       EqualsIgnoreCase(col.name, "single_race_vehicle_groups") ||
                       EqualsIgnoreCase(col.name, "vehicle_group_id") || 
                       EqualsIgnoreCase(col.name, "group_id")) {
                sGroupOffset = static_cast<int>(runningOffset);
            } else if (EqualsIgnoreCase(col.name, "vehicle_class_id") || 
                       EqualsIgnoreCase(col.name, "vehicle_class") || 
                       EqualsIgnoreCase(col.name, "class_id")) {
                sClassOffset = static_cast<int>(runningOffset);
            }
            runningOffset += col.size;
        }

        if (sGroupOffset != -1 && sClassOffset != -1) {
            uint8_t* templateRow = srvcTable->first_chunk->row_data;
            std::set<int> existingClassesFor22;
            int maxId = 1000;

            for (ChunkNode* ch = srvcTable->first_chunk; ch != nullptr; ch = ch->next) {
                if (!ch->row_data) continue;
                for (uint32_t r = 0; r < ch->row_count; ++r) {
                    uint8_t* rowPtr = ch->row_data + r * srvcTable->row_stride;
                    if (sIdOffset != -1) {
                        int curId = *reinterpret_cast<int32_t*>(rowPtr + 4 + sIdOffset);
                        if (curId > maxId) maxId = curId;
                    }
                    int gid = *reinterpret_cast<int32_t*>(rowPtr + 4 + sGroupOffset);
                    int cid = *reinterpret_cast<int32_t*>(rowPtr + 4 + sClassOffset);
                    if (gid == 22) {
                        existingClassesFor22.insert(cid);
                    }
                }
            }

            // Target single-race classes mapped to group 22 in ACAT:
            const int targetGroupClasses[] = {
                17, 18, 20, 21, 22, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34
            };

            std::vector<int> classesToAdd;
            for (int cid : targetGroupClasses) {
                if (existingClassesFor22.find(cid) == existingClassesFor22.end()) {
                    classesToAdd.push_back(cid);
                }
            }

            if (!classesToAdd.empty() && templateRow) {
                uint32_t addCount = static_cast<uint32_t>(classesToAdd.size());
                uint32_t addBytes = addCount * srvcTable->row_stride;
                uint8_t* addBuf = new uint8_t[addBytes]();

                for (uint32_t i = 0; i < addCount; ++i) {
                    uint8_t* row = addBuf + i * srvcTable->row_stride;
                    memcpy(row, templateRow, srvcTable->row_stride);

                    if (sIdOffset != -1) {
                        *reinterpret_cast<int32_t*>(row + 4 + sIdOffset) = ++maxId;
                    }
                    *reinterpret_cast<int32_t*>(row + 4 + sGroupOffset) = 22;
                    *reinterpret_cast<int32_t*>(row + 4 + sClassOffset) = classesToAdd[i];
                }

                ChunkNode* newChunk = new ChunkNode();
                newChunk->row_data = addBuf;
                newChunk->byte_size = addBytes;
                newChunk->extension_id = 0;
                newChunk->flags = 0;
                newChunk->next = nullptr;
                newChunk->row_count = addCount;

                ChunkNode* cTail = srvcTable->first_chunk;
                while (cTail->next) cTail = cTail->next;
                cTail->next = newChunk;
                srvcTable->chunk_count++;

                MOD_LOG_INFO("OutbreakMod", "Mapped {} vehicle classes to group 22 in 'single_race_vehicle_class'!", addCount);
            }
        }
    }

    // =========================================================================
    // 4. ADD GROUP 22 TO ALL DISCIPLINES IN 'eligible_vehicle_classes'
    // =========================================================================
    TableDescriptor* eligTable = GetTable("eligible_vehicle_classes");
    if (eligTable && eligTable->first_chunk) {
        int eIdOffset = -1;
        int eDiscOffset = -1;
        int eGroupOffset = -1;
        uint32_t runningOffset = 0;

        for (uint32_t c = 0; c < eligTable->num_columns; ++c) {
            const auto& col = eligTable->columns[c];
            if (EqualsIgnoreCase(col.name, "id") || col.key_type == 1) {
                eIdOffset = static_cast<int>(runningOffset);
            } else if (EqualsIgnoreCase(col.name, "discipline_id") || EqualsIgnoreCase(col.name, "discipline")) {
                eDiscOffset = static_cast<int>(runningOffset);
            } else if (EqualsIgnoreCase(col.name, "single_race_vehicle_group_id") || 
                       EqualsIgnoreCase(col.name, "single_race_vehicle_groups") || 
                       EqualsIgnoreCase(col.name, "vehicle_group_id") || 
                       EqualsIgnoreCase(col.name, "group_id")) {
                eGroupOffset = static_cast<int>(runningOffset);
            }
            runningOffset += col.size;
        }

        if (eDiscOffset != -1 && eGroupOffset != -1) {
            uint8_t* templateRow = eligTable->first_chunk->row_data;
            std::set<int> disciplinesWith22;
            int maxId = 1000;

            for (ChunkNode* ch = eligTable->first_chunk; ch != nullptr; ch = ch->next) {
                if (!ch->row_data) continue;
                for (uint32_t r = 0; r < ch->row_count; ++r) {
                    uint8_t* rowPtr = ch->row_data + r * eligTable->row_stride;
                    if (eIdOffset != -1) {
                        int curId = *reinterpret_cast<int32_t*>(rowPtr + 4 + eIdOffset);
                        if (curId > maxId) maxId = curId;
                    }
                    int did = *reinterpret_cast<int32_t*>(rowPtr + 4 + eDiscOffset);
                    int gid = *reinterpret_cast<int32_t*>(rowPtr + 4 + eGroupOffset);
                    if (gid == 22) {
                        disciplinesWith22.insert(did);
                    }
                }
            }

            // Target single-race disciplines from ACAT:
            const int targetDisciplines[] = { 1, 2, 5, 6, 7, 8, 9, 10, 11 };

            std::vector<int> discsToAdd;
            for (int did : targetDisciplines) {
                if (disciplinesWith22.find(did) == disciplinesWith22.end()) {
                    discsToAdd.push_back(did);
                }
            }

            if (!discsToAdd.empty() && templateRow) {
                uint32_t addCount = static_cast<uint32_t>(discsToAdd.size());
                uint32_t addBytes = addCount * eligTable->row_stride;
                uint8_t* addBuf = new uint8_t[addBytes]();

                for (uint32_t i = 0; i < addCount; ++i) {
                    uint8_t* row = addBuf + i * eligTable->row_stride;
                    memcpy(row, templateRow, eligTable->row_stride);

                    if (eIdOffset != -1) {
                        *reinterpret_cast<int32_t*>(row + 4 + eIdOffset) = ++maxId;
                    }
                    *reinterpret_cast<int32_t*>(row + 4 + eDiscOffset) = discsToAdd[i];
                    *reinterpret_cast<int32_t*>(row + 4 + eGroupOffset) = 22;
                }

                ChunkNode* newChunk = new ChunkNode();
                newChunk->row_data = addBuf;
                newChunk->byte_size = addBytes;
                newChunk->extension_id = 0;
                newChunk->flags = 0;
                newChunk->next = nullptr;
                newChunk->row_count = addCount;

                ChunkNode* cTail = eligTable->first_chunk;
                while (cTail->next) cTail = cTail->next;
                cTail->next = newChunk;
                eligTable->chunk_count++;

                MOD_LOG_INFO("OutbreakMod", "Added group 22 (ANY) to {} disciplines in 'eligible_vehicle_classes'!", addCount);
            }
        }
    }

    MOD_LOG_INFO("OutbreakMod", "ACAT Vehicle Class Unlocks applied successfully!");
}

} // namespace modloader
