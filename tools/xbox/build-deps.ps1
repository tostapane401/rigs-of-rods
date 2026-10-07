<#
.SYNOPSIS
    Builds the Rigs of Rods dependency stack for UWP / Xbox Dev Mode (x64, Release) into a prefix.

.DESCRIPTION
    Everything is compiled with CMAKE_SYSTEM_NAME=WindowsStore (cmake/toolchains/WindowsStore-x64.cmake)
    and the dynamic CRT, so it can be loaded inside the UWP AppContainer:

      zlib 1.3.1 -> zziplib 0.13.72 -> freetype 2.13.2 -> OGRE 1.11.6 (D3D11 only, STBI codec, no Cg)
      -> MyGUI 3.4.0 (Ogre platform, static, no plugins, Win32 clipboard compiled out)
      -> openal-soft 1.24.3 (has native UWP support)            [optional]
      -> Caelum / PagedGeometry (RigsOfRods forks)                [optional]

    fmt, rapidjson and angelscript are NOT built here: they come from Conan (conanfile.py,
    WindowsStore branch) through cmake/conan_provider.cmake when RoR itself is configured.

.PARAMETER Prefix
    Install prefix (becomes ROR_DEPENDENCY_DIR for the RoR configure step).

.PARAMETER Work
    Scratch folder for sources and build trees.
