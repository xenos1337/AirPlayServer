using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Windows.Threading;

namespace AirPlayServer;

// Mirrors AirPlayCore.h. Keep field order and types identical.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ApConfig
{
    public fixed byte DeviceName[256];
    public int PinEnabled;
    public int MatchMonitor;
    public int CustomWidth;
    public int CustomHeight;
    public int Quality;
    public int ScreenCast;
    public int ScreenCastCrop;
    public int LocalVolume;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ApCallbacks
{
    public delegate* unmanaged[Cdecl]<int, byte*, void> ConnectionChanged;
    public delegate* unmanaged[Cdecl]<int, int, void> VideoSizeChanged;
    public delegate* unmanaged[Cdecl]<int, byte*, byte*, void> PinRequest;
    public delegate* unmanaged[Cdecl]<int, void> PinClosed;
    public delegate* unmanaged[Cdecl]<int, int, void> KeyUp;
    public delegate* unmanaged[Cdecl]<void> DoubleClick;
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal unsafe struct ApStats
{
    public ulong TotalFrames;
    public ulong DroppedFrames;
    public ulong TotalBytes;
    public fixed float SourceFpsHistory[Native.HistorySize];
    public fixed float DisplayFpsHistory[Native.HistorySize];
    public fixed float FrameTimeHistory[Native.HistorySize];
    public fixed float LatencyHistory[Native.HistorySize];
    public fixed float BitrateHistory[Native.HistorySize];
    public fixed float AudioQueueHistory[Native.HistorySize];
    public int HistoryIndex;
    public float SourceFps;
    public float DisplayFps;
    public float FrameTimeMs;
    public float LatencyMs;
    public float BitrateMbps;
    public float TargetFps;
    public int VideoWidth;
    public int VideoHeight;
    public int AudioUnderruns;
    public int AudioDropped;
    public int AudioQueueSize;
    public float ConnectionSeconds;
    public float Zoom;
    public int Rotation;
    public int CleanFeedReady;
    public int ServerState;
    public float SenderVolume;
    public float InputLevel;
    public int AudioStreamRate;
    public int AudioDeviceRate;
    public fixed byte Renderer[32];
}

internal static unsafe partial class Native
{
    private const string Core = "AirPlayCore";

    public const int HistorySize = 30;

    public const int ModCtrl = 1;
    public const int ModShift = 2;
    public const int ModAlt = 4;

    public const int ServerOffline = 0;
    public const int ServerRunning = 1;
    public const int ServerStarting = 2;

    public const int BonjourOk = 0;
    public const int BonjourMissing = 1;
    public const int BonjourStopped = 2;
    public const int BonjourStartFailed = 3;

    [LibraryImport(Core, EntryPoint = "ap_debug_start", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int DebugStart(string commandLine, byte* pathOut, int pathOutSize);

    [LibraryImport(Core, EntryPoint = "ap_check_bonjour")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int CheckBonjour(uint* error);

    [LibraryImport(Core, EntryPoint = "ap_is_receiver_name_valid", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int IsReceiverNameValid(string name);

    [LibraryImport(Core, EntryPoint = "ap_get_monitor_size")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void GetMonitorSize(nint window, int* width, int* height);

    [LibraryImport(Core, EntryPoint = "ap_start")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int Start(nint topLevel, ApCallbacks* callbacks, ApConfig* config);

    [LibraryImport(Core, EntryPoint = "ap_get_view")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial nint GetView();

    [LibraryImport(Core, EntryPoint = "ap_set_config")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void SetConfig(ApConfig* config);

    [LibraryImport(Core, EntryPoint = "ap_rotate")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int Rotate();

    [LibraryImport(Core, EntryPoint = "ap_reset_view")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void ResetView();

    [LibraryImport(Core, EntryPoint = "ap_restart_receiver")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void RestartReceiver();

    [LibraryImport(Core, EntryPoint = "ap_set_capture_privacy")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void SetCapturePrivacy(int enabled);

    [LibraryImport(Core, EntryPoint = "ap_get_stats")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void GetStats(ApStats* stats);

    [LibraryImport(Core, EntryPoint = "ap_pin_answer")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void PinAnswer(int generation, int allow);

    [LibraryImport(Core, EntryPoint = "ap_pin_dismiss")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void PinDismiss(int generation);

    [LibraryImport(Core, EntryPoint = "ap_stop")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void Stop();

    public static bool IsValidName(string name) => IsReceiverNameValid(name) != 0;

    public static ApStats GetStats()
    {
        ApStats stats;
        GetStats(&stats);
        return stats;
    }
}

// AirPlayCore calls these on its own threads. They only copy the arguments and
// queue the work on the UI thread; blocking here could deadlock the receiver.
internal static unsafe class NativeCallbacks
{
    private static Dispatcher? dispatcher;
    private static MainWindow? window;

    public static ApCallbacks Create(MainWindow target)
    {
        window = target;
        dispatcher = target.Dispatcher;
        return new ApCallbacks
        {
            ConnectionChanged = &OnConnectionChanged,
            VideoSizeChanged = &OnVideoSizeChanged,
            PinRequest = &OnPinRequest,
            PinClosed = &OnPinClosed,
            KeyUp = &OnKeyUp,
            DoubleClick = &OnDoubleClick,
        };
    }

    private static void Post(Action<MainWindow> action)
    {
        MainWindow? target = window;
        if (target != null)
        {
            dispatcher?.BeginInvoke(action, target);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnConnectionChanged(int connected, byte* deviceName)
    {
        string name = Marshal.PtrToStringUTF8((nint)deviceName) ?? "";
        Post(w => w.OnConnectionChanged(connected != 0, name));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnVideoSizeChanged(int width, int height)
    {
        Post(w => w.OnVideoSizeChanged(width, height));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnPinRequest(int generation, byte* remoteAddress, byte* pin)
    {
        string remote = Marshal.PtrToStringUTF8((nint)remoteAddress) ?? "";
        string code = Marshal.PtrToStringUTF8((nint)pin) ?? "";
        Post(w => w.OnPinRequest(generation, remote, code));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnPinClosed(int generation)
    {
        Post(w => w.OnPinClosed(generation));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnKeyUp(int virtualKey, int modifiers)
    {
        Post(w => w.HandleShortcut(virtualKey,
            (modifiers & Native.ModCtrl) != 0,
            (modifiers & Native.ModShift) != 0,
            (modifiers & Native.ModAlt) != 0));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnDoubleClick()
    {
        Post(w => w.ToggleFullscreen());
    }
}
