# DiRT 3 - Virtual File System (VFS / NeStorage / NeFS) Reversing Notes
**Target Binary**: `dirt3_game.exe` (32-bit x86 Windows, Base `0x00400000`)  
**Cross-Reference Binary**: `Grid 2` (32-bit x86 macOS Mach-O, Base `0x00001000`, full DWARF debug symbols)  
**Author / Analysis**: Pair Programming Session via IDA Pro MCP Server Bridge

---

## 1. Executive Summary & Breakthrough Discovery

In investigating how to redirect file loading in **DiRT 3** to load loose mod files without modifying base game archives, we discovered that **Codemasters' EGO Engine natively features a cascading, layered Virtual File System (VFS)** managed by `neon::NeStorageManager`.

### Key Findings:
1. **Mount Cascading / Fallthrough is Native**:
   - `NeStorageManager` maintains a doubly-linked list of `Mount` structures.
   - When searching for mounts (`findMount`), the engine iterates **in reverse order** (from newest mounted tail backwards to oldest mounted head).
   - If a mount has its `checkExists` flag (`m_cascade` at offset `+281`) set to `1` (true):
     - `NeStorageManager::read`, `allocRead`, and `open` call the storage device's `exists(relPath)` method.
     - If the file **exists** on that device, the file is served from that device.
     - If the file **does not exist**, `NeStorageManager` automatically cascades backwards to the previous mount matching the path prefix!
2. **Built-in Loose Filesystem Device (`NeStorageDeviceFS`)**:
   - DiRT 3 has a fully implemented raw Windows filesystem storage device (`"filesystem"`, `NeStorageDeviceFS`, vtable `0x00ED3BB8`).
   - Its `exists` method (`0x0078C420`) calls `GetFileAttributesA(basePath + relPath)`.
   - Its `read` method (`0x0078D720`) calls Windows `CreateFileA` and `ReadFile`.
3. **ModLoader Strategy**:
   - We do **NOT** need to hook low-level Windows APIs (`CreateFileA`/`ReadFile`) or patch hundreds of game reading functions.
   - We simply hook the end of `game::StorageSystem::initialise` (`0x0059292d` - `0x0059293a`), right after vanilla devices are mounted and right before `cd("/data/")`.
   - ModLoader mounts each enabled mod folder to `"/data/"` using `NeStorageDeviceFS` with `checkExists = 1`.
   - Any modded file (car `.pssg`, track models, XML configs, `database.bin`, audio) will instantly override vanilla files on disk. Any unmodded file transparently falls through to vanilla bigfiles!

---

## 2. Core VFS Architecture & Memory Addresses

### A. Global Storage Manager (`NeGlobalStorageManager`)
- **Global Pointer**: `0x010CA17C` (`dword_10CA17C` = `neon::NeGlobalStorageManager::m_globalStorageManager`)
- **Getter Function**: `0x009D4930` (`sub_9D4930` = `neon::NeGlobalStorageManager::get()`)
- **Initializer**: `0x009D48C0` (`sub_9D48C0`)
- **Global Allocator Pool**: `0x00768E50` (`sub_768E50()` returns `NeIAllocPool*` at `&dword_1004DC0`)

### B. `neon::NeStorageManager` Structure Layout
`NeStorageManager` instance pointer is `dword_10CA17C`.
- `+0x00`: Vtable pointer
- `+0x08`: `NeIAllocPool* m_allocPool` (internal allocator)
- `+0xA0` (`+160`): `Mount* m_headMount` (first mounted device)
- `+0xA4` (`+164`): `Mount* m_tailMount` (last mounted device — **highest priority**)
- `+0xA8` (`+168`): `uint32_t m_numMounts` (current count, max limit = 16)
- `+0x1E4` (`+484`): `NeStringStatic<260> m_cwd` (current working directory, default: `"/data/"`)
- `+0x2F8` (`+760`): `NeCriticalSection m_criticalSection`

