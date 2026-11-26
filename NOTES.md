# Auto Modellista static recomp — notes

## Source dump
- Disc: Auto Modellista (USA, Japan), full ISO 7,825,162,240 bytes
- `default.xbe` SHA-1: `4094e271290350248b06af87826896ffa84f2554` (4,235,264 bytes)
- Every address below is only valid for this exact XBE.

## XBE facts
- Title: "auto modellista", Title ID `4343000E` (Capcom)
- Retail build, base `0x00010000`, entry `0x0017A750`, kernel thunk `0x002BC0A0`
- 183 kernel imports
- XDK 1.0.5558 for all libs: XAPILIB, D3D8, D3DX8, XGRAPHC, DSOUND, XBOXKRNL, XONLINES, XVOICE, LIBCMT, LIBCPMT

## Sections
| Section | VA | VSize | What |
|---|---|---|---|
| .text | 00011000 | 1EC0EC | game code (~2 MB) |
| D3D | 001FD100 | 232E4 | XDK D3D8 (replace) |
| D3DX | 00220400 | 22B90 | D3DX8 (lift or replace) |
| XGRPH | 00242FA0 | 2470 | XGraphics helpers (swizzle etc.) |
| DSOUND | 00245420 | 2636C | XDK DirectSound (replace) |
| XNET | 0026B7A0 | 14248 | networking (stub) |
| XONLINE | 0027FA00 | 2CD64 | Xbox Live (stub) |
| PSGSFD00/_I/_B/_P | 002AC780.. | — | CRI Sofdec FMV decoder (lift) |
| XPP | 002B2B40 | 954C | XInput/peripherals (replace) |
| .rdata | 002BC0A0 | 6E090 | |
| .data | 0032A140 | 642344 | ~6.3 MB virtual, mostly BSS |
| DOLBY | 0096C4A0 | 7180 | DSP code |

## Engine / middleware
- No RenderWare: Capcom in-house engine.
- CRI middleware: ADX audio, Sofdec video (PSGSFD sections).
- Game data lives in CRI AFS archives: `src/afs/afs00.afs`, `afs01_x{j,p,u}.afs` (region variants), `afs02.afs`; ADX/AIX audio: `Open.aix`, `Open2.aix`.
- Precompiled NV2A shaders on disc: `media/Shaders/*.xvu` (vertex microcode), `*.xpu` (pixel). The cel shading is probably in here.
- Xbox Live linked in: XONLINE/XNET need to report "no network" cleanly.

