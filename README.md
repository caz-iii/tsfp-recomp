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

Not started. See the progress table in [../CLAUDE.md](../CLAUDE.md).

## Planned layout

| Path | Purpose |
|---|---|
| `tools/ppcrecomp/` | PPC → C static recompiler (Gekko, including paired singles) |
| `src/runtime/` | Guest context, memory, function lookup for indirect calls |
| `src/hle/` | SDK replacements (gx, vi, pad, dvd, ax, card, os) |
| `src/assets/` | Disc readers and per-platform asset converters |
| `src/net/` | Online play (from PS2/Xbox reference) |

## Building

Requires CMake 3.16+, a C++17 compiler, and the SDL3 development libraries.

```
cmake -B build
cmake --build build
```

## License

Distributed under the MIT License. See `LICENSE` for more information.
