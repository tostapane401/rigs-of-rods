# Extra Conan host profile for UWP / Xbox, appended by the CI configure step:
#   -DCONAN_HOST_PROFILE="default;auto-cmake;<abs path>/tools/xbox/conan/uwp-extra.profile"
# Packages built from source by Conan (angelscript, fmt) must get the same WindowsStore fixes as
# RoR itself: Release-only try_compile, no appx packaging of helper executables, no /sdl.
[conf]
tools.cmake.cmaketoolchain:user_toolchain=["{{ os.path.join(profile_dir, '..', '..', '..', 'cmake', 'toolchains', 'WindowsStore-x64.cmake').replace('\\', '/') }}"]
