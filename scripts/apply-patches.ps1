# Applies this project's changes to the xboxrecomp submodule
# (D3D11 back-end interface additions, post-transform vertex cache, MMIO fault counter).
# Run once after cloning / `git submodule update`. Safe to re-run: skips if already applied.
$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$patch = Join-Path $root "patches\xboxrecomp-automodellista.patch"
$sub   = Join-Path $root "external\xboxrecomp"
git -C $sub apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -eq 0) { Write-Host "xboxrecomp patch already applied."; exit 0 }
git -C $sub apply --whitespace=nowarn $patch
Write-Host "xboxrecomp patch applied."
