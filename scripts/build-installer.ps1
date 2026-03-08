# Couch Conduit — Build Installer Script
# Creates an MSI package using WiX Toolset v4+ or a self-extracting ZIP if WiX is not available.
# Usage: .\scripts\build-installer.ps1 [-WixPath <path>] [-Version <x.y.z>] [-OutputDir <dir>]

param(
    [string]$WixPath = "",
    [string]$Version = "0.1.0",
    [string]$OutputDir = ".\dist",
    [string]$BuildDir = ".\build\src",
    [string]$FFmpegDir = ".\third_party\ffmpeg"
)

$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "  ========================================" -ForegroundColor Cyan
Write-Host "  ===  Couch Conduit Installer Builder ===" -ForegroundColor Cyan
Write-Host "  ========================================" -ForegroundColor Cyan
Write-Host ""

# Resolve paths
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path "$ProjectRoot\CMakeLists.txt")) {
    $ProjectRoot = Get-Location
}

$HostExe   = Join-Path $ProjectRoot "$BuildDir\host\Release\cc_host.exe"
$ClientExe = Join-Path $ProjectRoot "$BuildDir\client\Release\cc_client.exe"

if (-not (Test-Path $HostExe)) {
    Write-Error "cc_host.exe not found at $HostExe — build the project first"
    exit 1
}
if (-not (Test-Path $ClientExe)) {
    Write-Error "cc_client.exe not found at $ClientExe — build the project first"
    exit 1
}

# Create output directory
$OutDir = Join-Path $ProjectRoot $OutputDir
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# Collect FFmpeg DLLs
$FFmpegBin = Join-Path $ProjectRoot "$FFmpegDir\bin"
$FFmpegDlls = @()
if (Test-Path $FFmpegBin) {
    $FFmpegDlls = Get-ChildItem -Path $FFmpegBin -Filter "*.dll" | Select-Object -ExpandProperty FullName
    Write-Host "  Found $($FFmpegDlls.Count) FFmpeg DLLs" -ForegroundColor Green
}

# Collect VC++ Runtime DLLs (if present alongside the build)
$VcRedistDlls = @()
$VcRedistPaths = @(
    "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT"
)
foreach ($pattern in $VcRedistPaths) {
    $dirs = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue
    foreach ($dir in $dirs) {
        $VcRedistDlls += Get-ChildItem -Path $dir.FullName -Filter "*.dll" | Select-Object -ExpandProperty FullName
        break
    }
    if ($VcRedistDlls.Count -gt 0) { break }
}
if ($VcRedistDlls.Count -gt 0) {
    Write-Host "  Found $($VcRedistDlls.Count) VC++ Runtime DLLs" -ForegroundColor Green
}

# ─── Option 1: WiX MSI ──────────────────────────────────────────────────

$WixExe = ""
if ($WixPath) {
    $WixExe = Join-Path $WixPath "wix.exe"
} else {
    # Try to find wix on PATH
    $WixExe = Get-Command "wix" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if (-not $WixExe) {
        $WixExe = Get-Command "wix.exe" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    }
}

$WixAvailable = $WixExe -and (Test-Path $WixExe -ErrorAction SilentlyContinue)

if ($WixAvailable) {
    Write-Host "  Building MSI with WiX: $WixExe" -ForegroundColor Cyan

    # Generate WiX source
    $WxsPath = Join-Path $OutDir "CouchConduit.wxs"
    $MsiPath = Join-Path $OutDir "CouchConduit-$Version-x64.msi"

    # The WiX source is generated from a template
    Write-Host "  WiX MSI generation not yet automated — use the .wxs template" -ForegroundColor Yellow
    Write-Host "  See installer/CouchConduit.wxs" -ForegroundColor Yellow
} else {
    Write-Host "  WiX not found — skipping MSI. Will create portable ZIP instead." -ForegroundColor Yellow
}

# ─── Option 2: Portable ZIP ─────────────────────────────────────────────

Write-Host ""
Write-Host "  Creating portable ZIP package..." -ForegroundColor Cyan

$StagingDir = Join-Path $OutDir "CouchConduit-$Version-x64"
New-Item -ItemType Directory -Path $StagingDir -Force | Out-Null

# Copy executables
Copy-Item $HostExe   -Destination $StagingDir -Force
Copy-Item $ClientExe -Destination $StagingDir -Force

# Copy FFmpeg DLLs
foreach ($dll in $FFmpegDlls) {
    Copy-Item $dll -Destination $StagingDir -Force
}

# Copy VC++ Runtime DLLs
foreach ($dll in $VcRedistDlls) {
    Copy-Item $dll -Destination $StagingDir -Force
}

# Copy documentation
$DocsToInclude = @("README.md", "LICENSE", "TESTING.md")
foreach ($doc in $DocsToInclude) {
    $docPath = Join-Path $ProjectRoot $doc
    if (Test-Path $docPath) {
        Copy-Item $docPath -Destination $StagingDir -Force
    }
}

# Create a quick-start batch file for host
@"
@echo off
echo.
echo   Couch Conduit Host
echo   ==================
echo.
echo Starting host... (waiting for client connection)
echo Press Ctrl+C to stop.
echo.
cc_host.exe %*
pause
"@ | Set-Content -Path (Join-Path $StagingDir "start-host.bat") -Encoding ASCII

# Create a quick-start batch file for client
@"
@echo off
echo.
echo   Couch Conduit Client
echo   ====================
echo.
set /p HOST_IP="Enter host IP address: "
echo.
echo Connecting to %HOST_IP%...
echo Press Ctrl+C to stop.
echo.
cc_client.exe --host %HOST_IP% %*
pause
"@ | Set-Content -Path (Join-Path $StagingDir "start-client.bat") -Encoding ASCII

# Create ZIP
$ZipPath = "$StagingDir.zip"
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path "$StagingDir\*" -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Host ""
Write-Host "  ========================================" -ForegroundColor Green
Write-Host "  Package created:" -ForegroundColor Green
Write-Host "    $ZipPath" -ForegroundColor White
Write-Host "  Contents:" -ForegroundColor Green
Get-ChildItem -Path $StagingDir | ForEach-Object {
    $size = if ($_.Length) { "{0:N0} KB" -f ($_.Length / 1KB) } else { "dir" }
    Write-Host ("    {0,-30} {1}" -f $_.Name, $size) -ForegroundColor Gray
}
Write-Host "  ========================================" -ForegroundColor Green
Write-Host ""

# Cleanup staging
Remove-Item -Path $StagingDir -Recurse -Force
