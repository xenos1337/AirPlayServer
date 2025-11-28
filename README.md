# AirPlay 2 for Windows

A high-performance AirPlay 2 receiver for Windows with real-time video streaming, audio playback, and an elegant semi-transparent ImGui overlay interface.

> **Note**: This is a fork of [fingergit/airplay2-win](https://github.com/fingergit/airplay2-win) updated with additional features and improvements.

## ✨ Features

- **Full AirPlay 2 Support**: Stream video, audio, and mirror your screen from iOS/macOS devices
- **Modern UI**: Beautiful semi-transparent ImGui overlay with real-time connection status
- **Smooth Playback**: Optimized frame pacing at 30 FPS for stutter-free video
- **Low Latency**: Hardware-accelerated YUV to RGB conversion with efficient rendering
- **Single-Threaded Rendering**: Eliminates flickering and artifacts through proper synchronization
- **Flexible Display**: Automatic letterbox/pillarbox support with aspect ratio preservation
- **Fullscreen Mode**: Double-click to toggle fullscreen, or press F key
- **Resizable Window**: Live window resizing with instant feedback
- **Device Discovery**: Automatic mDNS/Bonjour service advertisement

## 🎯 System Requirements

- **OS**: Windows 10/11 (64-bit)
- **Visual Studio**: 2022 or later
- **Network**: iOS/macOS device and Windows PC on the same Wi-Fi network
- **Hardware**: Any modern CPU with SSE2 support (for YUV conversion)

## 🚀 Quick Start

### Building from Source

1. **Clone the repository**
   ```bash
   git clone https://github.com/xenos1337/AirPlayServer.git
   cd AirPlayServer
   ```

2. **Open in Visual Studio**
   - Launch Visual Studio 2019 or later
   - Open `airplay2-win.sln`

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
                Main Thread → Blit to Screen → ImGui Overlay → Display (30 FPS)
```

### Key Technical Features

- **Off-Screen Buffering**: Video rendering isolated to callback thread, eliminating race conditions
- **Single-Threaded Display**: Only main thread touches the screen surface (no flickering)
- **Frame Synchronization**: Demand-driven rendering based on frame availability
- **Double Buffering**: Hardware double buffering for tear-free presentation
- **Mutex Protection**: Critical sections prevent data corruption during frame handoff

## 📁 Project Structure

```
airplay2-win/
├── AirPlayServer/          # Main application
│   ├── CSDLPlayer.cpp      # Video/audio player and rendering
│   ├── CImGuiManager.cpp   # UI overlay management
│   ├── CAirServer.cpp      # AirPlay server wrapper
│   └── CAirServerCallback.cpp  # AirPlay event handlers
├── airplay2/               # Core AirPlay 2 protocol implementation
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

The server automatically uses your computer's hostname as the AirPlay device name. To customize:

1. Edit the device name in the UI when the application starts
2. Or modify `hostName` in `AirPlayServer.cpp` before building

## 🐛 Troubleshooting

### "Can be discovered but cannot connect"

**Solution**: Ensure both devices are on the same Wi-Fi network and subnet.

- If Windows is in a VM, use **bridged networking** (not NAT/shared)
- Check Windows Firewall settings and allow the application
- Verify no VPN or proxy is interfering with the connection

## 🔬 Performance Characteristics

- **Frame Rate**: Locked at 30 FPS for video and UI
- **CPU Usage**: ~2-10% on modern CPUs (idle/streaming)
- **Memory**: ~50-200 MB (varies with video resolution)
- **Latency**: ~30-200ms (network dependent)
- **Supported Resolutions**: Up to 1920x1080 (1080p)

## 📚 Technical Credits

This project builds upon and integrates the following open-source projects:

### Core Libraries
- [shairplay](https://github.com/juhovh/shairplay) - AirPlay protocol implementation
- [AirplayServer](https://github.com/KqSMea8/AirplayServer) - Base server implementation
- [mDNSResponder](https://github.com/jevinskie/mDNSResponder) - Service discovery (Bonjour)

### Dependencies
- [SDL 1.2.15](https://www.libsdl.org/) - Cross-platform multimedia library
- [Dear ImGui](https://github.com/ocornut/imgui) - Immediate mode GUI framework
- [FFmpeg](https://ffmpeg.org/) - Video/audio decoding libraries
- [FDK-AAC](https://github.com/mstorsjo/fdk-aac) - AAC audio codec
- [libplist](https://github.com/libimobiledevice/libplist) - Apple property list parser

### Additional References
- [xindawn-windows-airplay-mirroring-sdk](https://github.com/xindawndev/xindawn-windows-airplay-mirroring-sdk) - Windows integration patterns

## 🤝 Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

### Development Guidelines

1. Follow the existing code style (C++ with Windows API conventions)
2. Test thoroughly on Windows 10/11
3. Update documentation for new features
4. Ensure backward compatibility where possible

## 📄 License

This project inherits licenses from its constituent libraries. Please refer to individual library licenses for specific terms.

## 🌟 Acknowledgments

Special thanks to all the open-source contributors whose work made this project possible. This Windows port would not exist without the foundation laid by the AirPlay reverse engineering community.

---

**Note**: This is an unofficial implementation of AirPlay 2. Apple, AirPlay, and related trademarks are property of Apple Inc.
