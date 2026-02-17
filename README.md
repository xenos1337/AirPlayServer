# AirPlayServer - AirPlay Receiver for Windows

A high-performance AirPlay receiver for Windows with real-time video streaming and audio playback.

> **Note**: This is an updated version of [fingergit/airplay2-win](https://github.com/fingergit/airplay2-win)

## Installation

### Quick Install (Recommended)

1. **Download the installer**: [AirPlayInstaller.exe](https://github.com/xenos1337/AirPlayServer/raw/master/AirPlayInstaller/AirPlayInstaller.exe)
2. **Run as Administrator** — right-click the installer and select "Run as administrator"
3. Click **Install** — the installer will download the latest release, extract it to `C:\Program Files\AirPlay`, and create Desktop and Start Menu shortcuts
4. **Install [Bonjour for Windows](https://support.apple.com/kb/DL999)** if you don't have it already (required for device discovery). Installing iTunes also installs Bonjour.
5. Launch **AirPlay** from the Desktop shortcut or Start Menu

### Uninstalling

Run the installer again and click **Uninstall**. This removes the program files and shortcuts.

### Prerequisites

- Windows 10 or later (x64)
- **Apple Bonjour for Windows** — required for mDNS device discovery. Install from [Apple's download page](https://support.apple.com/kb/DL999) or install iTunes which bundles it. The server will warn you at startup if Bonjour is missing.

## Features

- **Full AirPlay Support**: Stream video, audio, and mirror your screen from iOS/macOS devices
- **Quality Presets**: Good Quality (30 FPS, Lanczos), Balanced (60 FPS, bilinear), Fast Speed (60 FPS, nearest-neighbor)
- **Smooth Playback**: Frame pacing with optimized rendering for stutter-free video
- **Low Latency**: Efficient YUV to RGB conversion with GPU texture upload
- **Resizable Window**: Live window resizing with instant feedback
- **Device Discovery**: Automatic mDNS/Bonjour service advertisement

## Usage

1. Launch AirPlayServer
2. The server automatically advertises itself on the network
3. Open Control Center on your iOS device (or AirPlay menu on macOS)
4. Select your Windows PC from the list of available AirPlay devices
5. Start streaming!

### Controls

| Key | Action |
|-----|--------|
| **H** | Toggle overlay UI |
| **F** / **Double-click** | Toggle fullscreen |
| **F1** | Toggle performance graphs |
| Mouse movement | Shows cursor (auto-hides after 5s) |

### Quality Presets

Changeable in real-time from the home screen or overlay:

| Preset | FPS | Scaling | Use Case |
|--------|-----|---------|----------|
| Good Quality | 30 | Lanczos | Best image quality |
| Balanced | 60 | Bilinear | Default, smooth + sharp |
| Fast Speed | 60 | Nearest-neighbor | Lowest latency |

## Troubleshooting

### Device not appearing on iPhone/iPad/Mac

- Ensure **Bonjour for Windows** is installed and the "Bonjour Service" is running (`services.msc`)
- Both devices must be on the **same Wi-Fi network and subnet**
- Check **Windows Firewall** — allow AirPlayServer through for Private networks

### Connects but no video/audio

- If Windows is in a VM, use **bridged networking** (not NAT)
- Verify no VPN or proxy is interfering with the connection

## Building from Source

Requires Visual Studio 2022 (v143 toolset) and Windows 10 SDK.

1. Clone the repository
   ```bash
   git clone https://github.com/xenos1337/AirPlayServer.git
   ```
2. Open `AirPlay.sln` in Visual Studio
3. Right-click **AirPlayServer** in Solution Explorer → "Set as Startup Project"
4. Build with `Ctrl+B`, run with `F5`

Output: `x64\Debug\AirPlayServer.exe`

## Project Structure

```
AirPlayServer/
├── AirPlayServer/           # Main GUI application (C++, SDL2, ImGui)
│   ├── CSDLPlayer.cpp       # Video/audio player and rendering
│   ├── CImGuiManager.cpp    # UI overlay management
│   ├── CAirServer.cpp       # AirPlay server wrapper
│   └── CAirServerCallback.cpp
├── AirPlayServerLib/        # Core AirPlay 2 protocol (C static lib)
│   └── lib/                 # RAOP, pairing, crypto, codecs
├── airplay2dll/             # AirPlay DLL wrapper + FFmpeg H.264 decode
├── dnssd/                   # mDNS/Bonjour service discovery DLL
├── AirPlayInstaller/        # Installer application (C# WinForms)
│   └── AirPlayInstaller.exe # Pre-built installer
├── external/                # Third-party libraries (SDL2, FFmpeg, ImGui)
└── AirPlay.sln              # Visual Studio solution
```

## Contributing

Issues, feature requests, and pull requests are welcome.

1. Follow the existing code style (C++ with Windows API conventions)
2. Test on Windows 10/11
3. Update documentation for new features

## License

This project inherits licenses from its constituent libraries. Refer to individual library licenses for terms.

## Acknowledgments

Thanks to [fingergit](https://github.com/fingergit/airplay2-win) and the AirPlay reverse engineering community.

---

**Note**: This is an unofficial implementation. Apple, AirPlay, and related trademarks are property of Apple Inc.
