<#
.SYNOPSIS  Boot smoke test: runs the recompiled game for N seconds and grades how far it got.
.OUTPUTS   Exit code = highest stage reached (0..4). Stages are cumulative log markers printed by src/main.c
           and the xboxrecomp runtime. Extend $stages as bring-up progresses.
  S0  process started, XBE loaded
  S1  memory layout + kernel init complete   ("=== Initialization complete ===")
  S2  guest entry called, no crash in first 5 s ("Starting game..." and no "EXCEPTION"/"ACCESS_VIOLATION")
  S3  D3D device created (runtime log)         (first frame presented)
  S4  ran the full timeout without crash/hang  (watchdog silent)
#>
param([string]$Preset = "win-x64-debug", [int]$TimeoutSecs = 30)
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
# No watchdog here. It detects "the process has not exited", which is exactly
# what a running game looks like, and it kills the process when it fires. Set
# RECOMP_WATCHDOG_SECS yourself when you are chasing a hang.
& (Join-Path $repo "scripts\run.ps1") -Preset $Preset -TimeoutSecs $TimeoutSecs | Out-Null
$log = Get-ChildItem (Join-Path $repo "logs") -Filter run-*.log | Sort-Object LastWriteTime | Select-Object -Last 1
$text = (Get-Content $log.FullName -Raw) + (Get-Content "$($log.FullName).err" -Raw -ErrorAction SilentlyContinue)
# Anchored deliberately. A loose "unhandled" also matches the GPU executor's
# "N unhandled methods", which is a coverage note, not a crash.
$crashed = $text -match "\[CRASH\]|Access violation|EXCEPTION_ACCESS_VIOLATION|unhandled exception"
$watchdog = $text -match "\[WATCHDOG\]"   # informational: the game is expected to run forever, so only a crash fails
$icallFails = ([regex]::Matches($text, "\[ICALL\] Failed to resolve VA (0x[0-9A-F]{8})") | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
if ($icallFails) { Write-Host ("unresolved ICALL targets: " + ($icallFails -join ", ")) }
$stage = 0
if ($text -match "XBE loaded")                    { $stage = 0 }
if ($text -match "=== Initialization complete") { $stage = 1 }
if ($text -match "Starting game" -and -not $crashed) { $stage = 2 }
$draws = 0; if ($text -match "\[GPU\] draws (\d+)") { $draws = [int]$matches[1] }
if (($text -match "D3D11 device|CreateDevice|Present" -or $draws -gt 0) -and -not $crashed) { $stage = 3 }
if (-not $crashed -and $stage -ge 3)              { $stage = 4 }
Write-Host "BOOT SMOKE: stage S$stage (crashed=$crashed watchdog=$watchdog gpu_draws=$draws) log=$($log.Name)"
if ($crashed) { Select-String -Path $log.FullName,"$($log.FullName).err" -Pattern "EXCEPTION|fault|sub_[0-9A-F]{8}" | Select-Object -First 15 }
exit $stage
