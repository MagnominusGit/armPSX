# armPSX

A PlayStation emulator for ARM and x86 handhelds and desktops. It aims for Mednafen's accuracy at a
speed that suits low-end ARM devices. Work in progress: it runs as an interpreter for now, and a
recompiler is planned.

## What comes from where

- The GPU, GTE, CD-ROM controller, SPU, MDEC, timers and the controller and memory card protocols
  are C ports of Mednafen 1.32.1's PS1 module (GPL-2.0-or-later), with its notices kept.
- The CPU, bus, DMA, scheduler and disc reader (.cue/.bin, .chd, .m3u) are armPSX's own. CPU timing
  follows Mednafen's model.
- Where hardware tests ([ps1-tests](https://github.com/JaCzekanski/ps1-tests)) show Mednafen
  differs from a real console, armPSX follows the hardware.
- CHD support uses libchdr (BSD-3-Clause) in `deps/libchdr`.

## Building

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

This builds `psxrun`, a command-line runner, and `armpsx_libretro.so`, a libretro core. The core
needs a BIOS in the system folder: `scph5501.bin` (USA), `scph5502.bin` (Europe) or `scph5500.bin`
(Japan).

    ./build/psxrun scph5501.bin 3600 game.chd

## License

GNU General Public License, version 2 or (at your option) any later version.