### C. `Mount` Object Layout (Size: 292 bytes = `0x124`)
Allocated via `m_allocPool->alloc(292, 4)`.
- `+0x00`: `NeStorageDevice* m_device` (device instance)
- `+0x04`: `void* m_stringVtable` (`0x00EC6F9C` = `NeStringStatic<260>` vtable)
- `+0x08`: `uint32_t m_bufCapacity` (260 = `0x104`)
- `+0x0C`: `char* m_mountPrefix` (pointer to string at `+0x10`, e.g. `"/data/"`)
- `+0x10`: `char m_prefixBuffer[260]` (buffer holding normalized mount prefix with trailing `/`)
- `+0x114` (`+276`): `uint32_t m_flags` (device options)
- `+0x118` (`+280`): `bool m_readOnly`
- `+0x119` (`+281`): `bool m_checkExists` (**`cascade` flag**: if `1`, triggers existence checks and enables fallthrough)
- `+0x11C` (`+284`): `Mount* m_next` (forward linked list pointer)
- `+0x120` (`+288`): `Mount* m_prev` (backward linked list pointer — used by `findMount`)

---

## 3. Key Functions in `NeStorageManager`

### 1. `mount` (`0x007635F0` / `sub_7635F0`)
```cpp
int __thiscall NeStorageManager::mount(
    void* thisPtr,               // ECX = dword_10CA17C
    const char* mountPoint,      // e.g. "/data/"
    void* params,                // NeStorageDeviceParams* (e.g. NeStorageDeviceParamsFS)
    bool readOnly,               // 0 for false, 1 for true
    bool checkExists,            // 1 to enable cascading/fallthrough!
    void** outMountHandle,       // [out] receives Mount*
    int tag                      // -1
);
```
**Mechanism**:
1. Checks that `m_numMounts < 16`.
2. Allocates `Mount` structure (292 bytes).
3. Looks up device name in `s_deviceRegistry` (`0x01004E50`) and calls its factory.
4. Appends new `Mount` to `m_tailMount` (`this + 164`).
5. Increments `m_numMounts`.

### 2. `unmount` (`0x007637E0` / `sub_7637E0`)
```cpp
int __thiscall NeStorageManager::unmount(
    void* thisPtr,               // ECX = dword_10CA17C
    void* mountHandle,           // Mount* returned by mount()
    int tag                      // -1
);
```
Unlinks `Mount` from doubly-linked list, destructs device, frees memory, and decrements `m_numMounts`.

### 3. `findMount` (`0x0075F1F0` / `sub_75F1F0`)
```cpp
Mount* __thiscall NeStorageManager::findMount(
    void* thisPtr,               // ECX
    Mount* startMount,           // NULL to start at m_tailMount, or previous mount to continue search
    const char* path,            // e.g. "cars/audi.xml" or "/data/cars/audi.xml"
    NeStringStatic<260>* outPath,// receives path stripped of mount prefix
    bool a5
);
```
**Algorithm**:
1. Normalizes `path` to full canonical path via `fullPath`.
2. If `startMount == NULL`, sets `current = this->m_tailMount` (**latest mounted device**).
3. Else, sets `current = startMount->m_prev`.
4. Loops backwards: checks `strncmp(current->m_mountPrefix, fullPath, prefixLen)`.
5. If matched: strips prefix from path and returns `current`.
6. If end reached: returns `NULL`.

### 4. `fullPath` (`0x0075EEA0` / `sub_75EEA0`)
- If path starts with `'/'` (e.g. `"/data/cars/xxx"`): used as-is.
- If path is relative (e.g. `"cars/xxx"`): prepends `m_cwd` (`"/data/"`), turning it into `"/data/cars/xxx"`.
- Normalizes `.` and `..` segments.

### 5. `cd` (`0x0075F350` / `sub_75F350`)
- Sets current working directory (`m_cwd` at `this + 484`). DiRT 3 sets this to `"/data/"` at startup.

### 6. `open` (`0x0075F3D0`), `read` (`0x0075FFA0`), `allocRead` (`0x0075F980`)
Each of these functions implements mount cascading:
```cpp
Mount* mount = findMount(NULL, path, &relPath);
while (mount) {
    if (mount->m_checkExists) { // flag at +281
        // Query slot 7 (exists) on device:
        mount->m_device->exists(relPath, &op);
        if (op.error == 0) {
            // FILE EXISTS ON THIS MOUNT!
            // Proceed with open/read on this mount's device:
            mount->m_device->read(relPath, ...);
            return;
        }
    } else {
        // No existence check requested; proceed directly
        mount->m_device->read(relPath, ...);
        return;
    }
    // File not found on this mount: cascade to next mount!
    mount = findMount(mount, path, &relPath);
}
// All mounts exhausted -> return ERROR_FILE_NOT_FOUND
```

