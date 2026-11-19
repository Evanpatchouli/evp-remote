#Requires -Version 7
[CmdletBinding()]
param(
    [string] $QtRoot = $env:EVP_QT_ROOT,
    [string] $WebRtcRoot = $env:EVP_WEBRTC_ROOT,
    [string] $FfmpegRoot = $env:EVP_FFMPEG_ROOT,
    [string] $BuildDir,
    [string] $DefaultSignalUrl = ''
)

$ErrorActionPreference = 'Stop'
if (!$IsWindows) { throw 'The Windows x64 client requires the MSVC toolchain on Windows.' }

$repo = Split-Path $PSScriptRoot -Parent
if (!$BuildDir) { $BuildDir = Join-Path $repo '.build/windows-x64' }
$configure = @('-S', (Join-Path $repo 'native/windows'), '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($QtRoot) {
    $qtDir = (Resolve-Path -LiteralPath (Join-Path $QtRoot 'lib/cmake/Qt6')).Path
    $configure += '-UQt6*_DIR', "-DQt6_DIR:PATH=$qtDir"
}
if ($WebRtcRoot) {
    $configure += '-UWEBRTC_*', "-DEVP_WEBRTC_ROOT:PATH=$((Resolve-Path -LiteralPath $WebRtcRoot).Path)"
}
if ($FfmpegRoot) {
    $configure += '-UFFMPEG_*', "-DEVP_FFMPEG_ROOT:PATH=$((Resolve-Path -LiteralPath $FfmpegRoot).Path)"
}

& cmake @configure
if ($LASTEXITCODE -ne 0) {
    throw 'CMake configuration failed. For a first build, configure EVP_QT_ROOT, EVP_WEBRTC_ROOT and EVP_FFMPEG_ROOT.'
}
& cmake --build $BuildDir --config Release --target EvpRemote --parallel
if ($LASTEXITCODE -ne 0) { throw 'The Release build of EvpRemote failed.' }

$package = @{
    BuildDir = $BuildDir
    QtRoot = $QtRoot
    WebRtcRoot = $WebRtcRoot
    FfmpegRoot = $FfmpegRoot
    DefaultSignalUrl = $DefaultSignalUrl
}
& (Join-Path $PSScriptRoot 'package-native-windows.ps1') @package
