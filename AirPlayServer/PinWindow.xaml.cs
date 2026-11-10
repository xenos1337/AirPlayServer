using System;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;

namespace AirPlayServer;

// Asks whether a device may connect, then shows the receiver's temporary PIN.
// The receiver waits for the answer and keeps the PIN valid until this closes.
public partial class PinWindow : Window
{
    private readonly string pin;
    private readonly bool hidePin;
    private DispatcherTimer? privacyDelay;

    internal PinWindow(int generation, string remoteAddress, string pin, bool hidePin)
    {
        Generation = generation;
        this.pin = pin;
        this.hidePin = hidePin;
        InitializeComponent();
        RemoteText.Text = $"From {remoteAddress}";
        Win32.MessageBeep(Win32.MbIconAsterisk);
    }

    public int Generation { get; }

    protected override void OnClosed(EventArgs e)
    {
        privacyDelay?.Stop();
        // Retires the request however the window was closed; a no-op once the
        // receiver has already moved on.
        Native.PinDismiss(Generation);
        base.OnClosed(e);
    }

    private void Allow_Click(object sender, RoutedEventArgs e)
    {
        AskPanel.Visibility = Visibility.Collapsed;
        if (!hidePin)
        {
            ShowPin();
            return;
        }

        nint hwnd = new WindowInteropHelper(this).Handle;
        if (!Win32.SetWindowDisplayAffinity(hwnd, Win32.WdaExcludeFromCapture))
        {
            FailedPanel.Visibility = Visibility.Visible;
            return;
        }
        // Give the capture exclusion time to take effect before the digits are
        // drawn.
        PinPanel.Visibility = Visibility.Visible;
        privacyDelay = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        privacyDelay.Tick += (_, _) =>
        {
            privacyDelay.Stop();
            ShowPin();
        };
        privacyDelay.Start();
    }

    private void ShowPin()
    {
        PinText.Text = pin;
        PinPanel.Visibility = Visibility.Visible;
        Native.PinAnswer(Generation, 1);
    }

    private void Deny_Click(object sender, RoutedEventArgs e)
    {
        Native.PinAnswer(Generation, 0);
        Close();
    }

    private void Cancel_Click(object sender, RoutedEventArgs e)
    {
        Close();
    }
}
