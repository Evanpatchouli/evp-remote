#Requires -Version 7
[CmdletBinding()]
param(
    [string] $QtRoot,
    [string] $WebRtcRoot,
    [string] $FfmpegRoot,
    [Parameter(Mandatory)] [string] $BuildDir,
    [string] $DefaultSignalUrl = '',
    [string] $OutputDir = '',
    [string] $NsisRoot = $env:EVP_NSIS_ROOT
)

$ErrorActionPreference = 'Stop'
if (!$IsWindows) { throw 'Package the Windows client on Windows x64.' }

$repo = Split-Path $PSScriptRoot -Parent
$manifest = Get-Content -LiteralPath (Join-Path $repo 'package.json') -Raw -Encoding utf8 | ConvertFrom-Json
$version = $manifest.version
$parsedVersion = $null
if ($version -isnot [string] -or ![semver]::TryParse($version, [ref]$parsedVersion) -or
    $parsedVersion.ToString() -cne $version) {
    throw 'package.json must contain a valid, canonical semantic version.'
}
$numericVersion = "$($parsedVersion.Major).$($parsedVersion.Minor).$($parsedVersion.Patch).0"

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

function New-PortableExecutable([string] $Archive, [string] $Destination) {
    $work = Split-Path -LiteralPath $Archive
    $assemblyInfo = Join-Path $work 'PortableVersion.cs'
    [IO.File]::WriteAllLines($assemblyInfo, @(
        'using System.Reflection;'
        '[assembly: AssemblyTitle("EVP Remote Portable")]'
        '[assembly: AssemblyProduct("EVP Remote")]'
        "[assembly: AssemblyFileVersion(`"$numericVersion`")]"
        "[assembly: AssemblyInformationalVersion(`"$version`")]"
    ))
    $arguments = @(
        $cscGuiArgs
        '/reference:System.dll', '/reference:System.IO.Compression.dll'
        '/reference:System.IO.Compression.FileSystem.dll', '/reference:System.Windows.Forms.dll'
        "/win32icon:$appIcon"
        "/resource:$([IO.Path]::GetFileName($Archive)),evp-remote-client.zip", "/out:$Destination"
        $assemblyInfo, (Join-Path $repo 'native/windows/portable/PortableLauncher.cs')
    )
    Push-Location -LiteralPath $work
    try {
        & $compiler @arguments
        if ($LASTEXITCODE -ne 0) { throw 'Building the single-file portable client failed.' }
    } finally { Pop-Location }
    Assert-X64Image $Destination
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo($Destination).ProductVersion -cne $version) {
        throw 'The portable executable does not match the package version.'
    }
}

function New-UpdateInstaller([string] $Destination) {
    $arguments = @(
        $cscGuiArgs
        '/reference:System.dll', '/reference:System.Windows.Forms.dll'
        "/out:$Destination", (Join-Path $repo 'native/windows/portable/UpdateInstaller.cs')
    )
    & $compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Building the client update helper failed.' }
}

function Find-NsisCompiler {
    if ($NsisRoot) { return Get-RequiredFile (Join-Path $NsisRoot 'makensis.exe') }
    $command = Get-Command makensis.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    foreach ($candidate in @(
        (Join-Path ${env:ProgramFiles(x86)} 'NSIS/makensis.exe'),
        (Join-Path $env:LOCALAPPDATA 'tauri/NSIS/makensis.exe')
    )) {
        if ([IO.File]::Exists($candidate)) { return $candidate }
    }
    throw 'Install Unicode NSIS 3 or configure EVP_NSIS_ROOT to the folder containing makensis.exe.'
}

function New-SetupExecutable([string] $Staging, [string] $Destination) {
    $arguments = @(
        '/WX', '/V2', '/INPUTCHARSET', 'UTF8', "/DPACKAGE_ROOT=$Staging", "/DOUTPUT_EXE=$Destination"
        "/DVERSION=$version", "/DNUMERIC_VERSION=$numericVersion", "/DAPP_ICON=$appIcon"
        (Join-Path $repo 'native/windows/installer/evp-remote.nsi')
    )
    & $makeNsis @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Building the Windows setup executable failed.' }
    [void](Get-RequiredFile $Destination)
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo($Destination).ProductVersion -cne $version) {
        throw 'The setup executable does not match the package version.'
    }
}

