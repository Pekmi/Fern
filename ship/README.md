# Fern local ship workspace

This folder is ignored by Git. It is used for release artifacts, local packaging scripts, installer batches, and zip outputs.

Build the default x64 release package. The default package is lean: it does not
embed ffmpeg or the .NET runtime.

```powershell
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1
```

Useful variants:

```powershell
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1 -Arch x64 -Configuration Release
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1 -Arch x64 -NoZip
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1 -Arch x64 -IncludeFfmpeg
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1 -Arch x64 -PackageMode Portable
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1 -Arch x64 -PackageMode Portable -FfmpegPath "C:\path\to\ffmpeg.exe"
```

Outputs:

- `ship/work/Fern-win-x64/` contains the unpacked package.
- `ship/dist/` contains timestamped zip files.
- The generated package includes `install.bat`.

Lean package prerequisites on the target machine:

- .NET 8 Desktop Runtime.
- Windows App Runtime 2.1.
- Optional: `ffmpeg` in PATH for Studio export/LUFS features.

Portable package behavior:

- `-PackageMode Portable` embeds the .NET runtime, Windows App SDK runtime files,
  and a compatible `ffmpeg.exe`.
- When multiple candidates are available, the script copies the smallest ffmpeg
  that supports Fern Studio features: H.264 via `libx264`, AAC, `volume`,
  `amix`, and `loudnorm`.
- Use `-FfmpegPath` to force a specific compatible ffmpeg binary.

Build prerequisites:

- .NET SDK with `net8.0-windows` support.
- CMake.
- Visual Studio Build Tools with C++ desktop tooling and Windows SDK.
- Optional: `ffmpeg` in PATH.