#>
param(
    [Parameter(Mandatory = $true)][string] $Prefix,
    [Parameter(Mandatory = $true)][string] $Work,
    [string] $Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"
$RoRRoot   = (Resolve-Path "$PSScriptRoot\..\..").Path
$Toolchain = Join-Path $RoRRoot "cmake\toolchains\WindowsStore-x64.cmake"
New-Item -ItemType Directory -Force -Path $Prefix, $Work | Out-Null
$Prefix = (Resolve-Path $Prefix).Path
$Work   = (Resolve-Path $Work).Path
$PrefixFwd = $Prefix -replace '\\', '/'

. (Join-Path $PSScriptRoot "ci-common.ps1")   # Invoke-Logged: logs + GitHub error annotations

function Get-Source([string] $name, [string] $url, [string] $tag = "") {
    $dir = Join-Path $Work "src\$name"
    if (-not (Test-Path $dir)) {
        if ($tag) { Invoke-Logged "git clone $name" { git -c credential.interactive=never clone --quiet --depth 1 --branch $tag $url $dir } -TimeoutMinutes 15 }
        else      { Invoke-Logged "git clone $name" { git -c credential.interactive=never clone --quiet --depth 1 $url $dir } -TimeoutMinutes 15 }
    }
    return $dir
}

function Build-CMake([string] $name, [string] $src, [string[]] $extra) {
    Write-Host "::group::$name"
    $bld = Join-Path $Work "build\$name"
    $cfg = @(
        "-S", $src, "-B", $bld,
        "-G", $Generator, "-A", "x64",
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
        # Also on the command line: some projects (openal-soft) test CMAKE_SYSTEM_NAME before
        # project(), i.e. before the toolchain file is read.
        "-DCMAKE_SYSTEM_NAME=WindowsStore", "-DCMAKE_SYSTEM_VERSION=10.0",
        "-DCMAKE_INSTALL_PREFIX=$PrefixFwd",
        "-DCMAKE_PREFIX_PATH=$PrefixFwd",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"   # CMake 4.x vs. old cmake_minimum_required()
    ) + $extra
    Invoke-Logged "configure $name" { cmake @cfg } -TimeoutMinutes 20
    Invoke-Logged "build $name" { cmake --build $bld --config Release --parallel --target INSTALL -- /nr:false /v:minimal /nologo } -TimeoutMinutes 60
    Write-Host "::endgroup::"
}

# -------------------------------------------------------------------------------------------------
# 1) zlib
$zlib = Get-Source "zlib" "https://github.com/madler/zlib.git" "v1.3.1"
Build-CMake "zlib" $zlib @("-DZLIB_BUILD_EXAMPLES=OFF")

# 2) zziplib (static, /MD)
$zz = Get-Source "zziplib" "https://github.com/gdraheim/zziplib.git" "v0.13.72"
Build-CMake "zziplib" $zz @(
    "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON", "-DMSVC_STATIC_RUNTIME=OFF",
    "-DZZIPMMAPPED=OFF", "-DZZIPFSEEKO=OFF", "-DZZIPWRAP=OFF", "-DZZIPSDL=OFF",
    "-DZZIPBINS=OFF", "-DZZIPTEST=OFF", "-DZZIPDOCS=OFF", "-DZZIP_COMPAT=OFF",
    "-DZZIP_LIBTOOL=OFF", "-DZZIP_PKGCONFIG=OFF", "-DBUILD_TESTS=OFF")
$zzipLib = Get-ChildItem -Path "$Prefix\lib" -Filter "zzip*.lib" | Where-Object { $_.Name -notmatch "fseeko|mmapped" } | Select-Object -First 1
if (-not $zzipLib) { throw "zziplib: no zzip*.lib installed in $Prefix\lib" }

# 3) freetype (static, no optional codecs)
$ft = Get-Source "freetype" "https://github.com/freetype/freetype.git" "VER-2-13-2"
Build-CMake "freetype" $ft @(
    "-DBUILD_SHARED_LIBS=OFF", "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_PNG=ON",
    "-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_BROTLI=ON", "-DFT_DISABLE_ZLIB=ON")

# 4) OGRE 1.11.6 - D3D11 only. WINDOWS_STORE makes OGRE build D3D11RenderWindowCoreWindow (C++/CX, /ZW
#    is set by OGRE's own CMake via VS_WINRT_COMPONENT) and disables GL/D3D9/Cg automatically.
$ogre = Get-Source "ogre" "https://github.com/OGRECave/ogre.git" "v1.11.6"
# CoreWindow swap chain: OGRE passes Width/Height = 0 ("automatic sizing", only defined for HWND swap
# chains). On Xbox that yields a tiny back buffer that the compositor shows unscaled in the middle
# of the TV (the whole game appeared as a ~16 px square). Use the real window size, like the
# SwapChainPanel path of the same file already does.
$d3dwin = Join-Path $ogre "RenderSystems\Direct3D11\src\OgreD3D11RenderWindow.cpp"
$c = Get-Content -Raw $d3dwin
if ($c -notmatch 'ROR_UWP_SWAPCHAIN_SIZE') {
    $c = $c.Replace('mSwapChainDesc.Width                = 0;', 'mSwapChainDesc.Width                = mWidth; /* ROR_UWP_SWAPCHAIN_SIZE */')
    $c = $c.Replace('mSwapChainDesc.Height               = 0;', 'mSwapChainDesc.Height               = mHeight;')
    $c = $c.Replace('_resizeSwapChainBuffers(0, 0);', '_resizeSwapChainBuffers(mWidth, mHeight);')
    if ($c -notmatch 'ROR_UWP_SWAPCHAIN_SIZE' -or $c -match '_resizeSwapChainBuffers\(0, 0\)') { throw "OGRE CoreWindow swap-chain patch did not apply" }
    Set-Content -NoNewline -Path $d3dwin -Value $c
}
Build-CMake "ogre" $ogre @(
    "-DOGRE_BUILD_DEPENDENCIES=OFF", "-DOGRE_DEPENDENCIES_DIR=$PrefixFwd",
    "-DZZip_INCLUDE_DIR=$PrefixFwd/include", "-DZZip_LIBRARY_REL=$($zzipLib.FullName -replace '\\','/')",
    "-DOGRE_STATIC=OFF", "-DOGRE_RESOURCEMANAGER_STRICT=0", "-DOGRE_CONFIG_ENABLE_ZIP=ON",
    "-DOGRE_BUILD_RENDERSYSTEM_D3D11=ON", "-DOGRE_BUILD_RENDERSYSTEM_D3D9=OFF",
    "-DOGRE_BUILD_RENDERSYSTEM_GL=OFF", "-DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=OFF", "-DOGRE_BUILD_RENDERSYSTEM_GLES2=OFF",
    "-DOGRE_BUILD_PLUGIN_CG=OFF", "-DOGRE_BUILD_PLUGIN_FREEIMAGE=OFF", "-DOGRE_BUILD_PLUGIN_EXRCODEC=OFF",
    "-DOGRE_BUILD_PLUGIN_STBI=ON", "-DOGRE_BUILD_PLUGIN_BSP=OFF", "-DOGRE_BUILD_PLUGIN_PCZ=OFF",
    "-DOGRE_BUILD_PLUGIN_OCTREE=ON", "-DOGRE_BUILD_PLUGIN_PFX=ON",
    "-DOGRE_BUILD_COMPONENT_RTSHADERSYSTEM=ON", "-DOGRE_BUILD_RTSHADERSYSTEM_CORE_SHADERS=ON",
    "-DOGRE_BUILD_RTSHADERSYSTEM_EXT_SHADERS=ON",
    "-DOGRE_BUILD_COMPONENT_PYTHON=OFF", "-DOGRE_BUILD_COMPONENT_JAVA=OFF", "-DOGRE_BUILD_COMPONENT_CSHARP=OFF",
    "-DOGRE_BUILD_COMPONENT_VOLUME=OFF", "-DOGRE_BUILD_COMPONENT_PROPERTY=OFF", "-DOGRE_BUILD_COMPONENT_HLMS=OFF",
    "-DOGRE_BUILD_SAMPLES=OFF", "-DOGRE_BUILD_TOOLS=OFF", "-DOGRE_BUILD_TESTS=OFF",
    "-DOGRE_INSTALL_SAMPLES=OFF", "-DOGRE_INSTALL_DOCS=OFF", "-DOGRE_INSTALL_PDB=OFF",
    # WiX is preinstalled on the runner: OGRE would then add a demo_installer target depending on SampleBrowser.
    "-DCMAKE_DISABLE_FIND_PACKAGE_Wix=TRUE", "-DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=TRUE", "-DCMAKE_DISABLE_FIND_PACKAGE_SDL2=TRUE")

$ogreCMakeDir = @("$Prefix\CMake", "$Prefix\lib\OGRE\cmake", "$Prefix\share\OGRE\cmake") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ogreCMakeDir) { throw "OGRE: OGREConfig.cmake not found under $Prefix" }
$ogreCMakeDir = $ogreCMakeDir -replace '\\', '/'

