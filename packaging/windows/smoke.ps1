# Runtime smoke test of a built music-service.exe on Windows: init, serve, readiness,
# an authenticated request, offline management refused while running, then a hard
# stop and a restart on the same data_dir (the OS releases the instance lock; data
# survives). The graceful Ctrl+C order is covered by the in-process server tests.
# Usage: pwsh packaging/windows/smoke.ps1 -Binary path\to\music-service.exe [-Port 17642]
param(
    [Parameter(Mandatory = $true)][string]$Binary,
    [int]$Port = 17642
)
$ErrorActionPreference = 'Stop'

$bin = (Resolve-Path $Binary).Path
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("music-service-smoke-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force (Join-Path $work 'downloads') | Out-Null
$config = Join-Path $work 'config.json'
@{
    config_version = 1
    data_dir = (Join-Path $work 'data')
    control = @{ host = '127.0.0.1'; port = $Port }
    destinations = @(@{ id = 'dst_default'; name = 'downloads'; root = (Join-Path $work 'downloads'); principals = @('prn_local') })
} | ConvertTo-Json -Depth 5 | Set-Content -Path $config -Encoding utf8NoBOM

function Invoke-Service([string[]]$Arguments) {
    & $bin --config $config @Arguments | Out-Host
    return $LASTEXITCODE
}
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "smoke test failed: $Message" }
}
function Start-Service-Process([string]$Tag) {
    $out = Join-Path $work "stdout-$Tag.txt"
    $err = Join-Path $work "stderr-$Tag.txt"
    $process = Start-Process -FilePath $bin -ArgumentList @('--config', $config, 'serve') -PassThru -NoNewWindow `
        -RedirectStandardOutput $out -RedirectStandardError $err
    for ($i = 0; $i -lt 100; $i++) {
        if ((Test-Path $out) -and ((Get-Content $out -Raw) -match '"event":"ready"')) { break }
        Start-Sleep -Milliseconds 100
    }
    Get-Content $out | Out-Host
    Assert ((Get-Content $out -Raw) -match '"event":"ready"') "service $Tag did not become ready"
    return $process
}

$service = $null
try {
    Write-Host "== system"
    Write-Host ([System.Environment]::OSVersion.VersionString) ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture)

    Write-Host "== version, check-config, init"
    Assert ((Invoke-Service @('version')) -eq 0) 'version'
    Assert ((Invoke-Service @('check-config')) -eq 0) 'check-config'
    Assert ((Invoke-Service @('init')) -eq 0) 'init'
    $token = (Get-Content (Join-Path $work 'data\secrets\bootstrap-token.txt') -TotalCount 1).Trim()

    Write-Host "== serve"
    $service = Start-Service-Process 'first'
    Assert ((Invoke-Service @('healthcheck')) -eq 0) 'healthcheck'
    $info = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/v1/info" -Headers @{ Authorization = "Bearer $token" }
    Assert ($info.data.service -eq 'music-service') 'info'
    $unauthenticated = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/api/v1/info" -SkipHttpErrorCheck
    Assert ($unauthenticated.StatusCode -eq 401) 'request without a token must be 401'
    Assert ((Invoke-Service @('token', 'list', '--principal', 'prn_local')) -ne 0) 'management must be refused while running'

    Write-Host "== hard stop and restart"
    Stop-Process -Id $service.Id -Force
    $service.WaitForExit()
    Assert ((Invoke-Service @('token', 'list', '--principal', 'prn_local')) -eq 0) 'lock released after the process ended'
    $service = Start-Service-Process 'second'
    $info = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/v1/info" -Headers @{ Authorization = "Bearer $token" }
    Assert ($info.data.service -eq 'music-service') 'token still valid after restart'
    Stop-Process -Id $service.Id -Force
    $service.WaitForExit()
    Write-Host "smoke test passed"
}
finally {
    if ($service) {
        Get-Process -Id $service.Id -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
