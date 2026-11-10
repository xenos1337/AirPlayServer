# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

Visual Studio 2026 (v145 toolset) for the native projects, Windows 10 SDK, and the .NET 10 SDK for the WPF app.

```
# From Visual Studio
# Open AirPlay.sln, then:
Ctrl+B (Build Solution)
F5 (Run with debugging)

# From x64 Native Tools Command Prompt
msbuild AirPlay.sln /restore /p:Configuration=Debug /p:Platform=x64
```

Output: `x64\Debug\AirPlayServer.exe`

**Startup Project**: AirPlayServer (set via right-click → "Set as Startup Project")

## Coding Rules

- When replacing or superseding code/files, always delete the old unused code/files. Never leave dead code or replaced files behind.

**Build dependency chain**: AirPlayLib (static lib) → airplay2dll (DLL) → AirPlayCore (DLL) → AirPlayServer (WPF exe). dnssd builds independently. The C# project depends on AirPlayCore through solution dependencies, not a ProjectReference.

### Runtime files

Everything lands in `x64\<Configuration>\` (x86: `<Configuration>\`):
- `AirPlayServer.exe`, `AirPlayServer.dll`, `.runtimeconfig.json`, `.deps.json` (WPF app, needs the .NET 10 Desktop Runtime)
- `AirPlayCore.dll`, `airplay2dll.dll`, `dnssd.dll`
- `SDL2.dll`, `avcodec-62.dll`, `avutil-60.dll`, `swscale-9.dll` (minimal LGPL FFmpeg 8.1.2)
- `msys-2.0.dll` (MSYS2 runtime, required by libplist)

AirPlayCore's post-build step copies SDL2, FFmpeg and plist DLLs.

### Build Caveats

- airplay2dll links `libplist.a` from a hardcoded MSYS2 path (`C:\msys64\mingw32\lib\gcc\i686-w64-mingw32\9.2.0`). CI creates the directory.
- `legacy_stdio_definitions.lib` is linked for CRT compatibility.
- The C# project uses `TreatWarningsAsErrors`.

## Architecture Overview

### Solution Structure

| Project | Type | Language | Purpose |
|---------|------|----------|---------|
| **AirPlayLib** | Static lib | C | AirPlay 2 protocol: RAOP, pairing, crypto, FFmpeg AAC-ELD decoding |
| **airplay2dll** | DLL | C++ | Wraps AirPlayLib + FFmpeg H.264 decoding, exports `fgServerStart*/Stop/Scale` |
| **AirPlayCore** | DLL | C++ | Receiver, SDL2 video renderer, audio output; flat C API in `AirPlayCore.h` |
| **AirPlayServer** | WinExe | C# / WPF (.NET 10) | All UI: windows, settings, PIN prompt, tray, fullscreen, picture in picture |
| **dnssd** | DLL | C | mDNS/Bonjour, exports `DNSService*` |
| **MirrorSimAdapter** | Console exe | C++ | Headless JSONL adapter used by CI |
| **AirPlayLibSmokeTests** | Console exe | C | AAC-ELD decoder smoke test |

### Entry Point

`AirPlayServer/App.xaml.cs` → `OnStartup`: debug log (`--debug`), Bonjour check (`ap_check_bonjour`), host name, `Settings.Load()`, `MainWindow.StartReceiver()` (calls `ap_start` with the window's HWND), then shows the window unless `--tray`. `--minimized` starts minimized.

### UI ↔ native boundary (`AirPlayCore/AirPlayCore.h`, mirrored by `AirPlayServer/Native.cs`)

- `ApConfig` (receiver name, PIN on/off, resolution, quality, screen cast, volume) is pushed with `ap_set_config` whenever a setting changes.
- `ApStats` is polled by the UI (`ap_get_stats`, 250 ms for the panel, 1 s for diagnostics). Packed to 4 bytes on both sides.
- `ApCallbacks` fire on native threads: connection changed, video size changed, PIN request/closed, key up and double-click from the video window. `NativeCallbacks` only queues them onto the WPF dispatcher; never block in them.
- Keep the C struct and the C# struct field-for-field identical.

### Threading Model

- **WPF UI thread**: all windows, settings, tray. Calls every `ap_*` export.
- **Render thread** (AirPlayCore): owns SDL. Creates the video child window (`AirPlayCoreVideo`, parented to the WPF window, PerMonitorV2), `SDL_CreateWindowFrom`, renderer, clean feed; runs `CSDLPlayer::loopEvents`. WPF hosts the child through `VideoHost` (HwndHost).
- **Network/decoder threads** (airplay2dll): `outputVideo`, `outputAudio`, `setConnected`, `requestPinApproval`.
- **SDL audio thread**: `sdlAudioCallback`.

The child window and the WPF window are on different threads, so their input queues are attached. `CSDLPlayer::start`/`stop` wait with `MsgWaitForMultipleObjects(QS_SENDMESSAGE)` to keep servicing sent messages. Shutdown order matters: `VideoHost.Dispose()`, then `ap_stop()`, then close windows (see `MainWindow.Quit`).

SDL only tracks keyboard focus for foreground windows, which a child never is, so key-ups and the cursor are handled in `CSDLPlayer::videoWindowProc`.

### Key Data Flow

```
Network → raop.c/airplay.c → FgAirplayChannel (FFmpeg H.264→YUV420P)
       → CAirServerCallback → CSDLPlayer::outputVideo (copy into YUV double buffer)
       → Render thread: paced SDL_UpdateYUVTexture → SDL_RenderCopyEx → present
