# Game extraction and Android setup

## 1. Extract your own original disc or ISO

Use your own original Xbox copy of **Def Jam: Fight for NY**. For the currently
supported dump, the platform is the original Xbox and the region is USA (NTSC-U).
An Xbox ISO/XISO must be extracted with a tool that understands Xbox disc images,
such as extract-xiso. Renaming `.iso` to `.zip` or `.xbe` does not extract it.
The app does not mount an ISO and does not contain the original game's data.

Keep **every extracted file and subfolder**, with its original name and directory
structure. Copy the contents of the extracted disc directory, not only its executable.
The executable is spelled **default.xbe**, not `defalt.xbe`.

## 2. Put the files in internal shared storage

The Android data root is `/storage/emulated/0/DefJamFFNY`, usually displayed by a
file manager as `Internal storage/DefJamFFNY`. Create `game` inside it and copy
all the extracted disc contents into `game`.

Expected structure (minimum checks, not a complete inventory):

```text
Internal storage/
  DefJamFFNY/
    game/
      default.xbe
      fighter.xml
      gamedata.xml
      gconfig.xml
      venue.xml
      assets/
      audio/
      fonts/
      motion/
      movies/
      screens/
      ...all other files and folders from your extracted disc...
    save/                 created/used by the port; keep existing saves
    settings.ini          created/updated by the port
    defjam-native.log     diagnostic log, replaced on a new game launch
```

- `game/default.xbe` is the original Xbox executable required by the port.
- `assets/` contains the game's assets; preserve every nested file and directory.
- `audio/` contains audio data; `fonts/` contains font resources.
- `motion/` contains motion data; `movies/` contains video resources.
- `screens/` contains screen resources. Keep the XML files and all additional
  extracted data alongside these folders exactly as supplied by your disc.
- `save/` belongs to your progress. Never replace it with somebody else's data or
  delete it during an update. Back up the existing data root before changing devices.
- `settings.ini` and `defjam-native.log` are runtime files, not disc files to extract.

Do not create an extra nested folder such as `game/Def Jam/default.xbe`.
Do not put an unextracted ISO in `game` or place files under Android's system `/`.
Root access is not required. On a device with removable storage, use the internal
shared-storage path above; the app currently uses that fixed location.

## 3. Open the app

Install the APK, allow the requested file access, and open the launcher. It must
show **Game found**. If it says `default.xbe not found`, check the exact path,
spelling, extraction nesting and storage permission. Finding the executable alone
does not prove the rest of the disc was copied correctly.

Start with internal resolution **1x**. Keep your existing settings and saves when
updating. Asynchronous rendering is enabled internally; parallel vertex preparation
is disabled. These are fixed settings, with no editing option.

## Diagnostics from the phone

If reporting a problem, collect the actual phone log immediately after the failed
attempt and before launching again. Do not substitute a PC-side log.

```powershell
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" pull /storage/emulated/0/DefJamFFNY/defjam-native.log "$env:USERPROFILE\Downloads\defjam-native.log"
```

Send the error and log to the maintainer, without disc files, save data or ISO images.
