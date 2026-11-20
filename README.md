# armPSX

A PlayStation emulator for ARM and x86 handhelds and desktops. It aims for Mednafen's accuracy at a
speed that suits low-end ARM devices. Work in progress.

It has three modes: Fast (the default, timing like PCSX-ReARMed's), Balanced (like SwanStation's) and
Exact (Mednafen's). The CPU runs in an interpreter or, on x86-64 and AArch64, a recompiler. The
picture comes from the software renderer, or from the OpenGL or Vulkan renderer at up to 8 times
the resolution; those draw the picture only, and the emulation's VRAM stays the software
renderer's.

## What comes from where

- The GPU, GTE, CD-ROM controller, SPU, MDEC, timers and the controller and memory card protocols
  are C ports of Mednafen 1.32.1's PS1 module (GPL-2.0-or-later), with its notices kept.
- The CPU, bus, DMA, scheduler and disc reader (.cue/.bin, .chd, .m3u) are armPSX's own. CPU timing
  follows Mednafen's model.
- Where hardware tests ([ps1-tests](https://github.com/JaCzekanski/ps1-tests)) show Mednafen
  differs from a real console, armPSX follows the hardware.
- CHD support uses libchdr (BSD-3-Clause) in `deps/libchdr`.
- The GLES context code (`src/gles.c`) and the GL done with Vulkan (`src/vkgl.c`) come from armN64.
- The Vulkan renderer's shaders are compiled with glslang (Khronos; licenses in `deps/glslang`), and
  the Vulkan headers in `deps/vulkan` are Apache-2.0 OR MIT.

## Building

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

This builds `psxrun`, a command-line runner, and `armpsx_libretro.so`, a libretro core. The core
needs a BIOS in the system folder: `scph5501.bin` (USA), `scph5502.bin` (Europe) or `scph5500.bin`
(Japan).

    ./build/psxrun scph5501.bin 3600 game.chd

## License

GNU General Public License, version 2 or (at your option) any later version.
