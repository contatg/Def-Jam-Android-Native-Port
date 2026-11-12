# Def Jam Android Native Port

An unofficial Android ARM64 port of **Def Jam: Fight for NY**.

**Android port maintained by contatg.**

**Online multiplayer is based on FFNY Online, created by sweeax-exe and the FFNY Online contributors.** See the [upstream project](https://github.com/sweeax-exe/ffny-online), its [MIT license](licenses/FFNY-Online-MIT.txt), and the [full credits](CREDITS.md).

The Android 1.4 source, build scripts and room server are available here. The port's own code uses the [MIT license](LICENSE), with upstream license notices preserved. See the [source and build guide](SOURCE.md).

## Your original game is required

The original Xbox disc/ISO and extracted game data are not distributed here.
You must extract your own original Xbox disc or ISO and copy the complete game
contents to your phone. An ISO file alone, or `default.xbe` alone, is insufficient.

Required executable location:

`/storage/emulated/0/DefJamFFNY/game/default.xbe`

Create **DefJamFFNY** in the top level of your phone's **internal shared storage**,
then place the extracted game contents inside its **game** folder. This does not
mean the Android system root or the `Android/data` folder.

See the [complete extraction and folder guide](SETUP.md) and [credits](CREDITS.md).

## APKs and updates

When published, official APKs are available from this repository's
[Releases page](https://github.com/contatg/Def-Jam-Android-Native-Port/releases).
Use an update signed with the same signing key to preserve app compatibility.
Keep a backup of your game data and saves; do not uninstall or clear app data
just to install an update.

No particular frame rate is guaranteed. This project is not affiliated with or
endorsed by EA, Xbox, Def Jam or the original rights holders. The game, music,
characters, logos and artwork belong to their respective owners.
