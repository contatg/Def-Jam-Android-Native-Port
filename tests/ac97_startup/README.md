# AC97 startup publication fixture

The runtime starts its register-polling thread before the title installs its
AC97 bus-master traps. Page ownership and the seeded shadow must be visible
before `VirtualProtect` makes a page inaccessible.

This Windows fixture calls the real title implementation. Its replacement
protection call forces the polling-thread interleaving: it checks ownership
and emulates `mov cl, byte ptr [rax]` against the seeded byte before protection
returns. It also checks failed first/second protection calls and repeated init.
Protecting before publishing ownership fails deterministically.

With MSVC and Ninja active, run:

```powershell
cmake -S tests/ac97_startup -B build/validation-ac97-startup -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/validation-ac97-startup
ctest --test-dir build/validation-ac97-startup --output-on-failure --no-tests=error
```

For a regression negative control, configure a separate build with
`-DAC97_FIXTURE_SOURCE=<absolute-path-to-old-ac97_bm.c>`; compilation should
succeed and the registered test should fail. The fixture contains no game data.
