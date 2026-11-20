using Microsoft.Win32.SafeHandles;
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

internal static class UpdateInstaller
{
    private const string ClientName = "EvpRemote.exe";
    private const string HelperName = "EvpRemoteUpdate.exe";
    private const string InstalledBackupName = ".evp-remote-update-backup";
    private const int ExitWaitMilliseconds = 60000;
    private const int SetupWaitMilliseconds = 300000;
    private const int VersionWaitMilliseconds = 30000;
    private const uint QueryAndSynchronize = 0x101000;
    private const uint WaitSignaled = 0;
    private const uint WaitTimedOut = 258;
    private const int InvalidParameter = 87;
    private const uint MoveReplaceExisting = 0x1;

    [STAThread]
    private static int Main(string[] arguments)
    {
        string helper = Assembly.GetExecutingAssembly().Location;
        string staging = null;
        string download = null;
        try
        {
            int clientId, launcherId;
            bool portable = arguments.Length != 0 && arguments[0] == "--portable";
            if (arguments.Length != 7 || (!portable && arguments[0] != "--setup") ||
                !Int32.TryParse(arguments[1], NumberStyles.None, CultureInfo.InvariantCulture, out clientId) ||
                !Int32.TryParse(arguments[2], NumberStyles.None, CultureInfo.InvariantCulture, out launcherId) ||
                clientId <= 0 || clientId == launcherId || (portable ? launcherId <= 0 : launcherId != 0) ||
                !Regex.IsMatch(arguments[5], @"\A[0-9A-Fa-f]{64}\z") ||
                !IsVersion(arguments[6]))
                throw new InvalidDataException("更新参数无效。");

            string original = AbsolutePath(arguments[3]);
            download = AbsolutePath(arguments[4]);
            staging = VerifyStaging(helper, download, portable);
            if (!PathEquals(Path.GetExtension(original), ".exe") ||
                (!portable && !PathEquals(Path.GetFileName(original), ClientName)) ||
                PathEquals(Path.GetDirectoryName(original), staging))
                throw new InvalidDataException("待更新的程序路径无效。");

            string installed = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Programs", "EVP Remote", ClientName);
            string destination = portable ? original : AbsolutePath(installed);
            using (SafeWaitHandle client = WatchProcess(clientId, portable ? null : original))
            using (SafeWaitHandle launcher = portable ? WatchProcess(launcherId, original) : null)
            using (var updateLock = new Mutex(false, UpdateMutexName(destination)))
            {
                if (!TakeUpdateLock(updateLock))
                    throw new IOException("另一个更新正在处理此程序。");
                try
                {
                    WaitForExit(client);
                    WaitForExit(launcher);
                    InstallUpdate(portable, original, destination, download, arguments[5], arguments[6]);
                }
                finally
                {
                    updateLock.ReleaseMutex();
                }
            }
            return 0;
        }
        catch (Exception error)
        {
            MessageBox.Show("更新失败。\n\n" + error.Message, "EVP 远程更新",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
        finally
        {
            if (staging != null)
                CleanStaging(staging, helper, download);
        }
    }

    private static bool IsVersion(string version)
    {
        return Regex.IsMatch(version, @"\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)" +
            @"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?\z");
    }

    private static string AbsolutePath(string path)
    {
        string root = Path.GetPathRoot(path);
        if (String.IsNullOrEmpty(root) || root.Length < 3 || path.IndexOf(':', root.Length) >= 0)
            throw new InvalidDataException("更新需要完整的文件路径。");
        return Path.GetFullPath(path);
    }

    private static bool PathEquals(string first, string second)
    {
        return String.Equals(first, second, StringComparison.OrdinalIgnoreCase);
    }

    private static string VerifyStaging(string helper, string download, bool portable)
    {
        string directory = Path.GetDirectoryName(helper);
        string assetName = portable ? "evp-remote-windows-x64-portable.exe" : "evp-remote-windows-x64-setup.exe";
        if (!PathEquals(Path.GetFileName(helper), HelperName) ||
            !PathEquals(Path.GetFileName(download), assetName) ||
            !PathEquals(Path.GetDirectoryName(download), directory) ||
            !Regex.IsMatch(Path.GetFileName(directory), @"\Aevp-update-[A-Za-z0-9]{6}\z") ||
            !IsInTemp(directory) ||
            (File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException("更新临时目录无效。");
        return directory;
    }

    private static bool IsInTemp(string path)
    {
        string parent = Path.GetDirectoryName(path);
        return parent != null && PathEquals(parent.TrimEnd('\\'), Path.GetFullPath(Path.GetTempPath()).TrimEnd('\\'));
    }

    private static SafeWaitHandle WatchProcess(int id, string expectedPath)
    {
        SafeWaitHandle process = OpenProcess(QueryAndSynchronize, false, id);
        if (process.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            process.Dispose();
            if (error == InvalidParameter)
                return null; // The client can exit before the detached updater starts.
            throw new Win32Exception(error);
        }

        try
        {
            var image = new StringBuilder(32768);
            int size = image.Capacity;
            if (!QueryFullProcessImageName(process, 0, image, ref size))
            {
                int error = Marshal.GetLastWin32Error();
                if (WaitForSingleObject(process, 0) != WaitSignaled)
                    throw new Win32Exception(error);
            }
            else
            {
                string path = Path.GetFullPath(image.ToString());
                if (expectedPath == null ? !IsPortableRuntime(path) : !PathEquals(path, expectedPath))
                    throw new InvalidDataException("正在运行的程序与更新路径不匹配。");
            }
            return process;
        }
        catch
        {
            process.Dispose();
            throw;
        }
    }

    private static bool IsPortableRuntime(string path)
    {
        string directory = Path.GetDirectoryName(path);
        return PathEquals(Path.GetFileName(path), ClientName) &&
            IsInTemp(directory) &&
            Regex.IsMatch(Path.GetFileName(directory), @"\Aevp-portable-[0-9a-f]{32}\z");
    }

    private static void WaitForExit(SafeWaitHandle process)
    {
        if (process == null)
            return;
        uint result = WaitForSingleObject(process, ExitWaitMilliseconds);
        if (result == WaitTimedOut)
            throw new TimeoutException("等待原程序退出超时，请关闭原程序后重试。");
        if (result != WaitSignaled)
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    private static string UpdateMutexName(string destination)
    {
        using (var hash = SHA256.Create())
            return @"Local\EVPRemoteUpdate-" + Convert.ToBase64String(
                hash.ComputeHash(Encoding.UTF8.GetBytes(destination.ToUpperInvariant())));
    }

    private static bool TakeUpdateLock(Mutex updateLock)
    {
        try { return updateLock.WaitOne(0); }
        catch (AbandonedMutexException) { return true; }
    }

    private static void InstallUpdate(bool portable, string original, string destination, string download, string digest, string version)
    {
        string originalVersion = CheckExecutable(original, null, false);
        bool canRestartOriginal = true;
        try
        {
            using (FileStream source = OpenExecutable(download))
            using (var hash = SHA256.Create())
            {
                CheckExecutable(source, download, version, !portable);
                string actualDigest = BitConverter.ToString(hash.ComputeHash(source)).Replace("-", "");
                if (!String.Equals(actualDigest, digest, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("下载文件的 SHA-256 校验失败。");
                source.Position = 0;

                if (portable)
                {
                    VerifyVersionCommand(download, version);
                    ReplacePortable(original, source);
                    return;
                }

                bool replaceInstalled = File.Exists(destination);
                string installedVersion = replaceInstalled ? CheckExecutable(destination, null, false) : null;
                RunSetup(download, replaceInstalled ? "/S /UPDATEBACKUP" : "/S", "安装", ref canRestartOriginal);
                try
                {
                    if (replaceInstalled)
                    {
                        string programs = Path.GetDirectoryName(Path.GetDirectoryName(destination));
                        string backup = Path.Combine(programs, InstalledBackupName);
                        CheckExecutable(Path.Combine(backup, ClientName), installedVersion, false);
                    }
                    CheckExecutable(destination, version, false);
                    VerifyVersionCommand(destination, version);
                    StartClient(destination);
                }
                catch (Exception launchError)
                {
                    if (replaceInstalled)
                    {
                        try { RunSetup(download, "/S /UPDATEROLLBACK", "恢复安装", ref canRestartOriginal); }
                        catch (Exception rollbackError)
                        {
                            throw new IOException(launchError.Message + "\n\n" + rollbackError.Message, launchError);
                        }
                    }
                    throw;
                }
                canRestartOriginal = false;
                if (replaceInstalled)
                {
                    try { RunSetup(download, "/S /UPDATECOMMIT", "完成安装", ref canRestartOriginal); }
                    catch (Exception commitError)
                    {
                        throw new IOException("新版已启动，无法完成旧版备份的清理。\n" + commitError.Message, commitError);
                    }
                }
            }
        }
        catch (Exception error)
        {
            if (canRestartOriginal)
            {
                try
                {
                    CheckExecutable(original, originalVersion, false);
                    StartClient(original);
                }
                catch (Exception restartError)
                {
                    throw new IOException(error.Message + "\n\n无法重新打开原程序。\n" + restartError.Message, error);
                }
            }
            throw;
        }
    }

    private static void RunSetup(string executable, string arguments, string action, ref bool canRestartOriginal)
    {
        using (Process installer = Start(executable, arguments))
        {
            if (!installer.WaitForExit(SetupWaitMilliseconds))
            {
                canRestartOriginal = false;
                throw new TimeoutException(action + "尚未完成，请等待安装程序结束后打开 EVP 远程。");
            }
            if (installer.ExitCode != 0)
                throw new IOException(action + "失败，安装程序退出代码为 " + installer.ExitCode + "。");
        }
    }

    private static void VerifyVersionCommand(string executable, string version)
    {
        using (Process probe = Start(executable, "--version", true))
        {
            var output = probe.StandardOutput.ReadToEndAsync();
            if (!probe.WaitForExit(VersionWaitMilliseconds))
            {
                try
                {
                    probe.Kill();
                    probe.WaitForExit(3000);
                }
                catch (InvalidOperationException) { }
                throw new TimeoutException("等待新版程序验证超时。");
            }
            if (probe.ExitCode != 0 || !output.Wait(1000) || output.Result.Trim() != version)
                throw new InvalidDataException("新版程序无法报告预期版本 " + version + "。");
        }
    }

    private static FileStream OpenExecutable(string path)
    {
        if ((File.GetAttributes(path) & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
            throw new InvalidDataException("程序路径必须指向普通文件。\n" + path);
        return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    }

    private static string CheckExecutable(string path, string version, bool setup)
    {
        using (FileStream stream = OpenExecutable(path))
            return CheckExecutable(stream, path, version, setup);
    }

    private static string CheckExecutable(FileStream stream, string path, string version, bool setup)
    {
        using (var reader = new BinaryReader(stream, Encoding.UTF8, true))
        {
            if (stream.Length < 64 || reader.ReadUInt16() != 0x5a4d)
                throw new InvalidDataException("程序文件不是有效的 Windows 可执行文件。\n" + path);
            stream.Position = 0x3c;
            uint offset = reader.ReadUInt32();
            if (offset < 64 || offset > stream.Length - 6)
                throw new InvalidDataException("程序文件的 PE 标头无效。\n" + path);
            stream.Position = offset;
            if (reader.ReadUInt32() != 0x4550)
                throw new InvalidDataException("程序文件的 PE 签名无效。\n" + path);
            ushort machine = reader.ReadUInt16();
            if (machine != 0x8664 && (!setup || machine != 0x014c))
                throw new InvalidDataException("程序文件不支持当前 Windows 架构。\n" + path);
        }
        stream.Position = 0;
        FileVersionInfo metadata = FileVersionInfo.GetVersionInfo(path);
        if (metadata.ProductName != "EVP Remote" || String.IsNullOrEmpty(metadata.ProductVersion) ||
            (version != null && metadata.ProductVersion != version))
            throw new InvalidDataException("程序文件的产品或版本信息不匹配。\n" + path);
        return metadata.ProductVersion;
    }

    private static void ReplacePortable(string original, Stream source)
    {
        // ReplaceFile requires the replacement and backup to be on the target volume.
        string sibling = Path.Combine(Path.GetDirectoryName(original), ".evp-update-" + Guid.NewGuid().ToString("N"));
        string replacement = sibling + ".new";
        string backup = sibling + ".bak";
        try
        {
            using (var destination = new FileStream(replacement, FileMode.CreateNew, FileAccess.Write))
            {
                source.CopyTo(destination);
                destination.Flush(true);
            }
            File.Replace(replacement, original, backup);
            StartClient(original);
        }
        catch (Exception error)
        {
            // A partial ReplaceFile failure can leave the original at the backup path.
            if (File.Exists(backup))
            {
                try
                {
                    if (!MoveFileEx(backup, original, MoveReplaceExisting))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                catch (Exception rollbackError)
                {
                    throw new IOException(error.Message + "\n\n恢复原程序失败。备份路径：\n" + backup +
                        "\n" + rollbackError.Message, error);
                }
            }
            throw;
        }
        finally
        {
            TryDelete(replacement);
        }
        TryDelete(backup);
    }

    private static Process Start(string executable, string arguments, bool captureOutput = false)
    {
        var start = new ProcessStartInfo(executable, arguments);
        start.UseShellExecute = false;
        start.WorkingDirectory = Path.GetDirectoryName(executable);
        start.RedirectStandardOutput = captureOutput;
        start.EnvironmentVariables.Remove("EVP_PORTABLE_LAUNCHER_PATH");
        start.EnvironmentVariables.Remove("EVP_PORTABLE_LAUNCHER_PID");
        Process process = Process.Start(start);
        if (process == null)
            throw new IOException("无法启动程序。\n" + executable);
        return process;
    }

    private static void StartClient(string executable)
    {
        using (Process client = Start(executable, ""))
        {
            if (client.WaitForExit(1500) && client.ExitCode != 0)
                throw new IOException("程序启动失败，错误代码为 " + client.ExitCode + "。");
        }
    }

    private static void TryDelete(string path)
    {
        try { File.Delete(path); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private static void CleanStaging(string staging, string helper, string download)
    {
        try
        {
            if ((File.GetAttributes(staging) & FileAttributes.ReparsePoint) != 0)
                return;
            TryDelete(download);

            // Windows holds the helper image open until exit. A separate process removes the two
            // known files and the empty staging directory after Windows releases the image.
            const string cleanup = "$stage = $env:EVP_CLEANUP_STAGE; " +
                "$deadline = [DateTime]::UtcNow.AddSeconds(20); " +
                "do { try { " +
                "if (![IO.Directory]::Exists($stage) -or " +
                "([IO.File]::GetAttributes($stage) -band [IO.FileAttributes]::ReparsePoint)) { exit }; " +
                "[IO.File]::Delete($env:EVP_CLEANUP_HELPER); [IO.File]::Delete($env:EVP_CLEANUP_DOWNLOAD); " +
                "[IO.Directory]::Delete($stage); exit " +
                "} catch { Start-Sleep -Milliseconds 250 } } while ([DateTime]::UtcNow -lt $deadline)";
            var start = new ProcessStartInfo(
                Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"),
                "-NoProfile -NonInteractive -Command \"" + cleanup + "\"");
            start.UseShellExecute = false;
            start.CreateNoWindow = true;
            start.WorkingDirectory = Environment.SystemDirectory;
            start.EnvironmentVariables["EVP_CLEANUP_STAGE"] = staging;
            start.EnvironmentVariables["EVP_CLEANUP_HELPER"] = helper;
            start.EnvironmentVariables["EVP_CLEANUP_DOWNLOAD"] = download;
            using (Process cleaner = Process.Start(start))
            {
            }
        }
        catch (Win32Exception) { }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern SafeWaitHandle OpenProcess(uint access, bool inherit, int id);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageName(SafeWaitHandle process, int flags, StringBuilder path, ref int size);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(SafeWaitHandle process, int milliseconds);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool MoveFileEx(string existing, string destination, uint flags);
}
