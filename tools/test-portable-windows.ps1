#Requires -Version 7
[CmdletBinding()]
param(
    [string] $ReleaseDir,
    [string] $GuiTestUser
)

$ErrorActionPreference = 'Stop'
if (!$IsWindows -or ![Environment]::Is64BitProcess) { throw 'Run with 64-bit PowerShell on Windows.' }
$repo = Split-Path $PSScriptRoot -Parent
$version = (Get-Content -LiteralPath (Join-Path $repo 'package.json') -Raw | ConvertFrom-Json).version
if (!$ReleaseDir) { $ReleaseDir = Join-Path $repo "release/$version" }
$stem = 'evp-remote-windows-x64'
$zip = (Get-Item -LiteralPath (Join-Path $ReleaseDir "$stem.zip")).FullName
$source = (Get-Item -LiteralPath (Join-Path $ReleaseDir "$stem-portable.exe")).FullName

if ($GuiTestUser) {
    $account = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    if ($account -ne $GuiTestUser -or ![Environment]::UserInteractive) {
        throw "GUI smoke requires an interactive session as $GuiTestUser (current account: $account)."
    }
    $store = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'EVP/EVP Remote'
    if ((Test-Path -LiteralPath $store) -or (Test-Path -LiteralPath 'HKCU:\Software\EVP\EVP Remote')) {
        throw 'GUI smoke requires a fresh test profile. Existing EVP settings or device data found.'
    }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class EvpPortableSmokeWindows {
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostThreadMessageW(uint thread, uint message, UIntPtr wParam, IntPtr lParam);
}
'@
}

function Test-ShippingPayload {
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo($exe).ProductVersion -cne $version) {
        throw "Portable EXE ProductVersion must match $version."
    }
    $file = [IO.File]::OpenRead($exe)
    $context = [Runtime.Loader.AssemblyLoadContext]::new('portable-smoke', $true)
    try {
        $pe = [Reflection.PortableExecutable.PEReader]::new($file, [Reflection.PortableExecutable.PEStreamOptions]::LeaveOpen)
        try {
            $headers = $pe.PEHeaders
            if ($headers.CoffHeader.Machine -ne 'Amd64' -or $headers.PEHeader.Subsystem -ne 'WindowsGui') {
                throw 'Expected an x64 Windows GUI launcher.'
            }
        } finally { $pe.Dispose() }
        $file.Position = 0
        $assembly = $context.LoadFromStream($file)
        $resource = $assembly.GetManifestResourceStream('evp-remote-client.zip')
        if (!$resource) { throw 'Missing embedded evp-remote-client.zip.' }
        try { $embeddedHash = (Get-FileHash -InputStream $resource -Algorithm SHA256).Hash }
        finally { $resource.Dispose() }
        if ($embeddedHash -ne (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash) {
            throw 'Embedded ZIP differs from the release ZIP.'
        }
    } finally { $file.Dispose(); $context.Unload() }

    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entry = $archive.GetEntry('EvpRemote.exe')
        if (!$entry -or !$archive.GetEntry('platforms/qwindows.dll')) { throw 'Missing native client or Qt Windows plugin.' }
        $stream = $entry.Open()
        try { (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash }
        finally { $stream.Dispose() }
    } finally { $archive.Dispose() }
}

