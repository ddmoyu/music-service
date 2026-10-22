# Installs an unpacked release package as a real Windows service and checks it, then removes
# it. Needs an elevated PowerShell (CI runners are). Uses throwaway directories with Chinese
# names and a service name of its own, so an existing installation is not touched.
#   - install-service.ps1 registers it as NT SERVICE\<name>, starts it, healthcheck passes;
#   - the bootstrap token works against the running service; the data belongs to the
#     service account and secrets/ is not readable through inherited rights;
#   - the log file under data_dir receives the startup and shutdown records;
#   - Stop-Service ends it cleanly (exit code 0) and the instance lock is free afterwards;
#   - a restart serves the same token; uninstall-service.ps1 -RemoveData removes it.
# Usage: pwsh packaging/windows/service_smoke.ps1 -PackageDir <unpacked package root>
param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [int]$Port = 17662
)
$ErrorActionPreference = 'Stop'

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "service smoke test failed: $Message" }
}

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'service smoke test needs an elevated PowerShell (Run as administrator); nothing was changed'
}
$root = (Resolve-Path $PackageDir).Path
$name = 'music-service-smoke'
$work = Join-Path $env:ProgramData ("music-service-服务测试-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$downloads = Join-Path $work '下载'
$configDir = Join-Path $work '配置'
New-Item -ItemType Directory -Force $downloads | Out-Null
$exe = Join-Path $root 'bin\music-service.exe'
$scripts = Join-Path $root 'share\music-service\service'
try {
    & (Join-Path $scripts 'install-service.ps1') -Name $name -InstallDir $root -ConfigDir $configDir `
        -DataDir (Join-Path $configDir 'data') -DownloadDir $downloads -Port $Port
    $service = Get-Service -Name $name
    Assert ($service.Status -eq 'Running') 'not running after install'
    $account = (Get-CimInstance Win32_Service -Filter "Name='$name'").StartName
    Assert ($account -eq "NT SERVICE\$name") "runs as $account"

    $token = (Get-Content (Join-Path $configDir 'bootstrap-token.txt') -TotalCount 1).Trim()
    $info = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/v1/info" -Headers @{ Authorization = "Bearer $token" }
    Assert ($info.data.service -eq 'music-service') 'info with the bootstrap token'
    foreach ($dir in @('secrets', 'secrets\credentials', 'backups')) {
        # The administrator may no longer read these; the owner is still visible.
        $owner = (Get-Acl (Join-Path $configDir "data\$dir")).Owner
        Assert ($owner -eq "NT SERVICE\$name") "$dir is owned by $owner"
    }

    Stop-Service -Name $name
    $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(90))
    $state = sc.exe queryex $name | Out-String
    Assert ($state -match 'WIN32_EXIT_CODE\s+:\s+0\b') "stopped with an error:`n$state"
    $log = Get-Content (Join-Path $configDir 'data\logs\music-service.log') -Raw
    Assert ($log -match '"event":"shutdown.started"') 'no shutdown record in the log'
    & $exe --config (Join-Path $configDir 'config.json') principal list | Out-Host
    Assert ($LASTEXITCODE -eq 0) 'the instance lock is still held after stopping'

    Start-Service -Name $name
    $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
    $again = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/v1/info" -Headers @{ Authorization = "Bearer $token" }
    Assert ($again.data.service -eq 'music-service') 'info after restart'
    Write-Host 'service smoke test passed'
} finally {
    # Cleanup must not hide why the test failed.
    try {
        & (Join-Path $scripts 'uninstall-service.ps1') -Name $name -ConfigDir $configDir -RemoveData
    } catch {
        Write-Warning "cleanup: $_"
    }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
