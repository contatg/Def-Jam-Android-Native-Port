# src/hooks — host-side code for the port

Hand-written C and C++ that sits beside the translated game code without
editing it. CMake picks up every `.c` and `.cpp` file in this directory.

| Files | Purpose |
|---|---|
| `d3d11_translator.c`, `nv2a_regs.c` | The Direct3D 11 device and window for the push-buffer translator; the graphics registers that cannot be plain memory |
| `ac97_bm.c` | The audio bus-master registers the game waits on at start-up |
| `pc_settings.c`, `pc_settings.h` | Player-facing settings, `settings.ini` and start-up paths |
| `pc_input.c`, `pc_input.h` | Gamepad, keyboard and mouse input and their settings |
| `pc_ui.cpp`, `pc_launcher.cpp`, `pc_ui_settings.c`, `pc_ui*.h` | The settings screen, shared by the launcher and the in-game overlay |
| `save_compat.c`, `save_compat.h` | Keeps profiles from earlier builds readable |
| `test_telemetry.c`, `test_observations.c` | Read-only game-state probes for the test harness (`RECOMP_TEST_OBSERVATIONS=1`) |
| `watchpoint.c`, `codeguard.c`, `postcmd.c` | Diagnostics: write watchpoints, code-overwrite detection, synthetic front-end commands |

Replacements for single game functions go in `src/recomp_manual.c`, not here.