function Invoke-PortableSmoke([bool] $ShowWindow) {
    $start = [Diagnostics.ProcessStartInfo]::new($exe)
    $start.UseShellExecute = $false
    $start.WorkingDirectory = $appDir
    $start.Environment['TEMP'] = $scratch
    $start.Environment['TMP'] = $scratch
    $start.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($key in @($start.Environment.Keys | Where-Object { $_ -like 'QT_*' })) {
        [void]$start.Environment.Remove($key)
    }
    $start.Environment['EVP_SIGNAL_URL'] = ''
    if (!$ShowWindow) {
        $start.ArgumentList.Add('--version')
        $start.RedirectStandardOutput = $true
    }
    $launcher = [Diagnostics.Process]::Start($start)
    $child = $null
    try {
        if ($ShowWindow) {
            $timer = [Diagnostics.Stopwatch]::StartNew()
            while (!$launcher.HasExited -and $timer.Elapsed.TotalSeconds -lt 60) {
                if (!$child) {
                    $filter = "ParentProcessId = $($launcher.Id) AND Name = 'EvpRemote.exe'"
                    $found = Get-CimInstance Win32_Process -Filter $filter
                    if ($found -and $found.ExecutablePath -and
                        $found.ExecutablePath.StartsWith("$scratch\", [StringComparison]::OrdinalIgnoreCase)) {
                        $candidate = Get-Process -Id $found.ProcessId
                        [void]$candidate.Handle
                        if ($candidate.Path -ne $found.ExecutablePath) {
                            $candidate.Dispose()
                            throw 'Child process changed during window discovery.'
                        }
                        $child = $candidate
                    }
                }
                if ($child) {
                    $child.Refresh()
                    if ($child.MainWindowHandle -ne 0 -and $child.MainWindowTitle -ceq 'EVP 远程') { break }
                }
                Start-Sleep -Milliseconds 200
            }
            if ($launcher.HasExited -or !$child -or $child.MainWindowHandle -eq 0 -or $child.MainWindowTitle -cne 'EVP 远程') {
                throw 'The extracted client did not show its Qt home window.'
            }
            if ((Get-FileHash -LiteralPath $child.Path).Hash -ne $clientHash) { throw 'Unexpected running client image.' }
            [uint32] $windowProcess = 0
            $thread = [EvpPortableSmokeWindows]::GetWindowThreadProcessId($child.MainWindowHandle, [ref]$windowProcess)
            if (!$thread -or $windowProcess -ne $child.Id -or !$child.CloseMainWindow()) {
                throw 'Could not close the isolated Qt home window.'
            }
            $timer.Restart()
            do {
                $child.Refresh()
                if ($child.HasExited -or $launcher.HasExited) { throw 'Closing the window ended the portable client.' }
                if ($child.MainWindowHandle -eq 0) { break }
                Start-Sleep -Milliseconds 100
            } while ($timer.Elapsed.TotalSeconds -lt 15)
            if ($child.MainWindowHandle -ne 0) { throw 'The Qt client did not hide after WM_CLOSE.' }
            if ($child.WaitForExit(1000) -or $launcher.HasExited) {
                throw 'The portable client did not stay alive in the notification area.'
            }
            # Deliver WM_QUIT to the verified test client's UI thread for controlled teardown.
            if (![EvpPortableSmokeWindows]::PostThreadMessageW($thread, 0x12, [UIntPtr]::Zero, [IntPtr]::Zero) -or
                !$child.WaitForExit(15000) -or $child.ExitCode -ne 0) {
                throw 'The isolated Qt client did not exit after WM_QUIT.'
            }
        } else {
            $output = $launcher.StandardOutput.ReadToEndAsync()
        }
        if (!$launcher.WaitForExit(60000) -or $launcher.ExitCode -ne 0) {
            throw 'Portable launcher timed out or returned a nonzero exit code.'
        }
        if (!$ShowWindow -and (!$output.Wait(5000) -or
            $output.Result.Trim() -cnotmatch "^(?:EVP Remote\s+)?$([regex]::Escape($version))$")) {
            throw "Expected native --version output for $version."
        }
        if (@(Get-ChildItem -LiteralPath $scratch -Force).Count -or
            @(Get-ChildItem -LiteralPath $appDir -Force | Where-Object FullName -ne $exe).Count) {
            throw 'Portable runtime files remain after launcher exit.'
        }
    } finally {
        try {
            if (!$launcher.HasExited) { $launcher.Kill($true); $launcher.WaitForExit() }
            if ($child -and !$child.HasExited) { $child.Kill(); $child.WaitForExit() }
        } finally {
            if ($child) { $child.Dispose() }
            $launcher.Dispose()
        }
    }
}

$tempRoot = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath([IO.Path]::GetTempPath()))
$rootName = "EVP 验收 $([guid]::NewGuid())"
$root = Join-Path $tempRoot $rootName
[void](New-Item -ItemType Directory -Path $root)
try {
    $appDir = [IO.Directory]::CreateDirectory((Join-Path $root 'app')).FullName
    $scratch = [IO.Directory]::CreateDirectory((Join-Path $root 'temp')).FullName
    $exe = Join-Path $appDir "$stem-portable.exe"
    [IO.File]::Copy($source, $exe)
    $clientHash = Test-ShippingPayload
    Invoke-PortableSmoke $false
    Write-Host "Verified launcher metadata, embedded ZIP, native --version $version and cleanup."
    if ($GuiTestUser) {
        Invoke-PortableSmoke $true
        Write-Host 'Verified packaged Qt home window, background survival after WM_CLOSE, and controlled shutdown and cleanup.'
    }
} finally {
    $resolved = (Resolve-Path -LiteralPath $root).Path
    if ((Split-Path $resolved -Parent) -ne $tempRoot -or (Split-Path $resolved -Leaf) -cne $rootName) {
        throw "Unexpected smoke directory: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
