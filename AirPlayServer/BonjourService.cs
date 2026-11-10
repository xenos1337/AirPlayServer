using System.ComponentModel;
using System.Diagnostics;

namespace AirPlayServer;

internal enum BonjourRestartResult
{
    Restarted,
    Declined,
    Failed,
}

internal static class BonjourService
{
    private const int ErrorCancelled = 1223;

    // Stops and starts the Bonjour service. Controlling a service needs
    // administrator rights, so Windows shows a UAC prompt. Blocks until done.
    public static BonjourRestartResult Restart()
    {
        ProcessStartInfo info = new("cmd.exe", "/c net stop \"Bonjour Service\" & net start \"Bonjour Service\"")
        {
            UseShellExecute = true,
            Verb = "runas",
            WindowStyle = ProcessWindowStyle.Hidden,
        };
        try
        {
            using Process? process = Process.Start(info);
            if (process == null)
            {
                return BonjourRestartResult.Failed;
            }
            process.WaitForExit();
            return process.ExitCode == 0 ? BonjourRestartResult.Restarted : BonjourRestartResult.Failed;
        }
        catch (Win32Exception e) when (e.NativeErrorCode == ErrorCancelled)
        {
            return BonjourRestartResult.Declined;
        }
        catch (Win32Exception)
        {
            return BonjourRestartResult.Failed;
        }
    }
}
