<#
.SYNOPSIS
    Assembles, signs and bundles the Rigs of Rods UWP package for Xbox Dev Mode.

.DESCRIPTION
    Layout (package root == InstalledLocation, mounted READ-ONLY at runtime):
        RoR.exe, *.dll (RoR + OGRE + plugins + deps), plugins.cfg
        resources\*.zip, languages\, content\
        Assets\*.png, AppxManifest.xml, resources.pri
    Output folder:
        RigsOfRods_<ver>_x64.msix          signed package
        RigsOfRods_<ver>.msixbundle        signed bundle (same content, one architecture)
        Microsoft.VCLibs.x64.14.00.appx    dependency to upload together with the package
        RigsOfRods-dev.cer                 public certificate (only needed for PC sideloading)

.PARAMETER Pfx / PfxPassword
    Optional code-signing certificate. Without it a self-signed one is generated (fine for Dev Mode).
    Use a stable certificate (GitHub secret) if you want updates to install over the previous build.
#>
param(
    [Parameter(Mandatory = $true)][string] $BinDir,      # <build>\bin of the RoR UWP build
    [Parameter(Mandatory = $true)][string] $DepsPrefix,  # prefix from build-deps.ps1
    [Parameter(Mandatory = $true)][string] $OutDir,
    [Parameter(Mandatory = $true)][string] $Version,     # a.b.c.d, each <= 65535
    [string] $Publisher = "CN=RigsOfRodsDev",
    [string] $IdentityName = "RigsOfRods.XboxDevMode",
    [string] $Pfx = "",
    [string] $PfxPassword = ""
)

$ErrorActionPreference = "Stop"
$RoRRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$Layout = Join-Path $OutDir "layout"
if (Test-Path $Layout) { Remove-Item -Recurse -Force $Layout }
New-Item -ItemType Directory -Force -Path $Layout | Out-Null

. (Join-Path $PSScriptRoot "ci-common.ps1")   # Invoke-Logged: non-interactive, timeout, annotations

# --- Windows SDK tools -------------------------------------------------------------------------
$sdkBin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory |
    Where-Object { $_.Name -match '^10\.' -and (Test-Path (Join-Path $_.FullName "x64\makeappx.exe")) } |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $sdkBin) { throw "Windows SDK makeappx.exe not found" }
$makeappx = Join-Path $sdkBin.FullName "x64\makeappx.exe"
$makepri  = Join-Path $sdkBin.FullName "x64\makepri.exe"
$signtool = Join-Path $sdkBin.FullName "x64\signtool.exe"
Write-Host "Using Windows SDK tools from $($sdkBin.FullName)"

# --- Payload ------------------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $BinDir "RoR.exe"))) { throw "RoR.exe not found in $BinDir" }
Copy-Item (Join-Path $BinDir "RoR.exe") $Layout
Copy-Item (Join-Path $BinDir "plugins.cfg") $Layout
Get-ChildItem $BinDir -Filter *.dll | Copy-Item -Destination $Layout
Get-ChildItem (Join-Path $DepsPrefix "bin") -Filter *.dll -ErrorAction SilentlyContinue | Copy-Item -Destination $Layout -Force
# OGRE sometimes installs plugins into lib\OGRE on non-standard layouts.
Get-ChildItem (Join-Path $DepsPrefix "lib\OGRE") -Filter *.dll -ErrorAction SilentlyContinue | Copy-Item -Destination $Layout -Force
foreach ($d in @("resources", "languages", "content")) {
    $src = Join-Path $BinDir $d
    if (Test-Path $src) { Copy-Item -Recurse $src (Join-Path $Layout $d) }
}
if (-not (Test-Path (Join-Path $Layout "resources\skeleton.zip"))) { throw "resources\skeleton.zip missing - did the RoR build run its resource zipping step?" }

foreach ($dll in @("OgreMain.dll", "RenderSystem_Direct3D11.dll", "Codec_STBI.dll", "Plugin_ParticleFX.dll", "Plugin_OctreeSceneManager.dll")) {
    if (-not (Test-Path (Join-Path $Layout $dll))) { throw "Required module $dll is missing from the package layout" }
}

