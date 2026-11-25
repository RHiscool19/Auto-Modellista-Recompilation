# Auto Modellista Recompilation

A static recompilation of **Auto Modellista** (original Xbox, USA/Japan) into a native PC executable, built on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp). A Nintendo Switch homebrew build is the longer-term goal.

No game code or assets are in this repository. You need your own copy of the game.

## Requirements
- Windows 10/11, Visual Studio 2022, CMake 3.20+
- Python 3.10+ with `capstone`
- A dump of your own disc. The expected `default.xbe` SHA-1 is `4094e271290350248b06af87826896ffa84f2554`

## Layout
- `game_files/` — your extracted disc contents (git-ignored)
- `src/` — host entry point, manual function overrides, game-specific runtime code
- `src/recomp/gen/` — generated C from the recompiler (git-ignored, regenerate locally)
- `NOTES.md` — XBE facts, address notes and bring-up status

## Status
Early bring-up. See `NOTES.md`.
