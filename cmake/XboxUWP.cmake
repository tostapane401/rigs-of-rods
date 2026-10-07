# ==================================================================================================
#  Rigs of Rods - UWP / Xbox (Dev Mode) build integration
#
#  Activated automatically when configuring with CMAKE_SYSTEM_NAME=WindowsStore, e.g.
#    cmake -S . -B build-uwp -G "Visual Studio 17 2022" -A x64 ^
#          -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/WindowsStore-x64.cmake ^
#          -DROR_DEPENDENCY_DIR=C:/ror-uwp-deps ^
#          -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/conan_provider.cmake
#
#  Why a VS generator: CMake only emits AppContainer projects (WINAPI_FAMILY_APP, /APPCONTAINER,
#  WindowsApp.lib umbrella) for WindowsStore with the Visual Studio generators.
# ==================================================================================================

if (NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    return()
endif ()

if (NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    message(FATAL_ERROR "UWP/Xbox build requires a Visual Studio generator (got '${CMAKE_GENERATOR}').")
endif ()
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Xbox only runs x64 UWP packages: configure with -A x64.")
endif ()

set(ROR_PLATFORM_UWP ON)

# The runner/desktop OpenSSL is a Win32 build and must never leak into the UWP link.
set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL TRUE)
message(STATUS "RoR: UWP / Xbox Dev Mode build (SDK ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION})")

# UWP requires the dynamic CRT (VCLibs framework package).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# --------------------------------------------------------------------------------------------------
#  OIS "core": only the portable sources (types, Keyboard/JoyStick/ForceFeedback base classes).
#  The DirectInput backend does not exist in the UWP API set; RoR provides native devices in
#  source/main/utils/XboxInput.cpp instead.
# --------------------------------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(ois_src
    GIT_REPOSITORY https://github.com/wgois/OIS.git
    GIT_TAG        v1.4
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(ois_src)
if (NOT ois_src_POPULATED)
    FetchContent_Populate(ois_src) # sources only, OIS' own CMakeLists is NOT used
endif ()

add_library(ois_core STATIC
    ${ois_src_SOURCE_DIR}/src/OISEffect.cpp
    ${ois_src_SOURCE_DIR}/src/OISException.cpp
    ${ois_src_SOURCE_DIR}/src/OISForceFeedback.cpp
    ${ois_src_SOURCE_DIR}/src/OISJoyStick.cpp
    ${ois_src_SOURCE_DIR}/src/OISKeyboard.cpp
    ${ois_src_SOURCE_DIR}/src/OISObject.cpp)
target_include_directories(ois_core PUBLIC ${ois_src_SOURCE_DIR}/includes)
target_compile_definitions(ois_core PUBLIC OIS_STATIC_LIB)
set_target_properties(ois_core PROPERTIES FOLDER "Dependencies")
add_library(ois::ois ALIAS ois_core)
set(OIS_FOUND TRUE)

# --------------------------------------------------------------------------------------------------
#  Per-target settings for the game executable. Called from source/main/CMakeLists.txt.
# --------------------------------------------------------------------------------------------------
function(ror_configure_uwp_target target)
    target_compile_definitions(${target} PRIVATE
        ROR_PLATFORM_UWP
        WINAPI_FAMILY=WINAPI_FAMILY_APP
        _WIN32_WINNT=0x0A00
        _HAS_AUTO_PTR_ETC=1                 # legacy std:: helpers still used by some deps
        _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
        IMGUI_DISABLE_WIN32_FUNCTIONS          # Dear ImGui: no Win32 clipboard / IMM in the AppContainer
        _WINSOCK_DEPRECATED_NO_WARNINGS)

    # C++/WinRT needs C++17; RoR itself builds as C++14 on desktop. Only the WinRT translation
    # units are compiled as C++17 and they skip the (C++14) precompiled header.
    set(_winrt_sources
        platform/uwp/UwpApp.cpp
        utils/XboxInput.cpp
        utils/PlatformStorage.cpp
        utils/PlatformUtils.cpp)
    set_source_files_properties(${_winrt_sources} PROPERTIES
        COMPILE_OPTIONS "/std:c++17;/bigobj"
        SKIP_PRECOMPILE_HEADERS ON)

    # Prefix built by tools/xbox/build-deps.ps1: CMake's FindOpenAL returns <prefix>/include/AL,
    # while RoR includes <AL/al.h>.
    target_include_directories(${target} PRIVATE "${ROR_DEPENDENCY_DIR}/include")

    # MyGUI is built static for UWP: its FreeType dependency must be linked by the executable.
    find_package(Freetype REQUIRED)
    target_link_libraries(${target} PRIVATE Freetype::Freetype)

    # Umbrella import library for all APIs allowed in the AppContainer (+ D3D for Trim on suspend).
    target_link_libraries(${target} PRIVATE WindowsApp.lib d3d11.lib dxgi.lib)

    set_target_properties(${target} PROPERTIES
        VS_GLOBAL_AppContainerApplication "true"
        VS_GLOBAL_ApplicationType "Windows Store"
        VS_GLOBAL_ApplicationTypeRevision "10.0"
        VS_GLOBAL_WindowsTargetPlatformMinVersion "10.0.17763.0"
        # We package with makeappx in tools/xbox/package-msix.ps1 (full control over the layout,
        # resources and DLLs) instead of MSBuild's AppxPackage targets.
        VS_GLOBAL_AppxPackage "false"
        VS_GLOBAL_GenerateAppxPackageOnBuild "false"
        WIN32_EXECUTABLE YES)
endfunction()
