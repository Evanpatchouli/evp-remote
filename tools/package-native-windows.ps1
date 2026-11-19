#Requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $QtRoot,
    [Parameter(Mandatory)] [string] $WebRtcRoot,
    [Parameter(Mandatory)] [string] $FfmpegRoot,
    [Parameter(Mandatory)] [string] $BuildDir,
    [string] $DefaultSignalUrl = ''
)

$ErrorActionPreference = 'Stop'
if (!$IsWindows) { throw 'Build the portable preview with the Windows x64 CMake toolchain.' }

function Get-RequiredFile([string] $Path) {
    $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if (!$item -or $item.PSIsContainer -or $item.Length -eq 0) { throw "Missing or empty file: $Path" }
    $item.FullName
}

function Assert-X64Image([string] $Path) {
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Invalid Windows executable: $Path" }
        $reader.BaseStream.Position = 0x3c
        $peOffset = $reader.ReadUInt32()
        $reader.BaseStream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "Expected an x64 Windows executable: $Path"
        }
    }
    finally { $reader.Dispose() }
}

function Get-Imports([string] $Path) {
    $lines = & $dumpbin /nologo /dependents $Path
    if ($LASTEXITCODE -ne 0) { throw "Could not read runtime dependencies from $Path" }
    foreach ($line in $lines) {
        if ($line -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1] }
    }
}

function Assert-BuildInput([string] $Name, [string] $Expected) {
    if (!$buildSettings.ContainsKey($Name) -or
        [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($buildSettings[$Name])) -ne
        [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($Expected))) {
        throw "The CMake build uses a different $Name. Reconfigure it with the supplied dependency paths."
    }
}

$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$WebRtcRoot = (Resolve-Path -LiteralPath $WebRtcRoot).Path
$FfmpegRoot = (Resolve-Path -LiteralPath $FfmpegRoot).Path
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$buildSettings = @{}
$cache = Get-RequiredFile (Join-Path $BuildDir 'CMakeCache.txt')
foreach ($line in [IO.File]::ReadLines($cache)) {
    if ($line -match '^([A-Za-z0-9_]+):[A-Z]+=(.*)$') { $buildSettings[$Matches[1]] = $Matches[2] }
}
Assert-BuildInput 'Qt6_DIR' (Join-Path $QtRoot 'lib/cmake/Qt6')
Assert-BuildInput 'EVP_WEBRTC_ROOT' $WebRtcRoot
Assert-BuildInput 'EVP_FFMPEG_ROOT' $FfmpegRoot

$exe = Get-RequiredFile (Join-Path $BuildDir 'Release/EvpRemote.exe')
Assert-X64Image $exe
$deployQt = Get-RequiredFile (Join-Path $QtRoot 'bin/windeployqt.exe')
$qtCore = Get-RequiredFile (Join-Path $QtRoot 'bin/Qt6Core.dll')
$qtVersion = (Get-Item -LiteralPath $qtCore).VersionInfo.ProductVersion
if ($qtVersion -notmatch '^(\d+\.\d+\.\d+)(?:\.\d+)?$') { throw "Unrecognized Qt version: $qtVersion" }
$qtVersion = $Matches[1]
$qtReleaseSeries = $qtVersion.Substring(0, $qtVersion.LastIndexOf('.'))

