#Requires -Version 7
param([string] $ArchiveName)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$runtime = node -p "JSON.stringify({path:process.execPath, platform:process.platform, arch:process.arch})" | ConvertFrom-Json
if ($runtime.platform -ne 'win32' -or $runtime.arch -ne 'x64') {
    throw 'Build the portable endpoint with an x64 Windows Node.js distribution.'
}
$runtimeLicense = Join-Path (Split-Path $runtime.path -Parent) 'LICENSE'
if (!(Test-Path -LiteralPath $runtimeLicense)) {
    throw 'The Node.js distribution must include its LICENSE beside node.exe.'
}
$version = ([IO.File]::ReadAllText((Join-Path $root 'package.json')) | ConvertFrom-Json).version
$dist = [IO.Directory]::CreateDirectory((Join-Path $root 'dist')).FullName
if (!$ArchiveName) { $ArchiveName = "evp-remote-windows-x64-$version.zip" }
if ($ArchiveName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*\.zip$') { throw 'Use a ZIP filename without directory components.' }
$destination = Join-Path $dist $ArchiveName
$temporary = Join-Path $dist ("build-" + [guid]::NewGuid().ToString() + '.zip')
$archive = [IO.Compression.ZipFile]::Open($temporary, 'Create')
function Add-Entry([string]$Source, [string]$Name) {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $Source, $Name, 'Optimal')
}
try {
    try {
        Add-Entry (Join-Path $root 'LICENSE') 'LICENSE'
        Add-Entry $runtime.path 'runtime/node.exe'
        Add-Entry $runtimeLicense 'runtime/LICENSE'
        Add-Entry (Join-Path $PSScriptRoot 'Start-EVP-Remote.cmd') 'Start-EVP-Remote.cmd'
        foreach ($dir in 'desktop', 'web', 'node_modules/ws') {
            foreach ($item in (Get-ChildItem -LiteralPath (Join-Path $root $dir) -Recurse -File)) {
                $name = [IO.Path]::GetRelativePath($root, $item.FullName).Replace('\', '/')
                Add-Entry $item.FullName $name
            }
        }
    }
    finally {
        $archive.Dispose()
    }
    [IO.File]::Move($temporary, $destination, $true)
}
finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}
Get-FileHash -LiteralPath $destination -Algorithm SHA256
