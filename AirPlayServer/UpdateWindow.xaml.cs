using System.Diagnostics;
using System.Windows;

namespace AirPlayServer;

public partial class UpdateWindow : Window
{
    private readonly string pageUrl;

    internal UpdateWindow(UpdateInfo update)
    {
        pageUrl = update.PageUrl;
        InitializeComponent();
        TitleText.Text = $"AirPlay Server {update.Version} is available";
        DetailText.Text = $"You have version {UpdateChecker.CurrentVersion}. " +
            "Update opens the release page, where you can download the new version.";
    }

    // Closing the window without choosing asks again on the next check.
    public bool Ignored { get; private set; }

    private void Update_Click(object sender, RoutedEventArgs e)
    {
        Process.Start(new ProcessStartInfo(pageUrl) { UseShellExecute = true });
        Close();
    }

    private void Ignore_Click(object sender, RoutedEventArgs e)
    {
        Ignored = true;
        Close();
    }
}
