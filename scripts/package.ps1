# Couch Conduit — Packaging Script
# Creates distributable zip files for host and client
#
# Usage: .\scripts\package.ps1
# Output: dist\CouchConduit-Host.zip, dist\CouchConduit-Client.zip

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Push-Location $root

$dist = "$root\dist"
$hostDir = "$dist\CouchConduit-Host"
$clientDir = "$dist\CouchConduit-Client"

# Clean
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $hostDir | Out-Null
New-Item -ItemType Directory -Path $clientDir | Out-Null

Write-Host "=== Building Release ===" -ForegroundColor Cyan
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# ─── Host Package ───────────────────────────────────────────────────
Write-Host "`n=== Packaging Host ===" -ForegroundColor Cyan
Copy-Item "build\src\host\Release\cc_host.exe" $hostDir

# VC++ Runtime (if present locally — user can also install the redist)
$vcRedist = "C:\Windows\System32\msvcp140.dll",
            "C:\Windows\System32\vcruntime140.dll",
            "C:\Windows\System32\vcruntime140_1.dll"
foreach ($dll in $vcRedist) {
    if (Test-Path $dll) { Copy-Item $dll $hostDir }
}

Write-Host "Host files:"
Get-ChildItem $hostDir | ForEach-Object { Write-Host "  $($_.Name) ($([math]::Round($_.Length/1KB)) KB)" }

# ─── Client Package ─────────────────────────────────────────────────
Write-Host "`n=== Packaging Client ===" -ForegroundColor Cyan
Copy-Item "build\src\client\Release\cc_client.exe" $clientDir

# Only the FFmpeg DLLs actually needed (avcodec → avutil, swresample)
$ffmpegDlls = @("avcodec-62.dll", "avutil-60.dll", "swresample-6.dll")
foreach ($dll in $ffmpegDlls) {
    $src = "build\src\client\Release\$dll"
    if (Test-Path $src) {
        Copy-Item $src $clientDir
    } else {
        Write-Warning "Missing: $dll"
    }
}

# VC++ Runtime
foreach ($dll in $vcRedist) {
    if (Test-Path $dll) { Copy-Item $dll $clientDir }
}

Write-Host "Client files:"
Get-ChildItem $clientDir | ForEach-Object { Write-Host "  $($_.Name) ($([math]::Round($_.Length/1KB)) KB)" }

# ─── Create ZIPs ────────────────────────────────────────────────────
Write-Host "`n=== Creating ZIPs ===" -ForegroundColor Cyan
Compress-Archive -Path "$hostDir\*" -DestinationPath "$dist\CouchConduit-Host.zip" -Force
Compress-Archive -Path "$clientDir\*" -DestinationPath "$dist\CouchConduit-Client.zip" -Force

$hostZip = Get-Item "$dist\CouchConduit-Host.zip"
$clientZip = Get-Item "$dist\CouchConduit-Client.zip"
Write-Host "`nDone!" -ForegroundColor Green
Write-Host "  Host:   $($hostZip.Name) ($([math]::Round($hostZip.Length/1MB, 1)) MB)"
Write-Host "  Client: $($clientZip.Name) ($([math]::Round($clientZip.Length/1MB, 1)) MB)"

Pop-Location
