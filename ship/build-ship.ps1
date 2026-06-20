[CmdletBinding()]
param(
    [ValidateSet("x64", "x86", "arm64")]
    [string]$Arch = "x64",

    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",

    [ValidateSet("Lean", "Portable")]
    [string]$PackageMode = "Lean",

    [switch]$NoZip,
    [switch]$IncludeFfmpeg,
    [switch]$SkipFfmpeg,
    [string]$FfmpegPath = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Require-Command {
    param([string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Required command not found: $Name"
    }

    return $command
}

function Reset-Directory {
    param(
        [string]$Path,
        [string]$AllowedRoot
    )

    $allowed = [System.IO.Path]::GetFullPath($AllowedRoot)
    $target = [System.IO.Path]::GetFullPath($Path)

    if (-not $target.StartsWith($allowed, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete outside ship workspace: $target"
    }

    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $target | Out-Null
}

function Invoke-Checked {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    Write-Host "> $Command $($Arguments -join ' ')"
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

function Copy-WinUiResources {
    param(
        [string]$SourceDir,
        [string]$DestinationDir
    )

    if (-not (Test-Path -LiteralPath $SourceDir)) {
        throw "WinUI build output was not found: $SourceDir"
    }

    $resourceFiles = @("*.xbf", "*.pri")
    foreach ($pattern in $resourceFiles) {
        Get-ChildItem -LiteralPath $SourceDir -Filter $pattern -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $DestinationDir -Force
        }
    }

    $resourceDirectories = @("Assets", "Views")
    foreach ($directory in $resourceDirectories) {
        $sourcePath = Join-Path $SourceDir $directory
        if (Test-Path -LiteralPath $sourcePath) {
            Copy-Item -LiteralPath $sourcePath -Destination $DestinationDir -Recurse -Force
        }
    }
}

function Test-FernFfmpeg {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return $false
    }

    try {
        $encoders = & $Path -hide_banner -encoders 2>$null | Out-String
        if ($LASTEXITCODE -ne 0) {
            return $false
        }

        $filters = & $Path -hide_banner -filters 2>$null | Out-String
        if ($LASTEXITCODE -ne 0) {
            return $false
        }

        return (
            $encoders -match "\blibx264\b" -and
            $encoders -match "\baac\b" -and
            $filters -match "\bloudnorm\b" -and
            $filters -match "\bamix\b" -and
            $filters -match "\bvolume\b"
        )
    } catch {
        return $false
    }
}

function Resolve-FernFfmpeg {
    param([string]$RequestedPath)

    $candidatePaths = New-Object System.Collections.Generic.List[string]

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        $candidatePaths.Add($RequestedPath)
    } else {
        $pathCommand = Get-Command "ffmpeg" -ErrorAction SilentlyContinue
        if ($pathCommand) {
            $candidatePaths.Add($pathCommand.Source)
        }

        $knownPaths = @(
            (Join-Path $env:ProgramFiles "File Converter\ffmpeg.exe")
        )

        foreach ($knownPath in $knownPaths) {
            if (Test-Path -LiteralPath $knownPath) {
                $candidatePaths.Add($knownPath)
            }
        }
    }

    $compatible = @()
    foreach ($candidate in ($candidatePaths | Select-Object -Unique)) {
        $fullPath = [System.IO.Path]::GetFullPath($candidate)
        if (Test-FernFfmpeg -Path $fullPath) {
            $item = Get-Item -LiteralPath $fullPath
            $compatible += [PSCustomObject]@{
                Path = $item.FullName
                Length = $item.Length
            }
        } elseif (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
            throw "Requested ffmpeg is not compatible with Fern Studio features: $fullPath"
        }
    }

    if ($compatible.Count -eq 0) {
        return $null
    }

    return ($compatible | Sort-Object Length | Select-Object -First 1).Path
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ShipRoot = (Resolve-Path $PSScriptRoot).Path
$WorkRoot = Join-Path $ShipRoot "work"
$DistRoot = Join-Path $ShipRoot "dist"

$runtimeByArch = @{
    x64 = "win-x64"
    x86 = "win-x86"
    arm64 = "win-arm64"
}

$cmakeArchByArch = @{
    x64 = "x64"
    x86 = "Win32"
    arm64 = "ARM64"
}

$dotnetPlatformByArch = @{
    x64 = "x64"
    x86 = "x86"
    arm64 = "ARM64"
}

$RuntimeId = $runtimeByArch[$Arch]
$CmakeArch = $cmakeArchByArch[$Arch]
$DotnetPlatform = $dotnetPlatformByArch[$Arch]
$UiSelfContained = if ($PackageMode -eq "Portable") { "true" } else { "false" }
$UiPublishTrimmed = "false"
$UiPublishReadyToRun = "false"
$CopyFfmpeg = $IncludeFfmpeg -or ($PackageMode -eq "Portable")
if ($SkipFfmpeg) {
    $CopyFfmpeg = $false
}

$PackageName = "Fern-$RuntimeId"
$NativeBuildDir = Join-Path $WorkRoot "native-$Arch"
$UiPublishDir = Join-Path $WorkRoot "ui-$Arch"
$UiBuildOutputDir = Join-Path $Root "FernUI\bin\$DotnetPlatform\$Configuration\net8.0-windows10.0.19041.0\$RuntimeId"
$PackageRoot = Join-Path $WorkRoot $PackageName
$PayloadRoot = Join-Path $PackageRoot "payload"
$AppRoot = Join-Path $PayloadRoot "app"
$DaemonRoot = Join-Path $PayloadRoot "daemon"
$ToolsRoot = Join-Path $PayloadRoot "tools"

Require-Command "cmake" | Out-Null
Require-Command "dotnet" | Out-Null

$ffmpegPath = $null
if ($CopyFfmpeg) {
    $ffmpegPath = Resolve-FernFfmpeg -RequestedPath $FfmpegPath
    if ($ffmpegPath) {
        $ffmpegSizeMb = [math]::Round((Get-Item -LiteralPath $ffmpegPath).Length / 1MB, 2)
        Write-Host "Using ffmpeg: $ffmpegPath ($ffmpegSizeMb MB)"
    } else {
        Write-Warning "ffmpeg was not found in PATH. The package will still build, but Studio export/LUFS features need ffmpeg on the target machine."
    }
}

New-Item -ItemType Directory -Force -Path $WorkRoot, $DistRoot | Out-Null

if (-not $SkipBuild) {
    Invoke-Checked "cmake" @(
        "-S", $Root,
        "-B", $NativeBuildDir,
        "-A", $CmakeArch
    )

    Invoke-Checked "cmake" @(
        "--build", $NativeBuildDir,
        "--config", $Configuration,
        "--target", "Fern"
    )

    Reset-Directory -Path $UiPublishDir -AllowedRoot $ShipRoot
    Invoke-Checked "dotnet" @(
        "publish", (Join-Path $Root "FernUI\FernUI.csproj"),
        "-c", $Configuration,
        "-r", $RuntimeId,
        "--self-contained", $UiSelfContained,
        "-p:Platform=$DotnetPlatform",
        "-p:PublishSingleFile=false",
        "-p:PublishTrimmed=$UiPublishTrimmed",
        "-p:PublishReadyToRun=$UiPublishReadyToRun",
        "-p:WindowsPackageType=None",
        "-p:WindowsAppSDKSelfContained=$UiSelfContained",
        "-p:PublishDir=$UiPublishDir\"
    )

    Copy-WinUiResources -SourceDir $UiBuildOutputDir -DestinationDir $UiPublishDir
}

$nativeExeCandidates = @(
    (Join-Path $NativeBuildDir "$Configuration\Fern.exe"),
    (Join-Path $NativeBuildDir "Fern.exe")
)
$NativeExe = $nativeExeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $NativeExe) {
    throw "Native daemon was not found. Expected Fern.exe under $NativeBuildDir"
}

$UiExe = Join-Path $UiPublishDir "FernUI.exe"
if (-not (Test-Path -LiteralPath $UiExe)) {
    throw "Published UI was not found: $UiExe"
}

Reset-Directory -Path $PackageRoot -AllowedRoot $ShipRoot
New-Item -ItemType Directory -Force -Path $AppRoot, $DaemonRoot | Out-Null

Copy-Item -Path (Join-Path $UiPublishDir "*") -Destination $AppRoot -Recurse -Force
Copy-Item -LiteralPath $NativeExe -Destination (Join-Path $DaemonRoot "Fern.exe") -Force

if ($ffmpegPath) {
    New-Item -ItemType Directory -Force -Path $ToolsRoot | Out-Null
    Copy-Item -LiteralPath $ffmpegPath -Destination (Join-Path $ToolsRoot "ffmpeg.exe") -Force
}

$launcherBat = @'
@echo off
setlocal
set "FERN_HOME=%~dp0"
set "FERN_UI=%FERN_HOME%app\FernUI.exe"

if exist "%FERN_HOME%tools\ffmpeg.exe" set "PATH=%FERN_HOME%tools;%PATH%"

if not exist "%FERN_UI%" (
    echo Fern UI not found: "%FERN_UI%"
    exit /b 1
)

start "Fern UI" "%FERN_UI%"
endlocal
'@
Set-Content -Path (Join-Path $PayloadRoot "Fern.bat") -Value $launcherBat -Encoding ASCII

$stopBat = @'
@echo off
taskkill /IM Fern.exe /F >nul 2>nul
if errorlevel 1 (
    echo Fern daemon was not running.
) else (
    echo Fern daemon stopped.
)
'@
Set-Content -Path (Join-Path $PayloadRoot "stop-fern.bat") -Value $stopBat -Encoding ASCII

$uninstallBat = @'
@echo off
setlocal
set "INSTALL_DIR=%~dp0"

call "%INSTALL_DIR%stop-fern.bat" >nul 2>nul

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$desktop=[Environment]::GetFolderPath('Desktop'); Remove-Item -LiteralPath (Join-Path $desktop 'Fern.lnk') -Force -ErrorAction SilentlyContinue; $programs=[Environment]::GetFolderPath('Programs'); Remove-Item -LiteralPath (Join-Path $programs 'Fern\Fern.lnk') -Force -ErrorAction SilentlyContinue; Remove-Item -LiteralPath (Join-Path $programs 'Fern') -Force -ErrorAction SilentlyContinue"

echo Fern shortcuts removed.
echo Delete this folder to remove Fern completely:
echo %INSTALL_DIR%
endlocal
'@
Set-Content -Path (Join-Path $PayloadRoot "uninstall.bat") -Value $uninstallBat -Encoding ASCII

$installBat = @'
@echo off
setlocal
set "SOURCE=%~dp0payload"
set "INSTALL_DIR=%~1"

if "%INSTALL_DIR%"=="" set "INSTALL_DIR=%LOCALAPPDATA%\Programs\Fern"

if not exist "%SOURCE%\app\FernUI.exe" (
    echo Invalid Fern package. Missing payload\app\FernUI.exe.
    exit /b 1
)

if not exist "%SOURCE%\daemon\Fern.exe" (
    echo Invalid Fern package. Missing payload\daemon\Fern.exe.
    exit /b 1
)

echo Installing Fern to "%INSTALL_DIR%"...
taskkill /IM Fern.exe /F >nul 2>nul
taskkill /IM FernUI.exe /F >nul 2>nul
mkdir "%INSTALL_DIR%" >nul 2>nul
robocopy "%SOURCE%" "%INSTALL_DIR%" /MIR >nul
if %ERRORLEVEL% GEQ 8 (
    echo Install copy failed with robocopy exit code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

if "%FERN_NO_SHORTCUTS%"=="1" (
    echo Shortcut creation skipped.
    goto after_shortcuts
)

set "FERN_INSTALL_DIR=%INSTALL_DIR%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$install=$env:FERN_INSTALL_DIR; $ui=Join-Path $install 'app\FernUI.exe'; $work=Join-Path $install 'app'; $shell=New-Object -ComObject WScript.Shell; $desktop=[Environment]::GetFolderPath('Desktop'); $shortcut=$shell.CreateShortcut((Join-Path $desktop 'Fern.lnk')); $shortcut.TargetPath=$ui; $shortcut.WorkingDirectory=$work; $shortcut.IconLocation=$ui; $shortcut.Save(); $programs=[Environment]::GetFolderPath('Programs'); $folder=Join-Path $programs 'Fern'; New-Item -ItemType Directory -Force -Path $folder | Out-Null; $shortcut=$shell.CreateShortcut((Join-Path $folder 'Fern.lnk')); $shortcut.TargetPath=$ui; $shortcut.WorkingDirectory=$work; $shortcut.IconLocation=$ui; $shortcut.Save()"

if errorlevel 1 (
    echo Fern was copied, but shortcut creation failed.
) else (
    echo Shortcuts created.
)

:after_shortcuts
echo Fern installed.
if "%FERN_NO_LAUNCH_PROMPT%"=="1" exit /b 0

choice /C YN /N /M "Launch Fern now? [Y/N] "
if errorlevel 2 exit /b 0
start "Fern" "%INSTALL_DIR%\app\FernUI.exe"
endlocal
'@
Set-Content -Path (Join-Path $PackageRoot "install.bat") -Value $installBat -Encoding ASCII

$gitCommit = "unknown"
try {
    $gitCommitRaw = & git -C $Root rev-parse --short HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $gitCommitRaw) {
        $gitCommit = $gitCommitRaw.Trim()
    }
} catch {
    $gitCommit = "unknown"
}

$manifest = [ordered]@{
    name = "Fern"
    runtime = $RuntimeId
    configuration = $Configuration
    packageMode = $PackageMode
    uiSelfContained = ($UiSelfContained -eq "true")
    gitCommit = $gitCommit
    builtAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    includes = [ordered]@{
        ui = "payload/app/FernUI.exe"
        daemon = "payload/daemon/Fern.exe"
        ffmpeg = [bool]$ffmpegPath
    }
    requirements = @(
        if ($PackageMode -eq "Lean") {
            ".NET 8 Desktop Runtime ($Arch)"
            "Windows App Runtime 2.1 ($Arch)"
        }
        if (-not $ffmpegPath) {
            "ffmpeg in PATH for Studio export/LUFS"
        }
    )
    install = "install.bat"
}

$manifest | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $PackageRoot "manifest.json") -Encoding UTF8

if ($PackageMode -eq "Lean") {
    $requirementsText = @"
Fern lean package requirements

- .NET 8 Desktop Runtime ($Arch)
- Windows App Runtime 2.1 ($Arch)
- ffmpeg in PATH for Studio export/LUFS unless this package was built with -IncludeFfmpeg

Build a larger portable package with:
powershell -ExecutionPolicy Bypass -File .\ship\build-ship.ps1 -PackageMode Portable
"@
    Set-Content -Path (Join-Path $PackageRoot "requirements.txt") -Value $requirementsText -Encoding ASCII
}

if (-not $NoZip) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $zipPath = Join-Path $DistRoot "$PackageName-$stamp.zip"
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $PackageRoot "*") -DestinationPath $zipPath -Force
    Write-Host "Package zip: $zipPath"
}

Write-Host "Package folder: $PackageRoot"
Write-Host "Installer: $(Join-Path $PackageRoot 'install.bat')"
