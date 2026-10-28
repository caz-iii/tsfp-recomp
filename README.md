# tsfp-recomp

Native PC port of TimeSplitters: Future Perfect, built on SDL3 and OpenGL.

## About

tsfp-recomp is the playable PC port produced by the OpenMonkey project. The target is every single-player mode (Story, Arcade, Arcade League, Challenges, and the rest of the offline content), followed by online play based on the PS2/Xbox netcode.

The port is a native x86-64 executable. It uses a **hybrid** approach so that playability doesn't wait on the full decompilation:

1. **Static recompilation.** A build-time tool translates every PowerPC function in the GameCube `main.dol` into C. It uses function boundaries from `tsfp-decomp/config/G3FE69/symbols.txt` and runs against a guest CPU context and big-endian guest RAM. The host compiler turns the result into the executable. Nothing is emulated or interpreted at runtime.
2. **SDK replacement (HLE).** Dolphin SDK entry points are swapped, by symbol name, for native implementations:

   | SDK | Replacement |
   |---|---|
   | GX | OpenGL renderer (TEV → shaders) |
   | VI | SDL3 window and present |
   | PAD/SI | SDL3 gamepad, keyboard, and mouse |
   | DVD | Host filesystem over extracted assets |
   | AX/AI/DSP/ARQ | SDL3 audio |
   | CARD | Save files |
   | OS | Threads, alarms, and timers on host primitives |
3. **Decomp swap-in.** Matched translation units from [tsfp-decomp](../tsfp-decomp) are linked through the same recompiler at first. Each subsystem is then ported to native, endian-aware C++ once it is fully decompiled, until the recompiled layer is gone.

### Assets

Users supply their own disc image of Future Perfect from any region or platform (GameCube `.iso`/`.ciso`/`.rvz`, PS2 `.iso`, Xbox `.xiso`). The asset tool reads each platform's containers (GameCube per-level `.war`, PS2/Xbox `.pak` bundles), converts textures and audio to common formats, and writes a loose-file tree for the game to load. No game data is distributed.

## Status

Boots the NTSC-U GameCube release natively through the intro movies and the front end into
Story mode mission 1, which is playable, at about 60 fps. Missing: memory card saves (the
game offers "Continue without Saving"), fog and indirect-texture TEV stages, EFB copy
format conversion, and a listening pass over the audio mixer.

## Layout

| Path | Purpose |
|---|---|
| `tools/ppcrecomp/` | PPC -> C++ static recompiler (Gekko, including paired singles) |
| `src/runtime/` | Guest state and memory, indirect calls, interrupts, OS context switching, idle fiber |
| `src/hw/` | Native hardware: PI, VI, DI virtual disc, EXI (RTC/SRAM), SI pads, AI/DSP + AX mixer, ARAM |
| `src/gx/` | GX FIFO parser and the OpenGL renderer (TEV -> GLSL, texture decoding, EFB copies) |
| `src/video/` | SDL3 window, GL context, EFB framebuffer, presentation |
| `src/hle/` | The few guest functions replaced natively (`hle_functions.txt`) |

## Building

Requires CMake 3.20+, MSVC (Visual Studio 2022+), and Ninja. SDL3 is downloaded at configure
time (pinned version, hash checked). The build also needs your own `main.dol` and the decomp's
`symbols.txt` (defaults: `../tsfp-decomp/orig/G3FE69/sys/main.dol` and
`../tsfp-decomp/config/G3FE69/symbols.txt`; override with `-DTSFP_DOL=` / `-DTSFP_SYMBOLS=`).
Generated code is written to the build directory only.

```
cmake -B build/msvc -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/msvc
```

## Running

Pass the extracted GameCube disc directory (containing `sys/` and `files/`):

```
build/msvc/tsfp.exe "path/to/TimeSplitters - Future Perfect (USA)"
```

Controls: a connected gamepad in the GameCube layout, or the keyboard:

- WASD to move, IJKL to look
- Space = A, Backspace = B, E = X, Q = Y
- Enter = Start, Tab = Z
- Shift = L, Ctrl = R
- arrow keys = D-pad

Options (environment):

| Variable | Effect |
|---|---|
| `TSFP_SCALE=n` | internal resolution multiplier (default 2) |
| `TSFP_MUTE=1` | no audio |
| `TSFP_HEADLESS=1` | hidden window |
| `TSFP_DUMP_DIR=dir`, `TSFP_DUMP_EVERY=n` | save every n-th frame as BMP |
| `TSFP_INPUT="frame:BUTTONS,..."` | scripted pad presses |
| `TSFP_WATCHDOG=s` | periodic state and backtrace dump (0 = off) |

## License

Distributed under the MIT License. See `LICENSE` for more information.