# 5) MyGUI 3.4.0 - compile the Win32 clipboard integration out (OpenClipboard/GetModuleFileName
#    are not part of the UWP API set).
$mygui = Get-Source "mygui" "https://github.com/MyGUI/mygui.git" "MyGUI3.4.0"
foreach ($f in @("MyGUIEngine\include\MyGUI_ClipboardManager.h", "MyGUIEngine\src\MyGUI_ClipboardManager.cpp", "MyGUIEngine\src\MyGUI_WindowsClipboardHandler.cpp")) {
    $p = Join-Path $mygui $f
    $c = Get-Content -Raw $p
    $c = $c -replace '#if MYGUI_PLATFORM == MYGUI_PLATFORM_WIN32', '#if MYGUI_PLATFORM == MYGUI_PLATFORM_WIN32 && !defined(MYGUI_UWP)'
    Set-Content -NoNewline -Path $p -Value $c
}
# timeGetTime() (winmm) is not in the UWP API set -> GetTickCount64().
$timer = Join-Path $mygui "MyGUIEngine\src\MyGUI_Timer.cpp"
$c = Get-Content -Raw $timer
if ($c -notmatch 'MYGUI_UWP') {
    $c = $c.Replace('return timeGetTime();', "#if defined(MYGUI_UWP)`n`t`treturn (unsigned long)GetTickCount64();`n#else`n`t`treturn timeGetTime();`n#endif")
    Set-Content -NoNewline -Path $timer -Value $c
}
Build-CMake "mygui" $mygui @(
    "-DMYGUI_RENDERSYSTEM=3", "-DMYGUI_STATIC=ON", "-DMYGUI_DISABLE_PLUGINS=ON", "-DMYGUI_USE_FREETYPE=ON",
    "-DMYGUI_BUILD_DEMOS=OFF", "-DMYGUI_BUILD_TOOLS=OFF", "-DMYGUI_BUILD_PLUGINS=OFF", "-DMYGUI_BUILD_UNITTESTS=OFF",
    "-DMYGUI_BUILD_TEST_APP=OFF", "-DMYGUI_INSTALL_MEDIA=OFF",
    "-DOGRE_DIR=$ogreCMakeDir", "-DCMAKE_CXX_FLAGS_INIT=/DMYGUI_UWP")

