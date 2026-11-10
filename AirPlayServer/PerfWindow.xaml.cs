using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;

namespace AirPlayServer;

public partial class PerfWindow : Window
{
    private readonly DispatcherTimer timer;

    public PerfWindow()
    {
        InitializeComponent();
        timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        timer.Tick += (_, _) => Refresh();
        Refresh();
        timer.Start();
    }

    protected override void OnClosed(EventArgs e)
    {
        timer.Stop();
        base.OnClosed(e);
    }

    private unsafe void Refresh()
    {
        ApStats stats = Native.GetStats();

        SourceFpsText.Text = stats.SourceFps.ToString("0.0");
        DisplayFpsText.Text = $"{stats.DisplayFps:0.0} / {stats.TargetFps:0}";
        FrameTimeText.Text = $"{stats.FrameTimeMs:0.00} ms";
        LatencyText.Text = $"{stats.LatencyMs:0.00} ms";
        BitrateText.Text = $"{stats.BitrateMbps:0.00} Mbps";
        BufferText.Text = $"{stats.AudioQueueSize} frames";

        FrameTimeChart.SetValues(History(stats.FrameTimeHistory), stats.HistoryIndex);
        BitrateChart.SetValues(History(stats.BitrateHistory), stats.HistoryIndex);
        SourceFpsChart.SetValues(History(stats.SourceFpsHistory), stats.HistoryIndex);
        DisplayFpsChart.Guide = stats.TargetFps;
        DisplayFpsChart.SetValues(History(stats.DisplayFpsHistory), stats.HistoryIndex);
        LatencyChart.SetValues(History(stats.LatencyHistory), stats.HistoryIndex);
        AudioQueueChart.SetValues(History(stats.AudioQueueHistory), stats.HistoryIndex);

        VideoText.Text = stats.VideoWidth > 0 && stats.VideoHeight > 0
            ? $"{stats.VideoWidth} x {stats.VideoHeight}{AspectLabel(stats.VideoWidth, stats.VideoHeight)}"
            : "--";
        TransferredText.Text = stats.TotalBytes > 1073741824UL
            ? $"{stats.TotalBytes / 1073741824.0:0.00} GB"
            : $"{stats.TotalBytes / 1048576.0:0.0} MB";
        FramesText.Text = stats.DroppedFrames > 0
            ? $"{stats.TotalFrames} | {stats.DroppedFrames} dropped"
            : $"{stats.TotalFrames} | stable";
        UptimeText.Text = stats.ConnectionSeconds > 0 ? Uptime((int)stats.ConnectionSeconds) : "--";
        ReceiverText.Text = stats.ServerState switch
        {
            Native.ServerRunning => "Advertised",
            Native.ServerStarting => "Starting",
            _ => "Offline",
        };

        string renderer = Marshal.PtrToStringAnsi((nint)stats.Renderer) ?? "";
        RendererText.Text = renderer.Length > 0 ? renderer : "--";
        TargetText.Text = $"{stats.TargetFps:0} fps";
        ZoomText.Text = $"{stats.Zoom:0.00}x";
        RotationText.Text = $"{stats.Rotation} deg";
        CleanFeedText.Text = stats.CleanFeedReady != 0 ? "Ready" : "Off";

        SampleRateText.Text = stats.AudioStreamRate <= 0 ? "No audio"
            : stats.AudioStreamRate == stats.AudioDeviceRate ? $"{stats.AudioStreamRate} Hz"
            : $"{stats.AudioStreamRate} Hz, resampled to {stats.AudioDeviceRate} Hz";
        SenderVolumeText.Text = $"{Percent(stats.SenderVolume)}%";
        InputLevelText.Text = $"{Percent(stats.InputLevel)}%";
        UnderrunsText.Text = stats.AudioUnderruns.ToString();
        AudioDroppedText.Text = stats.AudioDropped.ToString();
    }

    private static int Percent(float value) => (int)(Math.Clamp(value, 0f, 1f) * 100f + 0.5f);

    private static unsafe float[] History(float* history)
    {
        return new ReadOnlySpan<float>(history, Native.HistorySize).ToArray();
    }

    private static string AspectLabel(int width, int height)
    {
        float ratio = (float)width / height;
        if (ratio > 1.76f && ratio < 1.78f) return " | 16:9";
        if (ratio > 1.59f && ratio < 1.61f) return " | 16:10";
        if (ratio > 1.32f && ratio < 1.34f) return " | 4:3";
        if (ratio > 2.32f && ratio < 2.35f) return " | 21:9";
        return "";
    }

    private static string Uptime(int totalSeconds)
    {
        int hours = totalSeconds / 3600;
        int minutes = totalSeconds % 3600 / 60;
        int seconds = totalSeconds % 60;
        if (hours > 0) return $"{hours}h {minutes:00}m {seconds:00}s";
        if (minutes > 0) return $"{minutes}m {seconds:00}s";
        return $"{seconds}s";
    }

    private void Window_KeyUp(object sender, KeyEventArgs e)
    {
        if (Owner is MainWindow main)
        {
            main.HandleKeyUp(e);
        }
    }
}
