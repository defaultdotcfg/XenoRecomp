# Builds the release zip: Launcher.exe + xenowerk.exe + recomplauncher.json (RecompLauncher reads the json from the top of the zip)
param([string]$Version = "0.2", [string]$Exe = "xenowerk.exe")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot; $csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$stage = Join-Path $here "dist\stage"; Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $stage | Out-Null
& $csc /nologo /target:winexe /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll "/out:$stage\Launcher.exe" "$here\launcher.cs" "$here\i18n.cs"
Copy-Item $Exe "$stage\xenowerk.exe"; Copy-Item "$here\recomplauncher.json" $stage
Compress-Archive "$stage\*" "$here\dist\XenoRecomp-v$Version-Windows-x64.zip" -Force
"dist\XenoRecomp-v$Version-Windows-x64.zip"
