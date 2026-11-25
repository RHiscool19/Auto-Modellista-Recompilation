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
- [ ] recomp → src/recomp/gen
- [ ] xemu reference run (boot + race on 2 tracks)
- [ ] First build
- [ ] Later: Ghidra FidDb naming pass (only 11 CRT functions identified)