```

### Synchronization (CSDLPlayer)

- `m_mutexVideo`: YUV buffers and video texture
- `m_mutexAudio`: audio queue
- `m_mutexConfig`: `ApConfig` copy, read once per frame into `m_frameConfig`
- `m_mutexStats`: `ApStats` snapshot written once per frame
- `m_mutexPinApproval` + `m_eventPinApproval`: PIN states IDLE/WAITING/SHOWING_CODE with a generation counter
- Interlocked flags: `m_yuvReady`/`m_yuvReadIdx`/`m_yuvWriteIdx`, `m_rotationAngle` (UI writes, render applies), `m_zoomResetPending`, `m_capturePrivacyActive`, `m_shuttingDown`, `m_quitRequested`

### Capture exclusion

`SetWindowDisplayAffinity` is applied from C# on the UI thread: the main window for capture privacy and Screen Cast "Hide interface", the PIN window for "Hide PIN". The clean feed (`CCleanFeedOutput`) is a separate SDL window kept behind the WPF window.

### Settings

`Settings.cs` reads/writes `%APPDATA%\AirPlayServer\airplay_settings.ini` through `IniFile` (UTF-8, compatible with files from the Windows profile API, unknown keys kept). A file next to the exe from earlier versions is copied over once. Saves are debounced by 1 s and done on quit. Autostart is `HKCU\...\Run\AirPlayServer` = `"<exe>" --tray`.

### Audio Pipeline

- SDL audio spec: `AUDIO_S16SYS`, system device rate (queried via WASAPI), 1024 sample buffer
- Closed-loop linear resampler keeps the queue near 8 frames (max 20, ±0.5% correction)

## UI

WPF Fluent theme (`ThemeMode="System"` in `App.xaml`, follows light/dark). Shared styles in `App.xaml`: `Card`, `SectionHeader`, `Description`, `Warning`, `ToggleSwitch` (CheckBox template; the Fluent theme has switch brushes but no switch style). Use theme brushes via `DynamicResource`, not hard-coded colors.

| Key | Action |
|-----|--------|
| H | Toggle session controls panel |
| Ctrl+Shift+H | Toggle capture privacy |
| P | Toggle picture in picture |
| F / double-click | Toggle fullscreen (Esc exits) |
| F1 | Diagnostics window |
| R | Rotate 90° |

## Key Constants

- AirPlay ports: 5001 (RAOP), 7001 (mirroring) — hardcoded in `CAirServer::start`
- Receiver restart after name/PIN/resolution changes: 1 s debounce, only while idle
- Cursor auto-hide over the video: 5000 ms

## Quality Presets

| Preset | FPS | Scaling |
|--------|-----|---------|
| 0 Best | 30 | best |
| 1 Balanced (default) | 60 | best |
| 2 Low latency | 60 | linear |

## External Dependencies

All pre-built in `external/`:
- **SDL2 2.30** — video rendering and audio output
- **FFmpeg 8.1.2** — minimal LGPL build with only AAC and H.264 decoders
- **libplist** — Apple property lists (requires MSYS2 runtime)
