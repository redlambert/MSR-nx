# Upstream projects (reference material)

Pinned to the commits read on 2026-11-07. None of these dictate our design.

| Project | Commit | License | What it gives us |
|---|---|---|---|
| gminustwo/msr2ds | a8f3f96 (v2.2.0, 2026-11-21); first read at 0d96ccf (2026-11-06) | GPL-2.0-or-later (file headers: "GPLv2 or any later version"), so usable in a GPL-3 port | Samus Returns function discovery (`tools/msr/recomp/analyze.py`, `extra_entries.txt`), the frame limit (`frame_rate.h`), the amiibo and difficulty extras and the Fusion suit (`cheats.h`, `native/runtime/msr/patches.cpp`), and since 0.7.0 its fallback interpreter (`native/runtime/interp_full.c`, copied as `runtime/cpu/interp_arm.c`). Since its 2.0 (2026-11-04) msr2ds is a static recompilation too (msr-native: its own recompiler, Vulkan and OpenGL renderers on SDL), no longer built on Azahar; its runtime and renderer are not ours, its findings about the game are. |
| JohnUzoka/TriAevum-Switch | 3546da8 (2026-11-05) | GPL-3.0-or-later | 3DS ARM11 AOT generator and Switch runtime for OoT3D; deko3d renderer (`docs/DEKO3D_PROGRESS.md`): precompiled shader package plus parametric uber fallback; Horizon threading, audio, pacing lessons (`docs/PORTING_GUIDE_3DS_SWITCH.md`). |
| GustavoHenriqueRocha/TriAevum-Switch | 62a6ac4 (2026-10-24) | GPL-3.0-or-later | The Portuguese original of the above. |
| fearkov/3dsrecomp | df82e6e (2026-11-04) | MIT (Cargo.toml; repo has no LICENSE file) | Generic 3DS function discovery and ARM/Thumb/VFP to C. |
| fearkov/zakuro | c856685 (2026-11-05) | MIT | 3DS emulator built to run recompiled code: HLE services, PICA, audio. |

## Notes

- Since 0.7.0 the repo is GPL-3.0-or-later (msr2ds's interpreter is in it). The earlier MIT code
  keeps its notice in LICENSE-MIT. MIT pieces (Zakuro, 3dsrecomp) fit either way.

## uam (Switch build)

- devkitPro/uam at 5a5afc2bae8b55409ab36ba45be63fcb73f68993, zlib license (mesa-imported/ parts MIT).
  The deko3d shader compiler, built for the Switch by tools/switch/uam/build.sh and linked into
  msr.nro so fragment shaders missing from the pack are compiled on the console.
