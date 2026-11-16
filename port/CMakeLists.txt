cmake_minimum_required(VERSION 3.25) # 3.25: the LANCommander Interposer (embedded, WHOLE_ARCHIVE)
project(HydroThunder VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# The port's version (hydro.exe's version resource, the log, htmaster --version); release tags are v<this>.
add_compile_definitions(HYDRO_VERSION="${PROJECT_VERSION}" HYDRO_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    HYDRO_VERSION_MINOR=${PROJECT_VERSION_MINOR} HYDRO_VERSION_PATCH=${PROJECT_VERSION_PATCH})

if(WIN32)
    # The C/C++ runtime is linked statically into every target, the vendored libraries' too (they're added with
    # policy CMP0091 at its new default, so they follow CMAKE_MSVC_RUNTIME_LIBRARY): no VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    enable_language(ASM_MASM)

    # Windows XP SP3 is the oldest supported Windows, for both executables:
    # - _WIN32_WINNT / WINVER hide every newer API in the SDK headers, so calling one is a compile error;
    # - the PE headers ask for Windows 5.01 (the linker's default, 6.0, doesn't load on XP);
    # - common/winxp_crt.* stand in for the few Vista+ functions the static CRT itself imports;
    # - tools/check_imports.py checks the linked executables (CI).
    add_subdirectory(common)
    function(hydro_target_winxp target)
        target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0501 WINVER=0x0501)
        target_link_libraries(${target} PRIVATE winxp_crt)
        target_link_options(${target} PRIVATE /SUBSYSTEM:CONSOLE,5.01)
    endfunction()
endif()

