using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Threading;
using Drawing = System.Drawing;
using Forms = System.Windows.Forms;

namespace AirPlayServer;

public partial class MainWindow : Window
{
    private const double PipLongEdge = 420;
    private const double PipMinLongEdge = 240;
    private const double PipEdgeMargin = 24;
    private const int ConnectVolume = 30;
    private static readonly TimeSpan ServerStartGrace = TimeSpan.FromSeconds(5);
    private const double PanelWidth = 320;
    private static readonly Duration PanelSlideDuration = new(TimeSpan.FromMilliseconds(200));
    private static readonly Duration FadeDuration = new(TimeSpan.FromMilliseconds(200));

    private const int VkEscape = 0x1B;
    private const int VkF1 = 0x70;

    private readonly Settings settings;
    private readonly string hostName;
    private readonly DispatcherTimer statsTimer;
    private readonly DispatcherTimer disconnectTimer;
    private readonly DispatcherTimer saveTimer;
    private readonly Forms.NotifyIcon tray;
    private nint hwnd;
    private VideoHost? videoHost;
    private PinWindow? pinWindow;
    private PerfWindow? perfWindow;
    private ApStats lastStats;

    private bool connected;
    private string connectedName = "";
    private bool disconnectNotice;
    private int serverState = Native.ServerStarting;
    private DateTime serverStartingSince = DateTime.UtcNow;
    private int videoWidth;
    private int videoHeight;
    private int rotation;
    private bool videoReady;
    private FirewallStatus firewallStatus = FirewallStatus.Allowed;
    private bool firewallCheckRunning;
    private bool controlsPanelShown;
    private bool fullscreen;
    private bool pip;
    private bool capturePrivacy;
    private bool captureExcluded;
    private bool captureExclusionFailed;
    private bool quitting;
    private bool updatingControls = true;

    // Restored when leaving fullscreen or picture in picture.
    private WindowStyle savedStyle;
    private ResizeMode savedResizeMode;
    private WindowState savedState;
    private Rect savedBounds;
    private double savedMinWidth;
    private double savedMinHeight;

