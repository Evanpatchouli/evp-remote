#Requires -Version 7
[CmdletBinding()]
param(
    [string] $ReleaseDir,
    [string] $NsisRoot = $env:EVP_NSIS_ROOT
)

$ErrorActionPreference = 'Stop'
if (!$IsWindows -or ![Environment]::Is64BitProcess) { throw 'Run with 64-bit PowerShell on Windows.' }
$repo = Split-Path $PSScriptRoot -Parent
$version = (Get-Content -LiteralPath (Join-Path $repo 'package.json') -Raw | ConvertFrom-Json).version
$parsed = [semver]::Parse($version)
if (!$ReleaseDir) { $ReleaseDir = Join-Path $repo "release/$version" }
$stem = 'evp-remote-windows-x64'
$source = (Get-Item -LiteralPath (Join-Path $ReleaseDir "$stem-setup.exe")).FullName
$zip = (Get-Item -LiteralPath (Join-Path $ReleaseDir "$stem.zip")).FullName
$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($source)
if ($info.ProductName -cne 'EVP Remote' -or $info.ProductVersion -cne $version) {
    throw "Expected the EVP Remote setup for $version."
}

if ($NsisRoot) { $makeNsis = (Get-Item -LiteralPath (Join-Path $NsisRoot 'makensis.exe')).FullName }
else {
    $makeNsis = Get-Command makensis.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty Source
    if (!$makeNsis) {
        $makeNsis = @(
            (Join-Path ${env:ProgramFiles(x86)} 'NSIS/makensis.exe'),
            (Join-Path $env:LOCALAPPDATA 'tauri/NSIS/makensis.exe')
        ) | Where-Object { [IO.File]::Exists($_) } | Select-Object -First 1
    }
    if (!$makeNsis) { throw 'Install Unicode NSIS 3 or pass -NsisRoot.' }
}

function Invoke-Silent([string] $Path, [string] $Option = '') {
    $process = Start-Process -FilePath $Path -ArgumentList "/S $Option" -PassThru -WindowStyle Hidden
    try {
        if (!$process.WaitForExit(60000)) { throw "Silent command timed out: $Path" }
        $process.ExitCode
    } finally {
        if (!$process.HasExited) { $process.Kill($true); $process.WaitForExit() }
        $process.Dispose()
    }
}

