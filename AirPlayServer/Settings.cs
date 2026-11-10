using System;
using System.IO;
using System.Security;
using System.Text;
using Microsoft.Win32;

namespace AirPlayServer;

// The settings file lives under %APPDATA% (see FilePath). Earlier versions kept it
// next to the exe, which is read-only when installed under Program Files; it is
// copied over once.
internal sealed class Settings
{
    public const int MinReceiverWidth = 640;
    public const int MinReceiverHeight = 360;
    public const int MaxReceiverWidth = 7680;
    public const int MaxReceiverHeight = 4320;
    public const int MaxNameBytes = 255;

    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string RunValue = "AirPlayServer";

    // OverlayState values written by earlier versions.
    private const int OverlayExpanded = 0;
    private const int OverlayLauncher = 1;
    private const int OverlayHidden = 2;

    private const string FileName = "airplay_settings.ini";

    public static readonly string FilePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "AirPlayServer", FileName);

    private IniFile ini = null!;

    public string DeviceName = "";
    public int QualityPreset = 1;
    public bool AutoFullscreenOnConnect;
    public bool MinimizeOnClose;
    public bool HideToTrayOnDisconnect;
    public bool ShowControls = true;
    public bool MatchReceiverMonitor = true;
    public int CustomWidth = 1920;
    public int CustomHeight = 1080;
    public bool ScreenCastEnabled;
    public bool ScreenCastHideInterface = true;
    public bool ScreenCastCropToVideo = true;
    public bool RequirePin;
    public bool HidePin = true;
    public int LocalVolume = 100;

    public static int ClampReceiverDimension(int value, int minimum, int maximum)
    {
        return Math.Clamp(value, minimum, maximum) & ~1;
    }

    public static Settings Load()
    {
        string legacyPath = Path.Combine(AppContext.BaseDirectory, FileName);
        if (!File.Exists(FilePath) && File.Exists(legacyPath))
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
                File.Copy(legacyPath, FilePath);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
            }
        }

        Settings settings = new() { ini = IniFile.Load(FilePath) };

        string name = settings.ReadString("General", "DeviceName", "");
        if (name.Length > 0 && Native.IsValidName(name))
        {
            settings.DeviceName = name;
        }

        int preset = settings.ReadInt("General", "QualityPreset", 1);
        if (preset >= 0 && preset <= 2)
        {
            settings.QualityPreset = preset;
        }
        settings.AutoFullscreenOnConnect = settings.ReadInt("General", "AutoFullscreenOnConnect", 0) != 0;
        settings.MinimizeOnClose = settings.ReadInt("General", "MinimizeOnClose", 0) != 0;
        settings.HideToTrayOnDisconnect = settings.ReadInt("General", "HideToTrayOnDisconnect", 0) != 0;
        settings.MatchReceiverMonitor = settings.ReadInt("Display", "MatchReceiverMonitor", 1) != 0;
        settings.CustomWidth = ClampReceiverDimension(settings.ReadInt("Display", "CustomWidth", 1920),
            MinReceiverWidth, MaxReceiverWidth);
        settings.CustomHeight = ClampReceiverDimension(settings.ReadInt("Display", "CustomHeight", 1080),
            MinReceiverHeight, MaxReceiverHeight);
        settings.ScreenCastEnabled = settings.ReadInt("ScreenCast", "Enabled", 0) != 0;
        settings.ScreenCastHideInterface = settings.ReadInt("ScreenCast", "HideInterface", 1) != 0;
        settings.ScreenCastCropToVideo = settings.ReadInt("ScreenCast", "CropToVideo", 1) != 0;
        settings.RequirePin = settings.ReadInt("Security", "RequirePin", 0) != 0;
        settings.HidePin = settings.ReadInt("Security", "HidePin", 1) != 0;
        settings.LocalVolume = Math.Clamp(settings.ReadInt("Audio", "LocalVolume", 100), 0, 100);

        // Earlier versions had a third, collapsed "launcher" state. The controls
        // panel is either shown or hidden; only an explicit hide keeps it hidden.
        // A ShowOverlay value that disagrees with OverlayState was written by an
        // older build after the newer one and wins.
        string legacyOverlay = settings.ReadString("General", "ShowOverlay", "");
        int overlayState = settings.ReadInt("General", "OverlayState", -1);
        if (overlayState < OverlayExpanded || overlayState > OverlayHidden)
        {
            overlayState = legacyOverlay.Length == 0 ? OverlayLauncher
                : ParseLegacyFlag(legacyOverlay) ? OverlayExpanded : OverlayHidden;
        }
        else if (legacyOverlay.Length > 0 &&
            ParseLegacyFlag(legacyOverlay) != (overlayState == OverlayExpanded))
        {
            overlayState = ParseLegacyFlag(legacyOverlay) ? OverlayExpanded : OverlayHidden;
        }
        settings.ShowControls = overlayState != OverlayHidden;

        return settings;
    }

    public void Save()
    {
        if (Native.IsValidName(DeviceName))
        {
            Write("General", "DeviceName", DeviceName);
        }
        Write("Security", "RequirePin", Flag(RequirePin));
        Write("Security", "HidePin", Flag(HidePin));
        Write("Security", "PinProtected", null);
        Write("General", "QualityPreset", QualityPreset.ToString());
        Write("General", "AutoFullscreenOnConnect", Flag(AutoFullscreenOnConnect));
        Write("General", "MinimizeOnClose", Flag(MinimizeOnClose));
        Write("General", "HideToTrayOnDisconnect", Flag(HideToTrayOnDisconnect));
        Write("Display", "MatchReceiverMonitor", Flag(MatchReceiverMonitor));
        Write("Display", "CustomWidth", CustomWidth.ToString());
        Write("Display", "CustomHeight", CustomHeight.ToString());
        Write("ScreenCast", "Enabled", Flag(ScreenCastEnabled));
        Write("ScreenCast", "HideInterface", Flag(ScreenCastHideInterface));
        Write("ScreenCast", "CropToVideo", Flag(ScreenCastCropToVideo));
        Write("General", "OverlayState", (ShowControls ? OverlayExpanded : OverlayHidden).ToString());
        Write("General", "ShowOverlay", Flag(ShowControls));
        Write("Audio", "LocalVolume", LocalVolume.ToString());
        try
        {
            ini.Save();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }

    // The receiver name in effect: the saved one, or the host name.
    public string ReceiverName(string hostName)
    {
        return DeviceName.Length > 0 && Native.IsValidName(DeviceName) ? DeviceName : hostName;
    }

    public unsafe ApConfig ToConfig(string hostName)
    {
        ApConfig config = new()
        {
            PinEnabled = RequirePin ? 1 : 0,
            MatchMonitor = MatchReceiverMonitor ? 1 : 0,
            CustomWidth = CustomWidth,
            CustomHeight = CustomHeight,
            Quality = QualityPreset,
            ScreenCast = ScreenCastEnabled ? 1 : 0,
            ScreenCastCrop = ScreenCastCropToVideo ? 1 : 0,
            LocalVolume = LocalVolume,
        };
        byte[] name = Encoding.UTF8.GetBytes(ReceiverName(hostName));
        int length = Math.Min(name.Length, MaxNameBytes);
        for (int i = 0; i < length; i++)
        {
            config.DeviceName[i] = name[i];
        }
        config.DeviceName[length] = 0;
        return config;
    }

    public static bool QueryAutoStart()
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(RunValue) is string command && command.Length > 0;
        }
        catch (Exception e) when (e is SecurityException or UnauthorizedAccessException or IOException)
        {
            return false;
        }
    }

    public static void ApplyAutoStart(bool enabled)
    {
        try
        {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKey);
            if (enabled)
            {
                key.SetValue(RunValue, $"\"{Environment.ProcessPath}\" --tray", RegistryValueKind.String);
            }
            else
            {
                key.DeleteValue(RunValue, false);
            }
        }
        catch (Exception e) when (e is SecurityException or UnauthorizedAccessException or IOException)
        {
        }
    }

    private static bool ParseLegacyFlag(string value)
    {
        return int.TryParse(value, out int number) && number != 0;
    }

    private static string Flag(bool value) => value ? "1" : "0";

    private string ReadString(string section, string key, string fallback)
    {
        return ini.Get(section, key) ?? fallback;
    }

    private int ReadInt(string section, string key, int fallback)
    {
        return int.TryParse(ini.Get(section, key), out int value) ? value : fallback;
    }

    private void Write(string section, string key, string? value)
    {
        ini.Set(section, key, value);
    }
}
