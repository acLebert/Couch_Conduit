# Couch Conduit - Localhost Benchmark Runner
# Runs a matrix of bitrate tests with warm-up runs, collects CSV data,
# and generates a summary report.
#
# Usage: .\scripts\benchmark.ps1 [-Bitrates @(20000,10000,5000)] [-Runs 10]
#        [-Duration 30] [-Codec hevc] [-Fps 60] [-Resolution 1920x1080]

param(
    [int[]]$Bitrates   = @(20000, 10000, 5000),
    [int]$Runs         = 10,
    [int]$Duration     = 30,    # seconds per run
    [string]$Codec     = "hevc",
    [int]$Fps          = 60,
    [string]$Resolution = "1920x1080",
    [int]$WarmupDuration = 15,  # seconds for warm-up run
    [int]$SettleDelay  = 5      # seconds between runs
)

$ErrorActionPreference = "Stop"

# ── Paths ────────────────────────────────────────────────────────────
$repoRoot   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path "$repoRoot\build")) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
}
$hostExe    = Join-Path $repoRoot "build\src\host\Release\cc_host.exe"
$clientExe  = Join-Path $repoRoot "build\src\client\Release\cc_client.exe"

if (-not (Test-Path $hostExe)) {
    Write-Error "Host executable not found: $hostExe"
    exit 1
}
if (-not (Test-Path $clientExe)) {
    Write-Error "Client executable not found: $clientExe"
    exit 1
}

# ── Output directory ─────────────────────────────────────────────────
$timestamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir     = Join-Path $repoRoot "benchmarks\$timestamp"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$machineName = $env:COMPUTERNAME
$summaryRows = @()

Write-Host ""
Write-Host "  ============================================" -ForegroundColor Cyan
Write-Host "  ===   Couch Conduit Benchmark Runner     ===" -ForegroundColor Cyan
Write-Host "  ============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Machine    : $machineName"
Write-Host "  Codec      : $Codec"
Write-Host "  Resolution : $Resolution"
Write-Host "  FPS target : $Fps"
Write-Host "  Bitrates   : $($Bitrates -join ', ') kbps"
Write-Host "  Runs/rate  : $Runs (+ 1 warm-up)"
Write-Host "  Duration   : ${Duration}s per run (${WarmupDuration}s warm-up)"
Write-Host "  Output     : $outDir"
Write-Host ""

# ── Helper: run one test ─────────────────────────────────────────────
function Invoke-BenchmarkRun {
    param(
        [int]$Bitrate,
        [int]$RunNumber,  # 0 = warm-up
        [int]$RunDuration,
        [string]$OutDir
    )

    $label = if ($RunNumber -eq 0) { "warmup" } else { "run$RunNumber" }
    $hostCsv   = Join-Path $OutDir "host_${Bitrate}kbps_${label}.csv"
    $clientCsv = Join-Path $OutDir "client_${Bitrate}kbps_${label}.csv"

    # Start host in background
    $hostArgs = "--no-session --bitrate $Bitrate --fps $Fps --codec $Codec --duration $($RunDuration + 5) --csv `"$hostCsv`""
    $hostProc = Start-Process -FilePath $hostExe -ArgumentList $hostArgs `
        -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $OutDir "host_${Bitrate}kbps_${label}_stderr.log")

    # Give host time to start capture + encoder
    Start-Sleep -Seconds 3

    # Start client
    $clientArgs = "127.0.0.1 --no-session --resolution $Resolution --duration $RunDuration --csv `"$clientCsv`""
    $clientProc = Start-Process -FilePath $clientExe -ArgumentList $clientArgs `
        -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $OutDir "client_${Bitrate}kbps_${label}_stderr.log")

    # Wait for client to finish (it exits after --duration)
    $clientTimeout = ($RunDuration + 30) * 1000
    $clientProc.WaitForExit($clientTimeout) | Out-Null
    if (-not $clientProc.HasExited) {
        Write-Warning "Client did not exit in time - killing"
        $clientProc.Kill()
    }

    # Stop host
    Start-Sleep -Seconds 1
    if (-not $hostProc.HasExited) {
        # Send Ctrl+C via taskkill
        & taskkill /PID $hostProc.Id /F 2>$null | Out-Null
        Start-Sleep -Seconds 1
    }

    return @{
        HostCsv   = $hostCsv
        ClientCsv = $clientCsv
        HostPid   = $hostProc.Id
        ClientPid = $clientProc.Id
    }
}

# ── Main benchmark loop ─────────────────────────────────────────────
foreach ($bitrate in $Bitrates) {
    Write-Host "  ── Bitrate: $bitrate kbps ──────────────────────" -ForegroundColor Yellow

    # Warm-up run (not counted)
    Write-Host "    [warm-up] ${WarmupDuration}s ..." -NoNewline
    $warmup = Invoke-BenchmarkRun -Bitrate $bitrate -RunNumber 0 `
        -RunDuration $WarmupDuration -OutDir $outDir
    Write-Host " done" -ForegroundColor Green
    Start-Sleep -Seconds $SettleDelay

    # Counted runs
    for ($r = 1; $r -le $Runs; $r++) {
        Write-Host "    [run $r/$Runs] ${Duration}s ..." -NoNewline
        $result = Invoke-BenchmarkRun -Bitrate $bitrate -RunNumber $r `
            -RunDuration $Duration -OutDir $outDir
        Write-Host " done" -ForegroundColor Green

        # Parse client CSV for this run's data
        if (Test-Path $result.ClientCsv) {
            $rows = Import-Csv $result.ClientCsv
            foreach ($row in $rows) {
                $summaryRows += [PSCustomObject]@{
                    machine        = $machineName
                    bitrate_kbps   = $bitrate
                    codec          = $Codec
                    fps_target     = $Fps
                    resolution     = $Resolution
                    run            = $r
                    timestamp_iso  = $row.timestamp_iso
                    interval_s     = $row.interval_s
                    fps            = $row.fps
                    avg_decode_ms  = $row.avg_decode_ms
                    avg_render_ms  = $row.avg_render_ms
                    avg_pipeline_ms = $row.avg_pipeline_ms
                    min_pipeline_ms = $row.min_pipeline_ms
                    max_pipeline_ms = $row.max_pipeline_ms
                    dropped_frames = $row.dropped_frames
                    first_decode_ms = $row.first_decode_ms
                    first_present_ms = $row.first_present_ms
                }
            }
        }

        if ($r -lt $Runs) {
            Start-Sleep -Seconds $SettleDelay
        }
    }

    Write-Host ""
}

