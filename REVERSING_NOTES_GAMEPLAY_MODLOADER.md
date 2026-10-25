# Архитектура Игровой Логики, Контроллеров и Модлоадера DiRT 3 (EGO Engine 2.0)

Документ описывает результаты реверс-инжиниринга игровой логики, системы контроллеров/управления, отслеживания прогресса гонки, а также полную архитектуру внедрения (injection & hooking) для будущего C++ модлоадера DiRT 3.

Символы сопоставлены с помощью бинарного файла **GRID 2 (macOS Mach-O x86)** с полными отладочными символами DWARF и перенесены в IDB **DiRT 3 (`dirt3_game.exe`, 32-bit x86 Windows)**.

---

## Часть 1. Игровая логика и менеджмент гонки (`neRacing`)

### 1.1. `neRacing::RaceManager`
Центральный управляющий класс для процесса гонки, условий победы/поражения, отслеживания соперников и рассылки сообщений о статусах.

- **Конструктор DiRT 3**: `0x009A6220` (`neRacing__RaceManager__RaceManager`)
  - *Аналог в GRID 2*: `0x0118B900` (`neRacing::RaceManager::RaceManager()`)
- **Таблицы виртуальных методов (Vftable)**:
  - Основная: `0x00F13F94` (`vftable_neRacing__RaceManager`)
  - Вторичная: `0x00F13F38` (`vftable_neRacing__RaceManager_second`)
- **Основной цикл обновления**: `0x009A90E0` (`neRacing__RaceManager__update`)
  - Вызывается каждый кадр обновления физико-игровой сцены.
  - Обновляет состояние каждого участника заезда.
  - Проверяет условия финиша, фальстарта, задержки старта и дисквалификации.

#### Структура участников гонки
Внутри `neRacing::RaceManager` содержится массив структур участников:
- Массив участников: `this + 0x???`
- Количество слотов участников: до 8 (по умолчанию для основного массива, расширяется в сетевых режимах)
- Размер записи участника (`neRacing::VehicleRaceInformation`): `0xC20` (3104 байта) на одну машину.

---

### 1.2. `neRacing::VehicleRaceInformation`
Хранит всю динамическую информацию о конкретном автомобиле во время сессии заезда: текущий круг, контрольные точки (сплиты/чекпоинты), штрафы, статус финиша, время заезда.

- **Конструктор DiRT 3**: `0x0099C1A0` (`neRacing__VehicleRaceInformation__VehicleRaceInformation`)
- **Деструкторы**:
  - `0x009A4A80` (`neRacing__VehicleRaceInformation__dtor`)
  - `0x009A6550` (`neRacing__VehicleRaceInformation__scalar_dtor`)
- **Таблица виртуальных методов**: `0x00F13D08` (`vftable_neRacing__VehicleRaceInformation`)
- **Методы сериализации и синхронизации**:
  - `0x0099D750` (`neRacing__VehicleRaceInformation__serialise`) — упаковка состояния (для сети и повторов)
  - `0x0099DE90` (`neRacing__VehicleRaceInformation__deserialise`) — распаковка состояния
  - `0x0099E2C0` (`neRacing__VehicleRaceInformation__interpolateTo`) — интерполяция между сетевыми кадрами

#### Структура чекпоинтов:
- В конструкторе `0x0099C1A0` инициализируется массив из **51 сплит-точки / чекпоинта**:
  - Вызов `eh vector constructor iterator` по смещению `this + 136` (`0x88`)
  - Размер элемента сплит-точки: `0x38` (56 байт)
  - Количество элементов: `51` (максимально допустимое число секторов/чекпоинтов на спецучастке в DiRT 3)
- Содержит:
  - Время прохождения сплита (`float` / `double`)
  - Дельта к лидеру
  - Скорость на контрольной засечке
  - Валидность сектора (срезка трассы / off-track penalty)

---

