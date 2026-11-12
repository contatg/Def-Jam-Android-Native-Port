# Guest paths on a case-sensitive POSIX file system

Xbox file systems ignore case, and titles rely on it: Def Jam opens
`D:\GCONFIG.XML` and `D:\FONTS\FONTS.VIV` while its disc holds `gconfig.xml`
and `fonts/fonts.viv`. Windows and a default macOS volume ignore case as well,
so passing the title's spelling through worked everywhere except Linux, where
the opens failed and the title carried on with the data missing.

```
cmake -S tests/kernel_path_posix -B build/kernel-path
cmake --build build/kernel-path
ctest --test-dir build/kernel-path --output-on-failure
```

No game files are needed; the test builds a small tree under `/tmp`.

## What it pins down

1. **An existing file is found whatever case the title used,** directories
   included, and an exact spelling is still taken as it is.
2. **A file that does not exist yet keeps the title's spelling,** inside the
   directory that does, so a save is created where the title will look for it.
3. **Nothing above the mapped root is rewritten.** The game and save folders
   are the player's; a `Game` folder beside a `game` folder stays `Game`.

The lookup only runs when the exact path does not exist, so a title that
spells its paths the way the disc does pays nothing for it.
