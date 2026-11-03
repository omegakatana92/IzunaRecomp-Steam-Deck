# ng2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ng2 LANGUAGES C CXX)

# RC is needed for the icon and version block in resources/ng2.rc. Enabled only
# on Windows: it is the only platform with an .rc compiler, and the executable
# is Windows-only anyway.
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# [IzunaRecomp] No GPU emulation: the native graphics system and the Kasumi
# renderer (src/native) are the only graphics path, so the SDK leaves its Xenos
# GPU plugin (rexgpu-xenos) out entirely.
set(REXGLUE_GPU_EMULATION OFF CACHE BOOL "Build the Xenos GPU emulation plugin (rexgpu-xenos)" FORCE)
# [IzunaRecomp] Windows too draws with Vulkan (the Kasumi renderer): the SDK's
# Vulkan side on, its Direct3D 12 side (the emulated GPU's presenter) off, as
# on the PS5 and Linux (tools/windows/README.md).
if(WIN32)
    set(REXGLUE_USE_VULKAN ON CACHE BOOL "Enable Vulkan graphics backend" FORCE)
    set(REXGLUE_USE_D3D12 OFF CACHE BOOL "Enable D3D12 graphics backend" FORCE)
    # One .exe, the runtime linked in (as on the PS5): no GPU plugin to share it with.
    set(REXGLUE_STATIC_RUNTIME ON CACHE BOOL "Link the runtime statically" FORCE)
endif()

option(IZUNA_LINUX_DESKTOP "Build the experimental native Linux desktop application" OFF)
if(IZUNA_LINUX_DESKTOP)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR PS5 OR NG2_PS5_HOST_TEST)
        message(FATAL_ERROR "IZUNA_LINUX_DESKTOP requires native Linux, without PS5 host stubs")
    endif()
    set(REXGLUE_USE_VULKAN ON CACHE BOOL "Vulkan backend" FORCE)
    set(REXGLUE_STATIC_RUNTIME ON CACHE BOOL "Static runtime" FORCE)
