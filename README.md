# AirPlayServer - AirPlay Receiver for Windows

A high-performance AirPlay receiver for Windows with real-time video streaming and audio playback

> **Note**: This is a updated version of [fingergit/airplay2-win](https://github.com/fingergit/airplay2-win)

## ✨ Features

- **Full AirPlay Support**: Stream video, audio, and mirror your screen from iOS/macOS devices
- **Quality Presets**: Choose between Good Quality (30 FPS, high-quality scaling), Balanced (60 FPS, normal quality), or Fast Speed (60 FPS, low latency)
- **Smooth Playback**: Configurable frame pacing with optimized rendering for stutter-free video
- **Low Latency**: Hardware-accelerated YUV to RGB conversion with efficient rendering
- **Resizable Window**: Live window resizing with instant feedback
- **Device Discovery**: Automatic mDNS/Bonjour service advertisement

## 🚀 Quick Start

### Building from Source

1. **Clone the repository**
   ```bash
   git clone https://github.com/xenos1337/AirPlayServer.git
   cd AirPlayServer
   ```

2. **Open in Visual Studio**
   - Launch Visual Studio 2019 or later
   - Open `AirPlayServer.sln`

3. **Set Startup Project**
   - Right-click on `AirPlayServer` in Solution Explorer
   - Select "Set as Startup Project"

4. **Build**
   - Press `Ctrl + B` to build the solution
   - Or select `Build > Build Solution` from the menu

5. **Run**
   - Press `F5` to run with debugging
   - Or press `Ctrl + F5` to run without debugging
   - The executable will be located in `x64\Debug\AirPlayServer.exe`

### Running the Application

1. Launch `AirPlayServer.exe`
2. The server will automatically start and advertise itself on the network
3. Open Control Center on your iOS device (or AirPlay menu on macOS)
4. Select your Windows PC from the list of available AirPlay devices
5. Start streaming!

## 🏗️ Architecture

### Threading Model

- **Main Thread**: Handles SDL events, UI rendering (ImGui), and display presentation
- **Callback Thread**: Receives AirPlay video frames and writes to off-screen buffer
- **Audio Thread**: SDL audio callback for real-time audio playback

### Rendering Pipeline

```
AirPlay Stream → Callback Thread → Video Buffer (Off-screen)
                                         ↓
                Main Thread → Blit to Screen → ImGui Overlay → Display (30/60 FPS)
```

### Quality Presets

The application offers three quality presets that balance performance and visual quality:

- **Good Quality**: 30 FPS with high-quality Lanczos scaling - Best for visual quality
- **Balanced**: 60 FPS with fast bilinear scaling - Best balance of quality and performance (default)
- **Fast Speed**: 60 FPS with nearest-neighbor scaling - Lowest latency, best for responsiveness

Quality presets can be changed in real-time from both the home screen and the overlay UI. The selection is synchronized between views for a consistent experience.

### Key Technical Features

- **Off-Screen Buffering**: Video rendering isolated to callback thread, eliminating race conditions
- **Single-Threaded Display**: Only main thread touches the screen surface (no flickering)
- **Frame Synchronization**: Demand-driven rendering based on frame availability
- **Double Buffering**: Hardware double buffering for tear-free presentation
- **Mutex Protection**: Critical sections prevent data corruption during frame handoff

## 📁 Project Structure

```
AirPlayServer/
├── AirPlayServer/          # Main application
│   ├── CSDLPlayer.cpp      # Video/audio player and rendering
│   ├── CImGuiManager.cpp   # UI overlay management
│   ├── CAirServer.cpp      # AirPlay server wrapper
│   └── CAirServerCallback.cpp  # AirPlay event handlers
├── AirPlayServerLib/       # Core AirPlay 2 protocol implementation
│   └── lib/                # Protocol handlers, crypto, codecs
├── airplay2dll/            # AirPlay 2 DLL wrapper
├── dnssd/                  # mDNS/Bonjour service discovery
├── external/               # Third-party libraries
│   ├── ffmpeg/             # Video decoding
│   ├── imgui/              # UI framework
│   ├── SDL-1.2.15/         # Multimedia library
│   └── plist/              # Property list parsing
└── x64/Debug/              # Build output
```

## 🔧 Configuration

### Device Name

The server automatically uses your computer's hostname as the AirPlay device name. To customize:

1. Edit the device name in the UI when the application starts
2. Or modify `hostName` in `AirPlayServer.cpp` before building

### Quality Presets

You can change the quality preset at any time:

- **Home Screen**: Select from the quality preset tabs before or during streaming
- **Overlay UI**: Press `H` to show/hide the overlay, then select your preferred quality preset
- The preset affects both frame rate (30 FPS vs 60 FPS) and scaling algorithm quality

### UI Controls

- **H Key**: Toggle overlay UI visibility
- **Double-Click**: Toggle fullscreen mode
- **F Key**: Toggle fullscreen mode
- **Mouse Movement**: Shows cursor (auto-hides after 5 seconds of inactivity)

## 🐛 Troubleshooting

### "Can be discovered but cannot connect"

**Solution**: Ensure both devices are on the same Wi-Fi network and subnet.

- If Windows is in a VM, use **bridged networking** (not NAT/shared)
- Check Windows Firewall settings and allow the application
- Verify no VPN or proxy is interfering with the connection

## 🔬 Performance Characteristics

- **Frame Rate**: Configurable 30 FPS (Good Quality) or 60 FPS (Balanced/Fast Speed)
- **CPU Usage**: ~2-10% on modern CPUs (idle/streaming, varies by quality preset)
- **Memory**: ~50-200 MB (varies with video resolution)
- **Latency**: ~30-200ms (network dependent, lower with Fast Speed preset)
- **Supported Resolutions**: Up to 1920x1080 (1080p)
- **Scaling Quality**: Lanczos (Good), Bilinear (Balanced), or Nearest-Neighbor (Fast)

## 🤝 Contributing

We're open to issues, feature requests, and pull requests! We want to make this project as good as possible, and your feedback and contributions are invaluable.

**How you can help:**
- 🐛 **Report bugs**: Found an issue? Please open an issue with details about the problem
- 💡 **Suggest features**: Have an idea for improvement? We'd love to hear it!
- 🔧 **Submit PRs**: Code contributions are always welcome - see guidelines below
- 📝 **Improve documentation**: Help make the project more accessible to others

### Development Guidelines

1. Follow the existing code style (C++ with Windows API conventions)
2. Test thoroughly on Windows 10/11
3. Update documentation for new features
4. Ensure backward compatibility where possible

## 📄 License

This project inherits licenses from its constituent libraries. Please refer to individual library licenses for specific terms.

## 🌟 Acknowledgments

Special thanks to [fingergit](https://github.com/fingergit/airplay2-win) and all the open-source contributors whose work made this project possible. This Windows port would not exist without the foundation laid by the AirPlay reverse engineering community.

---

**Note**: This is an unofficial implementation of AirPlay. Apple, AirPlay, and related trademarks are property of Apple Inc.
