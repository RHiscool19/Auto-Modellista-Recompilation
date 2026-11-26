# Runs the recompiled game with logging. Usage:  .\scripts\run.ps1
# GPU rendering:  .\scripts\run.ps1 -D3D11
# Optional:  .\scripts\run.ps1 -Watchdog 120   (kill after N seconds and dump the
# guest stack; it is a plain timer, not hang detection, so it is off by default)
# Output: C:\AMRecomp\work\run_out.log / run_err.log, then prints the exit code.

param(
    [int]$Watchdog = 0,
    [switch]$D3D11,
    [string]$GuestCore = "0",   # host core for all guest threads (they must share one; see below)
    [int]$GpuIdleUs = 0,        # >0: the GPU thread sleeps this long when idle instead of spinning
    [switch]$Profile,           # sample the busiest threads; [PROFILE] lines every 10 s
    [string]$Work  = "C:\AMRecomp\work"
)

$Repo = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $Repo "build\Release\automodellista_recomp.exe"

if ($Watchdog -gt 0) { $env:RECOMP_WATCHDOG_SECS = "$Watchdog" }
else { Remove-Item Env:RECOMP_WATCHDOG_SECS -ErrorAction SilentlyContinue }
$env:RECOMP_AC97_READY    = "1"   # otherwise DirectSoundCreate fails and engine init is skipped
if ($D3D11) {
    # GPU rendering (src/d3d11_backend.c) with its own window
    $env:AM_D3D11 = "1"
    $env:RECOMP_STRIP_ALT_WINDING = "1"   # strips wind like hardware, so back-face culling keeps every triangle
    Remove-Item Env:RECOMP_FB_WINDOW -ErrorAction SilentlyContinue
} else {
    Remove-Item Env:AM_D3D11 -ErrorAction SilentlyContinue
    Remove-Item Env:RECOMP_STRIP_ALT_WINDING -ErrorAction SilentlyContinue
    $env:RECOMP_FB_WINDOW = "1"   # CPU-rendered guest framebuffer window
}
$env:RECOMP_VBLANK        = "paced" # 60 Hz vblank: CRI ADX/ADXF file streaming is driven by it
$env:RECOMP_PB_EXEC       = "1"   # execute the GPU pushbuffer so frames can actually draw
$env:RECOMP_WAIT_LOG      = "1"   # log waits that time out or block >= 500 ms
$env:RECOMP_TITLE_KEVENTS = "1"   # CRI builds its own KEVENTs; without this every wait fails instantly and spins
$env:RECOMP_USB           = "1"   # the game's own XDK input library drives USB hardware; this models it, fed by XInput pads
$env:RECOMP_INPUT_DIAG    = "1"   # report whether the input chain is wired
# Keep GPU/USB (contiguous) memory above the 9.5 MB game image. Otherwise its
# physical addresses (e.g. 0x00488000) fall inside the image's address range,
# and the USB model reads the game's .data instead of the controller's
# descriptors: enumeration never starts and the pad is never seen.
$env:RECOMP_CONTIG_AVOID_IMAGE = "1"
$env:RECOMP_HEAP_RECLAIM  = "1"   # reuse freed heap/contiguous memory; without it a second race exhausts the 64 MB GPU pool and crashes
$env:RECOMP_AUDIO_BUF_SAMPLES = "1024" # 6 x 1024 samples = 128 ms of audio queued: rides out the 20-80 ms gaps
$env:RECOMP_AUDIO_BUF_COUNT   = "6"    # in the APU thread that underran the default 43 ms (menu stutter)
$env:RECOMP_GUEST_CPUS    = $GuestCore   # all guest threads on one core, like the Xbox (CRI's suspend/resume scheduling needs it)
if ($Profile) { $env:RECOMP_PROFILE = "1"; $env:RECOMP_XF_TIME = "1" } else { Remove-Item Env:RECOMP_PROFILE, Env:RECOMP_XF_TIME -ErrorAction SilentlyContinue }
if ($GpuIdleUs -gt 0) { $env:RECOMP_GPU_IDLE_US = "$GpuIdleUs" }
else { Remove-Item Env:RECOMP_GPU_IDLE_US -ErrorAction SilentlyContinue }
# RECOMP_GUEST_LOCK=1 deadlocked at CRI's thread creation (main holds the lock
# while the new thread needs it to start), so make sure it is off:
Remove-Item Env:RECOMP_GUEST_LOCK -ErrorAction SilentlyContinue
# RECOMP_PB_WALK=dma makes D3D take its "sleep until the GPU interrupt" path,
# which the runtime doesn't deliver yet; keep the default segment scan.
Remove-Item Env:RECOMP_PB_WALK -ErrorAction SilentlyContinue

$p = Start-Process $exe -Wait -NoNewWindow -PassThru `
    -WorkingDirectory $Repo `
    -RedirectStandardOutput (Join-Path $Work "run_out.log") `
    -RedirectStandardError  (Join-Path $Work "run_err.log")
"Exit code: {0:X8}" -f $p.ExitCode