$visualStudio = $buildSettings['CMAKE_GENERATOR_INSTANCE']
if (!$visualStudio -or !(Test-Path -LiteralPath $visualStudio -PathType Container)) {
    throw 'The CMake build must identify its Visual Studio instance to deploy the matching x64 runtime.'
}
$toolset = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC/Tools/MSVC') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin/Hostx64/x64/dumpbin.exe') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$toolset) { throw 'The Visual Studio instance has no x64 dumpbin.' }
$dumpbin = Get-RequiredFile (Join-Path $toolset.FullName 'bin/Hostx64/x64/dumpbin.exe')
$redistVersion = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC/Redist/MSVC') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'x64') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$redistVersion) { throw 'The Visual Studio instance has no app-local x64 runtime.' }
$crt = Get-ChildItem -LiteralPath (Join-Path $redistVersion.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
    Select-Object -First 1
if (!$crt) { throw 'The Visual Studio x64 redistributables do not include a CRT directory.' }

$iniValue = ''
if ($DefaultSignalUrl) {
    $uri = $null
    if ($DefaultSignalUrl -match '[\r\n]' -or
        ![uri]::TryCreate($DefaultSignalUrl, [UriKind]::Absolute, [ref]$uri) -or
        $uri.Scheme -notin 'ws', 'wss' -or !$uri.Host -or $uri.UserInfo -or $uri.Fragment) {
        throw 'DefaultSignalUrl must be an absolute ws or wss URL without embedded credentials or a fragment.'
    }
    $iniValue = '"' + $uri.AbsoluteUri + '"'
}

$repo = Split-Path $PSScriptRoot -Parent
$dist = [IO.Directory]::CreateDirectory((Join-Path $repo 'dist')).FullName
$destination = Join-Path $dist 'evp-remote-windows-x64.zip'
$temporaryZip = Join-Path $dist ('native-preview-' + [guid]::NewGuid().ToString('N') + '.zip')
$tempDirectory = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$stagingName = 'evp-remote-native-' + [guid]::NewGuid().ToString('N')
$staging = [IO.Directory]::CreateDirectory((Join-Path $tempDirectory $stagingName)).FullName
$systemDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)

