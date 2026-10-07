# Shared helpers for the Xbox UWP CI scripts (dot-source this file).
#
# Invoke-Logged runs a native command:
#   - strictly non-interactive (environment below disables credential/telemetry/Conan prompts);
#   - with a watchdog timeout: after -TimeoutMinutes the whole child process tree is killed, so a
#     tool waiting for input can never hang the job until the 6 h GitHub limit;
#   - mirroring output to the console and to a log file;
#   - publishing the relevant lines as GitHub "::error" annotations on failure. Annotations are
#     readable through the public checks API, so failures can be diagnosed without the raw log.

$script:CiLogDir = if ($env:RUNNER_TEMP) { Join-Path $env:RUNNER_TEMP "ror-ci-logs" } else { Join-Path ([IO.Path]::GetTempPath()) "ror-ci-logs" }
New-Item -ItemType Directory -Force -Path $script:CiLogDir | Out-Null

# --- Non-interactive environment for every tool started from here -------------------------------
$env:GIT_TERMINAL_PROMPT       = "0"        # git: fail instead of asking for credentials
$env:GCM_INTERACTIVE           = "never"    # Git Credential Manager
$env:GIT_ASKPASS               = "echo"
$env:CONAN_NON_INTERACTIVE     = "1"        # Conan 1 compat; Conan 2 uses core:non_interactive (set in CI)
$env:PIP_NO_INPUT              = "1"
$env:NUGET_XMLDOC_MODE         = "skip"
$env:DOTNET_NOLOGO             = "1"
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"
$env:MSBUILDDISABLENODEREUSE   = "1"        # no lingering MSBuild nodes keeping the step alive
$env:VSCMD_SKIP_SENDTELEMETRY  = "1"

function Write-FailureAnnotation([string] $what, [string] $logFile) {
    $lines = @()
    if (Test-Path $logFile) { $lines = Get-Content $logFile }
    $pattern = '(?i)(CMake Error|\berror\b|fatal|Could not find|not found|undefined|unresolved|cannot open|TIMEOUT)'
    $hits = $lines | Where-Object { $_ -match $pattern -and $_ -notmatch '0 Error\(s\)|-- Looking for|-- Performing Test|warning' } |
        Select-Object -Unique | Select-Object -First 60
    $tail = $lines | Select-Object -Last 40
    $text = (@("== $what ==") + $hits + @("----- last 40 lines -----") + $tail) -join "`n"
    $chunk = 3500
    $n = [Math]::Min(8, [Math]::Ceiling($text.Length / $chunk))
    for ($i = 0; $i -lt $n; $i++) {
        $part = $text.Substring($i * $chunk, [Math]::Min($chunk, $text.Length - $i * $chunk))
        $part = $part.Replace('%', '%25').Replace("`r", '').Replace("`n", '%0A')
        Write-Host "::error title=$what ($($i + 1)/$n)::$part"
    }
}

# Kills every descendant process of this PowerShell session after $Minutes (Windows only).
function Start-Watchdog([int] $Minutes, [string] $what, [string] $marker) {
    if (-not $IsWindows) { return $null }
    $parent = $PID
    return Start-ThreadJob -ArgumentList $parent, $Minutes, $what, $marker -ScriptBlock {
        param($parent, $minutes, $what, $marker)
        Start-Sleep -Seconds ($minutes * 60)
        Set-Content -Path $marker -Value "TIMEOUT"
        Write-Host "::error title=TIMEOUT::$what exceeded $minutes minutes - killing child processes"
        Get-CimInstance Win32_Process -Filter "ParentProcessId=$parent" | ForEach-Object {
            & taskkill.exe /T /F /PID $_.ProcessId | Out-Null
        }
    }
}

function Invoke-Logged([string] $what, [scriptblock] $cmd, [int] $TimeoutMinutes = 90) {
    $safe   = ($what -replace '[^\w\-\.]', '_')
    $log    = Join-Path $script:CiLogDir "$safe.log"
    $marker = Join-Path $script:CiLogDir "$safe.timeout"
    Remove-Item $marker -ErrorAction SilentlyContinue
    $dog = Start-Watchdog $TimeoutMinutes $what $marker
    try {
        & $cmd 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log | Out-Host
        $code = $LASTEXITCODE
    } finally {
        if ($dog) { Stop-Job $dog -ErrorAction SilentlyContinue; Remove-Job $dog -Force -ErrorAction SilentlyContinue }
    }
    if (Test-Path $marker) {
        Add-Content -Path $log -Value "TIMEOUT: $what killed after $TimeoutMinutes minutes"
        $code = 124
    }
    if ($code -ne 0) {
        Write-FailureAnnotation $what $log
        throw "$what failed with exit code $code (log: $log)"
    }
}