### 1.3. Статусы автомобиля и сообщения событий
Функция установки статуса машины в гонке:
- **`neRacing::Vehicle::setRaceStatus`**: `0x00930B20`
  - Записывает статус в поле машины: `*(DWORD*)(this + 0x2508) = status` (индекс массива `this[2370]`)
  - Коды статусов:
    - `0`: **In Race** (участвует в заезде)
    - `1`: **Finished** (пересек финишную черту)
    - `2`: **Disqualified** (дисквалификация за срезки или время, отправляет сообщение `"VehicleDisqualifiedMessage"`)
    - `3`: **Retired / DNF** (сход с дистанции из-за фатальных повреждений, отправляет сообщение `"RetireMessage"`)

---

### 1.4. Траектории, сплайны и чекпоинты (`RouteTracker`)
- **`neRacing::progress::RouteTracker::renderDebugRoutes`**: `0x009A1F80`
  - Функция отладочной визуализации маршрутов, сплайнов дороги, зон сплитов и поворотов.
  - Позволяет отображать линии гоночной траектории, зоны триггеров поворотов и границы дороги.
  - Крайне полезна для моддеров, создающих кастомные треки и маршруты для ралли.

---

## Часть 2. Система контроллеров и управление (`neRacing::VehicleControllerSystem`)

### 2.1. Менеджер контроллеров
- **`neRacing::VehicleControllerSystem::update`**: `0x00B29F80`
  - *Аналог в GRID 2*: `0x0102F740` (`neRacing::VehicleControllerSystem::update(float)`)
  - Обрабатывает входные устройства (геймпад, руль с Force Feedback, клавиатуру) либо логику AI.
  - Передает команды управления в физическую сущность машины `neon::vehiclePhysics::Car`.
- **`neRacing::VehicleControllerSystem::postUpdate`**: `0x00B29F40`
  - Финализирует состояние контроллеров после шага физики.

### 2.2. Иерархия контроллеров
Каждая машина может управляться одним из трех базовых контроллеров:
1. **`neRacing::ai::VehicleController`** (`type = 0`):
   - Искусственный интеллект. Считывает данные из `RouteTracker`, держит идеальную траекторию, реагирует на коллизии и соперников.
2. **`neRacing::HumanVehicleController`** (`type = 1`):
   - Управление игроком. Считывает данные из подсистемы ввода EGO (`Input2`), рассчитывает мертвые зоны, чувствительность, линейность руля и эффекты Force Feedback.
3. **`neRacing::SwappableVehicleController`** (`type = 2`):
   - Контроллер-обертка, позволяющий на лету переключать управление между человеком и AI (например, при выходе в меню паузы, автопилоте после финиша или при повторе Flashback).

### 2.3. Точки внедрения управляющих команд (`Car::setControl`)
В `0x00B29F80` контроллер вызывает специализированные методы физической машины:
- **`neon::vehiclePhysics::Car::setControl(int controlId, float value)`**: `0x007CF920`
  - `controlId = 1`: Рулевое управление (Steer, диапазон `[-1.0f, 1.0f]`)
  - `controlId = 2`: Газ (Throttle, диапазон `[0.0f, 1.0f]`)
  - `controlId = 7`: Передача КПП (Gear selector)
- **`neon::vehiclePhysics::Car::setControl(int controlId, bool value)`**: `0x007CF900`
  - `controlId = 3`: Тормоз (Brake, `bool`)
  - `controlId = 4`: Ручной тормоз (Handbrake, `bool`)
  - `controlId = 8`: Буст / KERS (Boost, `bool`)
- **`neon::vehiclePhysics::Car::setControl(int controlId, int value)`**: `0x007CF970`
  - `controlId = 5`: Сцепление (Clutch)

> **Возможность для модов**: Перехват этих функций позволяет реализовывать кастомные системы помощи (ABS, ESP, Traction Control), телеметрию, автопилот, а также поддержку нестандартных контроллеров (прямое чтение DirectInput / FreePie / vJoy).

---

## Часть 3. Архитектура Модлоадера DiRT 3

На основе проведенного анализа модулей VFS, NeFS, физики, главного цикла и игровой логики определена оптимальная архитектура модлоадера:

```
[dirt3_game.exe]
       │ (load imports)
       ▼
[dinput8.dll] (Proxy Wrapper) ──► Loads original [C:\Windows\System32\dinput8.dll]
       │
       ├─► Инициализация MinHook
       ├─► Создание логгера (DiRT3Modloader.log)
       ├─► Загрузка конфигурации (modloader.json / ini)
       │
       ├─► [VFS Hooks]: NeStorageDeviceFS / NeStorageManager
       │      └─► Подмена путей к loose-файлам (mods/<mod_name>/...) без перепаковки NeFS
       │
       ├─► [MainLoop Hook]: CMR6::UpdateTask::tick (0x0069E1C0)
       │      └─► Вызов OnFrameTick() модов, горячая перезагрузка скриптов
       │
       ├─► [Render / UI Hook]: EndScene / Present (D3D9)
       │      └─► Отрисовка встроенного ImGui оверлея (меню модов, телеметрия, дебаг)
       │
       └─► [Physics / CTF Hook]: CarType::buildFromCTFreader (0x007F6090)
              └─► Переопределение характеристик машин на лету
```

---

### 3.1. Механизм внедрения (DLL Proxying)
- **Выбранная цель**: `dinput8.dll`.
  - `dirt3_game.exe` импортирует `dinput8.dll` для инициализации устройств ввода.
  - Достаточно реализовать единственную экспортируемую функцию:
    ```cpp
    HRESULT WINAPI DirectInput8Create(
        HINSTANCE hinst,
        DWORD dwVersion,
        REFIID riidltf,
        LPVOID *ppvOut,
        LPUNKNOWN punkOuter
    );
    ```
  - Прокси-библиотека подгружает настоящую системную `C:\Windows\System32\dinput8.dll` через `LoadLibraryW` и передает управление в оригинальный `DirectInput8Create`.
  - **Преимущество**: Модлоадер стартует в самом начале жизни процесса, задолго до создания графического устройства Direct3D9 и открытия первых файлов NeFS.

---

### 3.2. VFS Loose File Loader (Загрузка модов без перепаковки архивов)
Одна из главных проблем моддинга игр на EGO Engine — необходимость перепаковывать гигантские `.nefs` архивы и обновлять хеши.

**Решение на уровне хуков**:
1. Перехватывается метод `NeStorageManager::open` (`0x0077B4A0` / `0x0077B6D0`) или метод `NeStorageDeviceFS::open` (`0x00778900` / `0x00778B50`).
2. При запросе пути (например, `cars/fdr/fdr.pssg`, `tracks/locations/finland/route_0/scene.xml`):
   - Модлоадер проверяет наличие файла в папке `mods/<ActiveMods>/cars/fdr/fdr.pssg`.
   - Если файл существует на диске как loose-файл:
     - Либо перенаправляет вызов на открытие реального файла на жестком диске через нативный `NeStorageDeviceFS`.
     - Либо прозрачно подменяет дескриптор устройства хранения.
3. В результате игра использует модифицированные текстуры, 3D-модели, звуки и конфиги напрямую из папок модов!

---

### 3.3. Кадровый цикл и UI Оверлей (ImGui Hook)
1. **Хук логики кадра**:
   - Адрес: `0x0069E1C0` (`CMR6::UpdateTask::tick`)
   - Обеспечивает точный кадровый цикл для скриптов модов, обновления таймеров, сбора телеметрии.
2. **Графический оверлей**:
   - Перехват `IDirect3DDevice9::EndScene` или `Present` через Vftable хук Direct3D9 устройства (создаваемого в `0x008E8850`).
   - Интеграция **Dear ImGui**:
     - Внутриигровое меню управления модами (включение/отключение модов на лету).
     - Редактор физики в реальном времени (настройка дифференциалов, подвески, прижимной силы).
     - Селектор трасс и спавнер машин.
     - Отображение отладочных сплайнов трассы через `neRacing::progress::RouteTracker::renderDebugRoutes` (`0x009A1F80`).

---

### 3.4. Хуки физики и тюнинга
1. **Переопределение характеристик машин (`CarType`)**:
   - `0x007F6090` (`neon__vehiclePhysics__CarType__buildFromCTFreader`):
     - Модлоадер может инжектировать или подменять секции `.ctf` файлов (масса, аэродинамика, двигатель, тормозные моменты, передаточные числа) прямо в памяти без модификации оригинальных файлов.
