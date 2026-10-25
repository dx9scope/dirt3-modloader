# DiRT 3 & EGO Engine 2.0 - Vehicle & Track Physics Reverse Engineering Notes

This document provides a detailed breakdown of the vehicle physics engine, car tuning parameters, and track surface/material interaction systems in **DiRT 3** (`dirt3_game.exe`, 32-bit x86 Windows, EGO Engine 2.0). All discoveries were cross-referenced against **GRID 2** (`Grid 2`, 32-bit x86 Mach-O macOS, EGO Engine 3.0) with full DWARF debugging symbols and verified in the DiRT 3 binary.

All identified symbols have been renamed and permanently saved into the DiRT 3 IDA Pro database (`D:\DiRT3Modloader\game\dirt3\dirt3_game.exe.i64`).

---

## Table of Contents
1. [Architecture Overview](#1-architecture-overview)
2. [Vehicle Object Hierarchy](#2-vehicle-object-hierarchy)
3. [Low-Level Physics: `neon::vehiclePhysics::Car` Memory Layout](#3-low-level-physics-neonvehiclephysicscar-memory-layout)
4. [Car Parameters & CTF Parsing (`CarType::buildFromCTFreader`)](#4-car-parameters--ctf-parsing-cartypebuildfromctfreader)
5. [Track Surface & Material Physics System (`SurfaceSpec`)](#5-track-surface--material-physics-system-surfacespec)
6. [Tire Physics & Surface Interaction](#6-tire-physics--surface-interaction)
7. [Wet Track Material Remapping System](#7-wet-track-material-remapping-system)
8. [Multi-Threaded Execution (`CarJob`)](#8-multi-threaded-execution-carjob)
9. [Comprehensive Symbol Mapping Table (DiRT 3 vs GRID 2)](#9-comprehensive-symbol-mapping-table-dirt-3-vs-grid-2)

---

## 1. Architecture Overview

In EGO Engine 2.0 (DiRT 3), vehicle handling is split into distinct architectural layers:
```
+-------------------------------------------------------------+
|                  CMR6::Application / Main Loop               |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|           CMR6::UpdateTask / Game Update Pipeline           |
+-------------------------------------------------------------+
                              |
       +----------------------+-----------------------+
       |                                              |
       v                                              v
+-------------------------------+       +-------------------------------+
|     neRacing::VehicleSystem    |       |   neRacing::NeMaterialManager |
+-------------------------------+       +-------------------------------+
       |                                              |
       v                                              v
+-------------------------------+       +-------------------------------+
|       neRacing::Vehicle       |       | neon::NePhysicsMaterialManager|
|      (CMR6::Vehicle)          |       +-------------------------------+
+-------------------------------+                     |
       |                                              v
       v                                +-------------------------------+
+-------------------------------+       |     Physics::PhysicsMaterial  |
|   neon::vehiclePhysics::Car   | <---> | (Base + SurfaceSpec 192 bytes)|
+-------------------------------+       +-------------------------------+
       |
       +---> SuspensionSystem (Pacejka Magic Formula, contact patches)
       +---> Wheel Array [4] (384 bytes per wheel struct)
       +---> Drivetrain / Engine / Gearbox
       +---> Aerodynamics (Lift, Drag, Downforce)
```

---

## 2. Vehicle Object Hierarchy

### A. High-Level Gameplay Container: `neRacing::Vehicle` & `CMR6::Vehicle`
- **Base Class**: `neRacing::Vehicle`
  - DiRT 3 Constructor: `0x00921950` (`neRacing__Vehicle__Vehicle`)
  - DiRT 3 Vtable: `0x00F0D26C` (`vftable_neRacing__Vehicle`)
  - DiRT 3 Update: `0x0093B010` (`neRacing__Vehicle__update`)
  - DiRT 3 Tick: `0x0093B5E0` (`neRacing__Vehicle__tick`)
- **Derived Game Class**: `CMR6::Vehicle`
  - DiRT 3 Constructor: `0x005E6470` (`CMR6__Vehicle__Vehicle`)
  - DiRT 3 Vtable: `0x00EB58C8` (`vftable_CMR6__Vehicle`)
  - DiRT 3 Update: `0x005FBFE0` (`CMR6__Vehicle__update`)
- **Structure Offsets in `neRacing::Vehicle`**:
  - `+0x0000`: Pointer to virtual method table (`vftable_neRacing__Vehicle`)
  - `+0x0030` (`+48`): `neon::vehiclePhysics::Car*` - Pointer to low-level physics simulation object.
  - `+0x0039` (`+57`): Vehicle model name buffer (e.g., `"fdr"`, `"n24"`, 32 bytes).
  - `+0x00A0` (`+160`): AI & Opponent navigation tracking context.
  - `+0x2528` (`+9512`): Slipstream / drafting base multiplier.
  - `+0x2564` (`+9572` in `CMR6::Vehicle`): Car full display name buffer (64 bytes).

---

## 3. Low-Level Physics: `neon::vehiclePhysics::Car` Memory Layout

The low-level physics simulation is implemented in `neon::vehiclePhysics::Car`. Its main update loop is `0x008007B0` (`neon__vehiclePhysics__Car__update`), which calls `0x007FFEA0` (`neon__vehiclePhysics__Car__updateSubsystems`).

### Key Member Offsets in `neon::vehiclePhysics::Car`:

| Offset (Hex) | Offset (Dec) | Type | Description |
|---|---|---|---|
| `+0x021C` | `+540` | `int` | Collision state counter / impact event flag |
| `+0x0220` | `+544` | `bool` | Wheel spin / slip flag |
| `+0x0250` | `+592` | `int` | Physics frame tick counter |
| `+0x02A0` | `+672` | `Vector4` | Accumulated acceleration / inertia force |
| `+0x12D4` | `+4820` | `float[6]` | Driver inputs (`CAR_CONTROLS`: Steer, Throttle, Brake, Handbrake, Clutch, Gear) |
| `+0x12E0` | `+4832` | `float` | Aerodynamic Drag X multiplier |
| `+0x12E4` | `+4836` | `float` | Aerodynamic Drag Y multiplier |
| `+0x12E8` | `+4840` | `float` | Aerodynamic Drag Z multiplier |
| `+0x12EC` | `+4844` | `float` | Aerodynamic downforce threshold speed |
| `+0x130C` | `+4876` | `float` | Clutch input override (0.0 = fully engaged, 1.0 = disengaged) |
| `+0x13A0` | `+5024` | `float` | Front/Rear weight distribution (e.g. `0.52` = 52% Front, 48% Rear) |
| `+0x13BC` | `+5052` | `float` | Tire load sensitivity curve parameter 1 |
| `+0x13C0` | `+5056` | `float` | Tire load sensitivity curve parameter 2 |
| `+0x13C4` | `+5060` | `float` | Tire load sensitivity curve parameter 3 |
| `+0x1410` | `+5136` | `float` | Speed-dependent lateral stability factor |
| `+0x1420` | `+5152` | `float` | Differential left torque split |
| `+0x142C` | `+5164` | `float` | Differential right torque split |
| `+0x1610` | `+5648` | `float` | Friction circle / slip grip loss factor |
| `+0x1618` | `+5656` | `float` | Engine Redline / Maximum RPM |
| `+0x1620` | `+5664` | `bool` | Physics suspension lock flag |
| `+0x1624` | `+5668` | `float` | Engine Idle RPM |
| `+0x1630` | `+5680` | `float` | Clutch engagement threshold / bite point |
| `+0x16A0` | `+5792` | `float` | Front brake torque max |
| `+0x16A4` | `+5796` | `float` | Rear brake torque max |
| `+0x16E8` | `+5864` | `float` | Surface bump amplitude scaling factor |
| `+0x16EC` | `+5868` | `float` | Surface bump frequency / roughness factor |
| `+0x17DC` | `+6104` | `float` | Handbrake grip release fraction |
| `+0x17E0` | `+6108` | `float` | Handbrake torque max |
| `+0x19C0` | `+6592` | `float` | **Vehicle Mass** in kilograms (kg) |
| `+0x19C4` | `+6596` | `float` | Inverse vehicle mass (`1.0f / mass`) |
| `+0x19C8` | `+6600` | `float` | Linear velocity Vector X |
| `+0x19CC` | `+6604` | `float` | Linear velocity Vector Y |
| `+0x19D0` | `+6608` | `float` | Linear velocity Vector Z |
| `+0x19E0` | `+6624` | `Vector4` | World velocity vector (SIMD `__m128`) |
| `+0x19F0` | `+6640` | `Vector4` | World angular velocity vector (SIMD `__m128`) |
| `+0x1A00` | `+6656` | `Vector4` | Transform Orientation Matrix - Right vector |
| `+0x1A10` | `+6672` | `Vector4` | Transform Orientation Matrix - Up vector |
| `+0x1A20` | `+6688` | `Vector4` | Transform Orientation Matrix - Forward vector |
| `+0x1A30` | `+6704` | `Vector4` | Transform Orientation Matrix - Position vector |
| `+0x1A40` | `+6720` | `float` | Total distance traveled (meters) |
| `+0x1B30` | `+6960` | `float` | Speed boost / drafting multiplier |
| `+0x1B6C` | `+7020` | `int` | Number of active wheels (usually 4) |
| `+0x1CB0` | `+7344` | `float` | Current Throttle input (`0.0f` to `1.0f`) |
| `+0x1D58` | `+7512` | `int` | Current Gear (`0` = Reverse, `1` = Neutral, `2` = 1st, `3` = 2nd...) |
| `+0x1D5C` | `+7516` | `bool` | Gear shifting in progress |
| `+0x1D60` | `+7520` | `float` | Current shift timer / clutch slip duration |
| `+0x1E4C` | `+7756` | `Wheel[0]` | Wheel 0: Front Left (Stride = 384 bytes / 0x180) |
| `+0x1FCC` | `+8140` | `Wheel[1]` | Wheel 1: Front Right |
| `+0x214C` | `+8524` | `Wheel[2]` | Wheel 2: Rear Left |
| `+0x22CC` | `+8908` | `Wheel[3]` | Wheel 3: Rear Right |
| `+0x2404` | `+9220` | `float` | Forward speed in meters per second (m/s) |
| `+0x259C` | `+9628` | `int` | Active tire compound type (`0`=Standard/Dirt, `1`=Tarmac, `2`=Snow/Ice) |

---

## 4. Car Parameters & CTF Parsing (`CarType::buildFromCTFreader`)

In DiRT 3, car handling profiles are configured in `.ctf` files (compiled tuning format) or XML configuration files (`handling.xml` / `tune.xml`).
- Parser Function: `0x007F6090` (`neon__vehiclePhysics__CarType__buildFromCTFreader`)
- Tuning Applier: `0x007FDD40` (`neon__vehiclePhysics__CarTuningState__applyCTF`)
- Tuning Calculator: `0x008017A0` (`neon__vehiclePhysics__CarTuningState__calculateTunedValues`)

### Core CTF Parameters Parsed by the Engine:
1. **Aerodynamics**:
   - `air_density_multiple`, `air_resistance_x`, `air_resistance_y`, `air_resistance_z`, `downforce_multiplier`, `spoiler_angle`, `spoiler_drag`, `spoiler_downforce`.
2. **Engine & Transmission**:
   - `engine_idle_rpm`, `engine_max_rpm`, `engine_max_torque`, `engine_braking_torque`, `engine_inertia`, `torque_curve_rpm_0`..`rpm_n`, `gear_ratio_r`, `gear_ratio_1`..`gear_ratio_7`, `final_drive_ratio`, `shift_up_time`, `shift_down_time`.
3. **Drivetrain & Differentials**:
   - `drivetrain_type` (`fwd`, `rwd`, `4wd`), `front_diff_type`, `rear_diff_type`, `centre_diff_type`, `centre_diff_max_torque`, `differential_viscous_coupling`, `torque_split_front_rear`.
4. **Brakes & Steering**:
   - `brake_bias_front`, `brake_max_torque_front`, `brake_max_torque_rear`, `handbrake_torque`, `steering_lock_degrees`, `steering_speed`, `steering_return_speed`, `pad_lock_speed_multiplier`.
5. **Suspension & Chassis**:
   - `mass_kg`, `weight_distribution_front`, `centre_of_gravity_height`, `anti_roll_stiffness_front`, `anti_roll_stiffness_rear`, `spring_stiffness_front`, `spring_stiffness_rear`, `damper_bump_front`, `damper_bump_rear`, `damper_rebound_front`, `damper_rebound_rear`, `suspension_travel_front`, `suspension_travel_rear`.
6. **Tire & Grip Coefficients**:
   - `tyre_radius`, `tyre_width`, `tyre_grip_multiplier`, `tyre_lateral_stiffness`, `tyre_longitudinal_stiffness`, `camber_degrees_front`, `camber_degrees_rear`, `toe_in_front`, `toe_in_rear`.

---

## 5. Track Surface & Material Physics System (`SurfaceSpec`)

Track surfaces and ground physics are loaded from `surface_materials.xml` and parsed into `neon::vehiclePhysics::SurfaceSpec` objects.

### Lifecycle & Loading:
1. XML node `<MATERIALS>` is read by `neon__NePhysicsMaterialManager__initialise` (`0x007D3CD0`).
2. Each `<MATERIAL>` invokes registered subsystem callbacks.
3. Subsystem callback `neon__vehiclePhysics__Car_Materials_Initialiser` (`0x007EC1D0`) calls `neon__vehiclePhysics__SurfaceSpec__init` (`0x007EA0A0`), parsing the `<MECHANICS>` XML tag.
4. Each `Physics::PhysicsMaterial` instance contains a 64-byte aligned base physics material followed immediately by the 192-byte (`0xC0`) `SurfaceSpec` structure.

### `neon::vehiclePhysics::SurfaceSpec` Structure Layout (192 bytes / 0xC0):

| Offset (Hex) | Offset (Dec) | Type | Parameter Name | Default | Description |
|---|---|---|---|---|---|
| `+0x00` | `+0` | `float` | `grip` | `1.0f` | Base longitudinal and lateral grip multiplier |
| `+0x04` | `+4` | `float` | `m_waterDepth` | `0.0f` | Surface water depth in meters (triggers hydroplaning) |
| `+0x08` | `+8` | `float` | `ls_cs_full` | `0.0f` | Lateral scrub cornering stiffness at full slip |
| `+0x0C` | `+12` | `float` | `ls_cs_no` | `0.0f` | Lateral scrub cornering stiffness at zero slip |
| `+0x10` | `+16` | `float` | `ls_ss_no` | `0.0f` | Lateral scrub slip stiffness at zero slip |
| `+0x14` | `+20` | `float` | `ls_ss_full` | `0.0f` | Lateral scrub slip stiffness at full slip |
| `+0x18` | `+24` | `float` | `ls_amp` | `0.0f` | Lateral scrub wave amplitude |
| `+0x1C` | `+28` | `float` | `ls_wavelen` | `1.0f` | Lateral scrub wavelength |
| `+0x20` | `+32` | `bool` | `bumpstimebased` | `true` | If true, bumps oscillate with time; if false, with wheel distance |
| `+0x24` | `+36` | `float` | `bumpswavelength`| `0.001f`| Wavelength of ground micro-roughness bumps |
| `+0x28` | `+40` | `float` | `bumpsmagnitude` | `0.0f` | Amplitude / vertical height of micro-roughness bumps |
| `+0x2C` | `+44` | `float` | `bumpsrealproportion`|`0.0f`| Blending proportion of procedural bumps vs physical geometry |
| `+0x30` | `+48` | `float` | `bumpsproportionCarDependent`|`0.0f`| Suspension bump response factor dependent on car speed |
| `+0x34` | `+52` | `float` | `depth` | `0.0f` | Surface sink depth (e.g. mud, deep snow, gravel) |
| `+0x38` | `+56` | `float` | `minimumSpeedForSlowdown`|`25.0f`| Speed below which surface rolling resistance is minimum |
| `+0x3C` | `+60` | `float` | `speedForMaximumSlowdown`|`40.0f`| Speed above which surface rolling resistance is maximum |
| `+0x40` | `+64` | `float` | `slowdown` | `0.0f` | Rolling resistance / drag coefficient of loose surface |
| `+0x44` | `+68` | `float` | `lss_speed` | `15.0f` | Low speed scrub threshold speed |
| `+0x48` | `+72` | `float` | `lss_coefficent` | `0.0f` | Low speed scrub resistance multiplier |
| `+0x4C` | `+76` | `float` | `camera_factor` | `1.0f` | Camera shake / cockpit vibration intensity |
| `+0x50` | `+80` | `float` | `roughness` | `0.0f` | Terrain roughness rating |
| `+0x54` | `+84` | `char[64]`| `name` | `"?????"`| Material identifier name string (e.g. `"TARMAC"`, `"GRAVEL"`) |
| `+0x94` | `+148` | `float` | `hardness` | `1.0f` | Ground surface penetration resistance |
| `+0x98` | `+152` | `float` | `heat` | `1.0f` | Tire heat generation multiplier |
| `+0x9C` | `+156` | `float` | `wear` | `1.0f` | Tire wear rate multiplier |
| `+0xA0` | `+160` | `bool` | `dirt_build_up` | `false` | Enables car body mud/dirt accumulation |
| `+0xA4` | `+164` | `float` | `grip_tyre_tarmac`|`1.0f` | Grip multiplier override when equipped with Tarmac tires |
| `+0xA8` | `+168` | `float` | `grip_tyre_snow` | `1.0f` | Grip multiplier override when equipped with Snow tires |
| `+0xAC` | `+172` | `float` | `hardness_tyre_tarmac`|`1.0f`| Hardness override with Tarmac tires |
| `+0xB0` | `+176` | `float` | `hardness_tyre_snow`|`1.0f`| Hardness override with Snow tires |
| `+0xB4` | `+180` | `int` | `ffb_surface` | `11` | Force feedback surface vibration profile enum |

### Force Feedback (`ffb_surface`) Surface Types (`0x007D4700`):
| Enum Value | Name String | Description |
|---|---|---|
| `0` | `"TARMAC"` | Smooth high-frequency road rumble |
| `1` | `"RUMBLESTRIP"` | High-amplitude periodic kerb vibration |
| `2` | `"CONCRETE"` | Stiff low-frequency jolt profile |
| `3` | `"ROCK"` | Sharp irregular high-force shocks |
| `4` | `"GRAVEL"` | Continuous loose-stone high-frequency jitter |
| `5` | `"MUD"` | Heavy damped resistance with drag |
| `6` | `"SAND"` | Smooth high-drag resistance |
| `7` | `"GRASS"` | Soft irregular low-resistance bumps |
| `8` | `"WATER"` | Drag pulse and hydroplaning steering lightness |
| `9` | `"COBBLESTONE"` | Medium-amplitude continuous rattling |
| `10` | `"METAL"` | High-frequency metallic vibration (bridges, cattle grids) |
| `11` | `"DEFAULT"` | Fallback standard profile |

---

## 6. Tire Physics & Surface Interaction

### A. Wheel Ellipsoid Collision Detection (`0x007F4740`)
The function `neon__vehiclePhysics__SuspensionSystem__ellipsoidWheelCollision`:
1. Performs an ellipsoid raycast from the wheel axle towards the ground mesh.
2. Finds the hit `PhysicsMaterial*`.
3. Resolves the `SurfaceSpec*` offset inside the material data buffer.
4. Assigns `Wheel[i]->pSurfaceSpec = pSurfaceSpec`.
5. Evaluates micro-bumps using `bumpsmagnitude`, `bumpswavelength`, and `depth`.
6. Passes the contact normal vector and penetration depth into suspension spring displacement.

### B. Compound-Specific Grip Evaluation (`0x007FD240` & `0x007FFEA0`)
During tire contact calculations, the effective surface grip is evaluated dynamically:
```cpp
float effectiveGrip = pSurfaceSpec->grip;
if (car->activeTireCompound == TIRE_TARMAC) {
    effectiveGrip = pSurfaceSpec->grip * pSurfaceSpec->grip_tyre_tarmac;
} else if (car->activeTireCompound == TIRE_SNOW) {
    effectiveGrip = pSurfaceSpec->grip * pSurfaceSpec->grip_tyre_snow;
}
```

### C. Tire Forces & Pacejka Model (`0x007E64A0`)
`neon__vehiclePhysics__SuspensionSystem__calculateWheelTireForces`:
1. **Normal Load ($F_z$)**: Derived from vehicle mass, suspension compression, and aerodynamic downforce:
   $$F_z = \text{load}_{\text{static}} + k_s \cdot \Delta x + c_d \cdot v_x^2$$
2. **Slip Ratio ($\kappa$) & Slip Angle ($\alpha$)**:
   $$\kappa = \frac{\omega \cdot r - v_x}{v_x}, \quad \alpha = \arctan\left(\frac{v_y}{|v_x|}\right) - \delta$$
3. **Friction Ellipse Constraint**:
   $$\left(\frac{F_x}{\mu_x \cdot F_z}\right)^2 + \left(\frac{F_y}{\mu_y \cdot F_z}\right)^2 \le 1.0$$
   When the combined forces exceed $1.0$, the tire saturates and begins sliding, reducing steering responsiveness.
4. **Surface Slowdown Resistance (`0x007EC260`)**:
   Loose surfaces apply deceleration drag directly to the wheel angular velocity and car chassis:
   $$\text{slowdownFactor} = \text{clamp}\left(\frac{v - v_{\min}}{v_{\max} - v_{\min}}, 0.0, 1.0\right) \cdot \text{slowdown}$$

---

## 7. Wet Track Material Remapping System

DiRT 3 dynamically converts track materials between dry and wet weather configurations using FourCC material identifiers.
- XML File: `wet_materials_remap.xml`
- Parser: `neon__NePhysicsMaterialManager__initialiseRemaps` (`0x007E8920`)
- Lookups:
  - `neon__NePhysicsMaterialManager__getWetMaterial` (`0x007D3E60`)
  - `neon__NePhysicsMaterialManager__getDryMaterial` (`0x007D3EC0`)

### Remap Table Format:
In `wet_materials_remap.xml`:
```xml
<WETREMAPS>
    <REMAP src="ASPH" target="WASPH" />
    <REMAP src="GRVL" target="WGRV" />
    <REMAP src="DIRT" target="MUDD" />
</WETREMAPS>
```
Each entry maps a 4-byte FourCC dry code to a wet code. When wet conditions are enabled, `getWetMaterial` queries the material manager and swaps the physics properties to their corresponding wet parameters.

---

## 8. Multi-Threaded Execution (`CarJob`)

Vehicle physics calculations in DiRT 3 are offloaded to worker threads via the EGO Engine Job System:
- Vtable: `0x00EFDB70` (`vftable_neon__vehiclePhysics__CarJob`)
- Job Execution: `0x00802E20` (`neon__vehiclePhysics__CarJob__doJob`)
- Job Name: `0x007CFC50` (`neon__vehiclePhysics__CarJob__getName` returns `"CarJob"`)

In each tick, worker threads pull `CarJob` instances from the job queue, executing `neon__vehiclePhysics__Car__update` concurrently for player and AI vehicles.

---

## 9. Comprehensive Symbol Mapping Table (DiRT 3 vs GRID 2)

| Symbol Name | DiRT 3 Address | GRID 2 Address | Subsystem | Description |
|---|---|---|---|---|
| `neRacing__Vehicle__Vehicle` | `0x00921950` | `0x0123bb50` | Vehicle | High-level vehicle container constructor |
| `vftable_neRacing__Vehicle` | `0x00F0D26C` | `0x01991d90` | Vehicle | Virtual method table for `neRacing::Vehicle` |
| `neRacing__Vehicle__update` | `0x0093B010` | `0x01239e50` | Vehicle | Vehicle per-frame update loop |
| `neRacing__Vehicle__tick` | `0x0093B5E0` | `0x0123be80` | Vehicle | Vehicle simulation tick |
| `neRacing__Vehicle__removeFromPlay` | `0x00936E60` | `0x01237190` | Vehicle | Unloads vehicle from current active race |
| `CMR6__Vehicle__Vehicle` | `0x005E6470` | `0x005663e0` | Vehicle | DiRT 3 game-specific vehicle constructor |
| `vftable_CMR6__Vehicle` | `0x00EB58C8` | `0x01944888` | Vehicle | Virtual method table for `CMR6::Vehicle` |
| `CMR6__Vehicle__update` | `0x005FBFE0` | `0x00567a10` | Vehicle | DiRT 3 vehicle update loop |
| `neon__vehiclePhysics__Car__update` | `0x008007B0` | `0x00df8420` | Car Physics | Core physics simulation step |
| `neon__vehiclePhysics__Car__updateSubsystems`| `0x007FFEA0` | `0x00e08450` | Car Physics | Updates drivetrain, brakes, steering, suspension |
| `neon__vehiclePhysics__SuspensionSystem__update`| `0x007FD240`| `0x00e2cc50` | Suspension | Suspension geometry and Pacejka load solver |
| `neon__vehiclePhysics__CarType__buildFromCTFreader`| `0x007F6090`| `0x00e0dac0` | Parameters | Reads and parses car `.ctf` physics parameters |
| `neon__vehiclePhysics__CarTuningState__applyCTF` | `0x007FDD40`| `0x00e0b380` | Parameters | Applies parsed CTF parameters to car physics |
| `neon__vehiclePhysics__CarTuningState__calculateTunedValues`|`0x008017A0`| `0x00e0b620` | Parameters | Evaluates setup sliders and curves |
| `vftable_neon__vehiclePhysics__CarJob` | `0x00EFDB70` | `0x019808a0` | Job System | Virtual method table for `CarJob` |
| `neon__vehiclePhysics__CarJob__doJob` | `0x00802E20` | `0x00df8280` | Job System | Multithreaded execution method for `CarJob` |
| `neon__vehiclePhysics__CarJob__getName` | `0x007CFC50` | `0x00df8270` | Job System | Returns job name (`"CarJob"`) |
| `Physics__PhysicsMaterialManager__createMaterialMemory`| `0x00853B90`| `0x00dbf560`| Materials | Allocates contiguous memory buffer for materials |
| `neon__NePhysicsWorld__createMaterialMemory`| `0x00857320` | `0x00dbf4e0` | Materials | World-level wrapper for material memory alloc |
| `Physics__PhysicsMaterialManager__findMaterialFromFourCC`|`0x0080C2B0`| `0x00db7c90`| Materials | Looks up `PhysicsMaterial*` by 4-byte FourCC |
| `neon__NePhysicsMaterialManager__NePhysicsMaterialManager`|`0x007E8850`|`0x00e32cd0`| Materials | Subsystem material manager constructor |
| `neon__NePhysicsMaterialManager__dtor` | `0x007E88C0` | `0x00e32d80` | Materials | Subsystem material manager destructor |
| `neon__NePhysicsMaterialManager__addDataEntry` | `0x007D3C60` | `0x00df91e0` | Materials | Registers subsystem material memory chunk |
| `neon__NePhysicsMaterialManager__initialise` | `0x007D3CD0` | `0x00e32e80` | Materials | Parses `surface_materials.xml` |
| `neon__NePhysicsMaterialManager__initialiseRemaps`| `0x007E8920`| `0x00e330d0`| Materials | Parses `wet_materials_remap.xml` |
| `neon__NePhysicsMaterialManager__getWetMaterial`| `0x007D3E60` | `0x00e33390` | Materials | Converts dry material FourCC to wet material |
| `neon__NePhysicsMaterialManager__getDryMaterial`| `0x007D3EC0` | `0x00e333f0` | Materials | Converts wet material FourCC to dry material |
| `neRacing__NeMaterialManager__NeMaterialManager`| `0x00B04480`| `0x0102ec40` | Materials | Top-level game material manager constructor |
| `vftable_neRacing__NeMaterialManager` | `0x00F28008` | `0x01988130` | Materials | Virtual method table for `NeMaterialManager` |
| `neRacing__NeMaterialManager__process` | `0x00B0EA90` | `0x0102edf0` | Materials | Loads material XML dataset packages |
| `neRacing__NeMaterialManager__addDataEntry` | `0x00B04510` | `0x0102edb0` | Materials | Registers data entry with internal material mgr |
| `neon__vehiclePhysics__SurfaceSpec__init` | `0x007EA0A0` | `0x00df8720` | Surface | Parses `<MECHANICS>` surface parameters from XML |
| `neon__vehiclePhysics__SurfaceSpec__getFFBTypeFromString`|`0x007D4700`|`0x00e36050`| Surface | Converts string to FFB surface type enum |
| `neon__vehiclePhysics__Car_Materials_Initialiser`| `0x007EC1D0`| `0x00df8710` | Surface | Callback wrapper initializing `SurfaceSpec` |
| `neon__vehiclePhysics__Car__registerPhysicsMaterialDataEntries`|`0x007EC1F0`|`0x00df9160`| Surface | Registers `SurfaceSpec` size (192b) with material mgr |
| `neon__vehiclePhysics__Car__getPhysicsMaterialManager`|`0x007CF820`| `0x00df9260` | Surface | Returns global physics material manager pointer |
| `neon__vehiclePhysics__Car__updateSurfaceSlowdown`| `0x007EC260`| `0x00df8350` | Surface | Computes drag/resistance on loose surfaces |
| `neon__vehiclePhysics__Car__computeAverageGrip` | `0x007EA670` | `0x00df83d0` | Surface | Calculates mean grip coefficient across wheels |
| `neon__vehiclePhysics__SuspensionSystem__ellipsoidWheelCollision`|`0x007F4740`|`0x00e2da70`| Surface | Raycasts wheel contact & extracts `SurfaceSpec` |
| `neon__vehiclePhysics__Wheel__getDeformationAngleOffset`|`0x007D27C0`|`0x00df86d0`| Surface | Computes tire angular deformation contact patch |
| `neon__vehiclePhysics__SuspensionSystem__calculateWheelTireForces`|`0x007E64A0`|`0x00e2e1b0`| Surface | Solves Pacejka slip forces & friction ellipse |
| `neon__vehiclePhysics__Car__s_pPhysicsMaterialManager`|`0x01029D00`|`0x01ab8f38`| Surface | Global pointer to active `NePhysicsMaterialManager` |
| `neon__vehiclePhysics__Car__s_physicsMaterialEntryIndex`|`0x01029D04`| — | Surface | Offset index for `SurfaceSpec` in material buffer |
| `CMR6__VehiclePFXUpdateManager__registerMaterialDataEntries`|`0x00601820`|`0x0041faf0`| Particles | Registers particle material descriptors |