---

## 4. `NeStorageDeviceFS` (Loose Windows Filesystem)

The engine has a built-in device for accessing standard Windows directories.

- **Registration Name**: `"filesystem"`
- **Static Registrar**: `0x00DE3430`
- **Factory Function**: `0x0078E610` (`sub_78E610`)
- **Constructor**: `0x0078BBD0` (`sub_78BBD0`)
- **Device Vtable**: `0x00ED3BB8`

### Vtable Slot Mapping (`0x00ED3BB8`)
| Slot | Offset | Function | Description |
|---|---|---|---|
| 0 | `+0x00` | `0x0078E5E0` | Scalar deleting destructor (`~NeStorageDeviceFS`) |
| 1 | `+0x04` | `0x0078D260` | Destructor variant |
| 2 | `+0x08` | `0x0078D400` | `cancel(NeStorageOp*)` |
| 3 | `+0x0C` | `0x0078BDC0` | `cancelAll()` |
| 4 | `+0x10` | `0x0078E080` | `updatePriority(...)` |
| 5 | `+0x14` | `0x0078C2E0` | `dir(...)` (directory listing) |
| 6 | `+0x18` | `0x0078BDD0` | `mkdir(...)` |
| 7 | `+0x1C` | `0x0078C420` | **`exists(char* relPath, NeStorageOp*)`** — calls `GetFileAttributesA` |
| 8 | `+0x20` | `0x0078D720` | **`read(...)`** — calls `CreateFileA` + `ReadFile` |
| 9 | `+0x24` | `0x0078DA60` | `readNext(...)` |
| 10 | `+0x28` | `0x0078BF00` | `freeSpace(...)` |
| 11 | `+0x2C` | `0x0078DCC0` | `write(...)` — calls `CreateFileA` + `WriteFile` |
| 12 | `+0x30` | `0x0078DED0` | `writeNext(...)` |
| 13 | `+0x34` | `0x0078C170` | `resize(...)` |
| 14 | `+0x38` | `0x0078C910` | `rm(...)` (delete file) |
| 15 | `+0x3C` | `0x0078D590` | **`open(...)`** — calls `CreateFileA` |
| 16 | `+0x40` | `0x0078D350` | **`close(...)`** — calls `CloseHandle` |

### `NeStorageDeviceParamsFS` Layout (16 bytes)
Used when mounting `"filesystem"`:
```cpp
struct NeStorageDeviceParamsFS {
    void* vtable;              // +0x00: 0x00EABB3C (off_EABB3C)
    void* allocPool;           // +0x04: NeIAllocPool* (sub_768E50())
    const char* rootPath;      // +0x08: Root path (e.g. "mods\\MyMod" or "D:\\Games\\DiRT3\\mods\\MyMod")
    bool isAbsolutePath;       // +0x0C: 1 if absolute path, 0 if relative to game directory
    char padding[3];           // +0x0D: alignment padding
};
```

---

## 5. Vanilla Storage Initialization Sequence

Located in `game::StorageSystem::initialise` (`0x00592780` in DiRT 3):
1. `0x005927a4`: Fetches `STORAGE_MANAGER` and initializes `NeGlobalStorageManager`.
2. `0x005928d4`: `sub_5749A0` mounts temporary scratch device `"/ramdisc/"`.
3. `0x005928f5`: `sub_58EBC0` mounts primary game device:
   - Target: `"/data/"`
   - Device: `NeStorageDeviceFileStream` (streaming from primary base bigfiles)
   - `checkExists`: `0`
4. `0x00592900`: `sub_58ECC0` opens `win_000.nfs` (DLC / update archive).
5. `0x00592917`: `sub_58EE50` mounts network/DLC devices:
   - Target: `"/data/"` (device: `NetworkFileStream`, `checkExists`: `1`)
   - Target: `"/data/network/"`