2. **Переопределение свойств поверхностей (`SurfaceSpec`)**:
   - `0x007EA0A0` (`neon__vehiclePhysics__SurfaceSpec__init`):
     - Возможность настройки сцепления различных поверхностей (гравий, снег, лед, асфальт, грязь) под реалистичный симуляторный уровень (физические моды).
3. **Прямое управление физикой в тике**:
   - `0x008007B0` (`neon__vehiclePhysics__Car__update`):
     - Чтение и запись векторов скорости, угловой скорости, сил на колесах, износа резины.

---

## Часть 4. Сводная таблица ключевых адресов DiRT 3

| Модуль | Адрес (DiRT 3) | Сигнатура / Имя | Назначение |
| :--- | :--- | :--- | :--- |
| **Race** | `0x009A6220` | `neRacing__RaceManager__RaceManager` | Конструктор менеджера гонки |
| **Race** | `0x009A90E0` | `neRacing__RaceManager__update` | Главный цикл обновления заезда |
| **Race** | `0x00F13F94` | `vftable_neRacing__RaceManager` | Vftable менеджера гонки |
| **Vehicle Info** | `0x0099C1A0` | `neRacing__VehicleRaceInformation__VehicleRaceInformation` | Конструктор инфо о машине в гонке |
| **Vehicle Info** | `0x00F13D08` | `vftable_neRacing__VehicleRaceInformation` | Vftable инфо о машине в гонке |
| **Vehicle Info** | `0x0099D750` | `neRacing__VehicleRaceInformation__serialise` | Сериализация состояния заезда |
| **Vehicle Info** | `0x0099DE90` | `neRacing__VehicleRaceInformation__deserialise` | Десериализация состояния заезда |
| **Vehicle Status** | `0x00930B20` | `neRacing__Vehicle__setRaceStatus` | Установка статуса (0=Race, 1=Finish, 2=DQ, 3=DNF) |
| **Route** | `0x009A1F80` | `neRacing__progress__RouteTracker__renderDebugRoutes` | Отрисовка маршрутов, сплитов и поворотов |
| **Controller** | `0x00B29F80` | `neRacing__VehicleControllerSystem__update` | Обновление контроллеров машины (AI/Human) |
| **Controller** | `0x00B29F40` | `neRacing__VehicleControllerSystem__postUpdate` | Пост-обновление контроллеров |
| **Car Control** | `0x007CF920` | `neon__vehiclePhysics__Car__setControl_float` | Передача руля (`1`), газа (`2`), передач (`7`) |
| **Car Control** | `0x007CF900` | `neon__vehiclePhysics__Car__setControl_bool` | Передача тормоза (`3`), ручника (`4`), буста (`8`) |
| **Car Control** | `0x007CF970` | `neon__vehiclePhysics__Car__setControl_int` | Передача сцепления (`5`) |
| **VFS Device** | `0x00778900` | `neon__NeStorageDeviceFS__open` | Хук открытия файлов для loose-модов |
| **Main Loop** | `0x0069E1C0` | `CMR6__UpdateTask__tick` | Хук кадрового обновления для модов |
| **Car Update** | `0x008007B0` | `neon__vehiclePhysics__Car__update` | Хук шага физики автомобиля |
| **Car Config** | `0x007F6090` | `neon__vehiclePhysics__CarType__buildFromCTFreader` | Парсинг и загрузка параметров физики |
| **Surface** | `0x007EA0A0` | `neon__vehiclePhysics__SurfaceSpec__init` | Инициализация параметров поверхностей трассы |

---

## Итоги и готовность к разработке модлоадера
Все ключевые подсистемы (VFS/NeFS, инициализация, кадровый цикл, физический движок автомобилей, поверхности трасс, логика гонки и система контроллеров ввода) успешно исследованы и задокументированы с конкретными адресами и vftable.
База IDA `dirt3_game.exe.i64` сохранена и готова к дальнейшему использованию.
Следующим шагом может быть создание структуры проекта модлоадера в папке `modloader/` (CMake, прокси `dinput8.dll`, MinHook, перехват VFS и ImGui оверлей).
