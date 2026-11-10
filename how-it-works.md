# 🔬 How AirPlay Server Actually Works

## 📡 **Phase 1: Network Discovery & Advertisement**

### Service Discovery (mDNS/Bonjour)
```
1. Application starts → CAirServer::start()
2. Gets hostname (e.g., "MyPC") 
3. Calls fgServerStart() with:
   - Server name: "MyPC"
   - AirPlay port: 5001
   - Mirror port: 7001
4. dnssd library broadcasts:
   - Service type: _airplay._tcp
   - Service name: "MyPC"
   - Port: 7001
```

**What happens on your network:**
- The Windows PC broadcasts its presence via multicast DNS
- iOS/Mac devices scanning for AirPlay receivers see "MyPC"
- They can now initiate a connection

---

## 🤝 **Phase 2: Connection Handshake**

### When iOS device connects:

```
iOS Device                           Windows PC
    │                                    │
    ├──── HTTP POST /pair-setup ────────>│  (Encryption keys)
    │<─── 200 OK ────────────────────────┤
    │                                    │
    ├──── HTTP POST /pair-verify ───────>│  (Verify pairing)
    │<─── 200 OK ────────────────────────┤
    │                                    │
    ├──── HTTP GET /server-info ────────>│  (Capabilities)
    │<─── Device info (resolution) ──────┤
    │                                    │
    ├──── HTTP POST /play ──────────────>│  (Start streaming)
    │<─── 200 OK ────────────────────────┤
```

### Connection Flow in Code:

1. **`airplay.c`** receives HTTP requests
2. Routes to handlers: `airplay_handler_pairsetup`, `airplay_handler_play`, etc.
3. **`CAirServerCallback::connected()`** fires:
   ```cpp
   m_pPlayer->setConnected(true, deviceName);
   m_pPlayer->requestShowWindow();
   ```
4. Window appears with "Connected" status

---

## 🎬 **Phase 3: Video Streaming Pipeline**

### The Journey of a Video Frame:

```
┌─────────────────────────────────────────────────────────────────┐
│  NETWORK LAYER                                                   │
│  iOS/Mac Device sends H.264 encoded video over network           │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│  RECEPTION (raop.c / airplay.c)                                  │
│  - Receives encrypted H.264 packets                              │
│  - Decrypts using FairPlay keys                                  │
│  - Assembles packets into complete frames                        │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│  DECODING (FgAirplayChannel.cpp)                                 │
│  video_process() → FgAirplayChannel::decodeH264Data()            │
│  - Uses FFmpeg to decode H.264 → YUV420 planar format           │
│  - Creates SFgVideoFrame with:                                   │
│    • pts (timestamp)                                             │
│    • width/height                                                │
│    • YUV data planes (Y, U, V)                                   │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│  CALLBACK (CAirServerCallback.cpp)                               │
│  outputVideo() is called from CALLBACK THREAD                    │
│  - Validates device ID (prevents multiple clients)               │
│  - Forwards to: m_pPlayer->outputVideo(data)                     │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│  FRAME COPY (CSDLPlayer::outputVideo) - CALLBACK THREAD          │
│  1. If the size changed, ask the render thread to reallocate     │
│  2. Lock m_mutexVideo                                            │
│  3. Copy the Y, U and V planes into the write half of the        │
│     YUV double buffer                                            │
│  4. Publish it: swap read/write index, set m_yuvReady            │
│  5. Unlock m_mutexVideo                                          │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│  RENDER LOOP (CSDLPlayer::loopEvents) - AIRPLAYCORE RENDER THREAD│
│  Paced to 60 FPS (30 FPS with the Best preset):                  │
│                                                                   │
│  1. Copy the settings pushed by the WPF app (ap_set_config)      │
│  2. Poll SDL events for the video child window (resize, mouse    │
│     zoom/pan/double-click)                                       │
│  3. Apply rotation/zoom requests, restart the receiver if its    │
│     name, PIN policy or resolution changed while idle            │
│  4. If a new frame is ready and the frame interval has passed,   │
│     SDL_UpdateYUVTexture (the GPU converts YUV to RGB and scales)│
│  5. SDL_RenderCopyEx into the child window, render the clean     │
│     feed, SDL_RenderPresent                                      │
│  6. Update the stats snapshot the WPF app polls (ap_get_stats)   │
│  7. Sleep, then spin for the rest of the frame interval          │
└─────────────────────────────────────────────────────────────────┘

The video child window belongs to this thread and is hosted inside the WPF
window with an HwndHost. All other UI (home screen, controls, settings, PIN
prompt, tray) is WPF on the UI thread.
```

---

## 🎵 **Audio Pipeline** (Parallel to Video)

```
Network → raop.c → AAC Decoding → outputAudio() callback
         → SDL Audio Queue → SDL Audio Callback (sdlAudioCallback)
         → System Audio Output
```

**Audio specifics:**
- Decodes AirPlay AAC-ELD audio using the minimal LGPL FFmpeg build
- Buffers in queue (m_queueAudio) to handle jitter
- SDL pulls from queue at hardware sample rate
- Syncs with video using PTS timestamps

---

## 🔄 **Full Connection Lifecycle**

```
1. APP STARTS
   └→ mDNS advertises "MyPC" on network
   └→ Show home screen with device name

2. iOS CONNECTS
   └→ Pairing handshake (encryption)
   └→ CAirServerCallback::connected() fires
   └→ Show "Connected from: iPhone"

3. VIDEO STARTS
   └→ H.264 packets arrive
   └→ Decoded to YUV frames
   └→ Callback thread copies them into the YUV double buffer
   └→ Render thread uploads and presents them at 60 FPS

4. USER WATCHES
   └→ Video plays smoothly
   └→ Controls panel beside the video shows connection info
   └→ Can press H to hide UI
   └→ Can double-click for fullscreen

5. iOS DISCONNECTS
   └→ CAirServerCallback::disconnected() fires
   └→ Clear video to black
   └→ Show home screen again
   └→ Ready for next connection
```

---
