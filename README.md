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

In races the race fills the screen and the bottom screen becomes a HUD over it, like a console Mario Kart: the
racers list under the item box, the time at the top, a round map around you in the lower right corner, and the pause menu in the
middle. Touching them works like touching the DS's bottom screen. Outside races you get the big screen with the
small one in the corner.

There are a few extras:
- **SELECT + R** swaps the top and bottom screens.
- **SELECT + L** switches between that and showing just one screen.
- **Touch the top left corner** of the screen to open the settings. The game pauses while they are open; tap Back,
  tap outside the box or press Circle to go back. Tap a setting to change it (tap its left arrow to go back one), or use
  Up/Down to pick a setting and Left/Right to change it.
  - **Anti-aliasing**: Off, 2x or 4x, smooths the jagged edges of the 3D.
  - **Race screen**: **HUD** (the race fills the screen) or **Classic** (the big screen with the small one in the
    corner, like outside races).
  - **3D resolution**: 1x, 2x or 3x the DS resolution. 2x is the default.
  - **Mod**: None, or one of the mods you've added (see Mods). It applies when you restart the game.
  - **Unlock all**: tap Unlock (or press Cross on it) and restart the game to unlock every cup, character and kart.
    This changes your save for good. (Thanks to [mkds-android](https://github.com/RonyxDumb/mkds-android) for this, the
    round map, the mod support and some widescreen HUD fixes.)

## Mods
Put a mod for Mario Kart DS (Europe) in `ux0:data/mkds/mods/`: either an **.xdelta** patch, the usual way ROM hacks
are shared, or a ROM that's already patched (**.nds**). The file name is the mod's name. The next time the game starts,
it adds the mod (a progress bar shows it): the mod gets its own folder, `mods/<name>/`, with the game made from it and
its own save, and the .xdelta or .nds you put there is deleted. Then pick the mod in the settings under Mod and restart
the game. If a mod can't be added, the game says why and renames its file to `.failed`.

The game's code here is the port's own, not the ROM's, so a mod brings its files: every file of the game that the mod
changes (graphics, models, courses that take the place of the original ones, music, texts) is used. Courses, cups and
menus that a mod adds with its own code are not. With CTGP Nitro, for example, you get its title, menus and artwork
and its versions of the original courses, but not its new courses. When a mod changes the game's code, its sound
archive is left out too.

## Playing online
Nintendo WFC works out of the box through [Wiimmfi](https://wiimmfi.de), the community server that keeps DS online
play alive. Just connect your Vita to Wi-Fi and pick Nintendo WFC in the game: there's nothing to set up, the game
uses the Vita's own connection, so the game's Nintendo WFC Settings option is gone. The first time, the game creates your Wiimmfi account and remembers it in
`ux0:data/mkds/wfc.bin`, so keep that file if you move your save to another Vita.

## Files
| Where | What |
|---|---|
| `ux0:data/mkds/rom.nds` | your ROM |
| `ux0:data/mkds/mkds.sav` | your save |
| `ux0:data/mkds/language.txt` | the language you picked last |
| `ux0:data/mkds/settings.txt` | your settings |
| `ux0:data/mkds/wfc.bin` | your Nintendo WFC (Wiimmfi) account |
| `ux0:data/mkds/mods/<mod>/` | a mod you added: `game.nds` (the game with the mod) and `save.sav` (its save) |
| `ux0:data/mkds/mkds.log` | a short log of the last startup, plus any errors |

If something goes wrong, the log and any crash dump in `ux0:data/` (a `.psp2dmp` file) are the best things to share.

## Building it yourself
You'll need [VitaSDK](https://vitasdk.org) with the vitaShaRK, libmathneon and SceShaccCgExt packages
(install them with `vdpm`; vitaGL comes with the source in `vita/vitagl` and is built with anti-aliasing support), clang 17 or newer with the ARM target, CMake 3.22 or newer, and Ninja.

```bash
export VITASDK=/usr/local/vitasdk
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/vita/toolchain.cmake
ninja -C build
```

That gives you `build/mkds.vpk`. Clang compiles the code, and VitaSDK's GCC and tools link it and package the VPK.

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
- Tyler Zars and RonyxDumb for the Mario Kart DS decompilation
- Rinnegatamante for vitaGL
- GrapheneCt for CapUnlocker
- The Wiimmfi team for keeping DS online play alive
- XZ Embedded (Lasse Collin and contributors) for reading compressed mod patches
