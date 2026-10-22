# Installs music-service as a Windows service from an unpacked release package.
# Run elevated (PowerShell 7 or Windows PowerShell 5.1):
#   .\share\music-service\service\install-service.ps1 -DownloadDir D:\Music
# What it does:
#   - writes <ConfigDir>\config.json (kept if it exists) with data_dir <DataDir> and the
#     download directory as destination dst_default for prn_local;
#   - runs "init" once (skipped when the database exists) and moves the one-time bootstrap
#     token to <ConfigDir>\bootstrap-token.txt, readable by Administrators only;
#   - registers the service "<Name>" running "music-service.exe --config ... service" as the
#     virtual account "NT SERVICE\<Name>", started automatically, restarted on failure;
#   - makes that account the owner of <DataDir> (secrets/ stays owner-only) and gives it
#     read access to the configuration and modify access to the download directory;
#   - registers the event log source for startup errors and starts the service.
# The program and the package stay where they are; data never lives in the package.
param(
    [Parameter(Mandatory = $true)][string]$DownloadDir,
    [string]$Name = 'music-service',
    [string]$InstallDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path,
    [string]$ConfigDir = (Join-Path $env:ProgramData 'music-service'),
    [string]$DataDir = (Join-Path (Join-Path $env:ProgramData 'music-service') 'data'),
    [int]$Port = 17632
)
$ErrorActionPreference = 'Stop'
# The program writes UTF-8; read it as such (paths may be Chinese).
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# Gives the whole tree to $Account. secrets/ and backups/ admit their owner only (OWNER
# RIGHTS), so a directory handed over first could no longer be listed: everything is
# listed while it is still readable, then owners change deepest first, the root last.
function Set-TreeOwner([string]$Path, [string]$Account) {
    $items = @(Get-ChildItem -LiteralPath $Path -Recurse -Force | ForEach-Object { $_.FullName })
    $items = $items | Sort-Object { $_.Length } -Descending
    foreach ($item in @($items) + @($Path)) {
        icacls $item /setowner $Account /C /Q | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "cannot make $Account the owner of $item" }
    }
}

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'run this script from an elevated PowerShell'
}
$exe = Join-Path $InstallDir 'bin\music-service.exe'
if (-not (Test-Path $exe)) { throw "no program at $exe" }
if (-not (Test-Path $DownloadDir -PathType Container)) { throw "download directory $DownloadDir does not exist" }
if (Get-Service -Name $Name -ErrorAction SilentlyContinue) { throw "a service named $Name already exists" }
$account = "NT SERVICE\$Name"

New-Item -ItemType Directory -Force $ConfigDir | Out-Null
$config = Join-Path $ConfigDir 'config.json'
if (-not (Test-Path $config)) {
    $settings = [ordered]@{
        config_version = 1
        data_dir = $DataDir
        control = @{ host = '127.0.0.1'; port = $Port }
        media = @{ enabled = $true; host = '127.0.0.1'; port = $Port + 1 }
        events = @{ enabled = $true; host = '127.0.0.1'; port = $Port + 2 }
        features = @{ file_access = $true }
        destinations = @(@{ id = 'dst_default'; name = 'downloads'; root = (Resolve-Path $DownloadDir).Path; principals = @('prn_local') })
    }
    # UTF-8 without a byte order mark, in both PowerShell editions.
    [System.IO.File]::WriteAllText($config, ($settings | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding $false))
}

& $exe --config $config check-config
if ($LASTEXITCODE -ne 0) { throw "the configuration at $config is not valid" }
if (-not (Test-Path (Join-Path $DataDir 'service.sqlite3'))) {
    & $exe --config $config init
    if ($LASTEXITCODE -ne 0) { throw 'init failed' }
    $token = Join-Path $DataDir 'secrets\bootstrap-token.txt'
    $kept = Join-Path $ConfigDir 'bootstrap-token.txt'
    Move-Item -Force $token $kept
    icacls $kept /inheritance:r /grant:r 'Administrators:F' 'SYSTEM:F' | Out-Null
    Write-Host "bootstrap token: $kept (move it to the client, then delete it)"
}

$binary = "`"$exe`" --config `"$config`" service"
New-Service -Name $Name -BinaryPathName $binary -DisplayName 'music-service' -StartupType Automatic `
    -Description 'Music search, download and media backend' | Out-Null
sc.exe config $Name obj= $account | Out-Null
if ($LASTEXITCODE -ne 0) { throw "cannot run $Name as $account" }
sc.exe failure $Name reset= 86400 actions= restart/5000/restart/30000//0 | Out-Null

# The service account owns its data; secrets/ grants its owner only, so ownership is what
# gives the account access there. The program re-applies owner-only rights on start.
icacls $DataDir /grant "${account}:(OI)(CI)F" /Q | Out-Null
Set-TreeOwner $DataDir $account
icacls $ConfigDir /grant "${account}:(OI)(CI)RX" /Q | Out-Null
icacls (Resolve-Path $DownloadDir).Path /grant "${account}:(OI)(CI)M" /Q | Out-Null

$source = "HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\$Name"
if (-not (Test-Path $source)) {
    New-Item -Path $source -Force | Out-Null
    $messages = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\EventLogMessages.dll'
    if (Test-Path $messages) { New-ItemProperty -Path $source -Name EventMessageFile -Value $messages | Out-Null }
    New-ItemProperty -Path $source -Name TypesSupported -PropertyType DWord -Value 7 | Out-Null
}

$log = Join-Path $DataDir 'logs\music-service.log'
try {
    Start-Service -Name $Name
    (Get-Service -Name $Name).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
} catch {
    # Show why before anyone removes the data.
    Write-Warning "$Name did not start: $_"
    if (Test-Path $log) {
        Write-Host "== last lines of $log"
        Get-Content $log -Tail 20 -Encoding UTF8 | Out-Host
    }
    Write-Host '== event log'
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = $Name } -MaxEvents 5 -ErrorAction SilentlyContinue |
        Format-List TimeCreated, Message | Out-Host
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Service Control Manager' } -MaxEvents 3 -ErrorAction SilentlyContinue |
        Format-List TimeCreated, Message | Out-Host
    throw
}
& $exe --config $config healthcheck
if ($LASTEXITCODE -ne 0) { throw "$Name is running but not ready; see $(Join-Path $DataDir 'logs\music-service.log')" }
Write-Host "$Name is running as $account; logs: $log"