endif()
include(generated/rexglue.cmake)
# [IzunaRecomp] Not asynchronous exceptions for the recompiled game on
# Windows: they are for guest SEH scopes (__try in the game's code), which
# Ninja Gaiden II has none of, and they make the compiler treat every load and
# store of the generated code as able to throw - holding back its
# optimisation of exactly the code that runs most. (The PS5 and Linux builds
# never had them.)
if(WIN32 AND NOT IZUNA_RECOMP_ASYNC_EH)
    list(REMOVE_ITEM REXGLUE_RECOMP_OPTIONS -fasync-exceptions /EHa)
    if(REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
        set_source_files_properties(${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}
            PROPERTIES COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS}")
    endif()
endif()

# The port's own version, read from VERSION rather than typed here, so the
# number on screen is the number the release script packages. Listed as a
# configure dependency so bumping VERSION re-runs CMake - without that, the
# build happily bakes in the previous number.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NG2_VERSION LIMIT_COUNT 1)
string(STRIP "${NG2_VERSION}" NG2_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
message(STATUS "ng2 version ${NG2_VERSION}")

# Sources
set(NG2_SOURCES
    src/main.cpp
    src/diag_hooks.cpp
    src/native/d3d_trace.cpp  # [native renderer track]
    src/native/native_renderer.cpp
    src/native/native_draw.cpp       # (its parts share native_draw_internal.h)
    src/native/native_pipelines.cpp
    src/native/native_textures.cpp
    src/native/native_worker.cpp
    src/native/native_capture.cpp
    src/native/native_resolve_7e3.cpp
    src/native/native_mods.cpp       # [IzunaRecomp] mods: hidden shaders, replaced textures
    src/native/pc_display_clock.cpp  # [IzunaRecomp] the display's vblanks on Windows
    src/native/native_present.cpp  # [IzunaRecomp, M4] the native swap
    src/native/izuna_gpu.cpp       # [IzunaRecomp, M4] the native graphics system
    src/izuna/izuna_mods.cpp       # [IzunaRecomp] mods: game files from Mods/
    src/izuna/izuna_scripts.cpp    # [IzunaRecomp] script mods (Lua)
    src/izuna/izuna_hooks.cpp      # [IzunaRecomp] hooks on the game's functions
    src/izuna/izuna_hooks_none.cpp
    src/izuna/izuna_pc.cpp         # [IzunaRecomp] Windows: power, timers, priorities, crash dumps
    src/izuna/izuna_cpu_redirect.cpp  # [IzunaRecomp] Windows: an older CPU goes to ng2-compat.exe
    third_party/zstd/zstddeclib.c  # (zstd's decoder, BSD: the shader cache is zstd-compressed)
    third_party/smol-v/smolv.cpp   # (smol-v, public domain / MIT: each shader is smol-v encoded)
    src/patch_hooks.cpp
    src/ng2_audio_fix.cpp
    src/ng2_chapter_fix.cpp
    src/ng2_disc.cpp
    src/ng2_menu.cpp
    src/ng2_platform.cpp
    src/ng2_saveimport.cpp
    src/ng2_perf_hud.cpp
    src/ng2_diagnostics.cpp
    src/ng2_hwdetect.cpp
    src/ng2_perf.cpp
    src/ng2_autoskip.cpp
    src/third_party/stb_image.cpp
)

# [ps5 port] Ninja Gaiden II for PS5 (ps5/BUILD.md). Windows-only code is
# guarded in place with __PROSPERO__ (platform, hardware detection,
# performance readouts, diagnostics). The hub (the front screen), the in-game
# overlay and the PlayStation button prompts are the PS5's and Windows'.
# [ps5 port] A Linux desktop build of the PS5 app, for the native renderer's
# captures without a console (ps5/BUILD.md, "Host test build"): the PS5 code
# paths with the Sony system calls stubbed, on any Vulkan driver (Mesa's
# lavapipe will do). Never shipped.
option(NG2_PS5_HOST_TEST "Build the PS5 app for the Linux desktop (testing only)" OFF)
# [IzunaRecomp] The runtime linked in elsewhere (the Windows build,
# REXGLUE_STATIC_RUNTIME) carries stb_image too, as on the PS5 below.
if(REXGLUE_STATIC_RUNTIME AND NOT PS5)
    list(REMOVE_ITEM NG2_SOURCES src/third_party/stb_image.cpp)
endif()
if(PS5 OR NG2_PS5_HOST_TEST)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/ps5/VERSION.txt" NG2_PS5_VERSION LIMIT_COUNT 1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/ps5/VERSION.txt")
    # The runtime is linked statically on PS5 and already carries stb_image
    # (src/ui/image_decode.cpp); a second copy is a duplicate definition.
    list(REMOVE_ITEM NG2_SOURCES src/third_party/stb_image.cpp)
    list(APPEND NG2_SOURCES
        src/ps5/ps5_paths.cpp
        src/ps5/ps5_pad.cpp
        src/ps5/ps5_sound.cpp
        src/ps5/ps5_textures.cpp
        src/ps5/ps5_videoout.cpp
        src/ps5/ps5_button_prompts.cpp
        src/ps5/hub/hub_ui.cpp
        src/ps5/hub/hub_settings.cpp
        src/ps5/hub/hub_pages.cpp
        src/ps5/hub/hub_backdrop.cpp
        src/ps5/hub/hub_sfx.cpp
        src/ps5/hub/ps5_hub.cpp
        src/ps5/overlay/ps5_overlay.cpp)
    message(STATUS "Ninja Gaiden II for PS5 ${NG2_PS5_VERSION}")
endif()
# [IzunaRecomp] Windows: the PS5's hub as the front screen (src/ps5/hub), with
# the PC's input, sound and folders in the same files (ps5_pad.cpp,
# ps5_sound.cpp, ps5_paths.cpp: SDL and the .exe's folder off the PS5).
if(WIN32 OR IZUNA_LINUX_DESKTOP)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/IZUNA_VERSION" IZUNA_VERSION LIMIT_COUNT 1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/IZUNA_VERSION")
    list(APPEND NG2_SOURCES
        src/ps5/ps5_paths.cpp
        src/ps5/ps5_pad.cpp
        src/ps5/ps5_sound.cpp
        src/ps5/hub/hub_ui.cpp
        src/ps5/hub/hub_settings.cpp
        src/ps5/hub/hub_pages.cpp
        src/ps5/hub/hub_backdrop.cpp
        src/ps5/hub/hub_sfx.cpp
        src/ps5/hub/ps5_hub.cpp
        src/ps5/overlay/ps5_overlay.cpp
        src/ps5/ps5_button_prompts.cpp
        src/ps5/ps5_textures.cpp)
    message(STATUS "IzunaRecomp ${IZUNA_VERSION} for Windows")
