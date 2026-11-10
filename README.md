# Deadstation Portable
A performance-focused PSP emulator supporting local multiplayer and downloadable recomp modules.

<img width="960" height="720" alt="sc_20261109_082630" src="https://github.com/user-attachments/assets/f73a171f-ff65-4217-b7a4-255ae22649a3" />
<img width="960" height="720" alt="sc_20261109_082440" src="https://github.com/user-attachments/assets/402a6169-83b8-4345-ac6a-4e6eaec61cfa" />
<img width="960" height="720" alt="sc_20261109_082322" src="https://github.com/user-attachments/assets/f8b59e53-77ba-401b-ad9b-2f451665250c" />

# Features
## Emulator
- Many games play well or close to full speed on the TrimUI Brick/Hammer.
- USB-C / Bluetooth device-to-device co-op play.
- In-app Recomp store for game mods

# Supported Devices
## Trimui Brick/Hammer
1. Install Deadstation Portable via the [NextUI](https://github.com/LoveRetro/NextUI) [Pak Store](https://github.com/LoveRetro/nextui-pak-store).
2. Put PSP games (`.iso`, `.cso`, `.chd` or `.pbp`) in a `/Roms/PlayStation Portable (PSP-DEADSTATION-PORTABLE)/` folder on your SD Card.
3. No PSP firmware or BIOS is needed.

# Credits
- [PPSSPP](https://github.com/hrydgard/ppsspp): the emulator Deadstation Portable is built from.

---

## AI Section

Notes for agents working on this repo. Everything above the line is the maintainer's: leave it as written.

Deadstation Portable is a PSP emulator for the TrimUI Brick (NextUI, tg5040) only, as `Emus/tg5040/PSP-DEADSTATION-PORTABLE.pak`, built from [PPSSPP](https://github.com/hrydgard/ppsspp) v1.20.4. It draws with Vulkan on the Brick's PowerVR GE8300 straight to the panel through fb0, with no surface or swapchain; SDL2 reads the pad and plays the sound. A recomp module is a separate executable that it hosts: it starts the module as a child process and sleeps until MENU, and the menu, the module's options and the store stay the emulator's.

### Layout

- `core/`: PPSSPP's emulation (`Common/`, `Core/`, `GPU/`, `ext/`), cut down to aarch64 Linux and Vulkan: no other platform or renderer, no menus of its own, no debugger, cheats, texture packs, filters or online services. The fork is detached: change it in place. Its assets are `core/assets/`. The emulator's own code is in `core/deadstation/`: the frame loop (`main.cpp`), the fb0 panel (`panel.*`), the sound (`audio.*`), the on-screen text (`osd.*`), settings, controls, save states and the menu's emulator side.
- `ui/`: the menu library, a submodule: pages, the module host, the store, and the tg5040 toolchain. Everything on the screen but the game is drawn on the CPU by it. The emulator's settings are its `options/psp.cpp`.
- `ffmpeg/`: PPSSPP's FFmpeg, a submodule, compiled from source (`shell/brick/ffmpeg.sh`): the PSP's movies and some of its sounds.
- `store/`: the recomp modules the store lists (`registry.txt`). An Action rebuilds the list on the `store` branch, hourly if GitHub keeps to its schedule, so run it by hand after publishing a module.
- `shell/brick/`: build and package scripts. `pak.json`: the Pak Store's entry.

Only source, build scripts and READMEs go in git; never disc images or anything made from them.

A game runs at 59.93 Hz and the panel with it: the DSI's frame is 788 lines instead of 784 while it plays (`panel.cpp`), and the game's vblank is set to that rate (`__DisplaySetPanelRate`), so one game frame goes to one panel frame. The sound is never stretched (`audio.cpp`): PPSSPP's resampler is gone, and a game behind its time leaves the speaker waiting. The kernel's console is off while playing (`launch.sh`): its messages hold up the display's flip.

### Build

On an x86-64 Linux host with clang, lld, CMake, Ninja, Docker (only to copy the toolchain image's sysroot), `vulkan-headers`, `glslang` and `curl` (headers).

```
git submodule update --init ui ffmpeg
ui/toolchain/toolchain.sh                # the tg5040 sysroot, out of ghcr.io/loveretro/tg5040-toolchain
ui/toolchain/devlibs.sh SERIAL           # the Brick's libcurl.so.4, over adb
shell/brick/ffmpeg.sh                    # build-brick/ffmpeg
shell/brick/build.sh                     # build-brick/deadstation/core/deadstation/deadstation-portable
shell/brick/package.sh                   # build-brick/PSP-DEADSTATION-PORTABLE.pak and its .zip
```

### Recomp modules

- Each is its own repository. A disc goes by its id as the PSP has it (`ULUS10391`, from `UMD_DATA.BIN`): the `disc` lines in a module's `module.cfg`.
- Listing one: add its repository to `store/registry.txt`. The store then offers its latest release.
- Any error of a module's shows the error with one button, `A TRY EMULATOR CORE`: Core becomes the emulator and the game starts again in it.

### Install

Copy `build-brick/PSP-DEADSTATION-PORTABLE.pak` to the SD card's `Emus/tg5040/`. Games go in a folder such as `Roms/PlayStation Portable (PSP-DEADSTATION-PORTABLE)/`: an `.iso`, `.cso`, `.chd` or `.pbp`. No PSP firmware is needed. Settings, the game's saves and save states are kept in `.userdata/tg5040/PSP-DEADSTATION-PORTABLE/`.

### Controls

MENU opens the menu (`ui/`, `core/deadstation/menu.cpp`): Continue, Save and Load (ten slots, with pictures), Options, Recomp Store and Quit. The Brick's A, B, X and Y are the PSP's Circle, Cross, Triangle and Square; L1 and R1 are L and R. The Brick has no stick: R2 swaps the d-pad between the PSP's d-pad and its analog nub, remembered for each game. FN2 saves a state, FN1 loads it. Controls remaps them all.

### Licences

GPL-2.0-or-later, as PPSSPP (`LICENSE`). FFmpeg is LGPL-2.1-or-later, built without its GPL parts; the rest of `core/ext/` is under its own licences, in its folders.
