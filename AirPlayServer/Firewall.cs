using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.CSharp.RuntimeBinder;

namespace AirPlayServer;

internal enum FirewallStatus
{
    Allowed,
    Blocked,
    PublicNetwork,
}

// Windows Firewall blocks the receiver's ports unless an inbound rule allows
// this exe. Dismissing Windows' first-run prompt leaves a block rule behind and
// the prompt never returns, so devices see the receiver but cannot connect.
internal static class Firewall
{
    private const int ProfileDomain = 1;
    private const int ProfilePrivate = 2;
    private const int ProfilePublic = 4;
    private const int DirectionInbound = 1;
    private const int ActionBlock = 0;
    private const string RuleName = "AirPlay Server";

    // Reading the rules does not need administrator rights.
    public static FirewallStatus Check(string program)
    {
        try
        {
            Type? policyType = Type.GetTypeFromProgID("HNetCfg.FwPolicy2");
            if (policyType == null)
            {
                return FirewallStatus.Allowed;
            }
            dynamic policy = Activator.CreateInstance(policyType)!;
            int activeProfiles = policy.CurrentProfileTypes;

            bool firewallOn = false;
            foreach (int profile in new[] { ProfileDomain, ProfilePrivate, ProfilePublic })
            {
                if ((activeProfiles & profile) != 0 && policy.FirewallEnabled[profile])
                {
                    firewallOn = true;
                }
            }
            if (!firewallOn)
            {
                return FirewallStatus.Allowed;
            }

            bool allowed = false;
            bool blocked = false;
            foreach (dynamic rule in policy.Rules)
            {
                if (rule.Direction != DirectionInbound || !rule.Enabled ||
                    (rule.Profiles & activeProfiles) == 0 ||
                    !string.Equals((string?)rule.ApplicationName, program, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                if (rule.Action == ActionBlock)
                {
                    blocked = true;
                }
                else
                {
                    allowed = true;
                }
            }

            if (blocked)
            {
                return FirewallStatus.Blocked;
            }
            if (allowed)
            {
                return FirewallStatus.Allowed;
            }
            // The rule added below only covers private and domain networks.
            return (activeProfiles & (ProfileDomain | ProfilePrivate)) == 0
                ? FirewallStatus.PublicNetwork : FirewallStatus.Blocked;
        }
        catch (Exception e) when (e is COMException or RuntimeBinderException or InvalidCastException)
        {
            return FirewallStatus.Allowed;
        }
    }

    // Replaces this exe's inbound rules with one allowing private and domain
    // networks. Needs administrator rights, so Windows shows a UAC prompt.
    // Returns false if the prompt was declined or the change failed.
    public static bool AllowProgram(string program)
    {
        string commands =
            $"netsh advfirewall firewall delete rule name=all dir=in program=\"{program}\" & " +
            $"netsh advfirewall firewall add rule name=\"{RuleName}\" dir=in action=allow " +
            $"program=\"{program}\" enable=yes profile=private,domain";
        ProcessStartInfo info = new("cmd.exe", $"/c \"{commands}\"")
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
                return false;
            }
            process.WaitForExit();
            return process.ExitCode == 0;
        }
        catch (Win32Exception)
        {
            return false;
        }
    }
}
