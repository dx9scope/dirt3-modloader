set_project("DiRT3Modloader")
set_version("1.0.0")

add_rules("mode.debug", "mode.release")

-- Set target platform and architecture (32-bit x86 Windows)
set_plat("windows")
set_arch("x86")
set_languages("c++20")

-- Add packages via xrepo
add_requires("minhook")

-- Target Steam Game Directory
local steam_game_dir = "D:/SteamLibrary/steamapps/common/DiRT 3 Complete Edition"

target("dinput8")
    set_kind("shared")
    set_filename("dinput8.dll")

    add_packages("minhook")

    -- Include directories
    add_includedirs("src", "src/core", "src/modules", "src/api")

    -- Source files
    add_files("src/core/**.cpp")
    add_files("src/modules/**.cpp")

    -- Windows system libraries
    add_syslinks("user32", "shell32", "shlwapi")

    -- Export DirectInput8Create without MSVC name mangling
    add_files("exports.def")

    -- Build configurations
    if is_mode("release") then
        set_optimize("fastest")
        set_symbols("debug")
    else
        set_symbols("debug")
        set_optimize("none")
    end

    -- Automatically deploy to Steam DiRT 3 directory after every build
    after_build(function (target)
        if os.isdir(steam_game_dir) then
            local target_dll = target:targetfile()
            local target_pdb = target:symbolfile()
            
            os.cp(target_dll, path.join(steam_game_dir, "dinput8.dll"))
            if target_pdb and os.isfile(target_pdb) then
                os.cp(target_pdb, path.join(steam_game_dir, "dinput8.pdb"))
            end
            
            -- Ensure mods/ directory exists in game folder
            local mods_dir = path.join(steam_game_dir, "mods")
            if not os.isdir(mods_dir) then
                os.mkdir(mods_dir)
            end
            
            cprint("${bright green}[Modloader] Successfully deployed dinput8.dll to Steam: %s", steam_game_dir)
        else
            cprint("${yellow}[Modloader] Steam directory not found: %s", steam_game_dir)
        end
    end)

target("gravity_mod")
    set_kind("shared")
    set_filename("gravity_mod.dll")

    add_includedirs("src/api")
    add_files("plugins/gravity_mod/gravity_mod.cpp")
    add_syslinks("user32")

    if is_mode("release") then
        set_optimize("fastest")
        set_symbols("debug")
    else
        set_symbols("debug")
        set_optimize("none")
    end

    after_build(function (target)
        if os.isdir(steam_game_dir) then
            local mods_dir = path.join(steam_game_dir, "mods")
            if not os.isdir(mods_dir) then
                os.mkdir(mods_dir)
            end
            os.cp(target:targetfile(), path.join(mods_dir, "gravity_mod.dll"))
            local target_pdb = target:symbolfile()
            if target_pdb and os.isfile(target_pdb) then
                os.cp(target_pdb, path.join(mods_dir, "gravity_mod.pdb"))
            end
            local ini_file = "plugins/gravity_mod/gravity_mod.ini"
            if os.isfile(ini_file) then
                os.cp(ini_file, path.join(mods_dir, "gravity_mod.ini"))
            end
            cprint("${bright green}[GravityMod] Successfully deployed gravity_mod.dll to %s", mods_dir)
        end
    end)

target("telemetry_mod")
    set_kind("shared")
    set_filename("telemetry_mod.dll")

    add_includedirs("src/api")
    add_files("plugins/telemetry_mod/telemetry_mod.cpp")
    add_syslinks("user32")

    if is_mode("release") then
        set_optimize("fastest")
        set_symbols("debug")
    else
        set_symbols("debug")
        set_optimize("none")
    end

    after_build(function (target)
        if os.isdir(steam_game_dir) then
            local mods_dir = path.join(steam_game_dir, "mods")
            if not os.isdir(mods_dir) then
                os.mkdir(mods_dir)
            end
            os.cp(target:targetfile(), path.join(mods_dir, "telemetry_mod.dll"))
            local target_pdb = target:symbolfile()
            if target_pdb and os.isfile(target_pdb) then
                os.cp(target_pdb, path.join(mods_dir, "telemetry_mod.pdb"))
            end
            cprint("${bright green}[TelemetryMod] Successfully deployed telemetry_mod.dll to %s", mods_dir)
        end
    end)