# 6) Networking - required: current RoR sources do not compile without USE_SOCKETW / USE_CURL.
#    SocketW: plain Winsock (allowed in UWP with the internetClient capability), no OpenSSL.
$sw = Get-Source "socketw" "https://github.com/RigsOfRods/socketw.git"
Build-CMake "socketw" $sw @(
    "-DBUILD_SHARED_LIBS=OFF", "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE",
    "-DCMAKE_CXX_FLAGS_INIT=/D_WINSOCK_DEPRECATED_NO_WARNINGS")

#    libcurl: plain HTTP for now. Schannel needs SSPI (InitSecurityInterface & co.), which is not
#    part of the UWP API set; a TLS backend that works in the AppContainer (e.g. mbedTLS) is a
#    follow-up. Without TLS RoR compiles and runs; https downloads (repository browser) fail.
$curl = Get-Source "curl" "https://github.com/curl/curl.git" "curl-8_10_1"
Build-CMake "curl" $curl @(
    "-DBUILD_SHARED_LIBS=ON", "-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF", "-DBUILD_LIBCURL_DOCS=OFF",
    "-DBUILD_MISC_DOCS=OFF", "-DENABLE_CURL_MANUAL=OFF", "-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF",
    "-DUSE_NGHTTP2=OFF", "-DUSE_LIBIDN2=OFF", "-DCURL_BROTLI=OFF", "-DCURL_ZSTD=OFF", "-DCURL_DISABLE_LDAP=ON",
    "-DENABLE_UNICODE=OFF", "-DCURL_ZLIB=ON", "-DCURL_USE_OPENSSL=OFF", "-DCURL_USE_SCHANNEL=OFF",
    "-DCURL_WINDOWS_SSPI=OFF", "-DCURL_ENABLE_SSL=OFF", "-DENABLE_IPV6=ON",
    "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE")

# 7) Optional components: failures are reported but do not stop the pipeline. RoR's CMake turns the
#    matching ROR_USE_* option OFF when the package is missing (cmake_dependent_option).
function Build-Optional([string] $name, [scriptblock] $body) {
    try { & $body }
    catch {
        Write-Host "::warning title=$name::Optional dependency $name failed to build for UWP: $($_.Exception.Message)"
        Write-Host "::endgroup::"
    }
}

Build-Optional "openal-soft" {
    $oal = Get-Source "openal-soft" "https://github.com/kcat/openal-soft.git" "1.24.3"
    Build-CMake "openal-soft" $oal @(
        "-DALSOFT_UTILS=OFF", "-DALSOFT_EXAMPLES=OFF", "-DALSOFT_TESTS=OFF",
        "-DALSOFT_INSTALL_EXAMPLES=OFF", "-DALSOFT_INSTALL_UTILS=OFF")
}
Build-Optional "caelum" {
    $cae = Get-Source "caelum" "https://github.com/RigsOfRods/ogre-caelum.git"
    Build-CMake "caelum" $cae @("-DCaelum_BUILD_SAMPLES=OFF", "-DINSTALL_OGRE_PLUGIN=OFF", "-DOGRE_DIR=$ogreCMakeDir")
}
Build-Optional "pagedgeometry" {
    $pg = Get-Source "pagedgeometry" "https://github.com/RigsOfRods/ogre-pagedgeometry.git"
    Build-CMake "pagedgeometry" $pg @("-DOGRE_DIR=$ogreCMakeDir")
}

Write-Host "UWP dependency prefix ready: $Prefix"
Get-ChildItem "$Prefix\bin" -Filter *.dll -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  bin\$($_.Name)" }
