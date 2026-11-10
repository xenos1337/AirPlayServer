using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;

namespace AirPlayServer;

public partial class App : Application
{
    private const string InstanceMutexName = @"Local\AirPlayServer.Instance";
    private const string ActivateEventName = @"Local\AirPlayServer.Activate";

    // Held for the life of the process; both receivers would need ports 5001/7001.
    private Mutex? instanceMutex;
    private EventWaitHandle? activateEvent;
    // Must stay referenced: a collected registration stops listening.
    private RegisteredWaitHandle? activateWait;

    protected override unsafe void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        string commandLine = string.Join(" ", e.Args);
        instanceMutex = new Mutex(true, InstanceMutexName, out bool firstInstance);
        if (!firstInstance)
        {
            // Bring the running receiver to the front instead, unless this is the
            // sign-in entry starting in the tray.
            if (!commandLine.Contains("--tray") &&
                EventWaitHandle.TryOpenExisting(ActivateEventName, out EventWaitHandle? running))
            {
                running.Set();
                running.Dispose();
            }
            Shutdown();
            return;
        }
        activateEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ActivateEventName);

        bool debugRequested = commandLine.Contains("--debug") || commandLine.Contains("/debug");
        byte* logPath = stackalloc byte[260];
        if (Native.DebugStart(commandLine, logPath, 260) != 0)
        {
            MessageBox.Show("Debug logging is enabled.\n\nLog file:\n" + Marshal.PtrToStringAnsi((nint)logPath),
                "AirPlayServer - Debug Logging", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else if (debugRequested)
        {
            MessageBox.Show("Debug logging was requested, but the log file could not be created.\n\n" +
                "Check that %LOCALAPPDATA% is writable and try again.",
                "AirPlayServer - Debug Logging Error", MessageBoxButton.OK, MessageBoxImage.Error);
        }

        if (!CheckBonjour())
        {
            Shutdown(1);
            return;
        }

        MainWindow window = new(Settings.Load(), GetHostName());
        MainWindow = window;
        if (!window.StartReceiver())
        {
            MessageBox.Show("The AirPlay receiver could not be started.", "AirPlayServer",
                MessageBoxButton.OK, MessageBoxImage.Error);
            window.Quit(1);
            return;
        }

        activateWait = ThreadPool.RegisterWaitForSingleObject(activateEvent,
            (_, _) => window.Dispatcher.InvokeAsync(window.ShowFromTray), null, Timeout.Infinite, false);

        if (commandLine.Contains("--minimized"))
        {
            window.WindowState = WindowState.Minimized;
        }
        // --tray (used by the sign-in entry) stays in the notification area
        // until a device connects.
        if (!commandLine.Contains("--tray"))
        {
            window.Show();
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        activateWait?.Unregister(null);
        activateEvent?.Dispose();
        instanceMutex?.Dispose();
        base.OnExit(e);
    }

    protected override void OnSessionEnding(SessionEndingCancelEventArgs e)
    {
        base.OnSessionEnding(e);
        (MainWindow as MainWindow)?.Quit();
    }

    private static unsafe bool CheckBonjour()
    {
        uint error = 0;
        int status = Native.CheckBonjour(&error);
        switch (status)
        {
            case Native.BonjourMissing:
                MessageBoxResult choice = MessageBox.Show(
                    "Apple Bonjour is not installed.\n\n" +
                    "Bonjour is required for AirPlay device discovery.\n\n" +
                    "Click OK to open the Bonjour download page, or Cancel to exit.",
                    "AirPlay Server - Bonjour Not Found", MessageBoxButton.OKCancel, MessageBoxImage.Warning);
                if (choice == MessageBoxResult.OK)
                {
                    Process.Start(new ProcessStartInfo("https://support.apple.com/kb/DL999") { UseShellExecute = true });
                }
                return false;
            case Native.BonjourStopped:
            case Native.BonjourStartFailed:
                string problem = status == Native.BonjourStopped
                    ? "Bonjour Service is installed but not running."
                    : $"Bonjour Service could not be started (error {error}).";
                MessageBoxResult start = MessageBox.Show(problem + "\n\n" +
                    "Start it now? Windows will ask for administrator permission.",
                    "AirPlay Server - Bonjour Stopped", MessageBoxButton.YesNo, MessageBoxImage.Warning);
                if (start != MessageBoxResult.Yes)
                {
                    return false;
                }
                switch (BonjourService.Restart())
                {
                    case BonjourRestartResult.Restarted:
                        return true;
                    case BonjourRestartResult.Failed:
                        MessageBox.Show("Bonjour Service could not be started.\n\n" +
                            "Start the service manually via services.msc.",
                            "AirPlay Server - Bonjour Error", MessageBoxButton.OK, MessageBoxImage.Error);
                        return false;
                    default:
                        return false;
                }
            default:
                return true;
        }
    }

    private static string GetHostName()
    {
        string name = "";
        try
        {
            name = Dns.GetHostName();
        }
        catch (SocketException)
        {
        }
        if (name.Length == 0)
        {
            name = Environment.MachineName;
        }
        return name.Length > 0 ? name : "AirPlay Server";
    }
}
