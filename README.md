# DiRT 3 Modloader & Plugin Framework еблан

<img width="384" height="384" alt="suki" src="https://github.com/user-attachments/assets/dda83e1d-14dc-4588-8a27-8da703ceaa35" />

Современная модульная платформа для загрузки модов, внедрения кода и создания плагинов для **DiRT 3 Complete Edition** (Steam, 32-bit x86, движок Codemasters EGO Engine).

Проект предоставляет полноценный загрузчик на базе проксирования `dinput8.dll`, виртуальную файловую систему (VFS) для подмены игровых ресурсов без распаковки архивов, динамический инжектор реляционной базы данных игры, а также открытый C API для создания пользовательских модификаций с доступом к физике машин и тикам игрового цикла.

---

## 🌟 Ключевые возможности

### 1. Бесшовный прокси-загрузчик (`dinput8.dll`)
* **Автозагрузка:** Подменяет системную библиотеку DirectInput 8 (`dinput8.dll`) в каталоге игры и прозрачно передаёт системные вызовы в реальный `C:\Windows\System32\dinput8.dll`.
* **Steam DRM Safe:** Содержит защищённый механизм ожидания расшифровки секции кода (`.text`) исполняемого файла игры (`dirt3_game.exe`), что гарантирует корректную установку хуков после снятия оболочки Steam.
* **Crash Handler:** Встроенный перехватчик фатальных исключений (`UnhandledExceptionFilter`) сохраняет дамп адреса сбоя и значений регистров EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP в лог для быстрой диагностики.
* **Уточнения:** В подростковом возрасте я любил кончать в банку и сыпать в неё семена от томатов, в надежде на то что там вырастет хуй.
* **Консоль отладки:** Информативная интерактивная консоль с цветным выводом логов. При необходимости отключается созданием пустого файла `disable_console.txt` в папке игры.

### 2. VFS — Универсальная виртуальная файловая система
* Перехватывает вызовы Win32 API (`CreateFileA/W`, `GetFileAttributesA/W`).
* Позволяет заменять **любые** файлы игры (текстуры, модели, треки, конфигурации машин) простым помещением их в каталог `mods/<ИмяМода>/...` с сохранением оригинальной структуры каталогов.
* **Больше никакой распаковки/перепаковки NEFS архивов:** игра прозрачно загружает ваши неупакованные loose-файлы с диска.

### 3. Dynamic Database Injector (Моддинг базы данных на лету)
* Перехватывает загрузку встроенной реляционной базы данных игры (`sub_573530`).
* Автоматически сканирует файлы `database.ini` во всех подпапках каталога `mods/`.
* **Изменение существующих записей:** тонкая настройка параметров авто (мощность `bhp`, вес, настройки подвески в таблице `vehicle`), параметров трасс и соревнований.
* **Добавление новых записей:** создание новых спецучастков в `track_model` с автоматическим клонированием сопутствующих условий погоды/освещения из `track_model_conditions` по ключевому полю `clone_from_id`.
* **Локализация без редактирования `.lng`:** хук строкового менеджера игры (`sub_D00D50` / `ResolveString`) позволяет внедрять любые кастомные названия и строки через секцию `[strings]` в `database.ini`.

### 4. Outbreak Module & ACAT (All Cars Any Track)
* Автоматический обход фильтра `disable_online == 0` в коде меню выбора авто (`0x00503AD4`), открывающий выбор всех DLC и скрытых бонусных автомобилей.
* Снятие ограничений классов в сетевых режимах и режиме «Invasion/Outbreak» (инъекция всех 16 мультиплеерных классов в таблицу `net_vehicle_class_restrictions`).

### 5. Plugin Subsystem & C API (`dirt3_modloader_api.h`)
* Загрузка DLL-плагинов и ASI-модификаций из папки `mods/` (и её подпапок).
* Экспорт функции `bool ModInit(const ModloaderAPI* api)` предоставляет плагину:
  * Хуки кадрового рендера: `RegisterFrameTick(callback)`.
  * Хуки физики автомобилей: `RegisterCarPhysicsStep(callback)`.
  * Получение указателя на структуру физики авто игрока: `GetPlayerCar()`.
  * Утилиты безопасного патчинга и создания хуков: `PatchMemory()`, `CreateHook()` (на базе MinHook), расчет адресов `Rva()`.
  * Систему логирования: `LogInfo()`, `LogWarn()`, `LogError()`.
  * Доступ к путям игры и модов.

