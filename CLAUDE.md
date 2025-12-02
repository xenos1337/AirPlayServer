# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

This is a Visual Studio 2019+ C++ project. Open `AirPlayServer.sln` and build:

```
# From Visual Studio
Ctrl+B (Build Solution)
F5 (Run with debugging)
Ctrl+F5 (Run without debugging)

# From Developer Command Prompt (x64)
msbuild AirPlayServer.sln /p:Configuration=Debug /p:Platform=x64
```

Output location: `x64\Debug\AirPlayServer.exe`

**Startup Project**: AirPlayServer (set via right-click → "Set as Startup Project")

## Architecture Overview

### Solution Structure (Build Order)

1. **AirPlayServerLib** (static lib) - Core AirPlay 2 protocol implementation in C
2. **airplay2dll** (DLL) - Wraps AirPlayServerLib with FFmpeg decoding, exposes C++ API
3. **dnssd** (static lib) - mDNS/Bonjour service discovery
4. **AirPlayServer** (exe) - Windows GUI application using SDL + ImGui

### Threading Model

- **Main Thread**: SDL event loop, ImGui rendering, display presentation (`CSDLPlayer::loopEvents`)
- **Callback Thread**: Receives AirPlay frames, copies to source buffer (`outputVideo`)
- **Scaling Thread**: Background YUV scaling with double-buffered output (`ScalingThreadProc`)
- **Audio Thread**: SDL audio callback pulls from queue (`sdlAudioCallback`)

### Key Data Flow

```
Network → raop.c/airplay.c → FgAirplayChannel (FFmpeg H.264→YUV)
       → CAirServerCallback → CSDLPlayer::outputVideo (copy to m_srcYUV)
       → Scaling thread (m_srcYUV → m_scaledYUV double-buffer)
       → Main thread blits scaled buffer → ImGui overlay → SDL_Flip
```

### Core Classes (AirPlayServer/)

| File | Purpose |
|------|---------|
| `CSDLPlayer.cpp` | Video/audio rendering, window management, fullscreen toggle |
| `CImGuiManager.cpp` | ImGui overlay (home screen, connection status, quality presets) |
| `CAirServer.cpp` | Wraps airplay2dll, starts server with hostname |
| `CAirServerCallback.cpp` | Routes AirPlay events to player |

### Protocol Layer (AirPlayServerLib/lib/)

| File | Purpose |
|------|---------|
| `airplay.c` | HTTP request handlers for /pair-setup, /play, etc. |
| `raop.c` | RTSP-based audio streaming (RAOP) |
| `raop_rtp.c` | Audio RTP packet handling |
| `raop_rtp_mirror.c` | Video mirroring RTP handling |
| `pairing.c` | Device pairing and encryption |
| `fairplay_playfair.c` | FairPlay DRM decryption |

### DLL Interface (airplay2dll/)

`FgAirplayChannel.cpp` handles FFmpeg H.264 decoding with low-latency flags.

**Exported API** (Airplay2Head.h):
```cpp
void* fgServerStart(const char serverName[128], unsigned int raopPort,
                    unsigned int airplayPort, IAirServerCallback* callback);
void fgServerStop(void* handle);
float fgServerScale(void* handle, float fRatio);
```

**Callback Interface** (`IAirServerCallback`):
- `connected()` / `disconnected()` - Connection lifecycle
- `outputVideo(SFgVideoFrame*)` - Decoded YUV420 frame delivery
- `outputAudio(SFgAudioFrame*)` - Decoded audio samples
- `videoPlay()` / `videoGetPlayInfo()` - URL-based video playback

## External Dependencies

Pre-built libraries in `external/`:
- **FFmpeg**: Video decoding (avcodec, avutil, swscale)
- **SDL 1.2.15**: Window management, audio output
- **Dear ImGui**: UI overlay
- **FDK-AAC**: AAC audio decoding

## Quality Presets

Defined in `CImGuiManager.h` (`EQualityPreset`):

| Preset | FPS | Scaling Algorithm | Use Case |
|--------|-----|-------------------|----------|
| `QUALITY_GOOD` | 30 | SWS_LANCZOS | Best visual quality |
| `QUALITY_BALANCED` | 60 | SWS_FAST_BILINEAR | Default, balanced |
| `QUALITY_FAST` | 60 | SWS_POINT (nearest) | Lowest latency |

## Key Constants

- AirPlay port: 5001 (RAOP), 7001 (mirroring)
- Max resolution: 1920x1080
- Double-click threshold: 400ms (fullscreen toggle)
- Cursor auto-hide: 5000ms

## UI Controls

- **H**: Toggle overlay visibility
- **F** / **Double-click**: Toggle fullscreen
- Mouse movement shows cursor (auto-hides after 5s)

## Synchronization

Critical mutex usage:
- `m_mutexVideo`: Protects source buffer copy in `outputVideo`
- `m_mutexAudio`: Protects audio queue access

Double-buffered scaling (lockless producer-consumer):
- `m_writeBuffer` / `m_readBuffer`: Indices for double-buffered `m_scaledYUV[2][3]`
- `m_bufferReady`: Atomic flag for frame availability
- `m_scalingEvent`: Signals scaling thread when new source data arrives