function Publish-ClientArtifacts([object[]] $Files) {
    $lockName = Join-Path $dist '.evp-client-publish.lock'
    $publishLock = [IO.FileStream]::new($lockName, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
        [IO.FileShare]::None, 1, [IO.FileOptions]::DeleteOnClose)
    try {
        try {
            foreach ($file in $Files) {
                if ([IO.File]::Exists($file.Destination)) {
                    [IO.File]::Copy($file.Destination, $file.Backup)
                }
            }
            foreach ($file in $Files) {
                [IO.File]::Move($file.Source, $file.Destination, $true)
                $file.Published = $true
            }
        } catch {
            $publishFailure = $_
            $restoreFailures = foreach ($file in $Files | Where-Object Published) {
                try {
                    if ([IO.File]::Exists($file.Backup)) {
                        [IO.File]::Move($file.Backup, $file.Destination, $true)
                    } else { [IO.File]::Delete($file.Destination) }
                } catch { $file.Destination }
            }
            if ($restoreFailures) {
                throw "Publishing failed ($publishFailure). Could not restore $($restoreFailures -join ', '). Remaining backups are at $packageWork."
            }
            foreach ($file in $Files) { [IO.File]::Delete($file.Backup) }
            throw $publishFailure
        }
        foreach ($file in $Files) { [IO.File]::Delete($file.Backup) }
    } finally { $publishLock.Dispose() }
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

$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$buildSettings = @{}
$cache = Get-RequiredFile (Join-Path $BuildDir 'CMakeCache.txt')
foreach ($line in [IO.File]::ReadLines($cache)) {
    if ($line -match '^([A-Za-z0-9_]+):[A-Z]+=(.*)$') { $buildSettings[$Matches[1]] = $Matches[2] }
}
Assert-BuildInput 'CMAKE_HOME_DIRECTORY' (Join-Path $repo 'native/windows')
if (!$QtRoot) { $QtRoot = Join-Path $buildSettings['Qt6_DIR'] '../../..' }
if (!$WebRtcRoot) { $WebRtcRoot = $buildSettings['EVP_WEBRTC_ROOT'] }
if (!$FfmpegRoot) { $FfmpegRoot = $buildSettings['EVP_FFMPEG_ROOT'] }
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$WebRtcRoot = (Resolve-Path -LiteralPath $WebRtcRoot).Path
$FfmpegRoot = (Resolve-Path -LiteralPath $FfmpegRoot).Path
Assert-BuildInput 'Qt6_DIR' (Join-Path $QtRoot 'lib/cmake/Qt6')
Assert-BuildInput 'EVP_WEBRTC_ROOT' $WebRtcRoot
Assert-BuildInput 'EVP_FFMPEG_ROOT' $FfmpegRoot
Assert-BuildInput 'WEBRTC_INCLUDE' (Join-Path $WebRtcRoot 'include')
Assert-BuildInput 'WEBRTC_LIBRARY' (Join-Path $WebRtcRoot 'lib/webrtc_c.lib')
Assert-BuildInput 'FFMPEG_INCLUDE' (Join-Path $FfmpegRoot 'include')
foreach ($component in 'avcodec', 'avutil', 'swscale', 'swresample') {
    Assert-BuildInput "FFMPEG_$component" (Join-Path $FfmpegRoot "lib/$component.lib")
}

$ffmpeg = Get-RequiredFile (Join-Path $FfmpegRoot 'bin/ffmpeg.exe')
$ffmpegBuild = & $ffmpeg -L 2>&1 | ForEach-Object { $_.ToString() }
$ffmpegReport = $ffmpegBuild -join "`n"
if ($LASTEXITCODE -ne 0 -or $ffmpegReport -notmatch '(?m)^ffmpeg version (\S+)') {
    throw "Could not identify the FFmpeg build and its license from $ffmpeg."
}
$ffmpegVersion = $Matches[1]
$gplClient = $version -cin @('0.3.1', '0.3.2')
if ($gplClient) {
    if ($ffmpegReport -notmatch 'GNU General Public License' -or $ffmpegReport -notmatch '--enable-libx264\b' -or
        $ffmpegReport -match '--enable-nonfree\b') {
        throw 'The GPL client release requires a redistributable GPL FFmpeg SDK with libx264.'
    }
} elseif ($ffmpegReport -notmatch 'GNU Lesser General Public License' -or $ffmpegReport -match '(?m)(?:^|\s)--enable-(?:gpl|nonfree)(?:\s|$)') {
    throw 'Packaging requires a matching LGPL FFmpeg SDK built without --enable-gpl or --enable-nonfree.'
}

$exe = Get-RequiredFile (Join-Path $BuildDir 'Release/EvpRemote.exe')
Assert-X64Image $exe
if ([Diagnostics.FileVersionInfo]::GetVersionInfo($exe).ProductVersion -cne $version) {
    throw "The executable does not match package.json version $version. Rebuild EvpRemote before packaging."
}
$appIcon = Get-RequiredFile (Join-Path $repo 'native/windows/ui/evp-remote.ico')
$framework = Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319'
$compiler = Get-RequiredFile (Join-Path $framework 'csc.exe')
$cscGuiArgs = @('/nologo', '/noconfig', '/target:winexe', '/platform:x64', '/optimize+', '/codepage:65001')
$makeNsis = Find-NsisCompiler
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

if (!$OutputDir) {
    $releaseRoot = [IO.Directory]::CreateDirectory((Join-Path $repo 'release'))
    $OutputDir = Join-Path $releaseRoot.FullName $version
    [void][IO.Directory]::CreateDirectory($OutputDir)
    foreach ($existing in $releaseRoot.EnumerateDirectories($version)) {
        if ($existing.Name -cne $version) {
            throw "Release folder '$($existing.Name)' already exists with different casing from '$version'."
        }
    }
}
$dist = [IO.Directory]::CreateDirectory($OutputDir).FullName
$destination = Join-Path $dist 'evp-remote-windows-x64.zip'
$portable = Join-Path $dist 'evp-remote-windows-x64-portable.exe'
$setup = Join-Path $dist 'evp-remote-windows-x64-setup.exe'
$packageWork = Join-Path $dist ('.pack-' + [guid]::NewGuid().ToString('N'))
$temporaryZip = Join-Path $packageWork 'payload.zip'
$temporaryPortable = Join-Path $packageWork ([IO.Path]::GetFileName($portable))
$temporarySetup = Join-Path $packageWork ([IO.Path]::GetFileName($setup))
$publishFiles = @(
    [pscustomobject]@{ Source = $temporaryZip; Destination = $destination; Backup = (Join-Path $packageWork 'previous.zip'); Published = $false }
    [pscustomobject]@{ Source = $temporaryPortable; Destination = $portable; Backup = (Join-Path $packageWork 'previous-portable.exe'); Published = $false }
    [pscustomobject]@{ Source = $temporarySetup; Destination = $setup; Backup = (Join-Path $packageWork 'previous-setup.exe'); Published = $false }
)
$tempDirectory = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$stagingName = 'evp-remote-native-' + [guid]::NewGuid().ToString('N')
$staging = [IO.Directory]::CreateDirectory((Join-Path $tempDirectory $stagingName)).FullName
$systemDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)