6. `0x0059292d` - `0x0059293a`:
   ```asm
   0x0059292d: mov  ecx, dword_10CA17C
   0x00592933: push 0FFFFFFFFh
   0x00592935: push offset aData_0 ; "/data/"
   0x0059293a: call sub_75F350     ; cd("/data/")
   ```

---

## 6. Comparison: EGO 2.0 (DiRT 3) vs EGO 3.0 (GRID 2)

From our analysis of `Grid 2` debug symbols:
- In EGO 3.0, Codemasters added an explicit `game::ModSupport` class (`0x00373FE0`):
  - Enabled via command-line argument: `-mod <path/to/mods.xml>`.
  - Parsed XML listing `<Mod Path="mods/foo"/>`.
  - Implemented a custom `NeStorageDevice` (named `"misc"`) with `findFile` checking disk paths.
  - In `game::StorageSystem::initialise` (`0x0032DEC2`), it mounted this mod device directly to `"/data/"` with `checkExists = 1` right before `cd("/data/")`!
- In EGO 2.0 (DiRT 3), `game::ModSupport` was not yet written, **BUT the underlying VFS architecture (`NeStorageManager`, `NeStorageDeviceFS`, and mount cascading) is 100% identical!**
- This proves that mounting loose mod folders to `"/data/"` with `checkExists = 1` is not an external hack — **it is the exact architectural design Codemasters themselves used when adding official mod support to the engine in the next release!**

---

## 7. ModLoader File Redirection Implementation Blueprint

### Hook Target
- **Hook Location**: `0x0059292d` (in `game::StorageSystem::initialise`)
- **Timing**: Right after `sub_58EE50` returns and right before `sub_75F350` (`cd("/data/")`).
- **State**: Vanilla VFS is completely set up; zero game assets/databases have been loaded yet.

### C++ Implementation Structure for ModLoader
```cpp
#pragma pack(push, 1)
struct NeStorageDeviceParamsFS {
    void* vtable;           // 0x00EABB3C
    void* allocPool;        // sub_768E50()
    const char* rootPath;   // e.g. "mods\\ActiveMod\\"
    bool isAbsolutePath;    // true (1)
    char pad[3];
};
#pragma pack(pop)

typedef int* (*NeMemorySystem_getSingleton_t)();
typedef int (__thiscall *NeStorageManager_mount_t)(
    void* thisPtr, 
    const char* mountPoint, 
    void* params, 
    bool readOnly, 
    bool checkExists, 
    void** outMount, 
    int tag
);
typedef int (__thiscall *NeStorageManager_cd_t)(
    void* thisPtr, 
    const char* dir, 
    int tag
);

void MountActiveMods() {
    void* storageMgr = *(void**)0x010CA17C;
    auto pGetPool = (NeMemorySystem_getSingleton_t)0x00768E50;
    auto pMount = (NeStorageManager_mount_t)0x007635F0;
    void* allocPool = pGetPool();

    // 1. Scan mods directory (e.g. D:\Games\DiRT3\mods\)
    // 2. For each enabled mod in descending priority order (lowest to highest):
    static NeStorageDeviceParamsFS modParams;
    static void* outMountHandle = nullptr;

    modParams.vtable = (void*)0x00EABB3C;
    modParams.allocPool = allocPool;
    modParams.rootPath = "D:\\Games\\DiRT 3\\mods\\MyMod\\";
    modParams.isAbsolutePath = true;

    // Mount to "/data/" with checkExists = true (a5 = 1)
    pMount(storageMgr, "/data/", &modParams, false, true, &outMountHandle, -1);
}
```

### Advantages of Native VFS Cascading
1. **Zero Hook Overhead**: Hook is only run once during initialization.
2. **Format Agnostic**: Works for every file type (`.pssg`, `.xml`, `.bik`, `.lng`, `.bin`, `.jpk`, etc.).
3. **Subsystem Agnostic**: Works for cars, liveries, tracks, audio, frontend UI, shaders, physics, databases.
4. **Safe Fallthrough**: If a mod doesn't provide a file, vanilla game archives supply it instantly with zero error.
5. **Multi-Mod Priority**: Mounting Mod A, then Mod B means Mod B is searched first, then Mod A, then vanilla bigfiles!