## Pipeline results (xboxrecomp, Nov 2026)
Analysis output lives outside the repo in `C:\AMRecomp\work\` (derived from game code).
- disasm (all code sections): 17,011 functions, 965,659 instructions, 172,310 xrefs, 10,511 strings
  - .text 12,936 · D3D 754 · D3DX 618 · DSOUND 792 · XNET 486 · XONLINE 1,061 · XPP 275 · XGRPH 35 · Sofdec (PSGSFD*) 54 · DOLBY 0 (DSP code, correctly skipped)
- func_id: 23,351 entries after adding 6,340 vtable thunks; only 11 CRT functions matched by signature, 11,660 unknown
- abi_analysis: 17,011 entries, 772 thiscall
- recomp detected: __SEH_prolog 0x001F030C / 0x001FC730, __SEH_epilog 0x001F0347, setjmp 0x001F07B0, longjmp 0x001F0734

## Status
- [x] Dump extracted, XBE hashed and parsed
- [x] disasm, func_id, abi_analysis
- [x] Game project scaffolded from templates/new-game (entry 0x0017A750, AM_GAME_DIR CMake option)
- [x] recomp → src/recomp/gen: 17,020/17,020 functions, 0 failed, 1,811,390 lines in 18 files (35 s on Windows)
  - 1 unresolved call target stubbed
  - 220 unimplemented instructions (logged at runtime via RECOMP_UNIMPL):
    - 209x `aaa` around 0x0021A188, inside the D3D section: almost certainly data decoded as code (0x37 = '7'); D3D gets replaced anyway
    - cli/sti (3/4), sgdt + ljmp at 0x00181198..0x001811A9: privileged code, looks like a reboot/hardware path in .text
    - `fninit` at 0x001F76B8 (CRT FPU init, runs at startup) and `fxtract` at 0x0019A3C4 (float math helper): real FPU ops worth implementing
- [ ] xemu reference run (boot + race on 2 tracks)
- [x] First build: VS 2026 (MSVC 14.52), Release with generated code at /Od + /MP (AM_FAST_GEN_BUILD=ON); ~10 s compile, 0 errors
  - Optimizing the generated code with /O2 stalled 25+ min on recomp_0012.c (7,000+ line functions), hence /Od during bring-up
  - C4717 "recursive on all control paths": sub_001AEF50, sub_001F0E23. Will stack-overflow if reached; check if a crash lands there
- [x] First runs (scripts/run.ps1)
  - Exit code 3 = the watchdog's own _exit(3) after RECOMP_WATCHDOG_SECS, not a crash
  - Without RECOMP_AC97_READY=1 the game prints "Cannot create DirectSound!" and stalls; with it, DirectSound and the APU come up
  - Boot reaches: save dirs TDATA/UDATA 4343000e, Font.xpr, ~300 vertex shaders from media/Shaders, then opens all AFS archives + Open.aix/Open2.aix
  - Then: worker threads created via PsCreateSystemThreadEx with start 0x001F0E2D; ICALL can't resolve 0x001F0E2D or 0x001F7E8B (not detected as functions), thread dies holding guest lock 0x003FBDB0, main thread deadlocks
  - Fixed by config/seed_functions.json (scripts/regen.ps1): 17,013 functions, no more ICALL failures
- [ ] Run 3: 4 worker threads start (ctx 0x001F0E2D) and read the AFS header ("AFS\0"), then stall
  - Main thread spins in sub_00011746 (loads 3 AFS partitions): polls sub_001AC530 -> sub_001AC020 (partition status) waiting for 3 = done
  - Workers spin on KeWaitForSingleObject / NtSuspendThread / NtResumeThread (~46M kernel calls in 120 s)
  - RECOMP_VBLANK=paced: vblanks fire (~60 Hz) and partition loading in sub_00011746 now COMPLETES (TOC reads @2048/@4096)
- [ ] Run 4: main thread moves on: main 0x78032 -> 0x77F58 -> 0x77B06 -> 0x77690 -> sub_00011960 (sync file load from AFS: open 0x1AC320, read 0x1AB8A0, poll 0x1ABC70) -> deep in CRI server (0x1ABF1E .. 0x1B737E)
  - Read @122421248 (10 KB) completes on the host side, but the poll loop never sees it finish
  - Workers still spin on KeWaitForSingleObject (~25M calls): fits "title builds its own KEVENTs" -> waits fail instantly. Next: RECOMP_TITLE_KEVENTS=1
  - Still 0 pushbuffer calls, 0 flips: nothing drawn yet
- [ ] Run 5 (+ RECOMP_TITLE_KEVENTS=1): spinning stops (63K kernel calls in 120 s instead of ~30M)
  - 3 files now load via sub_00011960 (10 KB @122421248, 36 KB @123990016, 16 KB @125874176); the 4th creates its event but its read is never issued -> blocked, not spinning
  - NtSuspendThread x10669 vs NtResumeThread x7286: CRI schedules its threads by suspend/resume, which assumes the Xbox's single CPU + strict priorities. On a multi-core host a resume can land before the matching self-suspend (lost wakeup). Next: RECOMP_GUEST_CPUS=0, then RECOMP_GUEST_LOCK=1
- [x] Run 6 (+ RECOMP_GUEST_CPUS=0 + RECOMP_GUEST_LOCK=1): FIRST WINDOW. 536 file reads, all loading done
  - Creates Z: cache files CACHE\LocalCache00.bin / LocalCache08.bin; 512-byte reads return END_OF_FILE (new empty files). Watch this
  - GPU: 640x480 surface, 2 clears, 1 draw (4 indices, fullscreen texture 640x480 A8R8G8B8 at 0x80493000 = splash image?), 1 flip. Window black
  - Draw coordinates degenerate (x 0..0, y 16..16): vertex positions wrong or not yet decoded
  - Main thread then sits inside the D3D library: main -> 0x77FFD -> 0x77DD2 -> 0x104DE6 -> D3D 0x207852 .. 0x21DBD0, probably waiting on a GPU interrupt/fence. Next: RECOMP_PB_WALK=dma
- [ ] Run 7 (GUEST_CPUS=0 + GUEST_LOCK=1 + PB_WALK=dma): regression, no window. Only worker #2 is created; main is stuck inside CRI's thread creation (sub_001F0EA1 -> 0x17FFA4) waiting for the new thread, which can't get the guest lock. GUEST_LOCK removed from run.ps1
- [ ] Later: Ghidra FidDb naming pass (only 11 CRT functions identified)

## D3D facts
- Device pointer: guest 0x0021DBC8 (stock XDK layout: write sequence at +0x2C, pointer to GPU progress at +0x30)
- sub_00209540 = D3D_BlockOnTime (fence wait). Patches NOP 0x40100 value 5 ("signal me" software method) and sleeps when the GPU is far behind
- Present path from the game: 0x00104DE6 -> sub_002077A0 -> sub_00207760 -> sub_00206C80 -> sub_00209850 -> sub_00209540
- Run 8 (window, 1 draw, 0 fps): main stuck in sub_00209540 with NV2A PUT=GET = exactly the "GPU read pointer problem" in the toolkit docs
  - Bring-up fix: xbox_Nv2aMirrorFence(0x0021DBC8, 0x2C, 0x30) in main.c; PB_WALK off
  - RESULT: boots! ESRB notice, logos and most of the intro movie, with audio, ~48-50 fps (stutters, some artifacts)
  - The "crash" was the watchdog: RECOMP_WATCHDOG_SECS is a fixed timer (Sleep N s, then dump + _exit(3)), not hang detection. run.ps1 now leaves it off
  - Honest fix for later: replace sub_00209540 (kick off first when fence == device+0x2C, then wait on the real semaphore via nv2a_pb_set_semaphore_target, bounded 250 ms)

## Run 9: title screen, attract demo, crash
- Reaches the title screen. No input (controller plugged in after launch; untested). Attract demo starts, drops to ~0.7 fps (in-engine 3D on the CPU rasteriser with /Od code), then C0000005
- Crash: write to guest 0xC0B62C6E, guest esp 0x012013D8 (ABOVE the main stack top 0x011FFFF0)
  - Chain: game sub_000CFCB0 -> sub_001067D0 (float wrapper) -> sub_001EEB84 (CRT sqrt) -> FP error path sub_001F2347 -> sub_001F789D -> sub_001F3423
  - sqrt of a negative number takes the CRT error path, which the lifted code gets wrong
  - Fix: src/am_overrides.c replaces sub_001EEB84 with native sqrt (NaN for negative input); regen.ps1 passes it to --exclude-manual

## Run 10
- sqrt override works (no generated sub_001EEB84; old crash gone)
- New crash just before the attract demo: read from 0xBDB88D18 inside D3D's pushbuffer code (sub_00209120 "how far behind is the GPU" <- sub_002096B0 <- sub_00209800 <- sub_00203D90 <- sub_002041C0 <- game sub_00122770 / sub_001040B0)
  - Suspect: the fence-mirror shortcut lets D3D reuse ring space/fences the executor hasn't consumed (toolkit docs warn about exactly this). Fix: honest replacement of sub_00209540 (stdcall, 2 args, ret 8)
- No controller input with a Series X|S pad: the game's statically linked XPP library talks to USB host-controller hardware, so it never sees XInput unless RECOMP_USB=1 (OHCI model). Added to run.ps1 with RECOMP_INPUT_DIAG=1
- Run 11: still no input. RECOMP_USB alone does nothing: xbox_OhciInit() (src/usb/ohci.c) is never called by the template's main.c. Added the call in main.c after memory init
- Run 12 (USB trace): OHCI up, pad connects on port 3, port reset OK, but every ED the driver hands over (HCCA 0x00488000, control ED 0x004890C0) reads as all zeroes
  - Cause: contiguous allocations get physical addresses inside the image's VA range (0x10000..0x97C838); ohci.c bus_resolve() treats such addresses as the image itself. Fix: RECOMP_CONTIG_AVOID_IMAGE=1 (run.ps1)

## Run 13-14: controller works, race reached
- RECOMP_CONTIG_AVOID_IMAGE=1 fixed USB: pad enumerates, input works, rumble reports flow
- Save data created (U:\1983DD2F3706\SaveMeta.xbx, GameData)
- Save/load dialogs: text and buttons fine, background image drawn as striped garbage (texture format/swizzle, or fence-mirror corruption)
- Menus ~8-10 fps
- Run 13 (intro played): hung on a grey loading screen after Arcade > Single race (60 fps, 1 draw, audio looped then stopped). Not reproduced yet
- Run 14 (intro skipped, watchdog 120 s): Arcade > Single race > track/BGM/car/settings > race start, ~10 fps. Watchdog stack is inside D3D drawing with GPU PUT != GET: it was running normally, not hung
- cli/sti at 0x002B9DB2/0x002B9DC3 (XPP section) reached, harmless no-ops

## Run 15: race start
- Go race -> loading screen -> Suzuka intro camera (~1-2 fps) -> crash C0000005
- 384x "ICALL failed to resolve 0x0003DAF0" right before it: function missed by disasm (gap before sub_0003DB10). Seeded in config/seed_functions.json; full regen needed
- Crash itself (read 0x846995F0, guest esp back at stack top 0x011FFFF0) is probably fallout from the failed calls; recheck after regen
- Car select spins the car in 3D and looks right

## Run 16-17: IN RACE
- Run 16: crash before window in the XPP USB interrupt path (DPC thread, esp 0x00A3FF5C, list walk read 0x7FFFFFF8). Did not repeat: likely a race between the OHCI model's interrupt thread and the driver. Fix properly later
- Run 17: Suzuka single race running, ~3 fps, 430 draws/frame. Track, sky, signage, HUD, minimap, tach all render
- Visual bugs: car bodies nearly black (lighting / cel-shading path: vertex programs or register combiners), speed digits partially drawn
- Race runs stably (unoptimized) at 2-3 fps in race, 6-15 fps in the intro flyover
- Cars: distant cars often correct (white/red/silver); near cars black, sometimes flicker to the correct colour. Nondeterministic -> suspect the fence-mirror shortcut (D3D reusing vertex/constant memory the executor hasn't consumed) before suspecting shader translation
- Alpha-tested scenery (ferris wheel, trees, fences) shows as dithered dots

## Performance step 1
- CMake: AM_OPTIMIZE_GEN=ON compiles generated code at /O2 except AM_GEN_NOOPT_FILES (default recomp_0012.c: four ~7,400-line functions 0x1E0E10..0x1E8300)
- Other big functions to watch if a file stalls: sub_0021A16B (13,580 lines, recomp_0013.c; compiled fine at /O2 before), sub_000C3FB0 (5,006 lines, recomp_0003.c)

## Performance measurements
- Optimized gen build (all but recomp_0012.c at /O2): menus 10 -> 10-15 fps, race unchanged ~2 fps
- RECOMP_NO_VSH=1 (vertex programs off, 3D mostly missing): menus 40-60 fps, race intro 19-20 fps, in race still ~4 fps with 480 draws
  - => in menus the vertex-program interpreter is the main cost; in the race something else dominates
- Suspect for the race: recomp_0012.c is still /Od and holds sub_001E0E10/001E0EE2/001E4870/001E8300: 13 KB straight-line fixed-point code (fild/sar/shl/add, no calls), reached only indirectly. Looks like hand-unrolled DSP, probably CRI audio decoding (audio stutters). Unconfirmed: profile next (Release now emits a PDB)
- Profile 1 (VS CPU Usage, 2 min, race): nv2a_pb_exec_walk_begin 44.6% total / 26.5% self (GPU executor thread), kernelbase 12.4%, ucrtbase 9.0%, sub_00209360 8.1%, ntdll 7.7%; "xbox_translate_path" 33.6% total as a thread root (almost certainly a mislabel from /OPT:ICF folding identical functions; ICF now off)
  - sub_00209360 = D3D KickOff: sets NV2A 0x100410 bit 16 and spins until the executor clears it (xbox_Nv2aAckBusyBits, every 256 words). The game thread busy-waits on the executor at every kickoff
  - => the single GPU-executor thread (vertex programs + triangle setup on the CPU) is the bottleneck, and the game thread waits for it
- Profile 2 (/OPT:NOICF, race): nv2a_pb_exec_walk_begin 23.8% self (40.4% total), nv2a_rc_eval 6.3% + nv2a_rc_unpack 6.1% self (register combiners per pixel), kernelbase 12.3%, ucrtbase 8.9%, gdi32 8.2% (framebuffer window blit/scale), sub_00209360 7.7% (KickOff spin), ntdll 6.6%
  - Still-implausible labels: apu_wav_write 3% self and xbox_missing_verbose 2.4% self are both trivial early-return functions in this config, so a block of time is still attributed to the wrong symbols. Treat small runtime-function names in these profiles as unreliable
  - Conclusion: per-pixel work in the CPU rasteriser (incl. combiners) is the largest single cost, then the GPU-thread handshake, then presenting via GDI

## Plan: Direct3D 11 render back end
- Register through nv2a_backend_register() (src/kernel/nv2a_backend.h): clear / draw / flip, triangles arrive in surface pixels with 1/w, colour, UVs, texture info, blend/depth/stencil state
- Gaps to close in the toolkit interface: register-combiner state and texture stages 1-3 are not passed to back ends (needed for the cel shading); reuse xbox_d3d8's combiner -> HLSL translator
- Follow the doc's advice: compare window z as-is, implement stencil, cull from projected positions
- Later: vertex programs on the GPU (xbox_d3d8 has an NV2A microcode -> HLSL translator too)
- Quick win to test first: present via the back end instead of GDI (gdi32 8%)

## Goals
Done (D3D11 renderer, 2026-11-25):
- ~~Menu speed~~: 60 fps on -D3D11 (vertex cache + GPU drawing)
- ~~Striped save/load dialog background~~: correct on -D3D11
- ~~Black / flickering cars~~: correct colours on -D3D11 (CPU renderer still shows it)

Open, in order (2026-11-25 evening):
1. **Osaka hi.way < 1 fps**: ICALL to unrecompiled 0x000E02A0 (5,130 failures). Seeded -> full regen + test.
2. **Glyphs**: description text fixed by whole-texture hashing; loading-screen glyphs still missing and menus fell
   60 -> 40 fps from hashing every texture whole every frame. Now: textures that change after creation are marked
   dynamic and re-hashed (whole) on every draw (one text texture reused for several strings per frame); others sparse
   per frame + whole every 32 frames, staggered.
3. **Audio hangs after a race / quit**: a voice keeps looping (one sound, then another; on exit a buzz + beeping).
   Also menu stutter remains (APU thread gaps up to 90 ms even with the 128 ms buffer).
4. **Combiners**: 6 programs in the whole game; 5 run. The 6th is fog (final 130C0300 = mix(fog, r0, fog.a)) -> pass
   fog colour + per-vertex fog factor next. Pause menu correct.
5. Race 15-35 fps (Suzuka/US speedway). Full single race + results + replay work.
6. Release v0.2.0-alpha after 1-2 test.
7. Later: GPU vertex programs, APU MMIO without exceptions, honest sub_00209540, USB ISR race, Switch branch

## D3D11 back end v1 (src/d3d11_backend.c)
- Opt-in: `.\scripts\run.ps1 -D3D11` (sets AM_D3D11=1, turns off RECOMP_FB_WINDOW; main.c registers it before the game starts)
- One B8G8R8A8 render target + D24S8 per guest colour surface; presents the last >=512-wide surface, letterboxed 4:3
- Triangles arrive in surface pixels with 1/w; VS rebuilds clip space (x,y,z)*w so UVs/colour interpolate perspective-correctly; window z / fixed scale (65535 or 16777215, sticky, never per draw)
- Culling per triangle on the CPU from projected positions; rasteriser cull off
- Blend / alpha test (in PS) / depth / stencil / colour mask mapped from NV2A GL enums
- Textures: nv2a_backend_decode_texture, cached by (offset,size,format,pitch), re-hashed sparsely once per frame; render-to-texture uses the GPU copy
- Missing in v1: texture stages 1-3, specular (v1) and fog into the combiners, mip levels, vertex programs still on the CPU, nothing written back to guest memory
- Combiners (2026-11-25): Nv2aBatch.combiner carries the program; ps_main runs an HLSL port of nv2a_rc_eval (cbuffer RC, b1)
  with v0 + t0 + factors; v1/fog/t1-t3 read as 0 / (0,0,0,1). AM_D3D11_COMBINERS=0 falls back to texture x diffuse.
  CPU reference for the pause menu: opaque lavender band, black/white rules, black square + underline on the selection.

## Post-transform vertex cache (menu speed, step 1)
- nv2a_vsh_interp already decodes each program once per upload (vsh_run_decoded);
  the waste was running it once per *index*. raster_batch_program now keeps a
  4096-entry direct-mapped cache keyed by vertex index, valid for one batch,
  skipped when the program writes c[] (CXT_WRITE). RECOMP_XF_CACHE=0 disables.
- Prints `[XF-CACHE] N% of vertices reused` every 16M vertices.
- Lives in the xboxrecomp submodule (local changes, like the other runtime
  edits) -- these need to become a fork or a patch set before others can build.
- Step 2 (only if still slow): GPU vertex programs in the D3D11 back end, reusing
  d3d8_vsh_generate_hlsl. Needs the back-end interface to take raw attributes +
  constants instead of screen-space vertices, and clipping moved to the GPU.

## D3D11 v1 results (2026-11-25)
- Textures work after passing the P8 palette through Nv2aTexture (decode had forced palette=0).
- Black alternating triangles = strip winding: the executor emits (i,i+1,i+2) for every i;
  RECOMP_STRIP_ALT_WINDING=1 (now set by run.ps1 -D3D11) emits hardware winding.
- Menus/intro black: ~3 draws/frame, one samples a surface that is also a render target.
  Hypothesis: the CPU fills it. Back end now samples guest memory when its hash changed
  more recently than the last GPU draw into that target (stats line reports `guest N`).
- Race still ~4 fps: CPU side (pushbuffer decode, VSH interpreter, clipping) dominates.
- FIXED black menus: targets were recreated whenever the clip rect shrank (menus narrow it per
  panel) and every clear wiped the whole target. Now: grow-only targets sized clip_x+w, draws
  scissored to the clip rect, clears limited to CLEAR_RECT via ID3D11DeviceContext1::ClearView
  (Nv2aSurface gained clip_x/clip_y/clear_*). Sky came back with it. Menus 60 fps.
- Remaining visual nits: ESRB fade shows in a region not full screen (partial depth clears are
  skipped; or blend/clip); some menu text glyphs missing (m, p, y, w, k in descriptions).
- Race: still ~3 fps vs 30-60 in the pre-race pan of the same scene -> CPU/game-side, profile next.

## Runtime changes live in patches/
external/xboxrecomp is sp00nznet's repo and can't be pushed to. Our edits to it are kept as
patches/xboxrecomp-automodellista.patch (regenerate with
`git -C external/xboxrecomp diff > patches/xboxrecomp-automodellista.patch`) and applied by
scripts/apply-patches.ps1. Never commit inside the submodule.