try {
    $app = Join-Path $staging 'EvpRemote.exe'
    Copy-Item -LiteralPath $exe -Destination $app
    $deployArgs = @(
        '--release', '--dir', $staging, '--no-compiler-runtime', '--no-opengl-sw', '--no-ffmpeg'
        '--no-system-d3d-compiler', '--no-system-dxc-compiler', '--no-translations'
        '--no-plugins', '--verbose', '0', $app
    )
    $oldPath = $env:PATH
    try {
        $env:PATH = "$(Join-Path $QtRoot 'bin');$oldPath"
        & $deployQt @deployArgs
        if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }
    }
    finally { $env:PATH = $oldPath }

    foreach ($plugin in 'platforms/qwindows.dll', 'tls/qschannelbackend.dll') {
        $pluginDir = [IO.Directory]::CreateDirectory((Join-Path $staging (Split-Path $plugin -Parent))).FullName
        Copy-Item -LiteralPath (Get-RequiredFile (Join-Path $QtRoot "plugins/$plugin")) -Destination $pluginDir
    }
    $styles = Join-Path $QtRoot 'plugins/styles/qmodernwindowsstyle.dll'
    if (Test-Path -LiteralPath $styles -PathType Leaf) {
        $styleDir = [IO.Directory]::CreateDirectory((Join-Path $staging 'styles')).FullName
        Copy-Item -LiteralPath $styles -Destination $styleDir
    }
    $translations = [IO.Directory]::CreateDirectory((Join-Path $staging 'translations')).FullName
    Copy-Item -LiteralPath (Get-RequiredFile (Join-Path $QtRoot 'translations/qtbase_zh_CN.qm')) -Destination $translations
    foreach ($runtime in Get-ChildItem -LiteralPath $crt.FullName -File -Filter '*.dll') {
        Copy-Item -LiteralPath $runtime.FullName -Destination $staging
    }
    [IO.File]::WriteAllLines((Join-Path $staging 'evp-remote.ini'), @('[General]', "signalingUrl=$iniValue"))
    [IO.File]::WriteAllLines((Join-Path $staging '使用说明.txt'), @(
        'EVP 远程 · Windows x64'
        '两台电脑都完整解压同一个压缩包，双击 EvpRemote.exe。'
        '被控电脑：选「共享本机」，输入部署密钥，创建并复制一次性邀请码。'
        '控制电脑：选「远程连接」，填写相同的信令服务器地址和邀请码，点击「请求连接」。'
        '被控电脑：核对请求后点击「同意并开始共享」。可以选择共享系统声音及目标 144 帧/秒。'
        '控制电脑：点击远程画面后即可使用键鼠。全屏时按 Ctrl+Alt+F11 返回窗口。'
        '被控电脑可随时点「结束共享」。请仅将邀请码交给你信任的人。'
    ))
    # Pin Qt's runtime lookup to the deployed plugin and translation directories.
    [IO.File]::WriteAllLines((Join-Path $staging 'qt.conf'), @('[Paths]', 'Prefix=.', 'Plugins=.', 'Translations=translations'))

    $pending = [Collections.Generic.Queue[string]]::new()
    $copiedFfmpeg = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $ffmpegDll = '^(avcodec|avutil|swscale|swresample)-\d+\.dll$'
    foreach ($import in Get-Imports $app) {
        if ($import -match $ffmpegDll) { $pending.Enqueue($import) }
    }
    foreach ($component in 'avcodec', 'avutil', 'swscale', 'swresample') {
        if (!($pending | Where-Object { $_ -like "$component-*.dll" })) {
            throw "The Release executable doesn't import $component."
        }
    }
    while ($pending.Count) {
        $name = $pending.Dequeue()
        if (!$copiedFfmpeg.Add($name)) { continue }
        $library = Get-RequiredFile (Join-Path $FfmpegRoot "bin/$name")
        Copy-Item -LiteralPath $library -Destination $staging
        foreach ($import in Get-Imports $library) {
            if ($import -match $ffmpegDll) { $pending.Enqueue($import) }
        }
    }

    $licences = [IO.Directory]::CreateDirectory((Join-Path $staging 'licenses')).FullName
    $qtNotices = [IO.Directory]::CreateDirectory((Join-Path $licences 'qt')).FullName
    $qtSbom = [IO.Directory]::CreateDirectory((Join-Path $qtNotices 'sbom')).FullName
    foreach ($module in 'qtbase', 'qtwebsockets', 'qttranslations') {
        [void](Get-RequiredFile (Join-Path $QtRoot "sbom/$module-$qtVersion.spdx.json"))
    }
    Get-ChildItem -LiteralPath (Join-Path $QtRoot 'sbom') -File -Filter '*.spdx*' |
        Copy-Item -Destination $qtSbom
    $qtLicenseHashes = @{
        'GPL-3.0-only.txt' = '8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903'
        'LGPL-3.0-only.txt' = 'da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768'
        'BSD-3-Clause.txt' = '9f0490f18656c6f2435bd14f603ef0c96434d1825615363dce43abb42ed1dcce'
    }
    foreach ($license in $qtLicenseHashes.Keys) {
        $qtLicense = Join-Path $qtNotices $license
        $sourceLicense = Join-Path $QtRoot "LICENSES/$license"
        if (Test-Path -LiteralPath $sourceLicense -PathType Leaf) {
            Copy-Item -LiteralPath (Get-RequiredFile $sourceLicense) -Destination $qtLicense
        }
        else {
            $module = if ($license -eq 'BSD-3-Clause.txt') { 'qttranslations' } else { 'qtbase' }
            $licenseUrl = "https://raw.githubusercontent.com/qt/$module/v$qtVersion/LICENSES/$license"
            Invoke-WebRequest -Uri $licenseUrl -TimeoutSec 30 -MaximumRetryCount 2 -RetryIntervalSec 1 -OutFile $qtLicense
        }
        [void](Get-RequiredFile $qtLicense)
        if ($qtVersion -eq '6.8.3' -and (Get-FileHash -LiteralPath $qtLicense).Hash -ne $qtLicenseHashes[$license]) {
            throw "The Qt $qtVersion upstream license did not match its verified digest: $license"
        }
    }
    $webrtcNotices = [IO.Directory]::CreateDirectory((Join-Path $licences 'webrtc')).FullName
    foreach ($license in 'LICENSE', 'THIRD_PARTY_LICENSES.md') {
        Copy-Item -LiteralPath (Get-RequiredFile (Join-Path $WebRtcRoot $license)) -Destination $webrtcNotices
    }
    $webrtcHash = (Get-FileHash -LiteralPath (Get-RequiredFile (Join-Path $WebRtcRoot 'lib/webrtc_c.lib'))).Hash
    $webrtcProvenance = if ($webrtcHash -eq '389cf9618a75363e8917780d02905d34abfd07220b21c6713ec1d3addd109495') {
        @(
            'Verified release: Shiguredo webrtc-rs 0.157.0, Windows x64.'
            'Binary: https://github.com/shiguredo/webrtc-rs/releases/download/0.157.0/libwebrtc_c-windows_x86_64.tar.gz'
            'Source: https://github.com/shiguredo/webrtc-rs/tree/0.157.0'
            'Release archive SHA-256: efb4e3dbce5b759f7b4e8b74479098f5ace17521e6343211bf923616a3e5544c'
        )
    }
    $ffmpegNotices = [IO.Directory]::CreateDirectory((Join-Path $licences 'ffmpeg')).FullName
    Copy-Item -LiteralPath (Get-RequiredFile (Join-Path $FfmpegRoot 'LICENSE.txt')) -Destination $ffmpegNotices
    $ffmpeg = Get-RequiredFile (Join-Path $FfmpegRoot 'bin/ffmpeg.exe')
    $ffmpegBuild = & $ffmpeg -L 2>&1 | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0 -or ($ffmpegBuild -join [Environment]::NewLine) -notmatch '(?m)^ffmpeg version (\S+)') {
        throw "Could not identify the FFmpeg build and its license from $ffmpeg."
    }
    $ffmpegVersion = $Matches[1]
    $ffmpegSource = if ($ffmpegVersion -match '-g([0-9a-f]{7,40})(?:-|$)') {
        "Upstream FFmpeg revision: https://github.com/FFmpeg/FFmpeg/commit/$($Matches[1])"
    }
    [IO.File]::WriteAllLines((Join-Path $ffmpegNotices 'build-and-license.txt'), @($ffmpegBuild))
    $notices = @(
        'EVP Remote for Windows'
        ''
        "Qt $qtVersion supplies the application framework. Module SBOMs are in qt/sbom."
        "Qt source releases: https://download.qt.io/archive/qt/$qtReleaseSeries/$qtVersion/submodules/"
        "Qt license text: https://github.com/qt/qtbase/tree/v$qtVersion/LICENSES"
        "Qt translations license text: https://github.com/qt/qttranslations/tree/v$qtVersion/LICENSES"
        ''
        "FFmpeg $ffmpegVersion supplies media decoding and colorspace conversion."
        'The supplied FFmpeg license and executable build configuration are in ffmpeg/.'
        $ffmpegSource
        ''
        'WebRTC supplies peer connections and transport. License texts are in webrtc/.'
        $webrtcProvenance
        "Static SDK library SHA-256: $webrtcHash"
        ''
        "Microsoft Visual C++ x64 runtime version $($redistVersion.Name) comes from Visual Studio."
        'Redistribution terms: https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files'
    )
    [IO.File]::WriteAllLines((Join-Path $licences 'THIRD-PARTY-NOTICES.txt'), @($notices | Where-Object { $null -ne $_ }))

    foreach ($relative in 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'Qt6WebSockets.dll',
        'platforms/qwindows.dll', 'tls/qschannelbackend.dll', 'translations/qtbase_zh_CN.qm',
        'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
        [void](Get-RequiredFile (Join-Path $staging $relative))
    }
    foreach ($binary in Get-ChildItem -LiteralPath $staging -Recurse -File |
        Where-Object { $_.Extension -in '.exe', '.dll' }) {
        Assert-X64Image $binary.FullName
        foreach ($import in Get-Imports $binary.FullName) {
            if ($import -match '^(api-ms-win-|ext-ms-win-)') { continue }
            if (Test-Path -LiteralPath (Join-Path $staging $import) -PathType Leaf) { continue }
            if ($import -match '^(msvcp|vcruntime|concrt|vccorlib)\d') { throw "Missing app-local CRT library: $import" }
            if (!(Test-Path -LiteralPath (Join-Path $systemDirectory $import) -PathType Leaf)) {
                throw "$($binary.Name) imports undeployed library $import."
            }
        }
    }

    [IO.Compression.ZipFile]::CreateFromDirectory($staging, $temporaryZip, [IO.Compression.CompressionLevel]::Optimal, $false)
    [IO.File]::Move($temporaryZip, $destination, $true)
}
finally {
    if ([IO.File]::Exists($temporaryZip)) { [IO.File]::Delete($temporaryZip) }
    if ($staging.StartsWith($tempDirectory, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $staging -Leaf).StartsWith('evp-remote-native-', [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}
Get-FileHash -LiteralPath $destination -Algorithm SHA256