$id = 'evp-remote-test-' + [guid]::NewGuid().ToString('N')
$name = "EVP 远程安装测试 $id"
$temp = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath([IO.Path]::GetTempPath()))
$scratch = Join-Path $temp $id
$programs = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs'
$install = Join-Path $programs $name
$backup = Join-Path $programs ".$id-update-backup"
$key = "HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall/$id"
$link = Join-Path ([Environment]::GetFolderPath('Programs')) "$name.lnk"
$app = Join-Path $install 'EvpRemote.exe'
$uninstall = Join-Path $install 'Uninstall.exe'
try {
    $payload = Join-Path $scratch 'payload'
    $setup = Join-Path $scratch "$stem-setup.exe"
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $payload)
    $arguments = @(
        '/WX', '/V2', '/INPUTCHARSET', 'UTF8', "/DPACKAGE_ROOT=$payload", "/DOUTPUT_EXE=$setup"
        "/DAPP_ICON=$(Join-Path $repo 'native/windows/ui/evp-remote.ico')", "/DVERSION=$version"
        "/DNUMERIC_VERSION=$($parsed.Major).$($parsed.Minor).$($parsed.Patch).0", "/DINSTALL_NAME=$name", "/DINSTALL_ID=$id"
        (Join-Path $repo 'native/windows/installer/evp-remote.nsi')
    )
    & $makeNsis @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Failed to build the isolated installer.' }
    if ((Invoke-Silent $setup) -ne 0) { throw 'Silent installation failed.' }
    $entry = Get-ItemProperty -LiteralPath $key
    if ($entry.DisplayVersion -cne $version -or $entry.InstallLocation -ine $install -or
        $entry.UninstallString -ine "`"$uninstall`"" -or ![IO.File]::Exists($link)) {
        throw 'Installation metadata or Start menu shortcut is missing.'
    }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($link)
    if ($shortcut.TargetPath -ine $app -or $shortcut.WorkingDirectory -ine $install) {
        throw 'The Start menu shortcut has an unexpected target or working directory.'
    }
    if (Get-Process EvpRemote -ErrorAction SilentlyContinue | Where-Object Path -eq $app) {
        throw 'Silent installation started the application.'
    }

    $marker = Join-Path $install 'upgrade-test.txt'
    [IO.File]::WriteAllText($marker, 'old installation')
    $locked = [IO.File]::Open($app, 'Open', 'Read', 'Read')
    try {
        if ((Invoke-Silent $setup) -eq 0 -or ![IO.File]::Exists($marker) -or
            (Get-ItemProperty -LiteralPath $key).DisplayVersion -cne $version) {
            throw 'Setup failed to preserve a locked installation.'
        }
    } finally { $locked.Dispose() }

    $outside = [IO.Directory]::CreateDirectory((Join-Path $scratch 'outside')).FullName
    $sentinel = Join-Path $outside 'sentinel.txt'
    [IO.File]::WriteAllText($sentinel, 'outside the installation directory')
    [void](New-Item -ItemType Junction -Path (Join-Path $install 'junction-test') -Target $outside)
    if ((Invoke-Silent $setup) -ne 0 -or [IO.File]::Exists($marker) -or ![IO.File]::Exists($sentinel)) {
        throw 'Setup failed to replace the previous installation safely.'
    }

    $previous = '0.0.0-installer-smoke'
    Set-ItemProperty -LiteralPath $key -Name DisplayVersion -Value $previous
    [IO.File]::WriteAllText($marker, 'version to preserve until startup')
    if ((Invoke-Silent $setup '/UPDATEBACKUP') -ne 0 -or [IO.File]::Exists($marker) -or
        ![IO.File]::Exists((Join-Path $backup 'upgrade-test.txt')) -or
        (Get-ItemProperty -LiteralPath $key).PendingVersion -cne $previous -or
        (Invoke-Silent $setup) -eq 0) {
        throw 'Setup failed to preserve the managed installation pending startup.'
    }
    $locked = [IO.File]::Open((Join-Path $backup 'EvpRemote.exe'), 'Open', 'Read', 'Read')
    try {
        if ((Invoke-Silent $setup '/UPDATECOMMIT') -eq 0 -or
            (Get-ItemProperty -LiteralPath $key).PendingVersion -cne $previous -or
            ![IO.Directory]::Exists($backup)) {
            throw 'A failed commit did not preserve the recoverable update.'
        }
    } finally { $locked.Dispose() }
    if ((Invoke-Silent $setup '/UPDATEROLLBACK') -ne 0 -or ![IO.File]::Exists($marker) -or
        (Get-ItemProperty -LiteralPath $key).DisplayVersion -cne $previous -or
        [IO.Directory]::Exists($backup)) {
        throw 'Update rollback failed to restore the managed installation and its registry version.'
    }
    if ((Invoke-Silent $setup '/UPDATEBACKUP') -ne 0 -or (Invoke-Silent $setup '/UPDATECOMMIT') -ne 0 -or
        [IO.File]::Exists($marker) -or [IO.Directory]::Exists($backup) -or
        (Get-ItemProperty -LiteralPath $key).DisplayVersion -cne $version) {
        throw 'Update commit did not retire the previous managed installation.'
    }

    foreach ($file in Get-ChildItem -LiteralPath $payload -Recurse -File) {
        $relative = [IO.Path]::GetRelativePath($payload, $file.FullName)
        if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath (Join-Path $install $relative)).Hash) {
            throw "Installed file differs from ZIP: $relative"
        }
    }
    $process = Start-Process -FilePath $uninstall -ArgumentList '/S' -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0 -or [IO.Directory]::Exists($install) -or
        [IO.File]::Exists($link) -or (Test-Path -LiteralPath $key)) {
        throw 'Silent uninstall did not remove the installation, shortcut and registration.'
    }
    Write-Host "Verified EVP Remote $version setup metadata, silent install, locked replacement, update rollback and commit, payload hashes and uninstall."
} finally {
    [IO.File]::Delete($link)
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key }
    foreach ($folder in @(@($install, $programs, $name), @($backup, $programs, ".$id-update-backup"), @($scratch, $temp, $id))) {
        if ([IO.Directory]::Exists($folder[0])) {
            $resolved = (Resolve-Path -LiteralPath $folder[0]).Path
            if ((Split-Path $resolved -Parent) -ine $folder[1] -or (Split-Path $resolved -Leaf) -cne $folder[2]) {
                throw "Unexpected smoke path: $resolved"
            }
            [IO.Directory]::Delete($resolved, $true)
        }
    }
}
