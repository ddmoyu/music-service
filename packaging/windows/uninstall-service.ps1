# Removes the music-service Windows service and its program directory. Data, configuration
# and downloads are kept unless -RemoveData is given (which deletes <ConfigDir>, including
# data_dir when it lives there; downloads are never deleted). Run elevated.
param(
    [string]$Name = 'music-service',
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'music-service'),
    [string]$ConfigDir = (Join-Path $env:ProgramData 'music-service'),
    [switch]$RemoveData
)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'run this script from an elevated PowerShell'
}
$service = Get-Service -Name $Name -ErrorAction SilentlyContinue
if ($service) {
    if ($service.Status -ne 'Stopped') {
        Stop-Service -Name $Name
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(90))
    }
    sc.exe delete $Name | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cannot delete the service $Name" }
}
# Only a directory holding the program is removed, never an arbitrary path.
if ((Test-Path (Join-Path $InstallDir 'bin\music-service.exe')) -and
    (Test-Path (Join-Path $InstallDir 'share\music-service'))) {
    Remove-Item -Recurse -Force $InstallDir
}
$source = "HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\$Name"
if (Test-Path $source) { Remove-Item -Path $source -Recurse -Force }
if ($RemoveData -and (Test-Path $ConfigDir)) {
    # The service account owned the data; take it back before deleting.
    takeown /F $ConfigDir /R /D Y | Out-Null
    icacls $ConfigDir /grant 'Administrators:F' /T /C /Q | Out-Null
    Remove-Item -Recurse -Force $ConfigDir
}
Write-Host "$Name removed$(if ($RemoveData) { ' with its data' } else { '; data kept in ' + $ConfigDir })"
