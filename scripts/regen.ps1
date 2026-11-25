# Re-runs the full xboxrecomp pipeline for Auto Modellista and regenerates
# src/recomp/gen. Run from anywhere:  .\scripts\regen.ps1
# Only the last step (fast, ~1 min), e.g. after editing src\am_overrides.c:
#   .\scripts\regen.ps1 -RecompOnly
# Analysis output goes to $Work (outside the repo: it is derived from game code).

param(
    [string]$GameDir = "C:\AMRecomp\Source Files\Unpacked ISO",
    [string]$Work    = "C:\AMRecomp\work",
    [switch]$RecompOnly
)

$ErrorActionPreference = "Stop"
$Repo  = Split-Path -Parent $PSScriptRoot
$Tk    = Join-Path $Repo "external\xboxrecomp"
$Xbe   = Join-Path $GameDir "default.xbe"
$Seeds = Join-Path $Repo "config\seed_functions.json"
$Gen   = Join-Path $Repo "src\recomp\gen"
$Overrides = Join-Path $Repo "src\am_overrides.c"

New-Item -ItemType Directory -Force -Path $Work | Out-Null

function Step($name, [string[]]$argv) {
    Write-Host "== $name" -ForegroundColor Cyan
    $log = Join-Path $Work "$name.log"
    # Windows PowerShell 5.1 turns a native program's stderr into errors;
    # with "Stop" the first log line on stderr would abort the step.
    $ErrorActionPreference = "Continue"
    & py -3 -m @argv *> $log
    if ($LASTEXITCODE -ne 0) {
        Get-Content $log -Tail 20
        throw "$name failed (see $log)"
    }
    Get-Content $log -Tail 3
}

Push-Location $Tk
try {
  if (-not $RecompOnly) {
    Step "disasm" @("tools.disasm", $Xbe, "-o", "$Work\disasm",
                    "--seed-functions", $Seeds, "--force")
    Step "func_id" @("tools.func_id", $Xbe,
                     "--functions", "$Work\disasm\functions.json",
                     "--strings",   "$Work\disasm\strings.json",
                     "--xrefs",     "$Work\disasm\xrefs.json",
                     "-o", "$Work\func_id")
    Step "abi" @("tools.abi_analysis", $Xbe,
                 "--disasm-dir", "$Work\disasm",
                 "--func-id-dir", "$Work\func_id",
                 "--output-dir", "$Work\abi")
  }
    Step "recomp" @("tools.recomp", $Xbe, "--all", "--split", "1000",
                    "--gen-dir", $Gen,
                    "--disasm-dir", "$Work\disasm",
                    "--func-id-dir", "$Work\func_id",
                    "--abi-dir", "$Work\abi",
                    "--exclude-manual", $Overrides,
                    "--game-name", "Auto Modellista")
}
finally {
    Pop-Location
}
Write-Host "Done. Now build:  cmake --build build --config Release" -ForegroundColor Green