    internal MainWindow(Settings settings, string hostName)
    {
        this.settings = settings;
        this.hostName = hostName;
        InitializeComponent();

        statsTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
        statsTimer.Tick += (_, _) => RefreshStats();
        disconnectTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1400) };
        disconnectTimer.Tick += (_, _) =>
        {
            disconnectTimer.Stop();
            disconnectNotice = false;
            UpdateHome();
        };
        saveTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        saveTimer.Tick += (_, _) =>
        {
            saveTimer.Stop();
            settings.Save();
        };
        tray = CreateTray();

        ScreenCastCheck.IsChecked = settings.ScreenCastEnabled;
        HideInterfaceCheck.IsChecked = settings.ScreenCastHideInterface;
        CropCheck.IsChecked = settings.ScreenCastCropToVideo;
        QualityCombo.SelectedIndex = settings.QualityPreset;
        VolumeSlider.Value = settings.LocalVolume;
        updatingControls = false;

        UpdateView();
    }

    private double DpiScale => VisualTreeHelper.GetDpi(this).DpiScaleX;

    internal unsafe bool StartReceiver()
    {
        hwnd = new WindowInteropHelper(this).EnsureHandle();
        HwndSource.FromHwnd(hwnd).AddHook(WndProc);

        ApCallbacks callbacks = NativeCallbacks.Create(this);
        ApConfig config = settings.ToConfig(hostName);
        if (Native.Start(hwnd, &callbacks, &config) == 0)
        {
            return false;
        }

        // Created only now: HwndHost asks for the child window as soon as it is
        // part of a window that has a handle.
        videoHost = new VideoHost(Native.GetView()) { Visibility = Visibility.Collapsed };
        VideoArea.Children.Add(videoHost);

        UpdateCaptureExclusion();
        RefreshStats();
        statsTimer.Start();
        CheckFirewall();
        // Re-check after the user returns, e.g. from network settings.
        Activated += (_, _) =>
        {
            if (firewallStatus != FirewallStatus.Allowed)
            {
                CheckFirewall();
            }
        };
        return true;
    }

    private async void CheckFirewall()
    {
        if (firewallCheckRunning || Environment.ProcessPath is not string program)
        {
            return;
        }
        firewallCheckRunning = true;
        firewallStatus = await Task.Run(() => Firewall.Check(program));
        firewallCheckRunning = false;
        UpdateHome();
    }

    internal void Quit(int exitCode = 0)
    {
        if (quitting)
        {
            return;
        }
        quitting = true;
        // Whatever fails during cleanup, the process must still exit; with
        // every window closed it would otherwise keep running unseen.
        try
        {
            statsTimer.Stop();
            disconnectTimer.Stop();
            saveTimer.Stop();
            pinWindow?.Close();
            perfWindow?.Close();
            settings.Save();

            // Detach WPF from the video window before AirPlayCore destroys it.
            videoHost?.Dispose();
            Native.Stop();
        }
        finally
        {
            tray.Visible = false;
            tray.Dispose();
            Application.Current.Shutdown(exitCode);
        }
    }

    internal void OnConnectionChanged(bool isConnected, string deviceName)
    {
        if (quitting)
        {
            return;
        }
        bool changed = connected != isConnected;
        connected = isConnected;
        connectedName = deviceName;
        if (changed)
        {
            // AirPlayCore resets the view on every connection change.
            rotation = 0;
            videoReady = false;
        }
        if (changed && connected)
        {
            disconnectTimer.Stop();
            disconnectNotice = false;
            // Senders often report full volume when they connect; start quiet
            // and let the user turn it up.
            if (settings.LocalVolume > ConnectVolume)
            {
                settings.LocalVolume = ConnectVolume;
                updatingControls = true;
                VolumeSlider.Value = ConnectVolume;
                updatingControls = false;
                ApplySettings();
            }
            ShowFromTray();
        }
        else if (changed)
        {
            SetPictureInPicture(false);
            if (fullscreen)
            {
                ToggleFullscreen();
            }
            SetCapturePrivacy(false);
            perfWindow?.Close();
            if (settings.HideToTrayOnDisconnect)
            {
                Hide();
            }
            disconnectNotice = true;
            disconnectTimer.Stop();
            disconnectTimer.Start();
        }
        UpdateView();
    }

    // Called once the first frame of a session, or of a new video size, is on
    // screen. Until then the home screen shows that the device is connecting,
    // so the video, window size and controls all appear in one step.
    internal void OnVideoSizeChanged(int width, int height)
    {
        if (quitting || !connected)
        {
            return;
        }
        bool firstFrame = !videoReady;
        bool sizeChanged = width != videoWidth || height != videoHeight;
        videoWidth = width;
        videoHeight = height;
        videoReady = true;

        if (firstFrame && settings.AutoFullscreenOnConnect && !fullscreen)
        {
            ToggleFullscreen();
        }
        else if (sizeChanged && pip)
        {
            ResizePipToAspect();
        }
        else if (sizeChanged && !fullscreen && !settings.ScreenCastEnabled)
        {
            FitWindowToVideo();
        }
        UpdateView();
    }

    internal void OnPinRequest(int generation, string remoteAddress, string pin)
    {
        if (quitting)
        {
            return;
        }
        pinWindow?.Close();
        ShowFromTray();
        PinWindow window = new(generation, remoteAddress, pin, settings.HidePin) { Owner = this };
        window.Closed += (_, _) =>
        {
            if (pinWindow == window)
            {
                pinWindow = null;
            }
        };
        pinWindow = window;
        window.Show();
    }

    internal void OnPinClosed(int generation)
    {
        if (pinWindow != null && pinWindow.Generation == generation)
        {
            pinWindow.Close();
        }
    }

    internal void HandleKeyUp(KeyEventArgs e)
    {
        Key key = e.Key == Key.System ? e.SystemKey : e.Key;
        ModifierKeys modifiers = Keyboard.Modifiers;
        HandleShortcut(KeyInterop.VirtualKeyFromKey(key),
            (modifiers & ModifierKeys.Control) != 0,
            (modifiers & ModifierKeys.Shift) != 0,
            (modifiers & (ModifierKeys.Alt | ModifierKeys.Windows)) != 0);
    }

    internal void HandleShortcut(int virtualKey, bool ctrl, bool shift, bool alt)
    {
        if (quitting)
        {
            return;
        }
        bool command = ctrl || alt;
        switch (virtualKey)
        {
            case VkEscape:
                if (!command && fullscreen)
                {
                    ToggleFullscreen();
                }
                break;
            case 'F':
                if (!command)
                {
                    ToggleFullscreen();
                }
                break;
            case VkF1:
                if (connected && !pip)
                {
                    TogglePerfWindow();
                }
                break;
            case 'P':
                if (!command && connected)
                {
                    SetPictureInPicture(!pip);
                }
                break;
            case 'H':
                if (connected && ctrl && shift)
                {
                    SetCapturePrivacy(!capturePrivacy);
                }
                else if (!command && connected)
                {
                    // H leaves diagnostics for the controls, then toggles the controls.
                    if (perfWindow != null)
                    {
                        perfWindow.Close();
                        SetShowControls(true);
                    }
                    else
                    {
                        SetShowControls(!settings.ShowControls);
                    }
                }
                break;
            case 'R':
                if (!command)
                {
                    Rotate();
                }
                break;
        }
    }

    internal void ToggleFullscreen()
    {
        if (quitting)
        {
            return;
        }
        if (pip)
        {
            SetPictureInPicture(false);
        }
        if (!fullscreen)
        {
            SaveWindowState();
            fullscreen = true;
            // A borderless window only covers the taskbar when it is maximized
            // after the style change.
            WindowState = WindowState.Normal;
            WindowStyle = WindowStyle.None;
            ResizeMode = ResizeMode.NoResize;
            WindowState = WindowState.Maximized;
        }
        else
        {
            fullscreen = false;
            RestoreWindowState();
        }
        UpdateView();
    }

    // Applies changed settings to the receiver and schedules them to be saved.
    internal unsafe void ApplySettings()
    {
        ApConfig config = settings.ToConfig(hostName);
        Native.SetConfig(&config);
        UpdateCaptureExclusion();
        ScheduleSave();
        UpdateView();
    }

    private void ScheduleSave()
    {
        saveTimer.Stop();
        saveTimer.Start();
    }

    private Forms.NotifyIcon CreateTray()
    {
        Forms.ContextMenuStrip menu = new();
        menu.Items.Add("Open", null, (_, _) => ShowFromTray());
        menu.Items.Add("Quit", null, (_, _) => Dispatcher.BeginInvoke(() => Quit()));

        Drawing.Icon? icon = Environment.ProcessPath is string path ? Drawing.Icon.ExtractAssociatedIcon(path) : null;
        Forms.NotifyIcon notifyIcon = new()
        {
            Icon = icon ?? Drawing.SystemIcons.Application,
            Text = "AirPlay Receiver",
            ContextMenuStrip = menu,
            Visible = true,
        };
        notifyIcon.MouseClick += (_, e) =>
        {
            if (e.Button == Forms.MouseButtons.Left)
            {
                ShowFromTray();
            }
        };
        return notifyIcon;
    }

    internal void ShowFromTray()
    {
        if (quitting)
        {
            return;
        }
        Show();
        if (WindowState == WindowState.Minimized)
        {
            Win32.ShowWindow(hwnd, Win32.SwRestore);
        }
        // Activate() alone can lose to the Windows foreground lock while another
        // app is in front; a brief topmost cycle lifts the window first.
        if (!Topmost)
        {
            Topmost = true;
            Topmost = false;
        }
        Activate();
    }

    private void UpdateView(bool animatePanel = false)
    {
        bool showVideo = connected && videoReady;
        if (videoHost != null)
        {
            videoHost.Visibility = showVideo ? Visibility.Visible : Visibility.Collapsed;
        }
        if (!showVideo && HomePanel.Visibility != Visibility.Visible)
        {
            HomePanel.Visibility = Visibility.Visible;
            HomePanel.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, FadeDuration));
        }
        else if (showVideo)
        {
            HomePanel.Visibility = Visibility.Collapsed;
        }
        SetControlsPanel(showVideo && settings.ShowControls && !pip, animatePanel);
        // The open button cannot float over the native video, so it gets its own
        // strip; fullscreen stays clean and uses H instead.
        ShowControlsStrip.Visibility = showVideo && !settings.ShowControls && !pip && !fullscreen
            ? Visibility.Visible : Visibility.Collapsed;
        PipBar.Visibility = pip ? Visibility.Visible : Visibility.Collapsed;
        UpdateHome();
        RefreshControls();
    }

    // Slides the controls panel in or out. Its content keeps a fixed width and
    // is clipped, so nothing reflows while the panel moves.
    private void SetControlsPanel(bool show, bool animate)
    {
        if (show == controlsPanelShown)
        {
            return;
        }
        controlsPanelShown = show;
        if (!animate)
        {
            ControlsPanel.BeginAnimation(WidthProperty, null);
            ControlsPanel.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
            return;
        }

        DoubleAnimation slide = new(show ? PanelWidth : 0, PanelSlideDuration)
        {
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
        };
        if (show && ControlsPanel.Visibility != Visibility.Visible)
        {
            slide.From = 0;
        }
        else if (!show)
        {
            slide.Completed += (_, _) =>
            {
                if (!controlsPanelShown)
                {
                    ControlsPanel.Visibility = Visibility.Collapsed;
                }
            };
        }
        ControlsPanel.Visibility = Visibility.Visible;
        ControlsPanel.BeginAnimation(WidthProperty, slide);
    }

    private void UpdateHome()
    {
        bool running = serverState == Native.ServerRunning;
        // Registering with Bonjour normally takes well under a second; only a
        // hung service keeps it starting longer.
        bool starting = serverState == Native.ServerStarting &&
            DateTime.UtcNow - serverStartingSince < ServerStartGrace;
        bool unavailable = !running && !starting;

        if (connected)
        {
            HomeTitle.Text = connectedName.Length > 0 ? $"Connecting to {connectedName}" : "Connecting";
            HomeDetail.Text = "Waiting for the first video frame.";
        }
        else if (disconnectNotice)
        {
            HomeTitle.Text = connectedName.Length > 0 ? $"{connectedName} disconnected" : "Stream disconnected";
            HomeDetail.Text = "Waiting for another device";
        }
        else if (running)
        {
            HomeTitle.Text = "Waiting for a device";
            HomeDetail.Text = "Open Screen Mirroring on your Apple device and choose the receiver below.";
        }
        else if (starting)
        {
            HomeTitle.Text = "Starting receiver";
            HomeDetail.Text = "Registering with Bonjour.";
        }
        else
        {
            HomeTitle.Text = "Receiver unavailable";
            HomeDetail.Text = "Bonjour is not responding. Restart it to make the receiver available again.";
        }
        RestartBonjourButton.Visibility = unavailable && !disconnectNotice && !connected
            ? Visibility.Visible : Visibility.Collapsed;
        FirewallCard.Visibility = firewallStatus != FirewallStatus.Allowed && !connected
            ? Visibility.Visible : Visibility.Collapsed;
        if (firewallStatus == FirewallStatus.PublicNetwork)
        {
            FirewallTitle.Text = "This network is set to Public";
            FirewallDetail.Text = "Windows Firewall blocks devices on public networks. Set this network to Private if you trust it.";
            FirewallButton.Content = "Network settings";
        }
        else
        {
            FirewallTitle.Text = "Windows Firewall is blocking the receiver";
            FirewallDetail.Text = "Devices can see this PC but cannot connect.";
            FirewallButton.Content = "Allow";
        }
        ReceiverNameText.Text = settings.ReceiverName(hostName);
        StatusText.Text = running ? "Ready"
            : starting ? "Starting"
            : serverState == Native.ServerStarting ? "Waiting for Bonjour" : "Offline";
        StatusDot.SetResourceReference(Shape.FillProperty,
            running ? "SystemFillColorSuccessBrush"
            : starting ? "SystemFillColorCautionBrush" : "SystemFillColorCriticalBrush");
    }

    private void RefreshStats()
    {
        lastStats = Native.GetStats();
        if (lastStats.ServerState != serverState)
        {
            serverState = lastStats.ServerState;
            if (serverState == Native.ServerStarting)
            {
                serverStartingSince = DateTime.UtcNow;
            }
            UpdateHome();
        }
        else if (serverState == Native.ServerStarting)
        {
            UpdateHome();
        }
        if (ControlsPanel.Visibility == Visibility.Visible)
        {
            RefreshControls();
        }
    }

    private void RefreshControls()
    {
        ApStats stats = lastStats;

        string sender = connectedName.Length > 0 ? connectedName : "AirPlay session";
        SessionText.Text = sender;
        SessionText.ToolTip = $"Sender: {sender}\nReceiver: {settings.ReceiverName(hostName)}";

        string resolution = stats.VideoWidth > 0 && stats.VideoHeight > 0
            ? $"{stats.VideoWidth} x {stats.VideoHeight}" : "--";
        string fps = stats.DisplayFps > 0 ? stats.DisplayFps.ToString("0.0") : "--";
        string bitrate = stats.BitrateMbps > 0 ? $"{stats.BitrateMbps:0.0} Mbps" : "--";
        StatsText.Text = $"{resolution}   {fps} fps   {bitrate}";
        DroppedText.Text = $"{stats.DroppedFrames} of {stats.TotalFrames} frames dropped";
        DroppedText.Visibility = stats.DroppedFrames > 0 ? Visibility.Visible : Visibility.Collapsed;

        ViewText.Text = $"{stats.Zoom:0.0}x / {stats.Rotation} deg";
        ResetViewButton.IsEnabled = stats.Zoom > 1.0001f || stats.Rotation != 0;
        PrivacyToggle.IsChecked = capturePrivacy;
        ExclusionWarning.Visibility = captureExclusionFailed ? Visibility.Visible : Visibility.Collapsed;

        ScreenCastOptions.Visibility = settings.ScreenCastEnabled ? Visibility.Visible : Visibility.Collapsed;
        CleanFeedWaitingText.Visibility = stats.CleanFeedReady == 0 ? Visibility.Visible : Visibility.Collapsed;
        CaptureProtectionText.Visibility = settings.ScreenCastHideInterface && captureExclusionFailed
            ? Visibility.Visible : Visibility.Collapsed;

        AudioText.Text = $"Sender {Percent(stats.SenderVolume)}%  Input {Percent(stats.InputLevel)}%";
        VolumeText.Text = $"{settings.LocalVolume}%";
    }

    private static int Percent(float value) => (int)(Math.Clamp(value, 0f, 1f) * 100f + 0.5f);

    private void SetShowControls(bool show)
    {
        settings.ShowControls = show;
        ScheduleSave();
        UpdateView(animatePanel: true);
    }

    private void Rotate()
    {
        rotation = Native.Rotate();
        if (pip)
        {
            ResizePipToAspect();
        }
    }

    private void TogglePerfWindow()
    {
        if (perfWindow != null)
        {
            perfWindow.Close();
            return;
        }
        PerfWindow window = new() { Owner = this };
        window.Closed += (_, _) =>
        {
            if (perfWindow == window)
            {
                perfWindow = null;
            }
        };
        perfWindow = window;
        window.Show();
    }

    private void SetCapturePrivacy(bool enabled)
    {
        if ((enabled && !connected) || capturePrivacy == enabled)
        {
            return;
        }
        capturePrivacy = enabled;
        if (!UpdateCaptureExclusion() && enabled)
        {
            capturePrivacy = false;
            UpdateCaptureExclusion();
            RefreshControls();
            return;
        }
        Native.SetCapturePrivacy(enabled ? 1 : 0);
        // Keep the controls available locally so privacy can be turned off
        // again; capture software cannot see this window.
        SetShowControls(true);
    }

    // Privacy mode and Screen Cast's "Hide interface" both exclude this window
    // from screen capture.
    private bool UpdateCaptureExclusion()
    {
        bool exclude = capturePrivacy || (settings.ScreenCastEnabled && settings.ScreenCastHideInterface);
        if (exclude == captureExcluded)
        {
            return !exclude || !captureExclusionFailed;
        }
        if (Win32.SetWindowDisplayAffinity(hwnd, exclude ? Win32.WdaExcludeFromCapture : Win32.WdaNone))
        {
            captureExcluded = exclude;
            captureExclusionFailed = false;
            return true;
        }
        if (exclude)
        {
            captureExclusionFailed = true;
        }
        return false;
    }

    private void SaveWindowState()
    {
        savedStyle = WindowStyle;
        savedResizeMode = ResizeMode;
        savedState = WindowState == WindowState.Minimized ? WindowState.Normal : WindowState;
        savedBounds = WindowState == WindowState.Normal ? new Rect(Left, Top, Width, Height) : RestoreBounds;
        savedMinWidth = MinWidth;
        savedMinHeight = MinHeight;
    }

    private void RestoreWindowState()
    {
        WindowState = WindowState.Normal;
        WindowStyle = savedStyle;
        ResizeMode = savedResizeMode;
        MinWidth = savedMinWidth;
        MinHeight = savedMinHeight;
        Left = savedBounds.Left;
        Top = savedBounds.Top;
        Width = savedBounds.Width;
        Height = savedBounds.Height;
        WindowState = savedState;
    }

    private void SetPictureInPicture(bool enabled)
    {
        if (pip == enabled || (enabled && !connected))
        {
            return;
        }
        if (enabled)
        {
            if (fullscreen)
            {
                ToggleFullscreen();
            }
            SaveWindowState();
            pip = true;
            perfWindow?.Close();
            WindowState = WindowState.Normal;
            WindowStyle = WindowStyle.None;
            ResizeMode = ResizeMode.CanResize;
            MinWidth = 0;
            MinHeight = 0;
            Topmost = true;
            Title = "AirPlay Receiver - Picture in Picture";
            UpdateView();
            SizePip(PipLongEdge, true);
        }
        else
        {
            pip = false;
            Topmost = false;
            Title = "AirPlay Receiver";
            RestoreWindowState();
            SetShowControls(true);
        }
    }

    private double PipAspect()
    {
        int width = videoWidth > 0 ? videoWidth : 16;
        int height = videoHeight > 0 ? videoHeight : 9;
        if (rotation == 90 || rotation == 270)
        {
            (width, height) = (height, width);
        }
        return (double)width / height;
    }

    // Sizes the picture-in-picture video area so its long edge is longEdge DIPs
    // at the video's aspect ratio. The window is either placed in the work
    // area's bottom-right corner or kept centered where it is.
    private void SizePip(double longEdge, bool placeBottomRight)
    {
        double aspect = PipAspect();
        double scale = DpiScale;
        double videoWidthDips = aspect >= 1 ? longEdge : longEdge * aspect;
        double videoHeightDips = aspect >= 1 ? longEdge / aspect : longEdge;
        Win32.Rect window = Win32.GetWindowRect(hwnd);
        Win32.Rect client = Win32.GetClientRect(hwnd);
        int outerWidth = (int)Math.Round(videoWidthDips * scale) + window.Width - client.Width;
        int outerHeight = (int)Math.Round((videoHeightDips + PipBar.Height) * scale) + window.Height - client.Height;

        int x;
        int y;
        if (placeBottomRight)
        {
            Win32.Rect work = Win32.GetWorkArea(hwnd);
            int margin = (int)Math.Round(PipEdgeMargin * scale);
            x = Math.Max(work.Left, work.Right - outerWidth - margin);
            y = Math.Max(work.Top, work.Bottom - outerHeight - margin);
        }
        else
        {
            x = window.Left + (window.Width - outerWidth) / 2;
            y = window.Top + (window.Height - outerHeight) / 2;
        }
        Win32.SetWindowPos(hwnd, 0, x, y, outerWidth, outerHeight, Win32.SwpNoZOrder | Win32.SwpNoActivate);
    }

    private void ResizePipToAspect()
    {
        double scale = DpiScale;
        Win32.Rect client = Win32.GetClientRect(hwnd);
        double longEdge = Math.Max(client.Width / scale, client.Height / scale - PipBar.Height);
        SizePip(Math.Max(longEdge, PipMinLongEdge), false);
    }

    // Fits the window to the video within 95% x 80% of the monitor's work area,
    // keeping the window's center where the user left it.
    private void FitWindowToVideo()
    {
        if (videoWidth <= 0 || videoHeight <= 0 || WindowState != WindowState.Normal)
        {
            return;
        }
        double scale = DpiScale;
        Win32.Rect work = Win32.GetWorkArea(hwnd);
        Win32.Rect window = Win32.GetWindowRect(hwnd);
        Win32.Rect client = Win32.GetClientRect(hwnd);
        int chromeWidth = window.Width - client.Width;
        if (controlsPanelShown)
        {
            chromeWidth += (int)Math.Round(PanelWidth * scale);
        }
        if (ShowControlsStrip.Visibility == Visibility.Visible)
        {
            chromeWidth += (int)Math.Round(ShowControlsStrip.Width * scale);
        }
        int chromeHeight = window.Height - client.Height;

        int maxWidth = Math.Max(1, work.Width * 95 / 100 - chromeWidth);
        int maxHeight = Math.Max(1, work.Height * 80 / 100 - chromeHeight);
        int targetWidth = maxWidth;
        int targetHeight = (int)((long)targetWidth * videoHeight / videoWidth);
        if (targetHeight > maxHeight)
        {
            targetHeight = maxHeight;
            targetWidth = (int)((long)targetHeight * videoWidth / videoHeight);
        }

        int outerWidth = Math.Clamp(targetWidth + chromeWidth, (int)Math.Round(MinWidth * scale), work.Width);
        int outerHeight = Math.Clamp(targetHeight + chromeHeight, (int)Math.Round(MinHeight * scale), work.Height);
        int x = Math.Clamp((window.Left + window.Right - outerWidth) / 2, work.Left, work.Right - outerWidth);
        int y = Math.Clamp((window.Top + window.Bottom - outerHeight) / 2, work.Top, work.Bottom - outerHeight);
        Win32.SetWindowPos(hwnd, 0, x, y, outerWidth, outerHeight, Win32.SwpNoZOrder | Win32.SwpNoActivate);
    }

    private unsafe nint WndProc(nint window, int message, nint wParam, nint lParam, ref bool handled)
    {
        if (message == Win32.WmSizing && pip)
        {
            ConstrainPipRect((int)wParam, (Win32.Rect*)lParam);
            handled = true;
            return 1;
        }
        return 0;
    }

    // Keeps the picture-in-picture video area at the video's aspect ratio while
    // the user drags an edge or corner.
    private unsafe void ConstrainPipRect(int edge, Win32.Rect* rect)
    {
        double aspect = PipAspect();
        double scale = DpiScale;
        Win32.Rect window = Win32.GetWindowRect(hwnd);
        Win32.Rect client = Win32.GetClientRect(hwnd);
        int chromeWidth = window.Width - client.Width;
        int chromeHeight = window.Height - client.Height + (int)Math.Round(PipBar.Height * scale);
        int width = rect->Width - chromeWidth;
        int height = rect->Height - chromeHeight;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        bool left = edge is Win32.WmszLeft or Win32.WmszTopLeft or Win32.WmszBottomLeft;
        bool right = edge is Win32.WmszRight or Win32.WmszTopRight or Win32.WmszBottomRight;
        bool top = edge is Win32.WmszTop or Win32.WmszTopLeft or Win32.WmszTopRight;
        bool bottom = edge is Win32.WmszBottom or Win32.WmszBottomLeft or Win32.WmszBottomRight;

        int widthFromHeight = (int)(height * aspect + 0.5);
        int heightFromWidth = (int)(width / aspect + 0.5);
        bool driveFromWidth = !top && !bottom;
        if ((left || right) && (top || bottom))
        {
            driveFromWidth = Math.Abs(heightFromWidth - height) <= Math.Abs(widthFromHeight - width);
        }
        int adjustedWidth = driveFromWidth ? width : widthFromHeight;
        int adjustedHeight = driveFromWidth ? heightFromWidth : height;
        int minimumLongEdge = (int)Math.Round(PipMinLongEdge * scale);
        int minimumWidth = aspect >= 1 ? minimumLongEdge : (int)(minimumLongEdge * aspect + 0.5);
        int minimumHeight = aspect >= 1 ? (int)(minimumLongEdge / aspect + 0.5) : minimumLongEdge;
        if (adjustedWidth < minimumWidth || adjustedHeight < minimumHeight)
        {
            adjustedWidth = minimumWidth;
            adjustedHeight = minimumHeight;
        }
        adjustedWidth += chromeWidth;
        adjustedHeight += chromeHeight;

        if (left)
        {
            rect->Left = rect->Right - adjustedWidth;
        }
        else if (right)
        {
            rect->Right = rect->Left + adjustedWidth;
        }
        else
        {
            int centerX = (rect->Left + rect->Right) / 2;
            rect->Left = centerX - adjustedWidth / 2;
            rect->Right = rect->Left + adjustedWidth;
        }
        if (top)
        {
            rect->Top = rect->Bottom - adjustedHeight;
        }
        else if (bottom)
        {
            rect->Bottom = rect->Top + adjustedHeight;
        }
        else
        {
            int centerY = (rect->Top + rect->Bottom) / 2;
            rect->Top = centerY - adjustedHeight / 2;
            rect->Bottom = rect->Top + adjustedHeight;
        }
    }

    private void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (quitting)
        {
            return;
        }
        e.Cancel = true;
        if (settings.MinimizeOnClose)
        {
            Hide();
        }
        else
        {
            Dispatcher.BeginInvoke(() => Quit());
        }
    }

    private void Window_KeyUp(object sender, KeyEventArgs e)
    {
        HandleKeyUp(e);
    }

    private void PipBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        DragMove();
    }

    private void HideControls_Click(object sender, RoutedEventArgs e)
    {
        SetShowControls(false);
    }

    private void ShowControls_Click(object sender, RoutedEventArgs e)
    {
        SetShowControls(true);
    }

    private void ExitPip_Click(object sender, RoutedEventArgs e)
    {
        SetPictureInPicture(false);
    }

    private void Rotate_Click(object sender, RoutedEventArgs e)
    {
        Rotate();
    }

    private void ResetView_Click(object sender, RoutedEventArgs e)
    {
        Native.ResetView();
        rotation = 0;
    }

    private void Pip_Click(object sender, RoutedEventArgs e)
    {
        SetPictureInPicture(!pip);
    }

    private void Privacy_Click(object sender, RoutedEventArgs e)
    {
        SetCapturePrivacy(PrivacyToggle.IsChecked == true);
        // Shows the actual state if privacy could not be turned on.
        RefreshControls();
    }

    private void ScreenCast_Click(object sender, RoutedEventArgs e)
    {
        settings.ScreenCastEnabled = ScreenCastCheck.IsChecked == true;
        settings.ScreenCastHideInterface = HideInterfaceCheck.IsChecked == true;
        settings.ScreenCastCropToVideo = CropCheck.IsChecked == true;
        ApplySettings();
    }

    private void Quality_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (updatingControls || QualityCombo.SelectedIndex < 0)
        {
            return;
        }
        settings.QualityPreset = QualityCombo.SelectedIndex;
        ApplySettings();
    }

    private void Volume_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (updatingControls)
        {
            return;
        }
        settings.LocalVolume = (int)Math.Round(VolumeSlider.Value);
        ApplySettings();
    }

    private async void RestartBonjour_Click(object sender, RoutedEventArgs e)
    {
        RestartBonjourButton.IsEnabled = false;
        BonjourRestartResult result = await Task.Run(BonjourService.Restart);
        RestartBonjourButton.IsEnabled = true;
        if (result == BonjourRestartResult.Restarted)
        {
            Native.RestartReceiver();
        }
        else if (result == BonjourRestartResult.Failed)
        {
            MessageBox.Show(this, "Bonjour Service could not be restarted.\n\nStart the service manually via services.msc.",
                "AirPlay Server - Bonjour Error", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private async void Firewall_Click(object sender, RoutedEventArgs e)
    {
        if (firewallStatus == FirewallStatus.PublicNetwork)
        {
            Process.Start(new ProcessStartInfo("ms-settings:network-status") { UseShellExecute = true });
            return;
        }
        if (Environment.ProcessPath is not string program)
        {
            return;
        }
        FirewallButton.IsEnabled = false;
        await Task.Run(() => Firewall.AllowProgram(program));
        FirewallButton.IsEnabled = true;
        CheckFirewall();
    }

    private void Settings_Click(object sender, RoutedEventArgs e)
    {
        SettingsWindow dialog = new(settings, hostName, hwnd, ApplySettings) { Owner = this };
        dialog.ShowDialog();
        if (dialog.QuitRequested)
        {
            Quit();
        }
    }
}