Copy-Item -Recurse (Join-Path $RoRRoot "tools\xbox\package\Assets") (Join-Path $Layout "Assets")

# --- D3D shader compiler (redistributable) -------------------------------------------------------
# OGRE's D3D11 render system and the RTSS compile HLSL at runtime with D3DCompile. Ship the SDK's
# redistributable copy instead of relying on the console OS image.
$d3dc = @("${env:ProgramFiles(x86)}\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll") +
        (Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\d3dcompiler_47.dll" -ErrorAction SilentlyContinue | Sort-Object FullName | ForEach-Object FullName) |
        Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if ($d3dc) { Copy-Item $d3dc $Layout -Force; Write-Host "Packaged $d3dc" }
else { Write-Host "::warning title=d3dcompiler::d3dcompiler_47.dll redistributable not found in the Windows SDK" }

# --- Import check: every DLL a module imports must be in the package, in VCLibs or in the OS ------
$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
if ($dumpbin) {
    $inPackage = @{}
    Get-ChildItem $Layout -Filter *.dll | ForEach-Object { $inPackage[$_.Name.ToLower()] = $true }
    # Provided by the Microsoft.VCLibs.140.00 framework package or by the OS for UWP apps.
    $provided = @("vcruntime140_app.dll", "vcruntime140_1_app.dll", "msvcp140_app.dll", "msvcp140_1_app.dll",
                  "msvcp140_2_app.dll", "vccorlib140_app.dll", "concrt140_app.dll", "ucrtbase.dll",
                  "kernel32.dll", "d3d11.dll", "dxgi.dll", "ws2_32.dll", "bcrypt.dll", "xinput1_4.dll",
                  "mmdevapi.dll", "ole32.dll", "oleaut32.dll", "combase.dll", "ntdll.dll", "d3dcompiler_47.dll")
    $report = @()
    $missing = @()
    foreach ($m in (Get-ChildItem -Path "$Layout\*" -Include *.exe, *.dll)) {
        $deps = & $dumpbin /nologo /dependents $m.FullName | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim().ToLower() }
        $hdr = & $dumpbin /nologo /headers $m.FullName | Select-String -Pattern 'App Container' -SimpleMatch
        $other = $deps | Where-Object { $_ -notmatch '^(api|ext)-ms-' }
        $report += ("{0}: AppContainer={1}; imports: {2}" -f $m.Name, [bool]$hdr, ($other -join ", "))
        foreach ($d in $other) {
            if (-not $inPackage.ContainsKey($d) -and $provided -notcontains $d) { $missing += "$($m.Name) -> $d" }
        }
        if (-not $hdr -and $m.Extension -eq ".exe") { $missing += "$($m.Name) is NOT linked with /APPCONTAINER" }
    }
    $report | Set-Content (Join-Path $OutDir "package-imports.txt")
    $summary = ($report -join "`n").Replace('%', '%25').Replace("`n", '%0A')
    Write-Host "::notice title=Package imports::$summary"
    if ($missing.Count) {
        $msg = ("Missing at runtime:`n" + ($missing -join "`n")).Replace("`n", '%0A')
        Write-Host "::warning title=Package dependency check::$msg"
    }
} else {
    Write-Host "::warning title=dumpbin::dumpbin.exe not on PATH, import check skipped"
}

# --- VCLibs framework dependency ------------------------------------------------------------------
$vclibs = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64" -Filter "Microsoft.VCLibs.x64.14.00.appx" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $vclibs) { throw "Microsoft.VCLibs.x64.14.00.appx not found (install the 'Universal Windows Platform development' VS workload)" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($vclibs.FullName)
try {
    $entry = $zip.Entries | Where-Object { $_.FullName -eq "AppxManifest.xml" }
    $reader = New-Object System.IO.StreamReader($entry.Open())
    [xml]$vcManifest = $reader.ReadToEnd()
    $reader.Close()
} finally { $zip.Dispose() }
$vclibsVersion = $vcManifest.Package.Identity.Version
Write-Host "VCLibs framework version: $vclibsVersion"
Copy-Item $vclibs.FullName $OutDir

