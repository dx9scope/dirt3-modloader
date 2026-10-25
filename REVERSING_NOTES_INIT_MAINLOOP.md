# DiRT 3 (EGO 2.0) vs GRID 2 (EGO 3.0) — Reverse Engineering Notes
## Подсистема: Инициализация движка и Main Game Loop

Дата анализа: 19.09.2026
Источники:
* **Target binary**: DiRT 3 (`dirt3_game.exe`, x86 Windows, база `0x400000`)
* **Reference binary**: GRID 2 macOS Mach-O с отладочными символами Feral/Codemasters (`Grid 2`, база `0x1000`)

---

## 1. Точка входа: `WinMain`

| Параметр / Функция | DiRT 3 (x86) | GRID 2 macOS (x86) | Описание |
| :--- | :--- | :--- | :--- |
| **`WinMain`** | `0x006AC8F0` (`_WinMain@16`) | `0x005797E0` | Главная точка входа приложения |
| **Глобальный указатель на игру** | `0x00FF6900` (`dword_FF6900`) | `0x01A42538` (`_theGame`) | Указатель на экземпляр `CMR6::Application` |
| **Строка логов** | `0x00EC2D20` | `0x0166BAC7` | `"Codemasters Log - %s - %02d/%02d/%04d %02d:%02d\n"` |
| **Синглтон логов** | `0x00FE5A18` (`dword_FE5A18`) | `0x0199EF00` | `neon::NeSingleton<neon::NeLogWin*>::s_instance` |
| **Имя файла лога** | `0x00EC2D54` | `0x0166BAA7` | `"%s_%02d_%02d_%04d_%02d_%02d.txt"` |

### Аргументы командной строки в DiRT 3:
* `-logging` — включает подробный лог в файл Codemasters Log.
* `-safemode` — запуск в безопасном графическом режиме.
* `+connect_lobby <id>` — автоподключение к лобби Steam.
* `-wipe_cloud_saves` — очистка облачных сохранений Steam Cloud.
* `-force_dx9` — принудительный запуск в режиме DirectX 9 (в обход DX11).

---

## 2. Класс приложения: `CMR6::Application`

Главный управляющий объект игры:

| Метод `CMR6::Application` | Адрес DiRT 3 | Адрес GRID 2 | Описание |
| :--- | :--- | :--- | :--- |
| **Constructor** (`Application::Application`) | `0x0059B930` | `0x00329E40` | Инициализация всех подсистем, памяти, пулов, окон |
| **Destructor** (`Application::~Application`) | `0x0059B680` | — | Очистка и освобождение ресурсов |
| **`run()`** (Главный цикл игры) | `0x00584B90` | `0x003308B0` | **Main Game Loop** (Update & Render) |
| **`processCommandLineArgs()`** | `0x00584EF0` | `0x0032D1A0` | Парсинг ключей запуска |
| **`createStoragePools()`** | `0x0058E6A0` | `0x0032D780` | Инициализация пулов хранения данных |
| **`loadLanguageData()`** | `0x0058E900` | `0x00329910` | Загрузка аудио/речевых данных (`audio/speech/`) |
| **`prepareWindow()`** | `0x00584660` | `0x0032EA90` | Чтение конфига XML и подготовка настроек экрана |
| **`createTasks()`** | `0x00579D30` | `0x0032FA80` | Создание `RenderTask` и `UpdateTask` |

### Структура `CMR6::Application` (ключевые смещения в DiRT 3):
* `this + 164` (`0xA4`): Указатель на `CMR6::UpdateTask`
* `this + 168` (`0xA8`): Указатель на `CMR6::RenderTask`
* `this + 224` (`0xE0`): `neon::NeMessageQueue`
* `this + 228` (`0xE4`): `neon::NeMessageBroadcaster`
* `this + 280` (`0x118`): `neon::NeTimeManager` (менеджер времени/дельта-тайм)
* `dword_FE6C00`: Глобальный счетчик кадров (инкрементируется каждый цикл в `run()`)

---

## 3. Графика, Окно и DirectX Device

