> Este documento descreve o projeto de PC original. No patch Android v37, a tela e os caminhos estao descritos em LEIA-v37.md; idioma frances nativo e os outros recursos exclusivos do PC nao foram integrados.

# Language packs

A language pack is a **mod** that plays the game in another language: menus, Story subtitles, memory-card
messages, the fonts that draw them and the menu artwork with words in it. With no language mod on, the game
is in its own English. The port ships no translation; packs are made and hosted by their translators, and
this page is how they work.

A pack holds **only what its translator changed**, as small binary differences. At start-up the game
rebuilds the files the pack lists from **your own dump**, once, and opens those copies instead of the
originals. Your dump is never modified, and a pack can be shared without sharing the game.

## Playing in another language

1. Install the pack like any other mod: from the **Mods** page's project catalogue when it is listed
   there, or by unzipping its folder into the mods folder (**Mods → Open Mods Folder**):

   ```text
   <data>/mods/es-zarf/language.ini
   <data>/mods/es-zarf/patches/...
   ```

2. On the **Mods** page, tick it under **Installed mods**. Only one language is on at a time: ticking
   another turns the first off. Untick it to play in English. It is saved in `settings.ini` as:

   ```ini
   [language]
   pack = es-zarf
   ```

3. Play. The first start with a pack rebuilds its files (a few seconds; the copies, up to about 130 MB for
   a pack that translates menu art, go in `<data>/cache/language/<pack>`). Later starts reuse them until the
   pack or your dump changes.

Deleting the pack's folder and its cache folder removes it completely. A pack that does not match your dump
is not used: the game starts in English and the log says why (`[LANG] language pack '...' not used: ...`).
Packs are made for the USA dump that Setup asks for.

### Subtitles in every Story cutscene

The game has English subtitles for every Story cutscene (40 scenes, timed in `assets/subtitle.viv`) but
shows them only in the opening one, unless its language is French. `[language] story_subtitles` changes
that, with or without a language mod:

| Value | Meaning |
|---|---|
| `auto` (default) | What the language pack asks for (a translation normally asks for every cutscene); with no pack, as the game does |
| `all cutscenes` | Subtitles in every Story cutscene, in any language, English included |
| `as the game does` | The opening cutscene only |

The launcher has it under **Interface → Language and subtitles**. Env `RECOMP_STORY_SUBTITLES` and
`RECOMP_LANGUAGE_PACK` override the file for one run (the Mods page then shows the language as locked).

### The game's own French

The disc has a French translation of its own, `cmfren.loc` (2,825 of its 3,068 strings translated; the
other entries are names, song titles and 69 gym tips left as "WAITING FOR TRANSLATION"), with French
memory-card messages. The USA release never uses it: it always starts in US English. `[language] game =
french` (launcher: **Interface → Language and subtitles → Game language**, env `RECOMP_GAME_LANGUAGE`)
starts the game in French: menus, create-a-fighter, results and the Story subtitles, which the game then
shows in every cutscene by itself. The voices are English, and so is the menu icon art, which the disc has
in English only. No download is needed.

A language mod takes the place of this setting: a pack translates the English string table, so while one is
on the game starts in English for the pack to replace, and the log says `[LANG] game language french not
used: ...`. A French language mod could still carry art or text the built-in translation lacks.

## With texture packs (HD, widescreen menus)

The two never touch the same files: a language pack replaces game data (string tables, fonts, archives),
a texture pack replaces images as they are drawn. A language mod is therefore **not part of the Load
Order**: it changes the game's own images before any texture pack sees them, so it always sits beneath every
texture mod, wherever the others are.

Texture packs find what to replace by the original image's content. An image the translator redrew is a
different image, so:

- **Nothing English is drawn over the translation.** English HD replacements of a translated image simply
  stop matching, and the translated image shows at the game's own resolution.
- **Everything else keeps its HD replacement.** Fighters, venues and every image the translation left alone
  look as before.
- **A widescreen extension tied to a translated image disappears.** HD Widescreen Menus extends some menu
  backgrounds beside the 4:3 picture; where the anchoring image was translated, that side shows the plain
  side fill instead.

