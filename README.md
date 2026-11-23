# XenoRecomp

Static recompilation of the iOS game **Xenowerk 2.0.7** to a native Windows x64 executable, plus a launcher.

**This project does not include the game's assets.** You need your own legitimate copy of the game's `.ipa`.

## Usage
1. Download `XenoRecomp-v0.1.zip` from the Releases page and extract it anywhere.
2. Run `Launcher.exe`, click **Importar IPA... / Import IPA...** and select your Xenowerk 2.0.7 `.ipa`.
3. Click **Play**.

## Features
- Keyboard + mouse mapped to a virtual gamepad (configurable), or a real gamepad (XInput / DirectInput)
- Resolution, supersampling, filter, VSync, FPS limit, fullscreen
- Profiles with their own save data, save backup/restore
- Launcher in English, Portuguese, Spanish, French, German and Italian, plus a game-language selector
- Update check against GitHub Releases (can be turned off)

## Build the launcher
```
csc /target:winexe /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll /out:Launcher.exe launcher.cs i18n.cs
```

## Legal
The launcher source is MIT licensed (see `LICENSE`). `xenowerk.exe` is derived from the original game's code, which belongs to its copyright holders; it is distributed for preservation/interoperability purposes only and is **not** covered by the MIT license. This project is not affiliated with the game's developer or publisher. If you are a rights holder and want it removed, open an issue.