try {
    [void][IO.Directory]::CreateDirectory($packageWork)
    $app = Join-Path $staging 'EvpRemote.exe'
    Copy-Item -LiteralPath $exe -Destination $app
    New-UpdateInstaller (Join-Path $staging 'EvpRemoteUpdate.exe')
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
        "EVP 远程 $version · Windows x64"
        '双击安装版 EXE 安装后，从开始菜单打开 EVP Remote。便携版 EXE 可直接打开；使用 ZIP 时，先完整解压再打开 EvpRemote.exe。'
        ''
        '首次使用时，打开侧栏底部「设置」，保存信令地址。创建邀请码或使用「我的设备」的电脑还需保存部署密钥。仅凭邀请码协助他人时，只需保存地址。'
        '打开侧栏「远程协助」。被控端在「共享本机」标签中创建邀请码，控制端在「远程协助」标签中输入邀请码并请求连接。'
        '两种码的有效期可选 5、15、30 分钟或 1、2、5、12 小时，默认 5 分钟。通常由本机确认，生成时勾选「持码免本机确认」可预先授权本次邀请码或配对码。自定义邀请码需要升级信令服务。'
        '两端在「设置」中配置好同一台服务器后，打开「我的设备」。一端生成配对码，另一端粘贴并请求配对。配对成功后可以在设备列表中找到对方。'
        '在需要被控制的电脑上开启「允许可信设备远控本机」，保持客户端运行、桌面解锁。控制端从设备列表选择在线设备即可连接，不需再次配对或输入邀请码。'
        '关闭可信设备远控开关或移除配对，可在本机撤销授权。请仅私下分享配对码和邀请码，不要把部署密钥交给协助访客。'
        '控制电脑：点击远程画面后即可使用键鼠。全屏时按 Ctrl+Alt+F11 返回窗口。'
        '被控电脑可随时点「结束共享」。'
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
    if ($gplClient) {
        foreach ($notice in 'LICENSE', 'COPYING') {
            Copy-Item -LiteralPath (Get-RequiredFile (Join-Path $repo "native/windows/$notice")) -Destination $staging
        }
    } else {
        Copy-Item -LiteralPath (Get-RequiredFile (Join-Path $repo 'LICENSE')) -Destination $staging
    }
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
    foreach ($document in 'WEBRTC-BUILD-DEPS.txt', 'WEBRTC-NOTICE.txt', 'WEBRTC-VERSIONS.txt') {
        Copy-Item -LiteralPath (Get-RequiredFile (Join-Path $repo "native/windows/third-party/webrtc/$document")) -Destination $webrtcNotices
    }
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
    New-PortableExecutable $temporaryZip $temporaryPortable
    New-SetupExecutable $staging $temporarySetup
    $artifacts = foreach ($file in $publishFiles) {
        [pscustomobject]@{ Algorithm = 'SHA256'; Hash = (Get-FileHash -LiteralPath $file.Source -Algorithm SHA256).Hash; Path = $file.Destination }
    }
    Publish-ClientArtifacts $publishFiles
}
finally {
    if ([IO.Directory]::Exists($packageWork)) {
        foreach ($file in $publishFiles) { [IO.File]::Delete($file.Source) }
        [IO.File]::Delete((Join-Path $packageWork 'PortableVersion.cs'))
        if (!$([IO.Directory]::EnumerateFileSystemEntries($packageWork) | Select-Object -First 1)) {
            [IO.Directory]::Delete($packageWork)
        }
    }
    if ($staging.StartsWith($tempDirectory, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $staging -Leaf).StartsWith('evp-remote-native-', [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}
Write-Host "Packaged EVP Remote $version for Windows x64 in $dist"
$artifacts
