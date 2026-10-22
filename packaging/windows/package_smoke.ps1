# Checks a Windows release package as a user would get it, without elevation: the archive
# unpacks into a directory with a Chinese, deep path; it holds only the documented layout
# and no extra DLLs; the program imports system DLLs only (dumpbin when available); the
# runtime smoke test passes on the unpacked program; "service" outside the service manager
# fails with a hint instead of hanging.
# Usage: pwsh packaging/windows/package_smoke.ps1 -Package out\build\windows-msvc-release\music-service-*.zip
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [int]$Port = 17652
)
$ErrorActionPreference = 'Stop'

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "package smoke test failed: $Message" }
}

$zip = (Resolve-Path $Package).Path
$deep = ('很长的目录名-' + ('x' * 40) + '\') * 3
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("music-service-包-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$target = Join-Path $work $deep
New-Item -ItemType Directory -Force $target | Out-Null
try {
    Expand-Archive -Path $zip -DestinationPath $target
    $root = (Get-ChildItem $target -Directory | Select-Object -First 1).FullName
    Write-Host "== unpacked to $root ($($root.Length) characters)"
    $exe = Join-Path $root 'bin\music-service.exe'
    Assert (Test-Path $exe) 'bin\music-service.exe'
    foreach ($file in @('share\music-service\README.md', 'share\music-service\config.example.json',
            'share\music-service\licenses\THIRD-PARTY.md', 'share\music-service\licenses\mbedtls-LICENSE.txt',
            'share\music-service\licenses\taglib-COPYING.MPL.txt', 'share\music-service\service\install-service.ps1',
            'share\music-service\service\uninstall-service.ps1')) {
        Assert (Test-Path (Join-Path $root $file)) $file
    }
    $dlls = @(Get-ChildItem $root -Recurse -Filter *.dll)
    Assert ($dlls.Count -eq 0) "unexpected DLLs: $($dlls.FullName -join ', ')"
    $size = (Get-Item $exe).Length
    Write-Host "program: $size bytes"

    $dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
    if ($dumpbin) {
        $imports = & dumpbin /dependents $exe | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim().ToLower() }
        Write-Host "imports: $($imports -join ', ')"
        $runtime = @($imports | Where-Object { $_ -match '^(msvcp|vcruntime|ucrtbase|api-ms-win-crt)' })
        Assert ($runtime.Count -eq 0) "depends on the C/C++ runtime: $($runtime -join ', ')"
    } else {
        Write-Host 'dumpbin not found: import check skipped'
    }

    $hint = & $exe service 2>&1 | Out-String
    Assert ($LASTEXITCODE -eq 2) "service outside the SCM exited $LASTEXITCODE"
    Assert ($hint -match 'service manager') 'service outside the SCM gave no hint'

    & (Join-Path $PSScriptRoot 'smoke.ps1') -Binary $exe -Port $Port
    Write-Host 'package smoke test passed'
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