# --- Manifest ----------------------------------------------------------------------------------------
$manifest = Get-Content -Raw (Join-Path $RoRRoot "tools\xbox\package\AppxManifest.xml.in")
$manifest = $manifest.Replace("@IDENTITY_NAME@", $IdentityName).Replace("@PUBLISHER@", $Publisher)
$manifest = $manifest.Replace("@VERSION@", $Version).Replace("@VCLIBS_MINVERSION@", $vclibsVersion)
Set-Content -Path (Join-Path $Layout "AppxManifest.xml") -Value $manifest -Encoding UTF8

# --- resources.pri (tile/splash lookup) - non fatal ------------------------------------------------
try {
    $priCfg = Join-Path $OutDir "priconfig.xml"
    Invoke-Logged "makepri createconfig" { & $makepri createconfig /cf $priCfg /dq en-US /o } -TimeoutMinutes 15
    Invoke-Logged "makepri new" { & $makepri new /pr $Layout /cf $priCfg /mn (Join-Path $Layout "AppxManifest.xml") /of (Join-Path $Layout "resources.pri") /o } -TimeoutMinutes 15
} catch { Write-Host "::warning::makepri failed, packaging without resources.pri: $($_.Exception.Message)" }

# --- Pack ----------------------------------------------------------------------------------------
$msix = Join-Path $OutDir "RigsOfRods_$($Version)_x64.msix"
Invoke-Logged "makeappx pack" { & $makeappx pack /d $Layout /p $msix /o } -TimeoutMinutes 15

# --- Certificate -----------------------------------------------------------------------------------
if (-not $Pfx) {
    if (-not $PfxPassword) { $PfxPassword = [guid]::NewGuid().ToString("N") }
    Write-Host "Generating self-signed code signing certificate '$Publisher'"
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyUsage DigitalSignature `
        -FriendlyName "Rigs of Rods Xbox Dev Mode" -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}") -NotAfter (Get-Date).AddYears(2)
    $Pfx = Join-Path $env:RUNNER_TEMP "ror-dev.pfx"
    if (-not $env:RUNNER_TEMP) { $Pfx = Join-Path $OutDir "..\ror-dev.pfx" }
    $sec = ConvertTo-SecureString -String $PfxPassword -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $Pfx -Password $sec | Out-Null
    Export-Certificate -Cert $cert -FilePath (Join-Path $OutDir "RigsOfRods-dev.cer") | Out-Null
} else {
    $pfxObj = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($Pfx, $PfxPassword)
    if ($pfxObj.Subject -ne $Publisher) { throw "Certificate subject '$($pfxObj.Subject)' does not match manifest Publisher '$Publisher'" }
    [System.IO.File]::WriteAllBytes((Join-Path $OutDir "RigsOfRods-dev.cer"), $pfxObj.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
}

Invoke-Logged "signtool msix" { & $signtool sign /fd SHA256 /f $Pfx /p $PfxPassword $msix } -TimeoutMinutes 15

# --- Bundle (Device Portal accepts .msix and .msixbundle) -----------------------------------------
$bundleIn = Join-Path $OutDir "bundle-in"
New-Item -ItemType Directory -Force -Path $bundleIn | Out-Null
Copy-Item $msix $bundleIn
$bundle = Join-Path $OutDir "RigsOfRods_$($Version).msixbundle"
Invoke-Logged "makeappx bundle" { & $makeappx bundle /d $bundleIn /p $bundle /bv $Version /o } -TimeoutMinutes 15
Invoke-Logged "signtool bundle" { & $signtool sign /fd SHA256 /f $Pfx /p $PfxPassword $bundle } -TimeoutMinutes 15
Remove-Item -Recurse -Force $bundleIn, $Layout
Remove-Item -Force (Join-Path $OutDir "priconfig.xml") -ErrorAction SilentlyContinue

Copy-Item (Join-Path $RoRRoot "tools\xbox\INSTALL-XBOX.txt") $OutDir -ErrorAction SilentlyContinue
Write-Host "Package ready:"
Get-ChildItem $OutDir | ForEach-Object { Write-Host ("  {0,-48} {1,10:N0} KB" -f $_.Name, ($_.Length / 1KB)) }