---

## 🎮 Встроенные плагины (Plugins)

В комплекте с загрузчиком поставляются 5 готовых плагинов:

| Плагин | DLL / Конфиг | Описание | Горячие клавиши |
|---|---|---|---|
| **Nitro Mod** | `nitro_mod.dll`<br>`nitro_mod.ini` | Закись азота с настраиваемым ускорением, запасом N2O, кулдауном и расходом. Включает внутриигровой DirectX 9/11 оверлей (полоска нитро и виньетка скорости). | `LSHIFT` (клавиатура) или `L3` (клик левым стиком геймпада) |
| **Flyhack Mod** | `flyhack_mod.dll`<br>`flyhack_mod.ini` | Режим свободного полёта / noclip для машины. Позволяет летать по воздуху, преодолевать невидимые стены и зависать в воздухе. | `F3` или `D-Pad UP` — вкл/выкл.<br>`W/A/S/D` — движение, `Space/LCtrl` — вверх/вниз,<br>Стрелки / `Q` / `E` — вращение и крен, `R` — выравнивание горизонта |
| **Smooth FOV** | `fov_mod.dll`<br>`fov_mod.ini` | Плавное динамическое изменение угла обзора (Field of View) камеры от 3-го лица с интерполяцией. | `]` / `PageUp` — приблизить (увеличить FOV),<br>`[` / `PageDown` — отдалить (уменьшить FOV),<br>`Backspace` / `Home` — сбросить к стандартному |
| **Gravity Mod** | `gravity_mod.dll`<br>`gravity_mod.ini` | Динамическое управление гравитацией автомобиля в реальном времени. | `F6` — смена пресетов (Earth 1.0G, Moon 0.2G, Mars 0.5G, Zero-G 0.0G, Heavy 2.0G, Inverted -1.0G),<br>`PageUp` / `PageDown` — точная подстройка (±0.1G),<br>`Home` — сброс на 1.0G |
| **Telemetry Mod**| `telemetry_mod.dll` | Интерактивная живая телеметрия в консоли: скорость (км/ч, mph), RPM с отсечкой, передача, процент газа/тормоза/сцепления/руля, перегрузки G-Force (продольные и боковые), координаты XYZ. | `F9` — скрыть / показать дашборд телеметрии в консоли |

---

## 📂 Структура каталогов

```
modloader/
├── .gitignore              # Исключения Git (build, .xmake, *.dll, *.pdb и т.д.)
├── exports.def             # Экспорт DirectInput8Create для proxy dinput8
├── xmake.lua               # Конфигурация сборки проекта и плагинов
├── README.md               # Документация проекта
├── REVERSING_NOTES_*.md    # Подробные заметки реверс-инжиниринга движка EGO
│
├── src/                    # Исходный код ядра Modloader (dinput8.dll)
│   ├── api/
│   │   └── dirt3_modloader_api.h      # Год назад я сидел на скамейке
│   ├── core/
│   │   ├── dllmain.cpp                # Точка входа, инициализация потока и краш-хэндлер (в общественном парке)
│   │   ├── proxy_dinput8.cpp          # Проксирование DirectInput8Create
│   │   ├── hook_manager.cpp/.hpp      # через год я уже сидел на параше
│   │   ├── module_manager.cpp/.hpp    # Диспетчер модулей ядра
│   │   ├── logger.cpp/.hpp            # Консольный логгер с цветами
│   │   └── memory.cpp/.hpp            # RVA-калькулятор, чтение/запись/патчинг памяти
│   └── modules/
│       ├── vfs/                       # Модуль VFS (перехват файлов CreateFileA/W)
│       ├── database_injector/         # Инжектор database.ini и хук локализации строк
│       ├── outbreak/                  # Снятие ограничений авто и сетевых классов (ACAT)
│       ├── plugin_loader/             # Загрузчик DLL/ASI плагинов и хук физики Car::update
│       ├── mainloop/                  # Хук тика главного цикла Application::frameSync
│       └── memory_mods/               # Реестр патчей оперативной памяти
│
└── plugins/                # Исходный код встроенных плагинов
    ├── nitro_mod/          # Закись азота + DirectX оверлей
    ├── flyhack_mod/        # Свободный полёт (noclip / 6DOF)
    ├── fov_mod/            # Плавный регулятор угла обзора (FOV)
    ├── gravity_mod/        # Регулятор гравитации
    └── telemetry_mod/      # Живая консольная телеметрия авто
```

