# Conan 2 host profile for UWP / Xbox Dev Mode (x64). Normally generated automatically by
# cmake/conan_provider.cmake from CMAKE_SYSTEM_NAME=WindowsStore; kept here for manual use:
#   conan install . -pr:h tools/xbox/conan/uwp-x64.profile -pr:b default --build=missing
[settings]
os=WindowsStore
os.version=10.0
arch=x86_64
compiler=msvc
compiler.version=194
compiler.runtime=dynamic
compiler.cppstd=17
build_type=Release

[conf]
tools.cmake.cmaketoolchain:generator=Visual Studio 17 2022
tools.microsoft.msbuild:vs_version=17
