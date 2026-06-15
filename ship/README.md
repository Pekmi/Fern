# Fern local ship workspace

This folder is ignored by Git. It is used for release artifacts, local packaging scripts, installer batches, and zip outputs.

Build the default x64 release package:

```powershell
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1
```

Useful variants:

```powershell
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1 -Arch x64 -Configuration Release
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1 -Arch x64 -NoZip
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1 -Arch x64 -SkipFfmpeg
```

Outputs:

- `ship/work/Fern-win-x64/` contains the unpacked package.
- `ship/dist/` contains timestamped zip files.
- The generated package includes `install.bat`.

Current prerequisites:

- .NET SDK with `net8.0-windows` support.
- CMake.
- Visual Studio Build Tools with C++ desktop tooling and Windows SDK.
- Optional: `ffmpeg` in PATH. If found, it is copied into the package for Studio export/LUFS features.
