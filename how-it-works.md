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
│  VIDEO RENDERING (CSDLPlayer::outputVideo) - CALLBACK THREAD     │
│  1. Lock m_mutexVideo                                            │
│  2. Check if video dimensions changed → recreate buffer          │
│  3. Lock m_videoBuffer surface                                   │
│  4. Clear to black (letterbox/pillarbox)                         │
│  5. FOR EACH PIXEL:                                              │
│     - Read YUV values from data planes                           │
│     - Convert YUV → RGB using BT.601:                            │
│       r = Y + 1.402 * (V - 128)                                  │
│       g = Y - 0.344 * (U - 128) - 0.714 * (V - 128)              │
│       b = Y + 1.772 * (U - 128)                                  │
│     - Write RGB pixel to m_videoBuffer                           │
│  6. Unlock m_videoBuffer                                         │
│  7. Set m_hasNewFrame = true                                     │
│  8. Store m_lastFramePTS                                         │
│  9. Unlock m_mutexVideo                                          │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│  MAIN RENDER LOOP (CSDLPlayer::loopEvents) - MAIN THREAD         │
│  Running at 30 FPS:                                              │
│                                                                   │
│  1. frameStartTime = GetTickCount()                              │
│  2. Poll SDL events (keyboard, mouse, resize)                    │
│  3. Lock m_mutexVideo                                            │
│  4. SDL_BlitSurface(m_videoBuffer → m_surface)                   │
│     ↳ Copy off-screen buffer to screen surface                   │
│  5. m_hasNewFrame = false                                        │
│  6. ImGui::NewFrame()                                            │
│  7. Render UI overlay:                                           │
│     - Home screen (disconnected)                                 │
│     - Connection status (connected)                              │
│  8. ImGui::Render() → Draw to m_surface with alpha blending      │
│  9. SDL_Flip(m_surface) → Display to screen                      │
│  10. Unlock m_mutexVideo                                         │
│  11. Calculate frame time                                        │
│  12. SDL_Delay(33 - frameTime) → Cap at 30 FPS                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 🎵 **Audio Pipeline** (Parallel to Video)

```
Network → raop.c → AAC Decoding → outputAudio() callback
         → SDL Audio Queue → SDL Audio Callback (sdlAudioCallback)
         → System Audio Output
```

**Audio specifics:**
- Decodes AAC audio using FDK-AAC or FFmpeg
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
   └→ Callback thread writes to m_videoBuffer
   └→ Main thread blits and displays at 30 FPS

4. USER WATCHES
   └→ Video plays smoothly
   └→ ImGui overlay shows connection info
   └→ Can press H to hide UI
   └→ Can double-click for fullscreen

5. iOS DISCONNECTS
   └→ CAirServerCallback::disconnected() fires
   └→ Clear video to black
   └→ Show home screen again
   └→ Ready for next connection
```

---