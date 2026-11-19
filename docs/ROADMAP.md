# Roadmap

## Core options: parity with SwanStation's list

What each of SwanStation's libretro options does for the player, and where armPSX stands. Only the
option list was read, never its code. armPSX's core has no options yet.

| Area | SwanStation options | armPSX now | Plan |
|---|---|---|---|
| CPU | Execution mode (interpreter, cached interpreter, recompiler) | interpreter + exact recompiler, recompiler always on | option: Recompiler / Interpreter |
| CPU | Overclocking 50-290% | none | **wanted**: CPU clock option (same range) |
| CPU | (none) | exact only | **wanted**: modes Exact (Mednafen-equivalent, today's code), Balanced (SwanStation-equivalent), Fast (PCSX-ReARMed-equivalent, the default) |
| CPU | Recompiler icache, block linking, fast memory (+ rewrite) | always exact icache; links on; RAM fast path inline | nothing to expose |
| Console | Region (auto/J/U/PAL), per-region BIOS, fast boot, 8 MB dev RAM | region from disc; one BIOS | region option, BIOS per region, fast boot (skip the logo) |
| CD-ROM | Region check, read thread, preload/pre-cache CHD, readahead, mute CD audio, read speedup 1-10x, seek speedup | CHD/cue read in the frame | **wanted**: fast disk mode (read and seek speedup); mute CD audio, preload; region check off |
| CD-ROM | Apply image patches (PPF) | none | PPF soft patches |
| Video | Renderer (software, OpenGL, Vulkan, D3D), resolution scale 1-16x, MSAA, true colour, scaled dithering, texture filtering, downsampling, shader precompile, threaded software | software, native resolution | **wanted**: NEON software renderer, then GPU renderer (scale, true colour, filtering, MSAA) |
| Video | PGXP (geometry, culling, perspective textures/colours, depth buffer, vertex cache, CPU mode, tolerance) | none | with the GPU renderer |
| Video | Widescreen hack, disable interlacing, force NTSC timings, 24-bit 4:3, 24-bit chroma smoothing | none | after the renderers |
| Display | Aspect ratio (4:3, 16:9, ..., custom, native, PAR 1:1), crop mode (none/overscan/borders), active/line start-end offsets | fixed 4:3, full picture | aspect + crop options |
| Memory cards | Card 1/2 type (libretro, shared, per game, per title, none), single card per playlist | card 1 as libretro save RAM | card 2, per-game/shared, none |
| Input | Analog (DualShock) incl. combo, force analog, sticks as d-pad, axis scale, rumble + bias, multitap (port 1/2/both, 8 pads), GunCon (crosshair, X/Y scale), NeGcon (deadzone, twist) | digital pad, 2 ports | DualShock + rumble first, then multitap, GunCon, Justifier/mouse, NeGcon |
| Frontend | Run-ahead, OSD messages, log level, per-game compatibility settings | none | save states first (needed for run-ahead and rewind), cheats |
| Texture replacement | VRAM write replacement, preload | none | later, if asked |
| Audio | Per-frame audio batching | batched per frame already | nothing to expose |
| MDEC | Old MDEC routines | Mednafen's MDEC | nothing to expose |

Missing outside the option list: save states (`retro_serialize`), cheats, disc-control is in.
