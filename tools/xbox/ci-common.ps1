# Shared helpers for the Xbox UWP CI scripts (dot-source this file).
#
# Invoke-Logged runs a native command, mirrors its output to the console and to a log file, and on
# failure publishes the relevant lines as GitHub "::error" annotations. Annotations are visible on
# the run page and through the public checks API, so a failure can be diagnosed without downloading
# the raw job log (which requires an authenticated session).

$script:CiLogDir = if ($env:RUNNER_TEMP) { Join-Path $env:RUNNER_TEMP "ror-ci-logs" } else { Join-Path ([IO.Path]::GetTempPath()) "ror-ci-logs" }
New-Item -ItemType Directory -Force -Path $script:CiLogDir | Out-Null

function Write-FailureAnnotation([string] $what, [string] $logFile) {
    $lines = @()
    if (Test-Path $logFile) { $lines = Get-Content $logFile }
    $pattern = '(?i)(CMake Error|\berror\b|fatal|Could not find|not found|undefined|unresolved|cannot open)'
    $hits = $lines | Where-Object { $_ -match $pattern -and $_ -notmatch '0 Error\(s\)|-- Looking for|-- Performing Test|warning' } |
        Select-Object -Unique | Select-Object -First 60
    $tail = $lines | Select-Object -Last 40
    $text = (@("== $what ==") + $hits + @("----- last 40 lines -----") + $tail) -join "`n"
    # One annotation is limited in size; split into a few chunks.
    $chunk = 3500
    $n = [Math]::Min(8, [Math]::Ceiling($text.Length / $chunk))
    for ($i = 0; $i -lt $n; $i++) {
        $part = $text.Substring($i * $chunk, [Math]::Min($chunk, $text.Length - $i * $chunk))
        $part = $part.Replace('%', '%25').Replace("`r", '').Replace("`n", '%0A')
        Write-Host "::error title=$what ($($i + 1)/$n)::$part"
    }
}

function Invoke-Logged([string] $what, [scriptblock] $cmd) {
    $safe = ($what -replace '[^\w\-\.]', '_')
    $log = Join-Path $script:CiLogDir "$safe.log"
    & $cmd 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log | Out-Host
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Write-FailureAnnotation $what $log
        throw "$what failed with exit code $code (log: $log)"
    }
}