target("nitro_mod")
    set_kind("shared")
    set_filename("nitro_mod.dll")

    add_includedirs("src/api")
    add_files("plugins/nitro_mod/nitro_mod.cpp")
    add_files("plugins/nitro_mod/overlay.cpp")
    add_syslinks("user32")

    if is_mode("release") then
        set_optimize("fastest")
        set_symbols("debug")
    else
        set_symbols("debug")
        set_optimize("none")
    end

    after_build(function (target)
        if os.isdir(steam_game_dir) then
            local mods_dir = path.join(steam_game_dir, "mods")
            if not os.isdir(mods_dir) then
                os.mkdir(mods_dir)
            end
            os.cp(target:targetfile(), path.join(mods_dir, "nitro_mod.dll"))
            local target_pdb = target:symbolfile()
            if target_pdb and os.isfile(target_pdb) then
                os.cp(target_pdb, path.join(mods_dir, "nitro_mod.pdb"))
            end
            -- Only copy default config if user hasn't customised one yet
            local ini_file = "plugins/nitro_mod/nitro_mod.ini"
            local ini_dst = path.join(mods_dir, "nitro_mod.ini")
            if os.isfile(ini_file) and not os.isfile(ini_dst) then
                os.cp(ini_file, ini_dst)
            end
            cprint("${bright green}[NitroMod] Successfully deployed nitro_mod.dll to %s", mods_dir)
        end
    end)

target("flyhack_mod")
    set_kind("shared")
    set_filename("flyhack_mod.dll")

    add_includedirs("src/api")
    add_files("plugins/flyhack_mod/flyhack_mod.cpp")
    add_syslinks("user32")

    if is_mode("release") then
        set_optimize("fastest")
        set_symbols("debug")
    else
        set_symbols("debug")
        set_optimize("none")
    end

    after_build(function (target)
        if os.isdir(steam_game_dir) then
            local mods_dir = path.join(steam_game_dir, "mods")
            if not os.isdir(mods_dir) then
                os.mkdir(mods_dir)
            end
            os.cp(target:targetfile(), path.join(mods_dir, "flyhack_mod.dll"))
            local target_pdb = target:symbolfile()
            if target_pdb and os.isfile(target_pdb) then
                os.cp(target_pdb, path.join(mods_dir, "flyhack_mod.pdb"))
            end
            local ini_file = "plugins/flyhack_mod/flyhack_mod.ini"
            local ini_dst = path.join(mods_dir, "flyhack_mod.ini")
            if os.isfile(ini_file) then
                os.cp(ini_file, ini_dst)
            end
            cprint("${bright green}[FlyhackMod] Successfully deployed flyhack_mod.dll to %s", mods_dir)
        end
    end)

target("fov_mod")
    set_kind("shared")
    set_filename("fov_mod.dll")

    add_includedirs("src/api")
    add_files("plugins/fov_mod/fov_mod.cpp")
    add_syslinks("user32")

    if is_mode("release") then
        set_optimize("fastest")
        set_symbols("debug")
    else
        set_symbols("debug")
        set_optimize("none")
    end

    after_build(function (target)
        if os.isdir(steam_game_dir) then
            local mods_dir = path.join(steam_game_dir, "mods")
            if not os.isdir(mods_dir) then
                os.mkdir(mods_dir)
            end
            os.cp(target:targetfile(), path.join(mods_dir, "fov_mod.dll"))
            local target_pdb = target:symbolfile()
            if target_pdb and os.isfile(target_pdb) then
                os.cp(target_pdb, path.join(mods_dir, "fov_mod.pdb"))
            end
            local ini_file = "plugins/fov_mod/fov_mod.ini"
            local ini_dst = path.join(mods_dir, "fov_mod.ini")
            if os.isfile(ini_file) and not os.isfile(ini_dst) then
                os.cp(ini_file, ini_dst)
            end
            cprint("${bright green}[FovMod] Successfully deployed fov_mod.dll to %s", mods_dir)
        end
    end)



