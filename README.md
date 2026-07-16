# AirPlayServer for Windows

AirPlayServer receives AirPlay video, audio, and screen mirroring on Windows.

This project is an updated fork of [fingergit/airplay2-win](https://github.com/fingergit/airplay2-win).

## Main Screen
<div align="center">
    <img style="height: 512px; width: auto;" src="https://github.com/user-attachments/assets/ffedab29-b483-467d-991a-4060503b0fc0" />
</div>

<div align="center">
    <table>
        <tr>
            <th>Settings</th>
            <th>Pin Approval</th>
        </tr>
        <tr>
            <td>
                <img width="420" src="https://github.com/user-attachments/assets/f6f46275-112e-4126-9baf-61666502d03f" />
            </td>
            <td>
                <img width="420" src="https://github.com/user-attachments/assets/c492d993-0cae-4abb-892c-65c601e13a68" />
            </td>
        </tr>
    </table>
</div>

## Viewer
<div align="center">
    <img style="height: 720px; width: auto;" src="https://github.com/user-attachments/assets/9b0c071e-070a-43f0-89b5-a936d4c11ea7" />
</div>

## 
<div align="center">
    <table>
        <tr>
            <th>Controls</th>
            <th>PIP (Picture in Picture) Mode</th>
        </tr>
        <tr>
            <td>
                <img src="https://github.com/user-attachments/assets/3997a422-650b-42ad-8f42-9eba0e71af10" width="420" />
            </td>
            <td>
                <img src="https://github.com/user-attachments/assets/b98681b5-f79a-493e-b9af-c1cd6a89c8c9" width="420" />
            </td>
        </tr>
    </table>
</div>

## Install

1. Download [AirPlay2-Win-x64.zip](https://github.com/xenos1337/AirPlayServer/releases/latest).
2. Extract the archive.
3. Install [Bonjour for Windows](https://support.apple.com/kb/DL999) if it is not already installed. iTunes also includes Bonjour.
4. Run `AirPlayServer.exe`.

The app warns you at startup if Bonjour is missing or its service is not running.

### Requirements

- Windows 10 or later, x64
- Apple Bonjour for Windows, used for device discovery over mDNS

## Features

- AirPlay video, audio, and screen mirroring from iOS and macOS
- 30 and 60 FPS quality presets
- GPU texture upload and YUV to RGB conversion
- Frame pacing for smoother playback
- Live window resizing
- Automatic Bonjour service advertisement

## Use AirPlayServer

1. Start AirPlayServer.
2. Open Control Center on an iPhone or iPad, or open the AirPlay menu on a Mac.
3. Select the Windows PC from the list.
4. Start mirroring or playback.

### Optional AirPlay PIN

Enable `Require PIN` from the home screen to approve new connections with a temporary four-digit code. The PIN exists only in memory for the current server session and is never written to disk.

`Hide PIN from screen capture` protects the code from supported Windows recording APIs. After you accept a connection, AirPlayServer enables capture exclusion and waits one second before displaying the PIN locally. The exclusion remains active until the device connects or you cancel. If Windows cannot enable capture exclusion, the app does not display the PIN.

### Controls

| Key | Action |
|-----|--------|
| `H` | Toggle session controls |
| `Ctrl+Shift+H` | Toggle capture privacy |
| `P` | Toggle picture-in-picture mode |
| `F` or double-click | Toggle fullscreen |
| `F1` | Toggle diagnostics while connected |
| `R` | Rotate video 90 degrees clockwise |
| Mouse wheel | Zoom from fit to 5x |
| Left-drag while zoomed | Pan the video |
| Mouse movement | Show the cursor; it hides after five seconds |

The session controls include `Hide from captures`. This keeps the receiver visible on the local monitor, excludes its main window from supported Windows capture APIs, and sends a black frame to the clean feed. Select `Show in captures` or press `Ctrl+Shift+H` to turn it off.

Select `Picture in picture` or press `P` to open a small, borderless window that stays above other apps. PiP uses the current device's aspect ratio when resized. Move the pointer over the window to reveal the close button. You can drag the invisible strip along the top to move it. Press `P` again to restore the previous window size, position, and maximized state. PiP also closes when the device disconnects.

### Screen Cast and the OBS clean feed

Enable `Screen Cast mode` from the session controls to create a separate video-only window named `AirPlay Receiver - Clean Feed`. The normal receiver window remains available on the local display.

To use it in OBS:

1. Add a Window Capture source and choose the Windows 10/11 capture method.
2. Select `AirPlay Receiver - Clean Feed`.
3. Turn off `Capture Cursor` in OBS.

In Discord, open `Share Your Screen`, choose `Applications`, and select `AirPlay Receiver - Clean Feed`. The window exists only while a device is connected and AirPlayServer is rendering video.

The clean feed follows rotation, zoom, pan, and receiver window resizing. `Crop clean feed to video` removes letterboxing and pillarboxing. AirPlayServer hides the clean feed between sessions and restores it after a device reconnects.

`Hide interface from captures` excludes the local receiver from supported Display Capture paths on Windows 10 version 2004 and later. It is meant to keep controls out of a presentation. It is not DRM. Use Window Capture in OBS for the clean feed.

### Quality presets

You can change the preset from the session controls while video is playing.

| Preset | FPS | Scaling | Intended use |
|--------|-----|---------|--------------|
| Best | 30 | Best available | Sharpest image |
| Balanced | 60 | Best available | Default setting |
| Low latency | 60 | Linear | Fastest response |

## Troubleshooting

### The device does not appear

- Check that Bonjour is installed and that `Bonjour Service` is running in `services.msc`.
- Put both devices on the same Wi-Fi network and subnet.
- Allow AirPlayServer through Windows Firewall on private networks.

### The device connects but video or audio does not play

- If Windows is running in a virtual machine, use bridged networking instead of NAT.
- Disconnect any VPN or proxy that may be intercepting the local connection.

## Build from source

You need Visual Studio 2022 with the v143 toolset and a Windows 10 SDK.

1. Clone the repository:

   ```bash
   git clone https://github.com/xenos1337/AirPlayServer.git
   ```

2. Open `AirPlay.sln` in Visual Studio.
3. In Solution Explorer, right-click `AirPlayServer` and select `Set as Startup Project`.
4. Build with `Ctrl+B` or run with `F5`.

The Debug executable is written to `x64\Debug\AirPlayServer.exe`.

## Project layout

```text
AirPlayServer/
|-- AirPlayServer/           # Windows GUI, SDL2, and ImGui
|   |-- CSDLPlayer.cpp       # Video and audio playback
|   |-- CImGuiManager.cpp    # Home screen and session controls
|   |-- CAirServer.cpp       # AirPlay server wrapper
|   `-- CAirServerCallback.cpp
|-- AirPlayServerLib/        # AirPlay 2 protocol library
|   `-- lib/                 # RAOP, pairing, crypto, and codecs
|-- airplay2dll/             # DLL wrapper and FFmpeg H.264 decoder
|-- dnssd/                   # Bonjour discovery DLL
|-- external/                # SDL2, FFmpeg, ImGui, and other dependencies
`-- AirPlay.sln
```

## Contributing

Bug reports, feature requests, and pull requests are welcome. Follow the existing C++ and Windows API style, test on Windows 10 or 11, and update the documentation when behavior changes.

## License

The repository contains code from several libraries. Check each library's license for its terms.

## Credits

Thanks to [fingergit](https://github.com/fingergit/airplay2-win) and the AirPlay reverse engineering community.

This is an unofficial implementation. Apple, AirPlay, and related trademarks belong to Apple Inc.
