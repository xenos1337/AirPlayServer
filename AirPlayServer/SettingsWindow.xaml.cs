using System;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace AirPlayServer;

// Changes apply immediately, like the rest of the UI; the receiver restarts
// with a new name or PIN policy once it is idle.
public partial class SettingsWindow : Window
{
    private readonly Settings settings;
    private readonly Action applySettings;
    private string acceptedName;
    private bool suppressEvents = true;

    internal unsafe SettingsWindow(Settings settings, string hostName, nint mainWindow, Action applySettings)
    {
        this.settings = settings;
        this.applySettings = applySettings;
        InitializeComponent();

        int monitorWidth;
        int monitorHeight;
        Native.GetMonitorSize(mainWindow, &monitorWidth, &monitorHeight);
        MatchMonitorItem.Content = $"Match monitor ({monitorWidth} x {monitorHeight})";

        acceptedName = settings.ReceiverName(hostName);
        NameBox.Text = acceptedName;
        ResolutionCombo.SelectedIndex = settings.MatchReceiverMonitor ? 0 : 1;
        WidthBox.Text = settings.CustomWidth.ToString();
        HeightBox.Text = settings.CustomHeight.ToString();
        AutoFullscreenCheck.IsChecked = settings.AutoFullscreenOnConnect;
        MinimizeOnCloseCheck.IsChecked = settings.MinimizeOnClose;
        HideOnDisconnectCheck.IsChecked = settings.HideToTrayOnDisconnect;
        AutoStartCheck.IsChecked = Settings.QueryAutoStart();
        RequirePinCheck.IsChecked = settings.RequirePin;
        HidePinCheck.IsChecked = settings.HidePin;
        CheckForUpdatesCheck.IsChecked = settings.CheckForUpdates;
        VersionText.Text = UpdateChecker.IsReleaseBuild
            ? $"Looks for new releases on GitHub once a day. This is version {UpdateChecker.CurrentVersion}."
            : "Looks for new releases on GitHub once a day. Development builds are not checked.";
        suppressEvents = false;
        UpdateVisibility();
    }

    public bool QuitRequested { get; private set; }

    protected override void OnClosed(EventArgs e)
    {
        CommitCustomSize();
        base.OnClosed(e);
    }

    private void UpdateVisibility()
    {
        CustomSizePanel.Visibility = settings.MatchReceiverMonitor ? Visibility.Collapsed : Visibility.Visible;
        PinDescription.Text = settings.RequirePin
            ? "New connections need your approval. After you allow one, this app shows a 4-digit PIN."
            : "Off - devices on your network can connect.";
        PinWarning.Visibility = settings.RequirePin ? Visibility.Visible : Visibility.Collapsed;
        HidePinCard.Visibility = settings.RequirePin ? Visibility.Visible : Visibility.Collapsed;
    }

    private void NameBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (suppressEvents)
        {
            return;
        }
        string text = NameBox.Text;
        if (!Native.IsValidName(text) || Encoding.UTF8.GetByteCount(text) > Settings.MaxNameBytes)
        {
            // Reject the edit, as typing an unsupported character did before.
            int caret = Math.Max(0, NameBox.CaretIndex - (text.Length - acceptedName.Length));
            suppressEvents = true;
            NameBox.Text = acceptedName;
            suppressEvents = false;
            NameBox.CaretIndex = Math.Min(caret, acceptedName.Length);
            return;
        }
        acceptedName = text;
        settings.DeviceName = text;
        applySettings();
    }

    private void Resolution_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressEvents)
        {
            return;
        }
        settings.MatchReceiverMonitor = ResolutionCombo.SelectedIndex == 0;
        UpdateVisibility();
        applySettings();
    }

    private void Digits_PreviewTextInput(object sender, TextCompositionEventArgs e)
    {
        foreach (char c in e.Text)
        {
            if (!char.IsAsciiDigit(c))
            {
                e.Handled = true;
                return;
            }
        }
    }

    private void Size_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
        {
            CommitCustomSize();
        }
    }

    private void Size_LostKeyboardFocus(object sender, KeyboardFocusChangedEventArgs e)
    {
        CommitCustomSize();
    }

    private void CommitCustomSize()
    {
        int width = ParseDimension(WidthBox.Text, settings.CustomWidth,
            Settings.MinReceiverWidth, Settings.MaxReceiverWidth);
        int height = ParseDimension(HeightBox.Text, settings.CustomHeight,
            Settings.MinReceiverHeight, Settings.MaxReceiverHeight);
        WidthBox.Text = width.ToString();
        HeightBox.Text = height.ToString();
        if (width == settings.CustomWidth && height == settings.CustomHeight)
        {
            return;
        }
        settings.CustomWidth = width;
        settings.CustomHeight = height;
        applySettings();
    }

    private static int ParseDimension(string text, int current, int minimum, int maximum)
    {
        return int.TryParse(text, out int value) ? Settings.ClampReceiverDimension(value, minimum, maximum) : current;
    }

    private void Option_Click(object sender, RoutedEventArgs e)
    {
        settings.AutoFullscreenOnConnect = AutoFullscreenCheck.IsChecked == true;
        settings.MinimizeOnClose = MinimizeOnCloseCheck.IsChecked == true;
        settings.HideToTrayOnDisconnect = HideOnDisconnectCheck.IsChecked == true;
        settings.RequirePin = RequirePinCheck.IsChecked == true;
        settings.HidePin = HidePinCheck.IsChecked == true;
        settings.CheckForUpdates = CheckForUpdatesCheck.IsChecked == true;
        UpdateVisibility();
        applySettings();
    }

    private void AutoStart_Click(object sender, RoutedEventArgs e)
    {
        Settings.ApplyAutoStart(AutoStartCheck.IsChecked == true);
        AutoStartCheck.IsChecked = Settings.QueryAutoStart();
    }

    private void Quit_Click(object sender, RoutedEventArgs e)
    {
        QuitRequested = true;
        Close();
    }
}
