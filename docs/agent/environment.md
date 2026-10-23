# Environment gotchas

Moved here from `AGENTS.md` §6; the two that cost the most time (kill the exe before building, and
the cost of a re-lift) stay in `AGENTS.md` too.

- Build with `scripts\build.ps1` (finds the newest VS Build Tools via vswhere; VS 2019 works, VS 2022 not required so far). CMake is 3.20, so `CMakePresets.json` must stay at version 2. On macOS and Linux use `python scripts/pipeline.py build` (`docs/build-macos-linux.md`).
- The toolkit forces `/O2` on its kernel library; `CMakeLists.txt` strips `/RTC1` from Debug so MSVC accepts it.
- Kill `defjam_recomp.exe` **before** building. A running process holds the exe and the link fails with LNK1168, which reads like a build error and is not.
- A full re-lift (`recomp.ps1`) plus build is about 20 minutes. Changing the lifter means re-lifting all 17k functions, so batch lifter changes rather than testing them one at a time.
- The runtime writes `xbox_kernel.log` into the CWD; it is gitignored.
- Ghidra 12.1.3 and JDK 21 are installed; the Ghidra naming pass is optional but makes logs readable.
- xemu is installed as the reference emulator but needs Vlad's own BIOS/MCPX/EEPROM.
- PowerShell 5.1 cannot pipe a here-string, and its `Set-Content -Encoding utf8` adds a BOM: write a commit message to a file and use `git commit -F <file>`.
- A shell that collapses backslashes (some agent shells do; the Claude Code one does, see `CLAUDE.md`) breaks Windows paths and `\n` inside C string literals written from a heredoc. Write the script to a file and run it, use forward slashes, or build the escape as `chr(92) + 'n'`.
