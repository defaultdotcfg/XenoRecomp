# XenoRecomp

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

Static recompilation of the iOS game **Xenowerk 2.0.7** to a native Windows x64 executable, plus a launcher.

**This project contains no part of the game: no assets and no game code.** You need your own legitimate copy of the game's `.ipa`. The game's executable (`xenowerk.exe`) is built on your PC from that `.ipa`.

## How to play
You need your own copy of the game: the **Xenowerk 2.0.7 iOS `.ipa`** (not included).

- **With [RecompLauncher](https://recomplauncher.com):** install XenoRecomp, pick your `.ipa` on the setup screen and press Play. Setup builds the game (see below). Saves go to the game's `recompdata/save` folder.
- **With the included launcher:**
  1. Download the latest `XenoRecomp-vX.Y-Windows-x64.zip` from the [Releases](https://github.com/defaultdotcfg/XenoRecomp/releases) page and extract it anywhere.
  2. Run `Launcher.exe`, click **Importar IPA... / Import IPA...** and select your Xenowerk 2.0.7 `.ipa`.
  3. The launcher builds the game (see below). Then click **Play**.

## How the game is built on your PC
The first time (and after each update), the launcher:
1. Unpacks the game from your `.ipa`.
2. Downloads the build tools once, about 115 MB, each checked against a fixed SHA-256:
   - [Python 3.14.8](https://www.python.org/) (embeddable package, python.org)
   - [Capstone 5.0.7](https://www.capstone-engine.org/) (PyPI)
   - [Zig 0.17.1](https://ziglang.org/) (used as a C compiler, ziglang.org)
3. Translates the game's ARM code to C with the recompiler in `recomp/lifter` (about 1 minute).
4. Compiles that C together with the runtime in `recomp/runtime` into `xenowerk.exe` (a few minutes, depending on your CPU).

The tools are kept in `cache/tools` (`recompdata/cache/tools` with RecompLauncher). During the build you need about 1.5 GB free on the drive. To build again, delete `xenowerk.build` next to the launcher.

## Features
- Keyboard + mouse mapped to a virtual gamepad (configurable), or a real gamepad (XInput / DirectInput)
- Resolution, supersampling, filter, VSync, FPS limit, fullscreen
- Profiles with their own save data, save backup/restore
- Launcher in English, Portuguese, Spanish, French, German and Italian, plus a game-language selector
- Update check against GitHub Releases (can be turned off)

## Build the launcher
```
csc /target:winexe /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll /out:Launcher.exe launcher.cs i18n.cs builder.cs
```
`package.ps1` builds the release zip (launcher, `recomp/` and `recomplauncher.json`).

## Legal
Everything in this repository and in the release zips (launcher, recompiler and runtime) is our own code, MIT licensed (see `LICENSE`). The game and its code belong to their copyright holders; they are not distributed here. The executable that the launcher builds from your `.ipa` contains the game's code: it is for your own use and is not covered by the MIT license, so don't share it. This project is not affiliated with the game's developer or publisher.
