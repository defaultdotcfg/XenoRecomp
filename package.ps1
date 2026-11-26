# Builds the release zip: Launcher.exe + recomp\ (lifter + runtime sources) + recomplauncher.json.
# No game code goes in the zip: the launcher builds xenowerk.exe on the player's PC from their own .ipa.
# RecompLauncher reads recomplauncher.json from the top of the zip.
param([string]$Version = "0.3")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot; $csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$stage = Join-Path $here "dist\stage"; Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $stage | Out-Null
& $csc /nologo /target:winexe /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll "/out:$stage\Launcher.exe" "$here\launcher.cs" "$here\i18n.cs" "$here\builder.cs"
if ($LASTEXITCODE -ne 0) { throw "csc failed" }
Copy-Item "$here\recomp" "$stage\recomp" -Recurse
Get-ChildItem "$stage\recomp" -Recurse -Include __pycache__ -Directory | Remove-Item -Recurse -Force
Copy-Item "$here\recomplauncher.json", "$here\LICENSE", "$here\README.md" $stage
# guard: nothing built from the game may end up in the zip
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { ($_.Extension -in ".exe", ".dll", ".bin", ".o", ".obj" -and $_.Name -ne "Launcher.exe") -or $_.Name -like "xenowerk*" -or $_.Name -like "f_*.c" }
if ($bad) { throw "game-derived files in the release: $($bad.FullName -join ', ')" }
Compress-Archive "$stage\*" "$here\dist\XenoRecomp-v$Version-Windows-x64.zip" -Force
"dist\XenoRecomp-v$Version-Windows-x64.zip"