# Lobby master / relay server: portable C with no dependencies, so it builds on Windows and Linux.
add_executable(htmaster server/htmaster.c server/lobby.c common/htnet_proto.c common/sha256.c)
if(WIN32)
    target_compile_definitions(htmaster PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    target_link_libraries(htmaster PRIVATE ws2_32)
    hydro_target_winxp(htmaster)
else()
    target_compile_definitions(htmaster PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(htmaster PRIVATE -Wall -Wextra)
    # A fully static binary runs on any x86-64 Linux (drop it if the distro has no static libc).
    option(HTMASTER_STATIC "Link htmaster statically" ON)
    if(HTMASTER_STATIC)
        target_link_options(htmaster PRIVATE -static -s)
    endif()
endif()
install(TARGETS htmaster RUNTIME DESTINATION .)

if(NOT WIN32)
    message(STATUS "Not Windows: building htmaster only")
    return()
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The host runs original 32-bit x86 code in-process: configure with -A Win32")
endif()

add_executable(hydro
    host/main.c
    host/hydro.rc
    host/disk_setup.c
    host/image.c
    host/symtab.c
    host/hook.c
    host/veh.c
    host/window.c
    host/input.c
    host/menu_assist.c
    host/hle_win32.c
    host/hle_glide.c
    host/hle_hw.c
    host/hle_audio.c
    host/hle_net.c
    host/trace.c
    host/dgv.cpp
    host/markers.cpp
    host/settings.c
    host/menu_pc.c
    host/menu_lobby.c
    host/lobby_client.c
    server/lobby.c
    common/htnet_proto.c
    common/sha256.c
    generated/symtab.c
    game/ports.c
)
# Decompiled game modules: x87 codegen, like the original, so ports can match it bit-for-bit.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS game/*.c)
list(REMOVE_ITEM GAME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/game/ports.c)
target_sources(hydro PRIVATE ${GAME_SOURCES})
set_source_files_properties(${GAME_SOURCES} PROPERTIES COMPILE_OPTIONS "/arch:IA32;/fp:precise")
target_include_directories(hydro PRIVATE host include game ${REPO_ROOT}/vendor/dgVoodoo2/dgVoodooAPI/Inc)
target_compile_definitions(hydro PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
target_compile_options(hydro PRIVATE /W3 /wd4201 $<$<COMPILE_LANGUAGE:CXX>:/permissive- /EHsc>)
target_link_libraries(hydro PRIVATE chdr-static SDL2::SDL2-static user32 gdi32 comdlg32 winmm ws2_32 iphlpapi dnsapi
    shell32 ole32 windowscodecs)
hydro_target_winxp(hydro)

# The host's C++ follows the C++ Core Guidelines; this runs MSVC's checker for them with warnings as errors.
# (The rest of the port is C11, and the game modules mirror the original's code.)
option(HYDRO_CPP_CORE_CHECK "Check the C++ files against the C++ Core Guidelines (Visual Studio generator)" OFF)
if(HYDRO_CPP_CORE_CHECK)
    set(CORE_CHECK_RULESET
        "${CMAKE_GENERATOR_INSTANCE}/Team Tools/Static Analysis Tools/Rule Sets/CppCoreCheckRules.ruleset")
    if(NOT EXISTS "${CORE_CHECK_RULESET}")
        message(FATAL_ERROR "C++ Core Check rule set not found at ${CORE_CHECK_RULESET}")
    endif()
    file(TO_NATIVE_PATH "${CORE_CHECK_RULESET}" CORE_CHECK_RULESET) # cl can't parse it with forward slashes
    set_source_files_properties(host/dgv.cpp host/markers.cpp PROPERTIES COMPILE_OPTIONS
        "/W4;/WX;/analyze;/analyze:pluginEspXEngine.dll;/analyze:ruleset${CORE_CHECK_RULESET};/analyze:external-;/external:W0;/external:anglebrackets;/external:I${REPO_ROOT}/vendor/dgVoodoo2/dgVoodooAPI/Inc")
endif()

# CHD reader for the first-run extraction of hydro.chd (host/disk_setup.c). Only chdr-static is built.
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${REPO_ROOT}/vendor/libchdr ${CMAKE_CURRENT_BINARY_DIR}/libchdr EXCLUDE_FROM_ALL)

# SDL 2 for game controllers (host/input.c), built as a static library with only what that needs. SDL 2 (not 3)
# because it still runs on Windows XP. Its code sits below the original image, so unused parts are left out.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_LIBC ON CACHE BOOL "" FORCE) # the static CRT, not SDL's own (which clashes with it)
foreach(sub AUDIO VIDEO RENDER HAPTIC POWER FILESYSTEM SENSOR LOCALE MISC)
    set(SDL_${sub} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${REPO_ROOT}/vendor/SDL ${CMAKE_CURRENT_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)
# No dynamic API: its jump table references (so links in) every SDL function, and it would let an environment
# variable swap in another SDL DLL. SDL_dynapi.h rejects SDL_DYNAMIC_API=0 from the command line; this is the one
# switch it takes. (With MSVC its only other effect is that two of SDL's internal joystick locks aren't static.)
target_compile_definitions(SDL2-static PRIVATE SDL_THREAD_SAFETY_ANALYSIS)

# The original image must live at 0x00100000-0x007a30c0 (it has no relocations). The kernel
# fills low memory (stack, heaps) before main() runs, so hydro.exe itself owns that range:
# it's based at 0x10000 and carries the uninitialized .hyimg section (see host/image.c).
target_link_options(hydro PRIVATE /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /SAFESEH:NO
    /SECTION:.hyimg,ERW)
# Incremental linking pads every function and keeps unreferenced code (most of libchdr's deps),
# which pushes .text past 0x100000 in RelWithDebInfo/Debug.
target_link_options(hydro PRIVATE /INCREMENTAL:NO /OPT:REF)

# dgVoodoo 2.87.5 runtime: glide2x.dll (the Glide wrapper) and dgVoodooAPI.dll (writes dgVoodoo.conf for the
# operator menu's graphics settings, host/dgv.cpp). They must be the same release: older wrappers ignore the
# whole conf the 2.87 API writes. The vendor/dgVoodoo2 submodule supplies the API headers, but its DLLs are an
# older build, so the release package is downloaded instead (dgVoodoo's DLLs may be shipped with applications).
option(HYDRO_FETCH_DGVOODOO "Download the dgVoodoo 2.87.5 DLLs and put them next to hydro.exe" ON)
if(HYDRO_FETCH_DGVOODOO)
    set(DGV_URL https://github.com/dege-diosg/dgVoodoo2/releases/download/v2.87.5)
    set(DGV_DIR ${CMAKE_BINARY_DIR}/_deps/dgvoodoo_2_87_5)
    function(dgv_fetch zip sha256 probe)
        if(NOT EXISTS ${DGV_DIR}/${probe})
            file(DOWNLOAD ${DGV_URL}/${zip} ${CMAKE_BINARY_DIR}/_deps/${zip}
                EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON)
            file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/_deps/${zip} DESTINATION ${DGV_DIR})
        endif()
    endfunction()
    dgv_fetch(dgVoodoo2_87_5.zip 5ffde6927f7355ca3fdd5d785b581256a8e6539fa13e395a891ade6ba1040850 3Dfx/x86/Glide2x.dll)
    dgv_fetch(dgVoodooAPI_287_5.zip 12709f30209248c50e6fe37b838d16c5b55fea1363c0bcad21fa82015ae16b57
        Bin/x86/Release/dgVoodooAPI.dll)
    set(DGV_GLIDE ${DGV_DIR}/3Dfx/x86/Glide2x.dll)
    set(DGV_API ${DGV_DIR}/Bin/x86/Release/dgVoodooAPI.dll)
    add_custom_command(TARGET hydro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DGV_API} $<TARGET_FILE_DIR:hydro>/dgVoodooAPI.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DGV_GLIDE} $<TARGET_FILE_DIR:hydro>/glide2x.dll)
    install(FILES ${DGV_API} DESTINATION .)
    install(FILES ${DGV_GLIDE} DESTINATION . RENAME glide2x.dll)
endif()

install(TARGETS hydro RUNTIME DESTINATION .)

# LANCommander match statistics: races are reported to LANCommander through the LANCommander Interposer, embedded
# with only its MATCHES feature (every feature of the embeddable library is opt-in; the port needs no file, registry,
# network or input hooks). It brings its config (.interposer\Config.yml next to hydro.exe), its session log and the
# launcher's event channel, which is how races reach LANCommander when its launcher started the game.
#
# It's embedded in hydro_lancommander.dll, not in hydro.exe: the linker puts every code section before .hyimg, and
# the Interposer's ~400 KB of code would push .hyimg past 0x100000. hydro.exe delay-loads the DLL (host/lc_stats.c),
# so the game runs on without it. The DLL is built for XP the way the Interposer's own DLL is, with YY-Thunks.
option(HYDRO_LANCOMMANDER "Report races to LANCommander (hydro_lancommander.dll, the LANCommander Interposer)" ON)
if(HYDRO_LANCOMMANDER)
    set(LC_DIR ${REPO_ROOT}/vendor/LANCommander.Interposer)
    if(NOT EXISTS ${LC_DIR}/CMakeLists.txt)
        message(FATAL_ERROR "${LC_DIR} is empty. Run: git submodule update --init --recursive")
    endif()

    # The Interposer finds yaml-cpp and MinHook with find_package (vcpkg, in its own builds). Here they're submodules
    # built in-tree, and OVERRIDE_FIND_PACKAGE makes those find_package calls resolve to them.
    include(FetchContent)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(yaml-cpp SOURCE_DIR ${REPO_ROOT}/vendor/yaml-cpp OVERRIDE_FIND_PACKAGE EXCLUDE_FROM_ALL)
    FetchContent_Declare(minhook SOURCE_DIR ${REPO_ROOT}/vendor/minhook OVERRIDE_FIND_PACKAGE EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(yaml-cpp minhook)
    add_library(minhook::minhook ALIAS minhook)

    # YY-Thunks: what lets the v143 CRT and STL in the DLL load on XP (the Interposer's CMake also requires it to
    # configure). The Interposer's script fetches the release it pins, checked against its SHA-256, into the
    # submodule's build\YY-Thunks\ (which that repository ignores); the version is read from it as its CMake does.
    file(READ ${LC_DIR}/build/Get-YYThunks.ps1 LC_YY_SCRIPT)
    if(NOT LC_YY_SCRIPT MATCHES "\\$Version = '([0-9.]+)'")
        message(FATAL_ERROR "Could not read the YY-Thunks version from ${LC_DIR}/build/Get-YYThunks.ps1")
    endif()
    set(LC_YYTHUNKS_OBJ ${LC_DIR}/build/YY-Thunks/${CMAKE_MATCH_1}/objs/x86/YY_Thunks_for_WinXP.obj)
    if(NOT EXISTS ${LC_YYTHUNKS_OBJ})
        execute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${LC_DIR}/build/Get-YYThunks.ps1
            RESULT_VARIABLE LC_YY_RESULT)
        if(NOT LC_YY_RESULT EQUAL 0 OR NOT EXISTS ${LC_YYTHUNKS_OBJ})
            message(FATAL_ERROR "Fetching YY-Thunks for the LANCommander Interposer failed")
        endif()
    endif()

    # Only the embeddable library (no Interposer DLL, injector, tests or plugins), carrying only match statistics.
    set(INTERPOSER_EMBEDDED_ONLY ON CACHE BOOL "" FORCE)
    set(INTERPOSER_FEATURE_MATCHES ON CACHE BOOL "" FORCE)
    add_subdirectory(${LC_DIR} ${CMAKE_CURRENT_BINARY_DIR}/interposer EXCLUDE_FROM_ALL)

    # The DLL exports every Interposer* function (the whole archive is linked) plus HydroLancommanderAttach.
    add_library(hydro_lancommander SHARED lancommander/hydro_lancommander.c ${LC_YYTHUNKS_OBJ})
    target_link_libraries(hydro_lancommander PRIVATE LANCommander::Interposer::Embedded)
    # As build/Compatibility.cmake (interposer_ship_target) sets up the Interposer's DLL for XP: Windows 5.01 in the
    # PE header, and YY-Thunks' entry point, which chains to the CRT's and gives thread_local working TLS on XP.
    target_link_options(hydro_lancommander PRIVATE /SUBSYSTEM:WINDOWS,5.01 /ENTRY:DllMainCRTStartupForYY_Thunks
        /alternatename:_YY_ThunksOriginalDllMainCRTStartup@12=__DllMainCRTStartup@12)
    install(TARGETS hydro_lancommander RUNTIME DESTINATION .)

    # hydro.exe calls the Interposer API through the DLL's import library, delay-loaded so that a missing DLL only
    # turns reporting off. Without INTERPOSER_STATIC, the header declares the functions dllimport, as for the
    # Interposer's own DLL.
    target_sources(hydro PRIVATE host/lc_stats.c)
    target_compile_definitions(hydro PRIVATE HYDRO_LANCOMMANDER)
    target_include_directories(hydro PRIVATE ${LC_DIR}/LANCommander.Interposer/include)
    target_link_libraries(hydro PRIVATE hydro_lancommander delayimp)
    target_link_options(hydro PRIVATE /DELAYLOAD:hydro_lancommander.dll)
endif()
