<p align="center">
  <img src="assets/amr-icon-sub.svg" alt="amr: auto modellista recompiled" width="320">
</p>

# Auto Modellista Recompilation

> Unofficial fan project. Auto Modellista is © Capcom; this project is not affiliated with or endorsed by Capcom.

A static recompilation of **Auto Modellista** (Capcom, 2003, original Xbox, USA/Japan release) into a native Windows executable, built on the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit.

The game's x86 code is translated ahead of time into C and compiled with MSVC. There is no emulator or interpreter at runtime. The toolkit's runtime libraries stand in for the Xbox kernel, Direct3D 8, DirectSound and the controller hardware.

> **Photosensitivity warning:** this is an early alpha. The screen can flash or flicker (for example a single black frame during a race). If you are sensitive to flashing images, take care or wait for a later release.

> **No game code or assets are included in this repository.** You need your own legally obtained copy of the game. Generated code is derived from the game and is never committed.

## Status

Alpha. On the GPU renderer the game boots, runs its menus at full speed, and full races (results and replay included) play at 15–40 fps.

There are two renderers. The **D3D11 renderer** (`run.ps1 -D3D11`) draws on the GPU and is the one to use. The original **CPU renderer** (the default) is kept as a reference and is much slower.

| Area | D3D11 renderer | CPU renderer |
|---|---|---|
| Boot, ESRB / logos / intro movie | ✅ | ✅ |
| Title screen, menus, save create / load | ✅ ~60 fps | ✅ 10–15 fps |
| Arcade → single race → car select (3D car preview) | ✅ | ✅ |
| Textures, transparency, sky | ✅ | ⚠️ alpha-tested scenery dithered, striped dialog background |
| Car shading | ✅ correct colours | ❌ black / flickering |
| In race (Suzuka, US speedway, Osaka tested) | ✅ 15–40 fps | ⚠️ slow |
| Pause menu | ✅ | ✅ |
| Menu and dialog text | ⚠️ a few letters can be missing | ✅ |
| Controller (XInput pads via the emulated USB controller), rumble | ✅ | ✅ |
| Audio (music, sound effects) | ✅ (stutter fix being tested) | ✅ |

See [`NOTES.md`](NOTES.md) for the detailed bring-up log: XBE facts, addresses, every fix and why.

## Requirements

- Windows 10 or 11, x64
- Visual Studio 2026 (or 2022) with **Desktop development with C++** (the free Community edition or the Build Tools are enough)
- CMake 4.2 or newer (needed for the Visual Studio 2026 generator)
- Python 3.10+ with `capstone` (`py -3 -m pip install capstone`)
- Git
- Your own Auto Modellista (USA, Japan) disc, dumped and extracted

### Dumping the game

Dump your own disc (for example from a modded Xbox or an RGH Xbox 360 over FTP). If you end up with an `.iso`, unpack it:

1. Open **[xiso.antangelo.com](https://xiso.antangelo.com/)** in your browser (the web version of [xdvdfs](https://github.com/antangelo/xdvdfs); nothing to install).
2. Choose **unpack**, select your `.iso`, and pick an empty output folder.
3. Use that output folder as the game folder below.

Command-line alternatives: [extract-xiso](https://github.com/XboxDev/extract-xiso) (`extract-xiso -x game.iso`) or the [xdvdfs CLI](https://github.com/antangelo/xdvdfs) (`xdvdfs unpack game.iso out\`).

The extracted folder must contain `default.xbe`, `media\` and `src\`. The unpacked game is about 1.5 GB even though the ISO is much larger; the rest of the image is padding.

Check you have the expected executable:

```
certutil -hashfile default.xbe SHA1
```

| File | SHA-1 |
|---|---|
| `default.xbe` | `4094e271290350248b06af87826896ffa84f2554` |

Every address in this project is specific to that exact XBE.

## Building

### 1. Clone with the toolkit

```
git clone --recurse-submodules https://github.com/RHiscool19/Auto-Modellista-Recompilation.git
cd Auto-Modellista-Recompilation
```

If you already cloned without `--recurse-submodules`: `git submodule update --init`.

The submodule is this project's fork of the toolkit
([RHiscool19/xboxrecomp](https://github.com/RHiscool19/xboxrecomp), branch `automodellista`): sp00nznet's
xboxrecomp plus the changes this port needs, so no patching step is needed.

### 2. Generate the recompiled code

```
powershell -ExecutionPolicy Bypass -File .\scripts\regen.ps1 -GameDir "C:\path\to\extracted\game"
```

This runs the xboxrecomp pipeline (disassembly, function identification, ABI recovery, x86 → C) and writes about 1.8 million lines of C to `src\recomp\gen\`. The first run takes around five minutes. Analysis output goes to `C:\AMRecomp\work` by default (`-Work` to change it).

After editing only `src\am_overrides.c`, the quick version is enough:

```
powershell -ExecutionPolicy Bypass -File .\scripts\regen.ps1 -RecompOnly
```

### 3. Configure and build

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DAM_GAME_DIR="C:/path/to/extracted/game"
cmake --build build --config Release
```

Use `-G "Visual Studio 17 2022"` with Visual Studio 2022.

Build options:

| Option | Default | Meaning |
|---|---|---|
| `AM_GAME_DIR` | `C:/AMRecomp/Source Files/Unpacked ISO` | Extracted disc the executable loads from |
| `AM_OPTIMIZE_GEN` | `ON` | Optimize the generated code. `OFF` builds everything at `/Od`: much faster to compile, much slower to run |
| `AM_GEN_NOOPT_FILES` | `recomp_0012.c` | Generated files kept at `/Od` because MSVC's optimizer stalls on their very large functions |

An optimized build takes a while. If it sits on one generated file for more than about five minutes, add that file to `AM_GEN_NOOPT_FILES`.

### 4. Run

```
powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1 -D3D11
```

`-D3D11` uses the GPU renderer. Leave it off for the CPU renderer. Plug in the controller before starting. Logs are written to `C:\AMRecomp\work\run_out.log` and `run_err.log`.

`run.ps1` sets the runtime settings this game needs:

| Variable | Why |
|---|---|
| `RECOMP_AC97_READY=1` | Without a ready audio codec, `DirectSoundCreate` fails and the game skips its engine init |
| `RECOMP_VBLANK=paced` | CRI's file streaming (ADX/ADXF) is driven by the 60 Hz vertical blank |
| `RECOMP_TITLE_KEVENTS=1` | CRI builds its own kernel events; without host events every wait fails instantly and spins |
| `RECOMP_GUEST_CPUS=0` | CRI schedules its threads with suspend/resume and assumes the Xbox's single CPU |
| `RECOMP_PB_EXEC=1` | Executes the game's GPU command stream so frames are drawn |
| `AM_D3D11=1` | With `-D3D11`: draws through the D3D11 renderer in `src/d3d11_backend.c`, in its own window |
| `RECOMP_STRIP_ALT_WINDING=1` | With `-D3D11`: sends every triangle of a strip with the winding the hardware uses, so back-face culling keeps them all |
| `RECOMP_FB_WINDOW=1` | Without `-D3D11`: shows the CPU-rendered frame buffer in a window |
| `RECOMP_USB=1` | The game's own input library talks to the Xbox USB controller; this emulates it and feeds it XInput pads |
| `RECOMP_CONTIG_AVOID_IMAGE=1` | Keeps GPU/USB memory clear of the game image's address range, which the USB model depends on |
| `RECOMP_HEAP_RECLAIM=1` | Reuses memory the game frees. Without it, textures from the first race are never given back and loading a second race crashes |
| `RECOMP_AUDIO_BUF_SAMPLES=1024`, `RECOMP_AUDIO_BUF_COUNT=6` | A bigger audio buffer (128 ms instead of 43 ms) so short hiccups in the emulated audio chip don't stutter |

`.\scripts\run.ps1 -Watchdog 120` kills the game after 120 seconds and dumps the guest call stack, which is useful for diagnosing hangs.

## Repository layout

| Path | Contents |
|---|---|
| `src/main.c` | Host entry point: loads the XBE, sets up memory, kernel, audio, USB, then calls the game's entry point |
| `src/am_overrides.c` | Hand-written replacements for specific game functions |
| `src/d3d11_backend.c` | GPU renderer: takes the decoded GPU commands and draws them with Direct3D 11 |
| `src/recomp_manual.c` | Indirect-call overrides and diagnostics (from the toolkit template) |
| `src/recomp/gen/` | Generated code (git-ignored; created by `regen.ps1`) |
| `config/seed_functions.json` | Function entry points the disassembler misses, found by running the game |
| `scripts/regen.ps1` | Runs the recompilation pipeline |
| `scripts/run.ps1` | Runs the game with the right runtime settings and logging |
| `scripts/apply-patches.ps1`, `patches/` | Old way of carrying the toolkit changes (kept for one release; the fork replaces it) |
| `external/xboxrecomp` | The toolkit (git submodule, fork RHiscool19/xboxrecomp, branch `automodellista`) |
| `NOTES.md` | Bring-up log |

## Known issues

- **Flashing.** The screen can occasionally show a single black frame in races. A guard now suppresses most of these, but keep the photosensitivity warning above in mind.
- **Race performance.** 15–40 fps depending on the track, not yet a steady 60. Frame hitches also cause audio stutter.
- **Text.** A few letters can still be missing from menu and dialog text.
- **Fog** is not drawn yet on the D3D11 renderer.
- **CPU renderer.** Slow, car bodies render black. Use `-D3D11`.
- **Occasional crash at startup** in the USB interrupt path, likely a timing race. Relaunching works.
- **Xbox Live** features are not supported.

## Artists wanted

Want to make art for the project, such as texture redraws, HD remakes of menu graphics, or icons and banners? Get in touch by [opening an issue](https://github.com/RHiscool19/Auto-Modellista-Recompilation/issues/new) on this repository with a short note about what you'd like to work on and a link to some of your work.

A few ground rules, so everything can be shared openly:

- **Original work only.** Redraw textures by hand or from your own photos and references. Ripped game files, and upscales or edits of them, can't be accepted, because they contain Capcom's data.
- **No official logos or trademarks** (Capcom's or real car makers' badges).
- **You'll be credited** here and in the companion app, and you keep the rights to your work. Say which licence you're happy to share it under.

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and contributors (MIT), which does the heavy lifting: the recompiler and the runtime libraries. Parts of its audio and GPU code come from [xemu](https://xemu.app) (LGPL-2.1+).
- Auto Modellista is © Capcom. This project is not affiliated with or endorsed by Capcom or Microsoft.
