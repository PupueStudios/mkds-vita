# Mario Kart DS Vita

This is a native PS Vita port of Mario Kart DS, made from the Mario Kart DS decompilation. It's not an emulator:
the game's own code (in `game/`) runs directly on the Vita, and the parts of the DS it relies on (graphics, sound,
timers, save chip, wireless and so on) are rebuilt in `platform/`. The 3D is drawn by the Vita's GPU.

> [!CAUTION]
> This project was made with AI assistance.

There's no game data in here, so you'll need your own copy of the game.

## What you need
- A PS Vita or PS TV with HENkaku / Ensō.
- **CapUnlocker**, which lets the game use the Vita's fourth CPU core. The port needs it and will tell you at startup
  if it's missing. To install it, copy `CapUnlocker.skprx` to `ur0:tai/`, add the line `ur0:tai/CapUnlocker.skprx`
  under `*KERNEL` in `ur0:tai/config.txt`, then reboot and run HENkaku again.
- **libshacccg.suprx** in `ur0:data/`. This is the shader compiler; you can extract it once with ShaRKBR33D.
- Your own dump of **Mario Kart DS (Europe, AMCP)**.

## Installing
1. Copy your ROM to `ux0:data/mkds/rom.nds`.
2. Install `mkds.vpk` with VitaShell and start the game.

Each time the game starts you'll be asked for a language (English, Français, Deutsch, Italiano or Español). The one
you picked last time is already selected.

## Controls
The controls are the same as on the DS: Circle, Cross, Triangle and Square are A, B, X and Y (by where they sit on
the pad), the D-pad or left stick steers, and L, R, START and SELECT work as usual. Hold SELECT to blow up your
balloons in Balloon Battle. The front touch screen is the DS touch screen.

There are two extras:
- **SELECT + R** swaps the top and bottom screens.
- **SELECT + L** switches between showing both screens and just one.

## Files
| Where | What |
|---|---|
| `ux0:data/mkds/rom.nds` | your ROM |
| `ux0:data/mkds/mkds.sav` | your save (nickname, records, ghosts, missions) |
| `ux0:data/mkds/language.txt` | the language you picked last |
| `ux0:data/mkds/mkds.log` | a short log of the last startup, plus any errors |

If something goes wrong, the log and any crash dump in `ux0:data/` (a `.psp2dmp` file) are the best things to share.

## Building it yourself
You'll need [VitaSDK](https://vitasdk.org) with the vitaGL, vitaShaRK, libmathneon and SceShaccCgExt packages
(install them with `vdpm`), clang 17 or newer with the ARM target, CMake 3.22 or newer, and Ninja.

```bash
export VITASDK=/usr/local/vitasdk
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/vita/toolchain.cmake
ninja -C build
```

That gives you `build/mkds.vpk`. Clang compiles the code, and VitaSDK's GCC and tools link it and package the VPK.

## What's where
| Folder | What's inside |
|---|---|
| `game/` | the game's code from the decompilation, plus the DS SDK libraries it uses |
| `include/` | the game's headers, a few replaced for the port, and `port_prefix.h` |
| `msl_include/` | the C library headers the game is compiled against |
| `platform/` | the DS hardware the game expects: 2D, 3D, sound, timers, DMA, save chip, OS services |
| `platform/vita/` | the Vita side: startup, memory, threads, controls, audio, frame timing |
| `platform/gl/` | the 3D renderer and the code that puts both screens on the Vita's display |
| `vita/` | the toolchain file and the LiveArea images |

## How it works, briefly
- **Memory.** The DS's memory (RAM, hardware registers, palettes, video memory...) lives in normal memory blocks, and
  every fixed DS address in the game goes through `DS_ADDR()` / `DS_PTR()` to find its place.
- **Threads.** The game runs on the first CPU core, drawing to the screen on the second, the 2D graphics and sound on
  the third, and the 3D geometry on the fourth (that's what CapUnlocker is for).
- **3D.** The game's 3D commands are processed on their own thread. Models that don't change are turned into GPU
  meshes once and then drawn, lit and textured by the GPU, following the DS's own rules.
- **2D.** The DS's 2D layers are drawn on the CPU (only the lines that changed, when possible) straight into GPU
  textures, and the GPU blends in the 3D.
- **Frame timing.** The game runs in step with the Vita's screen, so every frame is ready just before it's shown.

## Credits
- **Mario Kart DS decompilation** by Tyler Zars and RonyxDumb
- **[vitaGL](https://github.com/Rinnegatamante/vitaGL)** by Rinnegatamante
- **[CapUnlocker](https://github.com/GrapheneCt/CapUnlocker)** by GrapheneCt
