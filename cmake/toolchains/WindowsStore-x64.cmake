# Toolchain: UWP (Windows Store / Xbox Dev Mode), x64, MSVC.
# Usage (generator MUST be Visual Studio, platform MUST be x64):
#   cmake -S . -B build-uwp -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/WindowsStore-x64.cmake
#
# CMake reacts to CMAKE_SYSTEM_NAME=WindowsStore by generating AppContainer projects:
#   - WINAPI_FAMILY=WINAPI_FAMILY_APP, /APPCONTAINER, WindowsApp.lib umbrella library
#   - variable WINDOWS_STORE=TRUE (used by OGRE 1.11's CMake: D3D11 CoreWindow, no Cg/GL/D3D9)
set(CMAKE_SYSTEM_NAME      WindowsStore)
set(CMAKE_SYSTEM_VERSION   10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Xbox One (Dev Mode) runs Windows 10 based OS builds: keep the min version conservative.
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION "10.0.17763.0" CACHE STRING "" FORCE)

# UWP mandates the dynamic CRT (shipped as the Microsoft.VCLibs.140.00 framework package).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)

# Release-only (RelWithDebInfo in RoR uses /DYNAMICBASE:NO, which UWP rejects).
set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "" FORCE)
# try_compile() builds "Debug" by default, which then does not exist in the generated projects.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# Never let MSBuild try to produce/sign an .appx for helper executables (try_compile checks, examples,
# tools of third-party libraries): it needs a certificate and fails the build. RoR is packaged by
# tools/xbox/package-msix.ps1 instead.
set(CMAKE_VS_GLOBALS "AppxPackage=false;GenerateAppxPackageOnBuild=false;AppxPackageSigningEnabled=false" CACHE STRING "" FORCE)

# AppContainer projects default to SDL checks (/sdl), which turn C4996 ("POSIX name deprecated",
# "strcpy unsafe") into hard errors in third-party C code (zziplib, freetype, ...). Appended so a
# -DCMAKE_<LANG>_FLAGS_INIT given on the command line is kept.
string(APPEND CMAKE_C_FLAGS_INIT   " /sdl- /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS")
string(APPEND CMAKE_CXX_FLAGS_INIT " /sdl- /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS")
