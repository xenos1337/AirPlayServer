using System.Runtime.InteropServices;

namespace AirPlayServer;

internal static unsafe partial class Win32
{
    public const uint WdaNone = 0x00;
    public const uint WdaExcludeFromCapture = 0x11;
    public const uint MbIconAsterisk = 0x40;
    public const int SwRestore = 9;
    public const int WmSizing = 0x0214;
    public const uint MonitorDefaultToNearest = 2;
    public const uint SwpNoZOrder = 0x0004;
    public const uint SwpNoActivate = 0x0010;

    public const int WmszLeft = 1;
    public const int WmszRight = 2;
    public const int WmszTop = 3;
    public const int WmszTopLeft = 4;
    public const int WmszTopRight = 5;
    public const int WmszBottom = 6;
    public const int WmszBottomLeft = 7;
    public const int WmszBottomRight = 8;

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;

        public readonly int Width => Right - Left;
        public readonly int Height => Bottom - Top;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MonitorInfo
    {
        public int Size;
        public Rect Monitor;
        public Rect Work;
        public uint Flags;
    }

    [LibraryImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool SetWindowDisplayAffinity(nint window, uint affinity);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool MessageBeep(uint type);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool ShowWindow(nint window, int command);

    [LibraryImport("user32.dll")]
    public static partial nint MonitorFromWindow(nint window, uint flags);

    [LibraryImport("user32.dll", EntryPoint = "GetMonitorInfoW")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool GetMonitorInfo(nint monitor, MonitorInfo* info);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool GetWindowRect(nint window, Rect* rect);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool GetClientRect(nint window, Rect* rect);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool SetWindowPos(nint window, nint insertAfter, int x, int y, int width, int height, uint flags);

    public static Rect GetWorkArea(nint window)
    {
        MonitorInfo info = new() { Size = sizeof(MonitorInfo) };
        nint monitor = MonitorFromWindow(window, MonitorDefaultToNearest);
        GetMonitorInfo(monitor, &info);
        return info.Work;
    }

    public static Rect GetWindowRect(nint window)
    {
        Rect rect;
        GetWindowRect(window, &rect);
        return rect;
    }

    public static Rect GetClientRect(nint window)
    {
        Rect rect;
        GetClientRect(window, &rect);
        return rect;
    }
}