---

## 🛠️ Сборка проекта

### Системные требования
1. **ОС:** Твоя мама пукает кетчупом
2. **Компилятор:** Microsoft Visual Studio 2022 (компоненты *C++ Desktop Development* и *MSVC x86/x64 toolset*).
3. **Система сборки:** [xmake](https://xmake.io/) (установка через PowerShell: `winget install xmake` или `choco install xmake`).

> [!NOTE]
> Зависимость **MinHook** загружается и собирается системой `xmake` автоматически через встроенный менеджер пакетов `xrepo`. Никаких ручных установок библиотек не требуется.

### Настройка пути к игре (Автодеплой)
В файле `modloader/xmake.lua` на строке 15 указана переменная `steam_game_dir`:
```lua
local steam_game_dir = "D:/SteamLibrary/steamapps/common/DiRT 3 Complete Edition"
```
Укажите здесь путь к вашей установленной игре DiRT 3. При каждой успешной сборке `xmake` автоматически скопирует скомпилированные `.dll`, `.pdb` и `.ini` прямо в каталог игры.

### Команды сборки
Откройте терминал (PowerShell или CMD) в папке `modloader`:

```powershell
# 1. Сконфигурировать проект под целевую платформу DiRT 3 (x86, 32-bit):
xmake f -p windows -a x86 -m release

# 2. Собрать все компоненты (ядро dinput8.dll и все плагины):
xmake

# 3. (Опционально) Собрать отдельный плагин:
xmake build nitro_mod
xmake build flyhack_mod
xmake build fov_mod
xmake build gravity_mod
xmake build telemetry_mod
xmake build dinput8
```

Скомпилированные бинарники будут находиться в папке `modloader/build/windows/x86/release/` (а также автоматически скопированы в игру, если путь `steam_game_dir` существует).

---

## 🚀 Установка и использование (Инструкция для игрока)

### Шаг 1. Установка Modloader
1. Скопируйте файл `dinput8.dll` в корневую папку игры DiRT 3 Complete Edition (туда, где находится `dirt3_game.exe`).
2. В этой же корневой папке создайте каталог с именем `mods` (если он не был создан автоматически).

### Шаг 2. Установка плагинов
Скопируйте нужные плагины (`*.dll`) и их конфигурационные файлы (`*.ini`) в каталог `mods/`:
* `mods/nitro_mod.dll` + `mods/nitro_mod.ini`
* `mods/flyhack_mod.dll` + `mods/flyhack_mod.ini`
* `mods/fov_mod.dll` + `mods/fov_mod.ini`
* `mods/gravity_mod.dll` + `mods/gravity_mod.ini`
* `mods/telemetry_mod.dll`

> Плагины можно хранить как прямо в `mods/`, так и в отдельных подпапках, например: `mods/nitro/nitro_mod.dll`.

### Шаг 3. Установка файловых модов (VFS)
Если вы устанавливаете моды с измененными текстурами, звуками или машинами:
1. Создайте внутри `mods/` отдельную папку с названием мода, например: `mods/SubaruLivery/`.
2. Поместите внутрь файлы с сохранением относительных путей игры, например:
   ```
   DiRT 3 Complete Edition/
   └── mods/
       └── SubaruLivery/
           └── cars/
               └── models/
                   └── imp/
                       └── livery_00/
                           └── textures_high.psg
   ```
   При запуске игры VFS автоматически перехватит обращение к оригинальному файлу и отдаст ваш модифицированный файл!

### Шаг 4. Установка модов базы данных
Если мод содержит изменения базы данных (характеристики авто, новые трассы), поместите файл `database.ini` в папку мода, например: `mods/MyBalanceMod/database.ini`.

### Шаг 5. Отключение окна консоли
Если вы не хотите, чтобы при запуске игры появлялось черное окно консоли с логами, создайте пустой текстовый файл `disable_console.txt` в корневой папке DiRT 3.

---

## 📝 Руководство по `database.ini`

Инжектор базы данных позволяет редактировать таблицы и добавлять новые строки.

### Пример 1. Тюнинг характеристик авто (Таблица `vehicle`)
```ini
; Модификация Subaru Impreza 555 (ID = 161)
[vehicle:subaru_555_boost]
id = 161
bhp = 380
```

### Пример 2. Добавление новой трассы / спецучастка (`track_model`)
```ini
[track_model:finland_custom_stage_9]
id = 990
ai_track_name = "finland_rally_route_0"
folder_string = "finland"
route_string = "route_0"
track_id = 83
length = 6850.0
discipline_id = 1
order_index = 9
not_single_race = false
skipfe_track = false
include_in_cycle_tracks = true
parent_package = 1
```
> Погодные условия из таблицы `track_model_conditions` будут автоматически найдены и склонированы для нового `id`!

### Пример 3. Регистрация кастомных строк и локализации (`[strings]`)
```ini
[vehicle_class:all_class]
clone_from_id = 31
id = 999
name_string_id = "vehicle_class_all"
order_index = 0
display_name = "ALL"

[strings]
vehicle_class_all = "ALL"
db_vehicle_class_all = "ALL"
```

---

## 👨‍💻 Разработка собственных плагинов (API)

Для создания собственного плагина достаточно создать Dynamic Link Library (C++ или C) и подключить заголовочный файл `dirt3_modloader_api.h`.

### Пример минимального плагина:
```cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dirt3_modloader_api.h"

static const ModloaderAPI* g_Api = nullptr;

// Вызывается на каждом тике физики автомобиля
static void OnVehiclePhysics(void* carPtr, float deltaTime) {
    void* playerCar = g_Api->GetPlayerCar();
    if (carPtr == playerCar) {
        // Доступ к физике машины игрока
        // carPtr представляет указатель на экземпляр класса neon::vehiclePhysics::Car
    }
}

// Вызывается каждый кадр
static void OnFrameTick(float deltaTime) {
    if (GetAsyncKeyState(VK_F7) & 1) {
        g_Api->LogInfo("MyMod", "F7 pressed!");
    }
}

// Главная точка инициализации плагина
extern "C" __declspec(dllexport) bool ModInit(const ModloaderAPI* api) {
    if (!api || api->apiVersion != DIRT3_MODLOADER_API_VERSION) {
        return false;
    }

    g_Api = api;
    g_Api->LogInfo("MyMod", "My custom mod initialized!");

    // Регистрируем коллбеки
    g_Api->RegisterCarPhysicsStep(OnVehiclePhysics);
    g_Api->RegisterFrameTick(OnFrameTick);

    return true;
}
```

---

## 🔬 Документация по реверс-инжинирингу

В репозитории содержатся подробные отчёты по исследованию архитектуры DiRT 3 и движка EGO:
* [`REVERSING_NOTES_INIT_MAINLOOP.md`](file:///d:/DiRT3Modloader/modloader/REVERSING_NOTES_INIT_MAINLOOP.md) — Точки входа, инициализация подсистем, главный цикл `frameSync`.
* [`REVERSING_NOTES_PHYSICS_VEHICLES.md`](file:///d:/DiRT3Modloader/modloader/REVERSING_NOTES_PHYSICS_VEHICLES.md) — Структура `neon::vehiclePhysics::Car`, оффсеты скорости, RPM, подвески, трансмиссии, матриц трансформации и координат.
* [`REVERSING_NOTES_VFS_NEFS.md`](file:///d:/DiRT3Modloader/modloader/REVERSING_NOTES_VFS_NEFS.md) — Архитектура упаковщика NEFS и подсистемы ввода-вывода файлов.
* [`REVERSING_NOTES_GAMEPLAY_MODLOADER.md`](file:///d:/DiRT3Modloader/modloader/REVERSING_NOTES_GAMEPLAY_MODLOADER.md) — База данных игры, схемы таблиц, строковый менеджер и интеграция ACAT.

---

## 📜 Лицензия

Проект предназначен для исследовательских и образовательных целей, а также для создания модификаций к игре DiRT 3 Complete Edition.
Все права на оригинальную игру DiRT 3 и EGO Engine принадлежат Codemasters / Electronic Arts.