### Регистрация окна и WndProc:
* **Регистрация класса окна**: `0x007CB030`
  * Класс: `L"NeonClass_%d"` (генерируется динамически с рандомизацией).
  * **Главный `WndProc` игры**: `0x007C9D70` (`sub_7C9D70`).
  * Сюда приходят все оконные сообщения (`WM_KEYDOWN`, `WM_MOUSEMOVE`, `WM_SIZE` и т.д.). Идеальное место для перехвата ввода (ImGui / меню модов).

### Инициализация DirectX (D3D9 / D3D11) и окна:
* **Функция создания устройства и окна**: `0x007C8C30` (`sub_7C8C30`).
* Создает окно через `CreateWindowExW` и сохраняет указатель на контекст дисплея в `GWL_USERDATA`:
  ```cpp
  SetWindowLongA(hWnd, GWL_USERDATA, (LONG)pDisplayDeviceContext);
  ```
* **Смещения в объекте контекста дисплея (`pDisplayDeviceContext`)**:
  * `+ 120` (`0x78`): `HWND hWnd` (дескриптор созданного окна)
  * `+ 124` (`0x7C`): `HDC`
  * `+ 128` (`0x80`): `IDirect3DDevice9*` (при работе в режиме DX9)
  * `+ 240` (`0xF0`): `ID3D11Device*` (при работе в режиме DX11)
  * `+ 244` (`0xF4`): `IDXGISwapChain*` (SwapChain для DX11)
  * `+ 248` (`0xF8`): `ID3D11DeviceContext*` (контекст устройства DX11)

> **Хук для оверлея / ImGui:**
> Имея `hWnd`, можно вызвать `GetWindowLongA(hWnd, GWL_USERDATA)` и мгновенно получить валидные указатели на `IDirect3DDevice9` или `IDXGISwapChain` / `ID3D11Device`!

---

## 4. Задачи движка: `RenderTask` и `UpdateTask`

Движок EGO Engine использует модель задач (Tasks) и очередей сообщений (MessageQueues):

### `CMR6::RenderTask`:
* **Конструктор**: `0x005ACAB0`
* **`initialise()`**: `0x005DF7F0` (GRID 2: `0x003BF5E0`)
* **Очередь сообщений**: `"render_task"`
* **Регистрация обновлений (Present / Window swap)**:
  * Функция `sub_5A1F50` (`0x005A1F50`):
    * `"Plugin update"`
    * `"Window swap"` (`this[1642]`) — swap chain present!
    * `"Vehicle PFX Data"`
    * `"Renderer"`

### `CMR6::UpdateTask`:
* **Конструктор**: `0x006947B0` (GRID 2: `0x0054E690`)
* **Vtable**: `0x00EC00CC`
* **`initialise()`**: `0x00694E00` (регистрирует все игровые системы: `VEHICLE_SYSTEM`, `PHYSICS_WORK`, `RACE_MANAGER`, `FE`, `REPLAY` и т.д.)
* **`startUpdate(double timeStep)`**: `0x00693010` (vtable slot 7, offset `+0x1C`)
* **Главный игровой тик (Update Loop)**: `0x0069E1C0` (vtable slot 1, offset `+0x04`)
  * Выполняет посекундное / покадровое обновление физики, машин, ИИ, игровой логики и сети.

---

## 5. Точки перехвата для ModLoader

1. **Точка внедрения DLL (Early Stage)**:
   * Проксирование `d3d9.dll` / `d3d11.dll` / `dxgi.dll` или перехват `SteamAPI_Init` (импортируется в `WinMain`).
2. **Точка инициализации модов (Core Init)**:
   * Хук в конце `CMR6::Application::Application` (`0x0059B930`) или в начале `CMR6::Application::run` (`0x00584B90`). На этом этапе память и все пулы движка уже созданы.
3. **Хук рендеринга (GUI / Оверлей / ImGui)**:
   * Перехват `sub_7C9D70` (`WndProc`) для мыши и клавиатуры.
   * Перехват `Present` (через указатели из `GWL_USERDATA` окна или через `sub_5A1F50` / VMT хук D3D устройства).
4. **Хук игрового тика (Game Tick / Mod Logic)**:
   * Перехват `sub_693010` (`UpdateTask::startUpdate`) или вызова в `sub_584B90`:
     позволяет выполнять код модов синхронно с физикой игры перед каждым кадром.