endif()

# The icon. Explorer, the desktop shortcut and the taskbar all read it from
# here - see the comment at the top of resources/ng2.rc.in.
#
# The .rc is generated so its version block comes from the VERSION file. A
# hardcoded copy drifts the moment VERSION is bumped, and drifts silently:
# nothing reads it except the Properties dialog.
if(WIN32)
    # VERSIONINFO wants four comma-separated numbers.
    # [IzunaRecomp] The .exe's details: IzunaRecomp and its version.
    if(IZUNA_VERSION)
        set(NG2_RC_VERSION "${IZUNA_VERSION}")
        set(NG2_RC_PRODUCT "Ninja Gaiden II - IzunaRecomp")
    else()
        set(NG2_RC_VERSION "${NG2_VERSION}")
        set(NG2_RC_PRODUCT "Ninja Gaiden II (ReXGlue recompilation)")
    endif()
    string(REPLACE "." ";" NG2_VERSION_PARTS "${NG2_RC_VERSION}")
    list(LENGTH NG2_VERSION_PARTS NG2_VERSION_PART_COUNT)
    while(NG2_VERSION_PART_COUNT LESS 4)
        list(APPEND NG2_VERSION_PARTS "0")
        list(LENGTH NG2_VERSION_PARTS NG2_VERSION_PART_COUNT)
    endwhile()
    list(JOIN NG2_VERSION_PARTS ", " NG2_VERSION_RC)

    # rc resolves a relative path against the .rc's own directory, and the
    # generated one lives in the build tree - so hand it an absolute path.
    # [IzunaRecomp] IzunaRecomp's own icon (tools/make_izuna_icon.py), not the PC port's.
    set(NG2_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/izuna.ico")
    if(NOT EXISTS "${NG2_ICON_PATH}")
        message(FATAL_ERROR
            "resources/ng2.ico is missing. Regenerate it: python tools/make_icon.py")
    endif()
    # Backslashes are escape characters to the resource compiler.
    string(REPLACE "\\" "/" NG2_ICON_PATH "${NG2_ICON_PATH}")

    configure_file(resources/ng2.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ng2.rc" @ONLY)
    list(APPEND NG2_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/ng2.rc")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/resources/ng2.rc.in")
endif()

if(WIN32)
    add_executable(ng2 WIN32 ${NG2_SOURCES})
else()
    add_executable(ng2 ${NG2_SOURCES})
endif()

rexglue_setup_target(ng2)

# [native renderer] The game's shaders, translated by our XenosRecomp fork.
# Windows builds them from the player's own copy at startup
# (src/izuna/izuna_shaders.cpp): ng2.exe carries none. The PS5 and the Linux
# host test (no DXC) have them built in: tools/native/build_shaders.sh writes
# out/native-shaders/shader_cache.cpp - made from the game: build output,
# never committed.
if(WIN32 OR IZUNA_LINUX_DESKTOP)
    target_compile_definitions(ng2 PRIVATE IZUNA_SHADERS_FROM_GAME=1)
else()
    set(NG2_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/out/native-shaders/shader_cache.cpp")
    if(NOT EXISTS "${NG2_SHADER_CACHE}")
        message(FATAL_ERROR "Missing ${NG2_SHADER_CACHE}: run tools/native/build_shaders.sh first")
    endif()
    target_sources(ng2 PRIVATE "${NG2_SHADER_CACHE}")
    set_source_files_properties("${NG2_SHADER_CACHE}" PROPERTIES COMPILE_OPTIONS "-O0;-w")
endif()
target_include_directories(ng2 PRIVATE src/native)
# (toml++ from the SDK tree: the mods' mod.toml and the saved order - src/izuna/izuna_mods.cpp)
set_source_files_properties(src/izuna/izuna_mods.cpp PROPERTIES INCLUDE_DIRECTORIES
                            ${REXSDK_DIR}/thirdparty/tomlplusplus/include)
# [IzunaRecomp] The thunks that make every game function hookable
# (tools/izuna/gen_hook_thunks.py, run after the codegen; src/izuna/izuna_hooks.h).
file(GLOB NG2_HOOK_THUNKS ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/izuna_hooks.*.cpp)
if(NG2_HOOK_THUNKS)
    target_sources(ng2 PRIVATE ${NG2_HOOK_THUNKS})
else()
    message(WARNING "No generated/default/izuna_hooks.*.cpp: the game's functions cannot be hooked by script mods "
                    "(run tools/izuna/gen_hook_thunks.py after the codegen)")
endif()
# Lua 5.4 for the script mods (third_party/lua/README.md): its core and the
# libraries a script may have, as C, quietly.
file(GLOB NG2_LUA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/src/*.c)
target_sources(ng2 PRIVATE ${NG2_LUA_SOURCES})
set_source_files_properties(${NG2_LUA_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(src/izuna/izuna_scripts.cpp PROPERTIES INCLUDE_DIRECTORIES
                            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/src)
# (the SDK's Vulkan presenter header includes <renderdoc_app.h>: the app's copy)
set_source_files_properties(src/native/native_present.cpp src/native/izuna_gpu.cpp PROPERTIES INCLUDE_DIRECTORIES
                            ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party)
target_link_libraries(ng2 PRIVATE xxHash::xxhash)

# Post-codegen guard for the local recompilation crash patches. These two fixes
# (local/diag/patch_missed_regs.py + patch_scanguard.py) edit the *generated*
# translated source, so a codegen regeneration overwrites them. That happened
# once and silently shipped two crashes back into v1.0.17 (a null write on
# Ninpo/a late boss, and the chapter 12->13 transition). Wiring the re-apply
# into the build graph after codegen and before the recomp compile makes that
# regression impossible: whenever codegen refreshes codegen.build.stamp, the
# patches are re-applied (idempotent - each script skips a file already carrying
# its marker) before the generated .cpp are compiled.
# [ps5 port] Not in a cross build: codegen and its patches run on the host
# (ps5/BUILD.md), and the PS5 build compiles what they left in generated/.
if(TARGET ng2_codegen AND TARGET ng2_recomp AND NOT CMAKE_CROSSCOMPILING)
    find_program(NG2_PYTHON NAMES python python3)
    if(NG2_PYTHON)
        set(_ng2_gen "${CMAKE_CURRENT_SOURCE_DIR}/generated/default")
        add_custom_command(
            OUTPUT "${_ng2_gen}/local_patches.stamp"
            DEPENDS "${_ng2_gen}/codegen.build.stamp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/local/diag/patch_missed_regs.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/local/diag/patch_scanguard.py"
            COMMAND "${NG2_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/local/diag/patch_missed_regs.py"
            COMMAND "${NG2_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/local/diag/patch_scanguard.py"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_ng2_gen}/local_patches.stamp"
            COMMENT "Re-applying NG2 post-codegen crash patches (missed_regs, scanguard)"
            VERBATIM)
        add_custom_target(ng2_local_patches
            DEPENDS "${_ng2_gen}/local_patches.stamp")
        add_dependencies(ng2_recomp ng2_local_patches)
    else()
        message(WARNING
            "python not found: NG2 post-codegen crash patches will NOT be auto-"
            "re-applied. After any codegen regeneration, run by hand: "
            "local/diag/patch_missed_regs.py and local/diag/patch_scanguard.py")
    endif()
endif()

target_compile_definitions(ng2 PRIVATE NG2_VERSION="${NG2_VERSION}")

if(PS5)
    # [ps5 port] Linux names for the POSIX code (as in the SDK), ImGui and stb
    # from the SDK tree, the hub's fonts, and the PS5 system services the app
    # uses directly (the user, the pad, audio out, the system shell).
    target_compile_definitions(ng2 PRIVATE
        NG2_PS5_VERSION="${NG2_PS5_VERSION}"
        NG2_PS5_FONTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ps5/assets/fonts")
    target_compile_options(ng2 PRIVATE "SHELL:-include ${REXSDK_DIR}/include/rex/ps5_compat.h")
    target_include_directories(ng2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
        ${REXSDK_DIR}/thirdparty/stb
        ${REXSDK_DIR}/thirdparty/xxHash)
    target_link_libraries(ng2 PRIVATE SceUserService ScePad SceAudioOut SceSystemService)
    # [IzunaRecomp] The swapchain's own sceVideoOutOpen / SubmitFlip (the PS5
    # Mesa's WSI) pass through src/ps5/ps5_videoout.cpp, which keeps the handle
    # and each flip: the display's vblank times and pending flips pace the
    # guest vblank. RADV
    # joins only in ps5/tools/ps5_package.sh's link, which wraps it there too.
    target_link_libraries(ng2 PRIVATE SceVideoOut)
    target_link_options(ng2 PRIVATE "-Wl,--wrap=sceVideoOutOpen" "-Wl,--wrap=sceVideoOutSubmitFlip")
elseif(NG2_PS5_HOST_TEST)
    # NG2's own sources take their PS5 paths (__PROSPERO__ for them only: the
    # SDK's platform detection checks __gnu_linux__ first, so it stays Linux),
    # with the handful of Sony calls they make stubbed.
    target_sources(ng2 PRIVATE src/ps5/host_test_stubs.cpp)
    target_compile_definitions(ng2 PRIVATE
        __PROSPERO__=1 NG2_PS5_HOST_TEST=1
        NG2_PS5_VERSION="${NG2_PS5_VERSION}-hosttest"
        NG2_PS5_FONTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ps5/assets/fonts")
    target_include_directories(ng2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
        ${REXSDK_DIR}/thirdparty/stb
        ${REXSDK_DIR}/thirdparty/xxHash)
elseif(WIN32 OR IZUNA_LINUX_DESKTOP)
    # [IzunaRecomp] The desktop build from the SDK's source tree (not its
    # installed package): ImGui and stb from there, as the PS5 build takes them.
    target_compile_definitions(ng2 PRIVATE
        IZUNA_VERSION="${IZUNA_VERSION}"
        NG2_PS5_FONTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ps5/assets/fonts")
    target_include_directories(ng2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
        ${REXSDK_DIR}/thirdparty/stb
        ${REXSDK_DIR}/thirdparty/xxHash)
endif()

# DXGI, for naming the GPU in the diagnostics report (src/ng2_hwdetect.cpp).
if(WIN32)
    target_link_libraries(ng2 PRIVATE dxgi)
    # [IzunaRecomp] Minidumps (dbghelp), 1 ms timers (winmm); a PDB beside the
    # .exe to read the minidumps with (kept with the build, not shipped).
    target_link_libraries(ng2 PRIVATE dbghelp winmm)
endif()
if(WIN32 OR IZUNA_LINUX_DESKTOP)
    # [IzunaRecomp] The game's shaders built from the player's own copy at
    # startup (src/izuna/izuna_shaders.cpp): our XenosRecomp fork's translator
    # (third_party/xenosrecomp, MIT) and DXC loaded from dxcompiler.dll beside
    # the executable (tools/windows/make_release.sh puts it there). Its shader
    # library, shader_common.h, is pasted into every shader it writes: built
    # in as a string.
    set(_izuna_sc "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xenosrecomp/shader_common.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_izuna_sc}")
    file(READ "${_izuna_sc}" _izuna_sc_text)
    string(LENGTH "${_izuna_sc_text}" _izuna_sc_len)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/izuna_shader_common.cpp"
        "// generated from third_party/xenosrecomp/shader_common.h (CMakeLists.txt)\n"
        "extern const char kIzunaShaderCommon[] = R\"IZUNASC(${_izuna_sc_text})IZUNASC\";\n"
        "extern const unsigned long kIzunaShaderCommonSize = ${_izuna_sc_len};\n")
    set(_izuna_xr
        third_party/xenosrecomp/shader_recompiler.cpp
        src/izuna/izuna_shaders.cpp
        src/izuna/izuna_readable.cpp)
    target_sources(ng2 PRIVATE ${_izuna_xr} "${CMAKE_CURRENT_BINARY_DIR}/izuna_shader_common.cpp")
    set_source_files_properties(${_izuna_xr} PROPERTIES
        COMPILE_DEFINITIONS "REEOT_RECOMP;NG2_RECOMP;_CRT_SECURE_NO_WARNINGS"
        COMPILE_OPTIONS "-Wno-switch;-Wno-unused-variable;-Wno-null-arithmetic;-fms-extensions;-w"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xenosrecomp;${CMAKE_CURRENT_SOURCE_DIR}/third_party/xenosrecomp/inc;${CMAKE_CURRENT_SOURCE_DIR}/third_party/smol-v;${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd")
endif()
if(WIN32)
    # [IzunaRecomp] The CPU level (tools/windows/toolchain.cmake): ng2.exe for
    # x86-64-v3 with the redirect for older CPUs, ng2-compat.exe for x86-64-v2.
    if(IZUNA_MARCH STREQUAL "x86-64-v3")
        target_compile_definitions(ng2 PRIVATE IZUNA_CPU_V3=1 IZUNA_CPU_LEVEL=3)
        set_source_files_properties(src/izuna/izuna_cpu_redirect.cpp PROPERTIES COMPILE_OPTIONS "-march=x86-64")
    else()
        target_compile_definitions(ng2 PRIVATE IZUNA_CPU_LEVEL=2)
        set_target_properties(ng2 PROPERTIES OUTPUT_NAME ng2-compat)
    endif()
    # (/DEBUG alone turns the linker's dead-code removal and folding off: back on)
    target_link_options(ng2 PRIVATE "-Wl,/DEBUG" "-Wl,/OPT:REF" "-Wl,/OPT:ICF")
endif()
if(NOT WIN32 AND NOT IZUNA_LINUX_DESKTOP)
    target_sources(ng2 PRIVATE src/izuna/izuna_shaders_none.cpp)  # [IzunaRecomp] (no startup build: the built-in table)
endif()

if(IZUNA_LINUX_DESKTOP)
    target_compile_definitions(ng2 PRIVATE IZUNA_LINUX_DESKTOP=1)
    target_link_libraries(ng2 PRIVATE ${CMAKE_DL_LIBS})
    target_link_options(ng2 PRIVATE "-Wl,-rpath,$ORIGIN")
endif()
