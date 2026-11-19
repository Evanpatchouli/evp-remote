using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

internal static class InputHelper
{
    private const int MonitorCount = 80;
    private const uint DesktopReadObjects = 0x0001;
    private const int ObjectName = 2;
    private const uint MouseMoveAbsolute = 0x8001;
    private const uint MouseWheel = 0x0800;
    private const uint KeyboardScanCode = 0x0008;
    private const uint KeyboardExtendedKey = 0x0001;
    private const uint KeyboardKeyUp = 0x0002;
    private const int ExtendedScanPrefix = 0xe000;

    private static readonly uint[] ButtonDown = { 0x0002, 0x0020, 0x0008 };
    private static readonly uint[] ButtonUp = { 0x0004, 0x0040, 0x0010 };
    private static readonly List<int> HeldButtons = new List<int>();
    private static readonly List<int> HeldKeys = new List<int>();
    private static readonly Input[] Pending = new Input[1];
    private static readonly int InputSize = Marshal.SizeOf(typeof(Input));

    [StructLayout(LayoutKind.Sequential)]
    private struct Input
    {
        internal uint Type;
        internal InputData Data;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputData
    {
        [FieldOffset(0)] internal MouseInput Mouse;
        [FieldOffset(0)] internal KeyInput Keyboard;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MouseInput
    {
        internal int X;
        internal int Y;
        internal uint Data;
        internal uint Flags;
        internal uint Time;
        internal UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KeyInput
    {
        internal ushort VirtualKey;
        internal ushort ScanCode;
        internal uint Flags;
        internal uint Time;
        internal UIntPtr ExtraInfo;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint count, Input[] inputs, int inputSize);

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);

    [DllImport("user32.dll")]
    private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);

    [DllImport("user32.dll")]
    private static extern bool CloseDesktop(IntPtr desktop);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetUserObjectInformation(IntPtr handle, int index, StringBuilder data,
        int bytes, out int requiredBytes);

    private static int Main()
    {
        try
        {
            CheckDisplay();
            CheckDesktop();
            Console.WriteLine("READY");
            string line;
            while ((line = Console.ReadLine()) != null)
                Apply(line);
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
        finally
        {
            ReleaseAll();
        }
    }

    private static void CheckDisplay()
    {
        if (GetSystemMetrics(MonitorCount) != 1)
            throw new InvalidOperationException("Windows input requires exactly one active display.");
    }

    private static void CheckDesktop()
    {
        IntPtr desktop = OpenInputDesktop(0, false, DesktopReadObjects);
        if (desktop != IntPtr.Zero)
        {
            try
            {
                StringBuilder name = new StringBuilder(64);
                int requiredBytes;
                if (GetUserObjectInformation(desktop, ObjectName, name, name.Capacity * 2, out requiredBytes) &&
                    string.Equals(name.ToString(), "Default", StringComparison.OrdinalIgnoreCase))
                    return;
            }
            finally
            {
                CloseDesktop(desktop);
            }
        }
        throw new InvalidOperationException("The interactive Windows desktop is unavailable.");
    }

    private static int Number(string value, int min, int max)
    {
        int number;
        if (!int.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out number) ||
            number < min || number > max)
            throw new InvalidOperationException("Invalid Windows input command.");
        return number;
    }

    private static void Apply(string line)
    {
        if (line == "release")
        {
            RequireSent(ReleaseAll());
            return;
        }

        CheckDisplay();
        if (line == "ping")
        {
            CheckDesktop();
            return;
        }

        string[] parts = line.Split(' ');
        if (parts.Length == 2 && parts[0] == "wheel")
        {
            int delta = Number(parts[1], -32768, 32767);
            RequireSent(Mouse(MouseWheel, 0, 0, unchecked((uint)delta)));
            return;
        }
        if (parts.Length == 3)
        {
            switch (parts[0])
            {
                case "move":
                    RequireSent(Mouse(MouseMoveAbsolute, Number(parts[1], 0, 65535), Number(parts[2], 0, 65535), 0));
                    return;
                case "button":
                    ChangeButton(Number(parts[1], 0, 2), Number(parts[2], 0, 1) != 0);
                    return;
                case "key":
                    int scan = Number(parts[1], 1, 0xe0ff);
                    if (scan <= 0xff || scan > ExtendedScanPrefix)
                    {
                        ChangeKey(scan, Number(parts[2], 0, 1) != 0);
                        return;
                    }
                    break;
            }
        }
        throw new InvalidOperationException("Invalid Windows input command.");
    }

    private static void ChangeButton(int button, bool down)
    {
        bool held = HeldButtons.Contains(button);
        if (held == down) return;
        RequireSent(Mouse(down ? ButtonDown[button] : ButtonUp[button], 0, 0, 0));
        if (down) HeldButtons.Add(button);
        else HeldButtons.Remove(button);
    }

    private static void ChangeKey(int scan, bool down)
    {
        bool held = HeldKeys.Contains(scan);
        if (!held && !down) return;
        RequireSent(Key(scan, down));
        if (!down) HeldKeys.Remove(scan);
        else if (!held) HeldKeys.Add(scan);
    }

    private static bool ReleaseAll()
    {
        for (int i = HeldButtons.Count - 1; i >= 0; i--)
        {
            if (Mouse(ButtonUp[HeldButtons[i]], 0, 0, 0)) HeldButtons.RemoveAt(i);
        }
        for (int i = HeldKeys.Count - 1; i >= 0; i--)
        {
            if (Key(HeldKeys[i], false)) HeldKeys.RemoveAt(i);
        }
        return HeldButtons.Count == 0 && HeldKeys.Count == 0;
    }

    private static bool Mouse(uint flags, int x, int y, uint data)
    {
        Pending[0] = new Input {
            Type = 0,
            Data = new InputData { Mouse = new MouseInput { Flags = flags, X = x, Y = y, Data = data } }
        };
        return SendInput(1, Pending, InputSize) == 1;
    }

    private static bool Key(int scan, bool down)
    {
        uint flags = KeyboardScanCode;
        if (scan > 0xff) flags |= KeyboardExtendedKey;
        if (!down) flags |= KeyboardKeyUp;
        Pending[0] = new Input {
            Type = 1,
            Data = new InputData { Keyboard = new KeyInput { ScanCode = (ushort)(scan & 0xff), Flags = flags } }
        };
        return SendInput(1, Pending, InputSize) == 1;
    }

    private static void RequireSent(bool sent)
    {
        if (!sent) throw new InvalidOperationException("Windows rejected input on the current desktop.");
    }
}