Measured for the Spanish pack (177 translated images): Faithful HD has HD versions of 176 of them,
HD Widescreen Menus 1.3.0 of 170 (one also anchors the main menu's left brick wall), 1.1.0 of 4, and
HD Fighters of none. Those screens mix HD and original-resolution images until someone upscales the
translated ones. HD versions of the translated images belong **in the language mod itself** (see
[Its own HD images](#its-own-hd-images)): they load whenever the mod is on, beneath every texture pack,
and since no English pack replaces a translated image, nothing covers them. Keep the usual order for the
rest (HD Widescreen Menus above Faithful HD).

## Making a pack

You need the USA dump's files, a copy of them with your translation in place, and Python 3.8 or newer
(`scripts/langpack.py` uses only the standard library).

1. Translate in a copy of the dump's folder, with whatever tools you use for EA's BIG archives, string
   tables, fonts and SHPX images. Change existing files and archive entries; do not add or remove any.
2. Write a `[language]` header (below) to a file, then:

   ```sh
   python scripts/langpack.py make <dump> <translated copy> --out <pack folder> --info header.ini
   python scripts/langpack.py check <pack folder> --game <dump>
   python scripts/langpack.py info <pack folder>
   ```

   `make` finds every changed file, and every changed entry inside the archives (nested ones included,
   compressed or not), and writes one delta per entry. `info` shows how many bytes of each are yours.
3. Copy the folder to the mods folder, tick it on the **Mods** page and play. Look for the `[LANG]` lines in
   the log.
4. Share the folder (zipped, the folder at the root of the zip) with a licence and credits file. It contains
   your changes only. For the project catalogue, a version declares the capability `language-v1` and a
   `min_recomp` of 0.13.0 or later (plus `textures-v1` when it has its own images); the installer accepts
   `.ini`, `.djd`, `.png`, `.dds`, `.md`, `.txt` and `.json` files in it.

Where things are, on the USA disc:

| What | Where |
|---|---|
| Menu, HUD and Story text, including subtitles | `assets/misc.viv`, entry `cmusen.loc` (UTF-16 string table; `@` and `#` are the game's line-break markup). The game loads this table for its USA language; put your translation here and no code change is needed |
| Memory-unit messages | `assets/misc.viv`, entry `mcusen.loc` |
| Subtitle font (add the accented letters your language needs) | `fonts/befonts.viv`, entry `Subtitl.xfn` |
| Fight HUD art | `assets/misc.viv`, entry `hud.xsh` |
| Menu art with words in it | `screens/screens.viv`, one archive per screen, its `.xsh` entry |
| Story cutscene subtitle timing | `assets/subtitle.viv` |
| The "LOADING" label and other images or strings built into the program | `default.xbe`, `.data` and `.rdata` only |

`default.xbe` is special: its code was converted to C when the game was built, so a pack may change only
its data sections (an embedded image, a string). `make` refuses a change anywhere else; behaviour changes
need a runtime patch in the port, like the Story subtitle option above.

### Its own HD images

A language mod may carry higher-resolution versions of the images it translates, so players install one
mod and get the translation in HD. Put a `manifest.ini` beside `language.ini`, in the texture-pack format
([texture modding](texture-modding-getting-started.md)), and the PNGs it lists:

```ini
[pack]
schema=1
name=Español HD images
png_mips=opacity

[textures]
<texture ID of the translated image>=textures/<texture ID>.png
```

The key is the ID of the **translated** image as the game draws it (capture it with the Spanish mod on, or
take the archive `source_id` of the translated entry), never an ID computed from the HD PNG. When the mod
is on, the game adds its folder to the texture-pack list beneath every texture pack (log:
`[LANG] es-zarf: its own images load beneath the texture packs`), even with texture packs switched off.
The Mods page and the installer check this manifest like any texture pack's. A `[wide]` section works too.

## Format

A pack folder holds `language.ini`, its deltas, optionally its own images (`manifest.ini` and PNGs, above),
and any other files (credits, licence, readme). A folder with `language.ini` is a language mod, with or
without `manifest.ini`; one with only `manifest.ini` is a texture pack.

```ini
[language]
schema=1
; name is shown on the Mods page; language is the ISO 639 code
name=Español
language=es
version=1.1.0
authors=ZARF (MOD-ZARF)
licence=CC BY-NC-SA 4.0
homepage=https://...
description=...
; all: subtitles in every Story cutscene (with [language] story_subtitles = auto)
story_subtitles=all

[files]
assets/misc.viv|cmusen.loc = patches/001-cmusen.loc.djd
screens/screens.viv|screens/intMain/mainMenu.big|mainMenu.xsh = patches/010-mainMenu.xsh.djd
default.xbe = patches/005-default.xbe.djd
```

- Comments are whole lines starting with `;` or `#`; text after a value is part of it.
- A target is a path in the dump (forward slashes, matched without regard to case), then `|` and an entry
  name for each archive level, as the game names them itself. ASCII letters, digits, `_`, `-`, `.` and `/`;
  no `..`. Delta paths are relative to the pack and end in `.djd`.
- A delta (`DJDELTA1`) states the size and SHA-256 of the original it applies to and of its result, then
  copy (from the original) and add (new bytes) operations. It applies to an entry's **decompressed** bytes,
  so it does not depend on how the archive compressed it. Little-endian; `scripts/langpack.py` is the reference.
- Rebuilding an archive: each changed entry is appended (aligned like the archive's own entries), RefPack
  again as literal blocks when the original was compressed, and the directory pointed at it; every other
  entry keeps its bytes and offset. ZARF's console patch lays archives out the same way, so the game is known
  to accept it.
- Every source is checked against its hash before anything is used; one mismatch and the pack is not used.
  The cache is rebuilt when `language.ini`, a delta, or the size or time of a dump file it reads changes.

The runtime side is `src/hooks/pc_language.c` (the kernel's `g_xbox_game_file_redirect` serves the copies) and
`src/patches/subtitles.c`; `tests/unit/test_langpack.py` checks that the game's rebuild and `langpack.py apply`
produce identical files.

## Known packs

| Language | Pack | Author | Licence |
|---|---|---|---|
| Spanish | `es-zarf`, from "Parche textos en español DJFFNY" v1.1 ([source](https://github.com/MODZARF/Parche-textos-en-espanol-DJFFNY-Compatible-con-Xbox-360)) | ZARF (MOD-ZARF) | CC BY-NC-SA 4.0; a conversion exists and awaits its author's agreement before it is shared |

French text is on the USA disc already (`cmfren.loc`); a French pack would point the USA slot at it.
A Brazilian Portuguese translation exists for the European PS2 release; porting it needs its author's permission.
