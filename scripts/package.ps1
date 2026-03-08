# Couch Conduit -- Packaging Script
# Creates a single distributable ZIP with host + client, launcher scripts,
# VC redistributable, and a README.
#
# Usage:  powershell -ExecutionPolicy Bypass -File scripts\package.ps1
# Output: dist\CouchConduit-v0.1.0-win64.zip

param(
    [string]$Config = "Release",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Push-Location $root

$Version  = "0.1.0"
$ZipName  = "CouchConduit-v$Version-win64"
$dist     = "$root\dist"
$stageDir = "$dist\$ZipName"

Write-Host ""
Write-Host "=== Couch Conduit Packager ===" -ForegroundColor Cyan
Write-Host "Version : $Version"
Write-Host "Config  : $Config"
Write-Host ""

# --- 1. Build --------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "[1/4] Building..." -ForegroundColor Yellow
    if (-not (Test-Path "build\CMakeCache.txt")) {
        cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    }
    cmake --build build --config $Config --target cc_host cc_client
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
    Write-Host "       Build OK" -ForegroundColor Green
    Write-Host ""
} else {
    Write-Host "[1/4] Skipping build (-SkipBuild)" -ForegroundColor DarkGray
    Write-Host ""
}

# --- 2. Stage files ---------------------------------------------------
Write-Host "[2/4] Staging files..." -ForegroundColor Yellow

if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir | Out-Null

# Executables
Copy-Item "build\src\host\$Config\cc_host.exe"     "$stageDir\"
Copy-Item "build\src\client\$Config\cc_client.exe"  "$stageDir\"

# FFmpeg DLLs (only the ones actually needed at runtime)
$ffmpegDlls = @("avcodec-62.dll", "avutil-60.dll", "swresample-6.dll")
foreach ($dll in $ffmpegDlls) {
    $src = "build\src\client\$Config\$dll"
    if (Test-Path $src) {
        Copy-Item $src "$stageDir\"
    } else {
        Write-Warning "Missing DLL: $dll"
    }
}

# VC Redistributable installer
$vcRedistSearch = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio" -Recurse -Filter "vc_redist.x64.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($vcRedistSearch) {
    Copy-Item $vcRedistSearch.FullName "$stageDir\vc_redist.x64.exe"
    Write-Host "       Bundled VC Redistributable" -ForegroundColor DarkGray
} else {
    Write-Warning "vc_redist.x64.exe not found - users may need to install it themselves"
}

# Launcher scripts and README
$assets = "$root\scripts\dist-assets"
if (Test-Path "$assets\README.txt")       { Copy-Item "$assets\README.txt"       "$stageDir\" }
if (Test-Path "$assets\Start-Host.bat")   { Copy-Item "$assets\Start-Host.bat"   "$stageDir\" }
if (Test-Path "$assets\Start-Client.bat") { Copy-Item "$assets\Start-Client.bat" "$stageDir\" }

Write-Host "       Staged files:" -ForegroundColor DarkGray
Get-ChildItem $stageDir | ForEach-Object {
    $sizeMb = [math]::Round($_.Length / 1048576, 1)
    Write-Host ("         " + $_.Name + "  " + $sizeMb + " M")
}
Write-Host ""

# --- 3. Create ZIP ----------------------------------------------------
Write-Host "[3/4] Creating ZIP..." -ForegroundColor Yellow
$ZipPath = "$dist\$ZipName.zip"
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path "$stageDir\*" -DestinationPath $ZipPath -CompressionLevel Optimal
$zipSizeMb = [math]::Round((Get-Item $ZipPath).Length / 1048576, 1)
Write-Host "       $ZipPath  ($zipSizeMb M)" -ForegroundColor Green
Write-Host ""

# --- 4. Done ----------------------------------------------------------
Write-Host "[4/4] Done!" -ForegroundColor Green
Write-Host "       ZIP ready at: $ZipPath"
Write-Host ""

Pop-Location