# ── Write combined raw CSV ───────────────────────────────────────────
$rawCsvPath = Join-Path $outDir "all_client_intervals.csv"
$summaryRows | Export-Csv -Path $rawCsvPath -NoTypeInformation
Write-Host "  Combined CSV: $rawCsvPath" -ForegroundColor Cyan

# ── Generate summary statistics ──────────────────────────────────────
$summaryCsvPath = Join-Path $outDir "summary.csv"
$summaryLines = @()
$summaryLines += "bitrate_kbps,codec,fps_target,resolution,machine,metric,avg,min,max,p95,stdev"

# Group by bitrate, compute stats for key metrics
foreach ($bitrate in $Bitrates) {
    $bitrateRows = $summaryRows | Where-Object { [int]$_.bitrate_kbps -eq $bitrate }
    if ($bitrateRows.Count -eq 0) { continue }

    $metrics = @(
        @{ Name = "fps";             Values = $bitrateRows | ForEach-Object { [double]$_.fps } },
        @{ Name = "avg_decode_ms";   Values = $bitrateRows | ForEach-Object { [double]$_.avg_decode_ms } },
        @{ Name = "avg_render_ms";   Values = $bitrateRows | ForEach-Object { [double]$_.avg_render_ms } },
        @{ Name = "avg_pipeline_ms"; Values = $bitrateRows | ForEach-Object { [double]$_.avg_pipeline_ms } },
        @{ Name = "min_pipeline_ms"; Values = $bitrateRows | ForEach-Object { [double]$_.min_pipeline_ms } },
        @{ Name = "max_pipeline_ms"; Values = $bitrateRows | ForEach-Object { [double]$_.max_pipeline_ms } },
        @{ Name = "dropped_frames";  Values = $bitrateRows | ForEach-Object { [double]$_.dropped_frames } }
    )

    foreach ($m in $metrics) {
        $vals = $m.Values | Sort-Object
        $count = $vals.Count
        if ($count -eq 0) { continue }

        $avg  = ($vals | Measure-Object -Average).Average
        $min  = $vals[0]
        $max  = $vals[$count - 1]
        $p95i = [Math]::Ceiling($count * 0.95) - 1
        $p95  = $vals[[Math]::Min($p95i, $count - 1)]

        # Standard deviation
        $mean = $avg
        $sumSq = 0.0
        foreach ($v in $vals) { $sumSq += ($v - $mean) * ($v - $mean) }
        $stdev = [Math]::Sqrt($sumSq / $count)

        $summaryLines += "$bitrate,$Codec,$Fps,$Resolution,$machineName,$($m.Name)," +
            "$([Math]::Round($avg, 3)),$([Math]::Round($min, 3)),$([Math]::Round($max, 3))," +
            "$([Math]::Round($p95, 3)),$([Math]::Round($stdev, 3))"
    }
}

$summaryLines | Out-File -FilePath $summaryCsvPath -Encoding UTF8
Write-Host "  Summary CSV : $summaryCsvPath" -ForegroundColor Cyan

# ── Markdown report ──────────────────────────────────────────────────
$mdPath = Join-Path $outDir "report.md"
$md = @()
$md += "# Couch Conduit Benchmark Report"
$md += ""
$md += "| Parameter | Value |"
$md += "|-----------|-------|"
$md += "| Machine | $machineName |"
$md += "| Date | $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') |"
$md += "| Codec | $Codec |"
$md += "| Resolution | $Resolution |"
$md += "| FPS Target | $Fps |"
$md += "| Runs/bitrate | $Runs |"
$md += "| Duration/run | ${Duration}s |"
$md += ""

foreach ($bitrate in $Bitrates) {
    $md += "## $bitrate kbps"
    $md += ""
    $md += "| Metric | Avg | Min | Max | P95 | StDev |"
    $md += "|--------|-----|-----|-----|-----|-------|"

    $bitrateLines = $summaryLines | Where-Object { $_ -match "^$bitrate," }
    foreach ($line in $bitrateLines) {
        $parts = $line.Split(",")
        if ($parts.Count -ge 11) {
            $name  = $parts[5]
            $avg   = $parts[6]
            $min   = $parts[7]
            $max   = $parts[8]
            $p95   = $parts[9]
            $stdev = $parts[10]
            $md += "| $name | $avg | $min | $max | $p95 | $stdev |"
        }
    }
    $md += ""
}

$md | Out-File -FilePath $mdPath -Encoding UTF8
Write-Host "  Report      : $mdPath" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Benchmark complete!" -ForegroundColor Green
Write-Host ""
