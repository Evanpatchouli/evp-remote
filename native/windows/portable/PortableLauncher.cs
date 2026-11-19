using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

internal static class PortableLauncher
{
    [STAThread]
    private static int Main(string[] arguments)
    {
        bool versionRequest = arguments.Length == 1 && arguments[0] == "--version";
        try
        {
            string tempRoot = Path.GetFullPath(Path.GetTempPath());
            string runName = "evp-portable-" + Guid.NewGuid().ToString("N");
            string runtime = Path.Combine(tempRoot, runName);
            Directory.CreateDirectory(runtime);
            try
            {
                using (Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("evp-remote-client.zip"))
                {
                    if (payload == null)
                        throw new InvalidDataException("便携客户端缺少运行文件。");
                    using (var archive = new ZipArchive(payload, ZipArchiveMode.Read))
                        archive.ExtractToDirectory(runtime);
                }

                var start = new ProcessStartInfo(Path.Combine(runtime, "EvpRemote.exe"), EncodeArguments(arguments));
                start.UseShellExecute = false;
                start.WorkingDirectory = runtime;
                start.RedirectStandardOutput = versionRequest;
                using (var client = Process.Start(start))
                {
                    if (client == null)
                        throw new IOException("无法启动客户端。");
                    if (versionRequest)
                        Console.Write(client.StandardOutput.ReadToEnd());
                    client.WaitForExit();
                    return client.ExitCode;
                }
            }
            finally
            {
                RemoveRuntime(runtime, tempRoot, runName);
            }
        }
        catch (Exception error)
        {
            if (versionRequest)
                Console.Error.WriteLine(error.Message);
            else
                MessageBox.Show(error.Message, "EVP 远程便携版", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    private static void RemoveRuntime(string path, string parent, string name)
    {
        for (int attempt = 0; ; ++attempt)
        {
            var owned = new DirectoryInfo(path);
            if (!owned.Exists || owned.Name != name ||
                !String.Equals(owned.Parent.FullName.TrimEnd('\\'), parent.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                return;
            try
            {
                owned.Delete((owned.Attributes & FileAttributes.ReparsePoint) == 0);
                return;
            }
            catch (IOException)
            {
                if (attempt == 5)
                    throw;
                Thread.Sleep(100 * (attempt + 1));
            }
        }
    }

    private static string EncodeArguments(string[] arguments)
    {
        var commandLine = new StringBuilder();
        foreach (string argument in arguments)
        {
            if (commandLine.Length != 0)
                commandLine.Append(' ');
            commandLine.Append('"');
            int backslashes = 0;
            foreach (char character in argument)
            {
                if (character == '\\')
                {
                    ++backslashes;
                    continue;
                }
                commandLine.Append('\\', character == '"' ? backslashes * 2 + 1 : backslashes);
                commandLine.Append(character);
                backslashes = 0;
            }
            commandLine.Append('\\', backslashes * 2);
            commandLine.Append('"');
        }
        return commandLine.ToString();
    }
}
