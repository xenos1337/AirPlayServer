# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

This is a Visual Studio 2019+ C++ project. Open `airplay2-win.sln` and build:

```
# From Visual Studio
Ctrl+B (Build Solution)
F5 (Run with debugging)
Ctrl+F5 (Run without debugging)

# From Developer Command Prompt (x64)
msbuild airplay2-win.sln /p:Configuration=Debug /p:Platform=x64
```

Output location: `x64\Debug\AirPlayServer.exe`

**Startup Project**: AirPlayServer (set via right-click → "Set as Startup Project")

## Architecture Overview

### Solution Structure (Build Order)

1. **airplay2** (static lib) - Core AirPlay 2 protocol implementation in C
2. **airplay2dll** (DLL) - Wraps airplay2 with FFmpeg decoding, exposes C++ API
3. **dnssd** (static lib) - mDNS/Bonjour service discovery
4. **AirPlayServer** (exe) - Windows GUI application using SDL + ImGui

### Threading Model

- **Main Thread**: SDL event loop, ImGui rendering, display presentation (`CSDLPlayer::loopEvents`)
- **Callback Thread**: Receives AirPlay frames, writes to off-screen buffer (`outputVideo`)
- **Audio Thread**: SDL audio callback pulls from queue (`sdlAudioCallback`)

### Key Data Flow

```
Network → raop.c/airplay.c → FgAirplayChannel (FFmpeg H.264→YUV)
       → CAirServerCallback → CSDLPlayer::outputVideo (YUV→RGB to m_videoBuffer)
       → Main thread blits m_videoBuffer to m_surface → ImGui overlay → SDL_Flip
```

### Core Classes (AirPlayServer/)

| File | Purpose |
|------|---------|
| `CSDLPlayer.cpp` | Video/audio rendering, window management, fullscreen toggle |
| `CImGuiManager.cpp` | ImGui overlay (home screen, connection status) |
| `CAirServer.cpp` | Wraps airplay2dll, starts server with hostname |
| `CAirServerCallback.cpp` | Routes AirPlay events to player |

### Protocol Layer (airplay2/lib/)

| File | Purpose |
|------|---------|
| `airplay.c` | HTTP request handlers for /pair-setup, /play, etc. |
| `raop.c` | RTSP-based audio streaming (RAOP) |
| `raop_rtp.c` | Audio RTP packet handling |
| `raop_rtp_mirror.c` | Video mirroring RTP handling |
| `pairing.c` | Device pairing and encryption |
| `fairplay_playfair.c` | FairPlay DRM decryption |

### DLL Interface (airplay2dll/)

`FgAirplayChannel.cpp` handles:
- FFmpeg H.264 decoding with low-latency flags
- YUV frame output via `IAirServerCallback` interface

## External Dependencies

Pre-built libraries in `external/`:
- **FFmpeg**: Video decoding (avcodec, avutil, swscale)
- **SDL 1.2.15**: Window management, audio output
- **Dear ImGui**: UI overlay
- **FDK-AAC**: AAC audio decoding

## Key Constants

- Frame rate: 30 FPS (locked in main loop)
- AirPlay port: 5001
- Mirror port: 7001
- Max resolution: 1920x1080
- Double-click threshold: 400ms (fullscreen toggle)

## Synchronization

Critical mutex usage:
- `m_mutexVideo`: Protects `m_videoBuffer` during frame writes and blits
- `m_mutexAudio`: Protects audio queue access
- Frame handoff uses `m_hasNewFrame` flag with mutex protection
