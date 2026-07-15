# AirPlayServer - AirPlay Receiver for Windows

A high-performance AirPlay receiver for Windows with real-time video streaming and audio playback.

> **Note**: This is an updated version of [fingergit/airplay2-win](https://github.com/fingergit/airplay2-win)

## Installation

### Quick Install (Recommended)

1. **Download the latest release**: [AirPlay2-Win-x64.zip](https://github.com/xenos1337/AirPlayServer/releases/latest)
2. Extract the zip to a folder of your choice
3. **Install [Bonjour for Windows](https://support.apple.com/kb/DL999)** if you don't have it already (required for device discovery). Installing iTunes also installs Bonjour.
4. Run **AirPlayServer.exe** — the server will warn you at startup if Bonjour is missing or not running

### Prerequisites

- Windows 10 or later (x64)
- **Apple Bonjour for Windows** — required for mDNS device discovery. Install from [Apple's download page](https://support.apple.com/kb/DL999) or install iTunes which bundles it. The server will warn you at startup if Bonjour is missing.

## Features

- **Full AirPlay Support**: Stream video, audio, and mirror your screen from iOS/macOS devices
- **Quality Presets**: Best (30 FPS), Balanced (60 FPS), and Low latency (60 FPS)
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

### Optional AirPlay PIN

On the disconnected home screen, enable **Require PIN**. The receiver generates a temporary four-digit PIN in memory for the current server session; it is never saved to disk. With **Hide PIN from screen capture** enabled, accepting a connection first excludes the receiver window from supported Windows capture and recording APIs. After a one-second safety delay, the real PIN appears locally and remains visible until the device connects or you cancel. Capture exclusion remains active for that entire interval. If Windows cannot enable exclusion, the PIN is not shown.

### Controls

| Key | Action |
|-----|--------|
| **H** | Toggle overlay UI |
| **Ctrl+Shift+H** | Toggle capture-only privacy |
| **P** | Toggle picture-in-picture mode |
| **F** / **Double-click** | Toggle fullscreen |
| **F1** | Toggle diagnostics while connected (temporarily replaces controls) |
| **R** | Rotate video 90° clockwise |
| **Mouse wheel** | Zoom video from fit to 5× |
| **Left-drag** (while zoomed) | Pan around the zoomed video |
| Mouse movement | Shows cursor (auto-hides after 5s) |

The connected-session controls also include **Hide from captures**. Capture privacy keeps the receiver fully visible and usable on your own monitor, excludes its main window from supported Windows recording and screen-sharing APIs, and makes the clean-feed output solid black. Select **Show in captures** or press **Ctrl+Shift+H** to disable it.

Select **Picture in picture** or press **P** while connected to move the receiver into a compact, borderless, always-on-top window at the lower-right of the current monitor. PiP locks resizing to the current device orientation and preserves your normal window position, size, and maximized state. Move the pointer over PiP to reveal its close **X**, or press **P** again to restore the normal receiver. Drag the invisible top strip to reposition it; disconnecting also exits PiP automatically.

### Screen Cast / OBS clean feed

While a device is connected, open the session controls and enable **Screen Cast mode**. The normal receiver window stays fully usable on your display, including its controls, while capture and sharing apps can use a clean video-only target named **AirPlay Receiver - Clean Feed**.

1. In OBS, add **Window Capture** and use the Windows 10/11 capture method.
2. Select `AirPlay Receiver - Clean Feed`.
3. Disable OBS's **Capture Cursor** option for a completely clean feed.

In Discord, choose **Share Your Screen** → **Applications** → `AirPlay Receiver - Clean Feed`. The target is available only while a device is connected and video is being rendered.

The clean-feed target follows the visible video area as you resize the receiver window, rotate, zoom, or pan. With **Crop clean feed to video** enabled, it excludes letterboxing and pillarboxing. The target is hidden between AirPlay sessions and reappears on reconnect.

**Hide interface from captures** also protects the local receiver window from supported Display Capture paths on Windows 10 version 2004 or newer. This is a presentation feature, not DRM; Window Capture is the supported OBS workflow.

### Quality Presets

Changeable in real-time from the connected-session overlay:

| Preset | FPS | Scaling | Use Case |
|--------|-----|---------|----------|
| Best | 30 | Best available | Sharpest image quality |
| Balanced | 60 | Best available | Default, smooth + sharp |
| Low latency | 60 | Linear | Fastest response |


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
