#include "CSDLPlayer.h"
#include <stdio.h>
#include <malloc.h>  // For _aligned_malloc/_aligned_free
#include "CAutoLock.h"

// Static instance pointer for window procedure callback
CSDLPlayer* CSDLPlayer::s_instance = NULL;

// Helper function to clamp value to 0-255
static inline Uint8 ClampByte(int value) {
	if (value < 0) return 0;
	if (value > 255) return 255;
	return (Uint8)value;
}

// Convert YUV pixel to RGB
static inline void YUVToRGB(int y, int u, int v, Uint8* r, Uint8* g, Uint8* b) {
	// BT.601 full range conversion (0-255)
	// Most modern streaming uses full range YUV
	int d = u - 128;
	int e = v - 128;
	
	*r = ClampByte(y + ((359 * e + 128) >> 8));
	*g = ClampByte(y - ((88 * d + 183 * e + 128) >> 8));
	*b = ClampByte(y + ((454 * d + 128) >> 8));
}

/* This function may run in a separate event thread */
int FilterEvents(const SDL_Event* event) {
// 	static int boycott = 1;
// 
// 	/* This quit event signals the closing of the window */
// 	if ((event->type == SDL_QUIT) && boycott) {
// 		printf("Quit event filtered out -- try again.\n");
// 		boycott = 0;
// 		return(0);
// 	}
// 	if (event->type == SDL_MOUSEMOTION) {
// 		printf("Mouse moved to (%d,%d)\n",
// 			event->motion.x, event->motion.y);
// 		return(0);    /* Drop it, we've handled it */
// 	}
	return(1);
}

CSDLPlayer::CSDLPlayer()
	: m_surface(NULL)
	, m_videoBuffer(NULL)
	, m_yuv(NULL)
	, m_bAudioInited(false)
	, m_bDumpAudio(false)
	, m_fileWav(NULL)
	, m_sAudioFmt()
	, m_displayRect()
	, m_videoWidth(0)
	, m_videoHeight(0)
	, m_lastFramePTS(0)
	, m_lastFrameTime(0)
	, m_hasNewFrame(false)
	, m_windowWidth(800)
	, m_windowHeight(600)
	, m_server()
	, m_hwnd(NULL)
	, m_bWindowVisible(false)
	, m_originalWndProc(NULL)
	, m_bResizing(false)
	, m_bFullscreen(false)
	, m_windowedRect()
	, m_windowedStyle(0)
	, m_windowedExStyle(0)
{
	ZeroMemory(&m_sAudioFmt, sizeof(SFgAudioFrame));
	ZeroMemory(&m_displayRect, sizeof(SDL_Rect));
	ZeroMemory(&m_windowedRect, sizeof(RECT));
	ZeroMemory(m_serverName, sizeof(m_serverName));
	ZeroMemory(m_connectedDeviceName, sizeof(m_connectedDeviceName));
	m_bConnected = false;
	m_bDisconnecting = false;
	m_dwDisconnectStartTime = 0;
	m_b1to1PixelMode = true;  // Enable 1:1 pixel mode by default for crisp video
	m_mutexAudio = CreateMutex(NULL, FALSE, NULL);
	m_mutexVideo = CreateMutex(NULL, FALSE, NULL);
	s_instance = this;
	
	// Initialize video statistics
	m_totalFrames = 0;
	m_droppedFrames = 0;
	m_fpsStartTime = 0;
	m_fpsFrameCount = 0;
	m_currentFPS = 0.0f;
	m_totalBytes = 0;
	m_bitrateStartTime = 0;
	m_currentBitrateMbps = 0.0f;
	m_frameSkipCounter = 0;
	m_currentQualityPreset = QUALITY_BALANCED;

	// Initialize scaler with double-buffering
	m_swsCtx = NULL;
	m_scaledWidth = 0;
	m_scaledHeight = 0;
	m_bScalerNeedsReinit = 0;
	m_scaledYUV[0][0] = NULL;
	m_scaledYUV[0][1] = NULL;
	m_scaledYUV[0][2] = NULL;
	m_scaledYUV[1][0] = NULL;
	m_scaledYUV[1][1] = NULL;
	m_scaledYUV[1][2] = NULL;
	m_scaledPitch[0] = 0;
	m_scaledPitch[1] = 0;
	m_scaledPitch[2] = 0;
	m_writeBuffer = 0;
	m_readBuffer = 0;
	m_bufferReady = 0;

	// Initialize source buffer for scaling thread
	m_srcYUV[0] = NULL;
	m_srcYUV[1] = NULL;
	m_srcYUV[2] = NULL;
	m_srcPitch[0] = 0;
	m_srcPitch[1] = 0;
	m_srcPitch[2] = 0;
	m_srcWidth = 0;
	m_srcHeight = 0;
	m_srcReady = 0;
	m_scalingThread = NULL;
	m_scalingEvent = NULL;
	m_scalingThreadRunning = 0;
}

CSDLPlayer::~CSDLPlayer()
{
	// Restore original window procedure before cleanup
	if (m_hwnd != NULL && m_originalWndProc != NULL) {
		SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)m_originalWndProc);
	}

	freeScaler();
	unInit();

	CloseHandle(m_mutexAudio);
	CloseHandle(m_mutexVideo);

	s_instance = NULL;
}

bool CSDLPlayer::init()
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return false;
	}

	/* Clean up on exit, exit on window close and interrupt */
	atexit(SDL_Quit);

	// Enable mouse events for ImGui
	SDL_EventState(SDL_MOUSEMOTION, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONUP, SDL_ENABLE);
	
	// Enable Unicode for text input in ImGui
	SDL_EnableUNICODE(1);

	initVideo(m_windowWidth, m_windowHeight);

	// Get the window handle for show/hide operations
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	if (SDL_GetWMInfo(&wmInfo) == 1) {
		m_hwnd = wmInfo.window;
		
		// Enable double-click detection by adding CS_DBLCLKS to window class
		LONG_PTR classStyle = GetClassLongPtr(m_hwnd, GCL_STYLE);
		SetClassLongPtr(m_hwnd, GCL_STYLE, classStyle | CS_DBLCLKS);
		
		// Subclass the window for live resize updates and double-click fullscreen
		m_originalWndProc = (WNDPROC)SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)WindowProc);
	}

	// Initialize ImGui
	if (!m_imgui.Init(m_surface)) {
		printf("Failed to initialize ImGui\n");
		// Continue anyway
	}

	// Start with window visible to show home screen
	showWindow();

	/* Filter quit and mouse motion events */
	SDL_SetEventFilter(FilterEvents);

	// Auto-start the server (server name should be set before init() is called)
	m_server.start(this, strlen(m_serverName) > 0 ? m_serverName : NULL);

	return true;
}

void CSDLPlayer::setServerName(const char* serverName)
{
	if (serverName != NULL && strlen(serverName) > 0) {
		strncpy_s(m_serverName, sizeof(m_serverName), serverName, _TRUNCATE);
	} else {
		m_serverName[0] = '\0';
	}
}

void CSDLPlayer::setConnected(bool connected, const char* deviceName)
{
	if (m_bConnected && !connected) {
		// Transitioning from connected to disconnected
		// Start the disconnect transition to show black screen
		m_bDisconnecting = true;
		m_dwDisconnectStartTime = GetTickCount();
		
		// Reset statistics on disconnect
		m_totalFrames = 0;
		m_droppedFrames = 0;
		m_fpsStartTime = 0;
		m_fpsFrameCount = 0;
		m_currentFPS = 0.0f;
		m_totalBytes = 0;
		m_bitrateStartTime = 0;
		m_currentBitrateMbps = 0.0f;
	} else if (!m_bConnected && connected) {
		// Reset statistics when connecting
		m_totalFrames = 0;
		m_droppedFrames = 0;
		m_fpsStartTime = 0;
		m_fpsFrameCount = 0;
		m_currentFPS = 0.0f;
		m_totalBytes = 0;
		m_bitrateStartTime = 0;
		m_currentBitrateMbps = 0.0f;
	}
	
	m_bConnected = connected;
	if (deviceName) {
		strncpy_s(m_connectedDeviceName, sizeof(m_connectedDeviceName), deviceName, _TRUNCATE);
	} else {
		m_connectedDeviceName[0] = '\0';
	}
}

void CSDLPlayer::unInit()
{
	unInitVideo();
	unInitAudio();

	SDL_Quit();
}

void CSDLPlayer::loopEvents()
{
	SDL_Event event;

	BOOL bEndLoop = FALSE;
	bool bShowUI = true;

	// Frame timing variables
	DWORD frameStartTime = 0;
	EQualityPreset lastQualityPreset = m_imgui.GetQualityPreset();
	// Sync initial preset to thread-safe variable
	InterlockedExchange(&m_currentQualityPreset, (LONG)lastQualityPreset);
	
	/* Main loop - poll events and render ImGui */
	while (!bEndLoop) {
		// Record frame start time
		frameStartTime = GetTickCount();
		
		// Process all pending events
		while (SDL_PollEvent(&event)) {
			// Process ImGui events first
			m_imgui.ProcessEvent(&event);
			
			// Skip application event processing if ImGui wants to capture it
			if (m_imgui.WantCaptureMouse() && (event.type == SDL_MOUSEMOTION || 
				event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)) {
				continue;
			}
			if (m_imgui.WantCaptureKeyboard() && (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)) {
				continue;
			}
			
			switch (event.type) {
		case SDL_USEREVENT: {
			if (event.user.code == VIDEO_SIZE_CHANGED_CODE) {
				unsigned int width = (unsigned int)(uintptr_t)event.user.data1;
				unsigned int height = (unsigned int)(uintptr_t)event.user.data2;
				if (width != m_videoWidth || height != m_videoHeight || m_yuv == NULL) {
					// Video source size changed - recreate overlay and video buffer
					{
						CAutoLock oLock(m_mutexVideo, "recreateOverlay");
						if (m_yuv != NULL) {
							SDL_FreeYUVOverlay(m_yuv);
							m_yuv = NULL;
						}
						if (m_videoBuffer != NULL) {
							SDL_FreeSurface(m_videoBuffer);
							m_videoBuffer = NULL;
						}
						// Free scaler when video size changes
						freeScaler();

						m_videoWidth = width;
						m_videoHeight = height;
					}

					// 1:1 pixel mode: resize window to match video for crisp rendering
					if (m_b1to1PixelMode && !m_bFullscreen) {
						resizeToVideoSize();
					}

					// Calculate display rect first (needed for overlay size)
					calculateDisplayRect();

					// Now create overlay at display rect size for high-quality scaling
					// When window matches video (1:1), displayRect matches video size
					// When window differs, displayRect is the scaled size
					{
						CAutoLock oLock(m_mutexVideo, "createOverlay");
						int overlayW = (m_displayRect.w > 0) ? m_displayRect.w : m_videoWidth;
						int overlayH = (m_displayRect.h > 0) ? m_displayRect.h : m_videoHeight;

						m_yuv = SDL_CreateYUVOverlay(overlayW, overlayH, SDL_IYUV_OVERLAY, m_surface);

						// Create off-screen video buffer matching window size
						if (m_surface != NULL) {
							m_videoBuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, m_windowWidth, m_windowHeight, 32,
								m_surface->format->Rmask, m_surface->format->Gmask,
								m_surface->format->Bmask, m_surface->format->Amask);
							if (m_videoBuffer != NULL) {
								SDL_FillRect(m_videoBuffer, NULL, SDL_MapRGB(m_videoBuffer->format, 0, 0, 0));
							}
						}

						// Initialize overlay to black to prevent green flicker
						if (m_yuv != NULL) {
							SDL_LockYUVOverlay(m_yuv);
							memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);
							memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * (m_yuv->h >> 1));
							memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * (m_yuv->h >> 1));
							SDL_UnlockYUVOverlay(m_yuv);
						}
					}

					clearToBlack();
				}
			}
			else if (event.user.code == SHOW_WINDOW_CODE) {
				showWindow();
			}
			else if (event.user.code == HIDE_WINDOW_CODE) {
				hideWindow();
			}
			else if (event.user.code == TOGGLE_FULLSCREEN_CODE) {
				toggleFullscreen();
			}
			break;
		}
		case SDL_VIDEORESIZE: {
			// This is now handled by WM_SIZE/WM_SIZING for live updates
			// Just ensure final size is correct
			if (event.resize.w != m_windowWidth || event.resize.h != m_windowHeight) {
				resizeWindow(event.resize.w, event.resize.h);
			}
			break;
		}
		case SDL_VIDEOEXPOSE: {
			// Redraw when window is exposed - just flip, video will be redrawn next frame
			SDL_Flip(m_surface);
			break;
		}
		case SDL_ACTIVEEVENT: {
			if (event.active.state & SDL_APPACTIVE) {
				if (event.active.gain) {
					//printf("App activated\n");
				}
				else {
					//printf("App iconified\n");
				}
			}
			break;
		}
		case SDL_KEYUP: {
			switch (event.key.keysym.sym)
			{
				case SDLK_ESCAPE: {
					// ESC exits fullscreen
					if (m_bFullscreen) {
						toggleFullscreen();
					}
					break;
				}
				case SDLK_f: {
					// F key also toggles fullscreen
					toggleFullscreen();
					break;
				}
			}
			break;
		}

		case SDL_QUIT: {
			printf("Quit requested, quitting.\n");
			// Stop server first before exiting the event loop
			// This ensures clean shutdown and prevents callbacks during destruction
			m_server.stop();
			// Small delay to allow server to fully stop
			SDL_Delay(100);
			bEndLoop = TRUE;
			break;
		}
			}
		} // End of event polling loop
		
		// Handle disconnect transition (show black screen briefly)
		if (m_bDisconnecting) {
			DWORD elapsed = GetTickCount() - m_dwDisconnectStartTime;
			if (elapsed >= 800) {  // Show black screen for 800ms
				m_bDisconnecting = false;
			}
		}
		
		// MAIN THREAD: Hardware-accelerated YUV rendering with ImGui overlay
		// Step 1: Display YUV overlay (minimize mutex hold time)
		{
			CAutoLock videoLock(m_mutexVideo, "renderLoop");

			if (m_hasNewFrame && m_yuv != NULL && m_surface != NULL) {
				// Hardware-accelerated YUV->RGB conversion and scaling
				SDL_DisplayYUVOverlay(m_yuv, &m_displayRect);
				m_hasNewFrame = false; // Mark frame as rendered immediately
				m_lastFrameTime = GetTickCount();
			} else if (m_yuv != NULL && m_surface != NULL && m_videoWidth > 0) {
				// No new frame, but redisplay existing YUV overlay
				SDL_DisplayYUVOverlay(m_yuv, &m_displayRect);
			} else if (m_surface != NULL) {
				// No video yet - fill with black
				SDL_FillRect(m_surface, NULL, SDL_MapRGB(m_surface->format, 0, 0, 0));
			}
		}
		// Mutex released - outputVideo can now write next frame while we do ImGui

		// Step 2: Render ImGui on top of m_surface (outside mutex for less contention)
		if (m_surface != NULL) {
			// Check if UI was just hidden - need to clear surface to remove old UI
			if (m_imgui.WasUIJustHidden()) {
				// Clear the entire surface to black to remove old UI content
				SDL_FillRect(m_surface, NULL, SDL_MapRGB(m_surface->format, 0, 0, 0));
			}

			m_imgui.NewFrame(m_surface);
			if (m_bConnected) {
				// Connected - show overlay with controls and video statistics
				m_imgui.RenderOverlay(&bShowUI, m_serverName, m_bConnected, m_connectedDeviceName,
					m_videoWidth, m_videoHeight, m_currentFPS, m_currentBitrateMbps,
					m_totalFrames, m_droppedFrames);
			} else if (m_bDisconnecting) {
				// Disconnecting - render nothing (show black screen)
			} else {
				// Disconnected - show home screen
				m_imgui.RenderHomeScreen(m_serverName, m_bConnected, m_connectedDeviceName);
			}
			// ImGui renders with alpha blending on top of the video layer
			m_imgui.Render(m_surface);

			// Step 3: Flip to display everything
			SDL_Flip(m_surface);
		}
		
		// Update quality preset for callback thread (thread-safe)
		EQualityPreset currentPreset = m_imgui.GetQualityPreset();

		// If quality preset changed, signal scaler to reinitialize and update cached preset
		if (currentPreset != lastQualityPreset) {
			lastQualityPreset = currentPreset;
			// Update thread-safe cached preset for callback thread
			InterlockedExchange(&m_currentQualityPreset, (LONG)currentPreset);
			// Set flag - the callback thread will reinit the scaler safely
			InterlockedExchange(&m_bScalerNeedsReinit, 1);
		}

		// Display runs at full speed (~60fps) - frame skipping in outputVideo handles 30fps cap
		// Just do minimal delay to prevent CPU spinning
		DWORD frameEndTime = GetTickCount();
		DWORD frameTime = frameEndTime - frameStartTime;

		if (frameTime < 16) {
			// Cap display at ~60fps to prevent excessive CPU usage
			SDL_Delay(16 - frameTime);
		}
	}
}

void CSDLPlayer::outputVideo(SFgVideoFrame* data)
{
	if (data->width == 0 || data->height == 0) {
		return;
	}

	// Frame skipping for 30fps mode (Good Quality)
	// Skip every other frame when in Good Quality mode to halve the frame rate
	// Use thread-safe cached preset (updated by main thread via InterlockedExchange)
	LONG preset = InterlockedCompareExchange(&m_currentQualityPreset, 0, 0);
	if (preset == QUALITY_GOOD) {
		m_frameSkipCounter++;
		if (m_frameSkipCounter % 2 == 0) {
			// Skip this frame - don't process it
			// Still count bytes for bitrate calculation
			m_totalBytes += data->dataTotalLen;
			return;
		}
	}

	// Check if video source dimensions changed
	if ((int)data->width != m_videoWidth || (int)data->height != m_videoHeight) {
		{
			CAutoLock oLock(m_mutexVideo, "videoSizeChange");
			if (NULL != m_yuv) {
				SDL_FreeYUVOverlay(m_yuv);
				m_yuv = NULL;
			}
			// Also free the video buffer since dimensions changed
			if (NULL != m_videoBuffer) {
				SDL_FreeSurface(m_videoBuffer);
				m_videoBuffer = NULL;
			}
		}
		m_evtVideoSizeChange.type = SDL_USEREVENT;
		m_evtVideoSizeChange.user.type = SDL_USEREVENT;
		m_evtVideoSizeChange.user.code = VIDEO_SIZE_CHANGED_CODE;
		m_evtVideoSizeChange.user.data1 = (void*)(uintptr_t)data->width;
		m_evtVideoSizeChange.user.data2 = (void*)(uintptr_t)data->height;

		SDL_PushEvent(&m_evtVideoSizeChange);
		return;
	}

	// CALLBACK THREAD: Write YUV data to hardware overlay
	// Use short mutex lock - only for overlay write, not for scaling

	// Get source YUV plane pointers
	const uint8_t* srcY = data->data;
	const uint8_t* srcU = data->data + data->dataLen[0];
	const uint8_t* srcV = data->data + data->dataLen[0] + data->dataLen[1];

	// SCALING/COPY PATH - all under mutex to prevent race with resize
	{
		CAutoLock oLock(m_mutexVideo, "outputVideo");

		// Read display rect inside mutex for thread safety
		int dstW = m_displayRect.w;
		int dstH = m_displayRect.h;

		// Check if we need to scale (display size differs from video size)
		bool needsScaling = (dstW != (int)data->width || dstH != (int)data->height);

		if (needsScaling && dstW > 0 && dstH > 0) {
			// SCALING PATH
			// Initialize/reinit scaler if needed
			initScaler(data->width, data->height, dstW, dstH);

			// Get write buffer for scaling
			LONG writeIdx = InterlockedCompareExchange(&m_writeBuffer, 0, 0);
			if (writeIdx < 0 || writeIdx > 1) writeIdx = 0;  // Safety check

			// Verify all buffers are valid before scaling
			if (m_swsCtx != NULL &&
				m_scaledYUV[writeIdx][0] != NULL &&
				m_scaledYUV[writeIdx][1] != NULL &&
				m_scaledYUV[writeIdx][2] != NULL &&
				srcY != NULL && srcU != NULL && srcV != NULL &&
				data->pitch[0] > 0 && data->pitch[1] > 0 && data->pitch[2] > 0 &&
				m_scaledWidth == dstW && m_scaledHeight == dstH) {

				// FRAME DROPPING: If previous frame hasn't been displayed, drop this new frame
				if (m_hasNewFrame) {
					m_droppedFrames++;
				} else {
					// Scale directly from source to our buffer
					const uint8_t* srcSlice[3] = { srcY, srcU, srcV };
					int srcStride[3] = { (int)data->pitch[0], (int)data->pitch[1], (int)data->pitch[2] };
					sws_scale(m_swsCtx, srcSlice, srcStride, 0, data->height,
						m_scaledYUV[writeIdx], m_scaledPitch);

					// Copy to overlay - verify overlay dimensions match
					if (m_yuv != NULL && m_yuv->w == dstW && m_yuv->h == dstH &&
						SDL_LockYUVOverlay(m_yuv) == 0) {
						// Fast bulk copy of scaled YUV to overlay
						// Y plane - single memcpy per row
						for (int y = 0; y < dstH; y++) {
							memcpy(m_yuv->pixels[0] + y * m_yuv->pitches[0],
								m_scaledYUV[writeIdx][0] + y * m_scaledPitch[0], dstW);
						}
						// U/V planes - combined loop
						for (int y = 0; y < dstH / 2; y++) {
							memcpy(m_yuv->pixels[1] + y * m_yuv->pitches[1],
								m_scaledYUV[writeIdx][1] + y * m_scaledPitch[1], dstW / 2);
							memcpy(m_yuv->pixels[2] + y * m_yuv->pitches[2],
								m_scaledYUV[writeIdx][2] + y * m_scaledPitch[2], dstW / 2);
						}

						SDL_UnlockYUVOverlay(m_yuv);
						m_lastFramePTS = data->pts;
						m_hasNewFrame = true;

						// Swap write buffer for next frame
						InterlockedExchange(&m_writeBuffer, 1 - writeIdx);
					}
				}
			}
		} else if (dstW > 0 && dstH > 0) {
			// DIRECT PATH: 1:1 pixel mapping - direct copy without scaling (crisp and fast)
			// Already inside mutex

			// FRAME DROPPING: If previous frame hasn't been displayed, drop this new frame
			if (m_hasNewFrame) {
				m_droppedFrames++;
			} else if (m_yuv != NULL &&
				m_yuv->w == (int)data->width && m_yuv->h == (int)data->height &&
				SDL_LockYUVOverlay(m_yuv) == 0) {
				// Y plane
				for (unsigned int y = 0; y < data->height; y++) {
					memcpy(m_yuv->pixels[0] + y * m_yuv->pitches[0],
						srcY + y * data->pitch[0], data->width);
				}
				// U/V planes - combined loop
				for (unsigned int y = 0; y < data->height / 2; y++) {
					memcpy(m_yuv->pixels[1] + y * m_yuv->pitches[1],
						srcU + y * data->pitch[1], data->width / 2);
					memcpy(m_yuv->pixels[2] + y * m_yuv->pitches[2],
						srcV + y * data->pitch[2], data->width / 2);
				}

				SDL_UnlockYUVOverlay(m_yuv);
				m_lastFramePTS = data->pts;
				m_hasNewFrame = true;
			}
		}
	} // End of mutex scope

	// Update statistics
	m_totalFrames++;
	m_totalBytes += data->dataTotalLen;
	
	// Calculate FPS and bitrate (update every second)
	DWORD currentTime = SDL_GetTicks();
	if (m_fpsStartTime == 0) {
		m_fpsStartTime = currentTime;
		m_bitrateStartTime = currentTime;
	}
	
	m_fpsFrameCount++;
	
	// Update FPS every second
	if (currentTime - m_fpsStartTime >= 1000) {
		m_currentFPS = (float)m_fpsFrameCount * 1000.0f / (float)(currentTime - m_fpsStartTime);
		m_fpsFrameCount = 0;
		m_fpsStartTime = currentTime;
	}
	
	// Update bitrate every second
	static unsigned long long lastTotalBytes = 0;
	if (currentTime - m_bitrateStartTime >= 1000) {
		unsigned long long bytesDelta = m_totalBytes - lastTotalBytes;
		m_currentBitrateMbps = (float)(bytesDelta * 8) / (1000.0f * 1000.0f); // Convert to Mbps
		lastTotalBytes = m_totalBytes;
		m_bitrateStartTime = currentTime;
	}
}

void CSDLPlayer::outputAudio(SFgAudioFrame* data)
{
	if (data->channels == 0) {
		return;
	}

	initAudio(data);

	if (m_bDumpAudio) {
		if (m_fileWav != NULL) {
			fwrite(data->data, data->dataLen, 1, m_fileWav);
		}
	}

	SAudioFrame* dataClone = new SAudioFrame();
	dataClone->dataTotal = data->dataLen;
	dataClone->pts = data->pts;
	dataClone->dataLeft = dataClone->dataTotal;
	dataClone->data = new uint8_t[dataClone->dataTotal];
	memcpy(dataClone->data, data->data, dataClone->dataTotal);

	{
		CAutoLock oLock(m_mutexAudio, "outputAudio");

		// AUDIO QUEUE LIMITING: Prevent unbounded growth causing delay accumulation
		// If queue exceeds 10 frames (~200ms at 48kHz), drop oldest frames
		while (m_queueAudio.size() >= 10) {
			SAudioFrame* oldFrame = m_queueAudio.front();
			m_queueAudio.pop();
			delete[] oldFrame->data;
			delete oldFrame;
		}

		m_queueAudio.push(dataClone);
	}
}

void CSDLPlayer::initVideo(int width, int height)
{
	m_windowWidth = width;
	m_windowHeight = height;
	
	// Create resizable window with double buffering to prevent flickering
	// SDL_DOUBLEBUF enables hardware double buffering for smooth rendering
	m_surface = SDL_SetVideoMode(width, height, 32, SDL_SWSURFACE | SDL_DOUBLEBUF | SDL_RESIZABLE);
	SDL_WM_SetCaption("AirPlay Server", NULL);

	// Fill with black initially
	clearToBlack();
	
	// Calculate display rect (will be 0x0 until video arrives)
	calculateDisplayRect();
}

void CSDLPlayer::resizeWindow(int width, int height)
{
	// Prevent re-entrancy
	if (m_bResizing) {
		return;
	}
	
	m_bResizing = true;
	
	m_windowWidth = width;
	m_windowHeight = height;
	
	// Need to recreate YUV overlay when surface changes
	CAutoLock oLock(m_mutexVideo, "resizeWindow");
	
	// Free old overlay, video buffer and scaler before recreating surface
	if (m_yuv != NULL) {
		SDL_FreeYUVOverlay(m_yuv);
		m_yuv = NULL;
	}
	if (m_videoBuffer != NULL) {
		SDL_FreeSurface(m_videoBuffer);
		m_videoBuffer = NULL;
	}
	freeScaler();  // Free scaler since display rect will change

	// Recreate surface with new size
	m_surface = SDL_SetVideoMode(width, height, 32, SDL_SWSURFACE | SDL_DOUBLEBUF | SDL_RESIZABLE);

	// Recreate video buffer matching new window size
	if (m_surface != NULL) {
		m_videoBuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32,
			m_surface->format->Rmask, m_surface->format->Gmask,
			m_surface->format->Bmask, m_surface->format->Amask);
		if (m_videoBuffer != NULL) {
			SDL_FillRect(m_videoBuffer, NULL, SDL_MapRGB(m_videoBuffer->format, 0, 0, 0));
		}
	}

	// Recalculate display rect for new window size (needed for overlay size)
	calculateDisplayRect();

	// Recreate YUV overlay at display rect size for high-quality scaling
	if (m_videoWidth > 0 && m_videoHeight > 0 && m_surface != NULL) {
		int overlayW = (m_displayRect.w > 0) ? m_displayRect.w : m_videoWidth;
		int overlayH = (m_displayRect.h > 0) ? m_displayRect.h : m_videoHeight;

		m_yuv = SDL_CreateYUVOverlay(overlayW, overlayH, SDL_IYUV_OVERLAY, m_surface);

		// Initialize overlay to black (Y=0, U=128, V=128) to prevent green flicker
		if (m_yuv != NULL) {
			SDL_LockYUVOverlay(m_yuv);
			memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);           // Y = 0
			memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * (m_yuv->h >> 1));  // U = 128
			memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * (m_yuv->h >> 1));  // V = 128
			SDL_UnlockYUVOverlay(m_yuv);
		}
	}

	// Re-acquire window handle
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	if (SDL_GetWMInfo(&wmInfo) == 1) {
		HWND newHwnd = wmInfo.window;
		// Only re-subclass if window handle changed
		if (newHwnd != m_hwnd) {
			m_hwnd = newHwnd;
			m_originalWndProc = (WNDPROC)SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)WindowProc);
		}
	}

	// Clear to black - video will be redrawn next frame
	clearToBlack();
	SDL_Flip(m_surface);

	m_bResizing = false;
}

// Handle live resize from Windows messages (during drag)
void CSDLPlayer::handleLiveResize(int width, int height)
{
	// Prevent re-entrancy (SDL_SetVideoMode can trigger WM_SIZE)
	if (m_bResizing) {
		return;
	}

	if (width <= 0 || height <= 0) {
		return;
	}

	if (width == m_windowWidth && height == m_windowHeight) {
		return;
	}

	m_bResizing = true;

	m_windowWidth = width;
	m_windowHeight = height;

	CAutoLock oLock(m_mutexVideo, "handleLiveResize");

	// Free old overlay, video buffer and scaler before recreating surface
	if (m_yuv != NULL) {
		SDL_FreeYUVOverlay(m_yuv);
		m_yuv = NULL;
	}
	if (m_videoBuffer != NULL) {
		SDL_FreeSurface(m_videoBuffer);
		m_videoBuffer = NULL;
	}
	freeScaler();  // Free scaler since display rect will change

	// Recreate surface with new size
	m_surface = SDL_SetVideoMode(width, height, 32, SDL_SWSURFACE | SDL_DOUBLEBUF | SDL_RESIZABLE);

	// Recreate video buffer matching new window size
	if (m_surface != NULL) {
		m_videoBuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32,
			m_surface->format->Rmask, m_surface->format->Gmask,
			m_surface->format->Bmask, m_surface->format->Amask);
		if (m_videoBuffer != NULL) {
			SDL_FillRect(m_videoBuffer, NULL, SDL_MapRGB(m_videoBuffer->format, 0, 0, 0));
		}
	}

	// Recalculate display rect for new window size (needed for overlay size)
	calculateDisplayRect();

	// Recreate YUV overlay at display rect size for high-quality scaling
	if (m_videoWidth > 0 && m_videoHeight > 0 && m_surface != NULL) {
		int overlayW = (m_displayRect.w > 0) ? m_displayRect.w : m_videoWidth;
		int overlayH = (m_displayRect.h > 0) ? m_displayRect.h : m_videoHeight;

		m_yuv = SDL_CreateYUVOverlay(overlayW, overlayH, SDL_IYUV_OVERLAY, m_surface);

		// Initialize overlay to black (Y=0, U=128, V=128) to prevent green flicker
		if (m_yuv != NULL) {
			SDL_LockYUVOverlay(m_yuv);
			memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);           // Y = 0
			memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * (m_yuv->h >> 1));  // U = 128
			memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * (m_yuv->h >> 1));  // V = 128
			SDL_UnlockYUVOverlay(m_yuv);
		}
	}

	// Redraw - clear to black, video will be redrawn next frame
	if (m_surface != NULL) {
		SDL_FillRect(m_surface, NULL, SDL_MapRGB(m_surface->format, 0, 0, 0));
		SDL_Flip(m_surface);
	}

	m_bResizing = false;
}

// Custom window procedure for live resize and double-click fullscreen
LRESULT CALLBACK CSDLPlayer::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (s_instance == NULL || s_instance->m_originalWndProc == NULL) {
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	
	switch (msg) {
		case WM_SIZE: {
			// WM_SIZE fires during resize with the new client area size
			if (wParam != SIZE_MINIMIZED) {
				int width = LOWORD(lParam);
				int height = HIWORD(lParam);
				if (width > 0 && height > 0) {
					s_instance->handleLiveResize(width, height);
				}
			}
			break;
		}
		case WM_LBUTTONDBLCLK: {
			// Double-click toggles fullscreen (post event to handle asynchronously)
			s_instance->requestToggleFullscreen();
			return 0;
		}
	}
	
	return CallWindowProc(s_instance->m_originalWndProc, hwnd, msg, wParam, lParam);
}

void CSDLPlayer::toggleFullscreen()
{
	if (m_hwnd == NULL) {
		return;
	}
	
	// Prevent re-entrancy
	if (m_bResizing) {
		return;
	}
	m_bResizing = true;
	
	if (!m_bFullscreen) {
		// Save current window state
		GetWindowRect(m_hwnd, &m_windowedRect);
		m_windowedStyle = GetWindowLong(m_hwnd, GWL_STYLE);
		m_windowedExStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
		
		// Get monitor dimensions for borderless fullscreen
		HMONITOR hMonitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
		if (hMonitor == NULL) {
			m_bResizing = false;
			return;
		}
		
		MONITORINFO mi;
		mi.cbSize = sizeof(MONITORINFO);
		if (!GetMonitorInfo(hMonitor, &mi)) {
			m_bResizing = false;
			return;
		}
		
		int screenWidth = mi.rcMonitor.right - mi.rcMonitor.left;
		int screenHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
		
		// First, modify window style and position
		SetWindowLong(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowLong(m_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
		
		// Move window to cover entire monitor (borderless fullscreen)
		SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, 
			screenWidth, screenHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		
		// Now resize SDL surface to match
		CAutoLock oLock(m_mutexVideo, "toggleFullscreen");
		if (m_yuv != NULL) {
			SDL_FreeYUVOverlay(m_yuv);
			m_yuv = NULL;
		}
		if (m_videoBuffer != NULL) {
			SDL_FreeSurface(m_videoBuffer);
			m_videoBuffer = NULL;
		}
		freeScaler();  // Free scaler since display rect will change

		m_surface = SDL_SetVideoMode(screenWidth, screenHeight, 32, SDL_SWSURFACE | SDL_DOUBLEBUF | SDL_NOFRAME);
		if (m_surface == NULL) {
			// Failed to create surface, restore window state
			SetWindowLong(m_hwnd, GWL_STYLE, m_windowedStyle);
			SetWindowLong(m_hwnd, GWL_EXSTYLE, m_windowedExStyle);
			int width = m_windowedRect.right - m_windowedRect.left;
			int height = m_windowedRect.bottom - m_windowedRect.top;
			if (width <= 0) width = 800;
			if (height <= 0) height = 600;
			SetWindowPos(m_hwnd, HWND_NOTOPMOST, m_windowedRect.left, m_windowedRect.top,
				width, height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			m_bResizing = false;
			return;
		}

		m_windowWidth = screenWidth;
		m_windowHeight = screenHeight;

		// Create video buffer for new screen size
		m_videoBuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, screenWidth, screenHeight, 32,
			m_surface->format->Rmask, m_surface->format->Gmask,
			m_surface->format->Bmask, m_surface->format->Amask);
		if (m_videoBuffer != NULL) {
			SDL_FillRect(m_videoBuffer, NULL, SDL_MapRGB(m_videoBuffer->format, 0, 0, 0));
		}

		// Re-acquire window handle after SDL_SetVideoMode (it may have changed)
		SDL_SysWMinfo wmInfo;
		SDL_VERSION(&wmInfo.version);
		if (SDL_GetWMInfo(&wmInfo) == 1) {
			HWND newHwnd = wmInfo.window;
			if (newHwnd != m_hwnd) {
				// Window handle changed, update and re-subclass
				m_hwnd = newHwnd;
				LONG_PTR classStyle = GetClassLongPtr(m_hwnd, GCL_STYLE);
				SetClassLongPtr(m_hwnd, GCL_STYLE, classStyle | CS_DBLCLKS);
				m_originalWndProc = (WNDPROC)SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)WindowProc);

				// Re-apply borderless style in case SDL reset it
				SetWindowLong(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
				SetWindowLong(m_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
				SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
					screenWidth, screenHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			}
		}

		// Recalculate display rect for fullscreen
		calculateDisplayRect();

		// Create overlay at display rect size for high-quality scaling
		if (m_videoWidth > 0 && m_videoHeight > 0 && m_surface != NULL) {
			int overlayW = (m_displayRect.w > 0) ? m_displayRect.w : m_videoWidth;
			int overlayH = (m_displayRect.h > 0) ? m_displayRect.h : m_videoHeight;

			m_yuv = SDL_CreateYUVOverlay(overlayW, overlayH, SDL_IYUV_OVERLAY, m_surface);
			if (m_yuv != NULL) {
				SDL_LockYUVOverlay(m_yuv);
				memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);
				memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * (m_yuv->h >> 1));
				memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * (m_yuv->h >> 1));
				SDL_UnlockYUVOverlay(m_yuv);
			}
		}

		clearToBlack();
		SDL_Flip(m_surface);

		m_bFullscreen = true;
	}
	else {
		// Restore window style
		SetWindowLong(m_hwnd, GWL_STYLE, m_windowedStyle);
		SetWindowLong(m_hwnd, GWL_EXSTYLE, m_windowedExStyle);

		int width = m_windowedRect.right - m_windowedRect.left;
		int height = m_windowedRect.bottom - m_windowedRect.top;
		if (width <= 0) width = 800;
		if (height <= 0) height = 600;

		SetWindowPos(m_hwnd, HWND_NOTOPMOST,
			m_windowedRect.left, m_windowedRect.top, width, height,
			SWP_FRAMECHANGED | SWP_SHOWWINDOW);

		// Resize SDL surface back
		CAutoLock oLock(m_mutexVideo, "toggleFullscreen");
		if (m_yuv != NULL) {
			SDL_FreeYUVOverlay(m_yuv);
			m_yuv = NULL;
		}
		if (m_videoBuffer != NULL) {
			SDL_FreeSurface(m_videoBuffer);
			m_videoBuffer = NULL;
		}
		freeScaler();  // Free scaler since display rect will change

		m_surface = SDL_SetVideoMode(width, height, 32, SDL_SWSURFACE | SDL_DOUBLEBUF | SDL_RESIZABLE);
		m_windowWidth = width;
		m_windowHeight = height;

		// Create video buffer for restored window size
		if (m_surface != NULL) {
			m_videoBuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32,
				m_surface->format->Rmask, m_surface->format->Gmask,
				m_surface->format->Bmask, m_surface->format->Amask);
			if (m_videoBuffer != NULL) {
				SDL_FillRect(m_videoBuffer, NULL, SDL_MapRGB(m_videoBuffer->format, 0, 0, 0));
			}
		}

		// Re-acquire window handle
		SDL_SysWMinfo wmInfo;
		SDL_VERSION(&wmInfo.version);
		if (SDL_GetWMInfo(&wmInfo) == 1) {
			HWND newHwnd = wmInfo.window;
			if (newHwnd != m_hwnd) {
				m_hwnd = newHwnd;
				LONG_PTR classStyle = GetClassLongPtr(m_hwnd, GCL_STYLE);
				SetClassLongPtr(m_hwnd, GCL_STYLE, classStyle | CS_DBLCLKS);
				m_originalWndProc = (WNDPROC)SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)WindowProc);
			}
		}

		// Recalculate display rect for windowed mode
		calculateDisplayRect();

		// Create overlay at display rect size for high-quality scaling
		if (m_videoWidth > 0 && m_videoHeight > 0 && m_surface != NULL) {
			int overlayW = (m_displayRect.w > 0) ? m_displayRect.w : m_videoWidth;
			int overlayH = (m_displayRect.h > 0) ? m_displayRect.h : m_videoHeight;

			m_yuv = SDL_CreateYUVOverlay(overlayW, overlayH, SDL_IYUV_OVERLAY, m_surface);
			if (m_yuv != NULL) {
				SDL_LockYUVOverlay(m_yuv);
				memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);
				memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * (m_yuv->h >> 1));
				memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * (m_yuv->h >> 1));
				SDL_UnlockYUVOverlay(m_yuv);
			}
		}

		clearToBlack();
		SDL_Flip(m_surface);
		
		m_bFullscreen = false;
	}
	
	m_bResizing = false;
}

void CSDLPlayer::calculateDisplayRect()
{
	if (m_videoWidth <= 0 || m_videoHeight <= 0) {
		// No video yet, center a placeholder rect
		m_displayRect.x = 0;
		m_displayRect.y = 0;
		m_displayRect.w = m_windowWidth;
		m_displayRect.h = m_windowHeight;
		return;
	}

	// Check for exact 1:1 pixel mapping (no scaling needed)
	if (m_windowWidth == m_videoWidth && m_windowHeight == m_videoHeight) {
		// Perfect 1:1 mapping - no interpolation blur
		m_displayRect.x = 0;
		m_displayRect.y = 0;
		m_displayRect.w = m_videoWidth;
		m_displayRect.h = m_videoHeight;
		return;
	}

	// Calculate aspect ratios
	float videoAspect = (float)m_videoWidth / (float)m_videoHeight;
	float windowAspect = (float)m_windowWidth / (float)m_windowHeight;

	int displayWidth, displayHeight;

	if (videoAspect > windowAspect) {
		// Video is wider than window - fit to width (letterbox top/bottom)
		displayWidth = m_windowWidth;
		displayHeight = (int)(m_windowWidth / videoAspect);
	} else {
		// Video is taller than window - fit to height (pillarbox left/right)
		displayHeight = m_windowHeight;
		displayWidth = (int)(m_windowHeight * videoAspect);
	}

	// Center the display rect
	m_displayRect.x = (m_windowWidth - displayWidth) / 2;
	m_displayRect.y = (m_windowHeight - displayHeight) / 2;
	m_displayRect.w = displayWidth;
	m_displayRect.h = displayHeight;
}

void CSDLPlayer::clearToBlack()
{
	if (m_surface != NULL) {
		SDL_FillRect(m_surface, NULL, SDL_MapRGB(m_surface->format, 0, 0, 0));
		SDL_Flip(m_surface);
	}
}

void CSDLPlayer::unInitVideo()
{
	if (NULL != m_surface) {
		SDL_FreeSurface(m_surface);
		m_surface = NULL;
	}

	{
		CAutoLock oLock(m_mutexVideo, "unInitVideo");
		if (NULL != m_videoBuffer) {
			SDL_FreeSurface(m_videoBuffer);
			m_videoBuffer = NULL;
		}
		if (NULL != m_yuv) {
			SDL_FreeYUVOverlay(m_yuv);
			m_yuv = NULL;
		}
		m_videoWidth = 0;
		m_videoHeight = 0;
	}

	unInitAudio();
}

void CSDLPlayer::initAudio(SFgAudioFrame* data)
{
	if ((data->sampleRate != m_sAudioFmt.sampleRate || data->channels != m_sAudioFmt.channels)) {
		unInitAudio();
	}
	if (!m_bAudioInited) {
		SDL_AudioSpec wanted_spec, obtained_spec;
		wanted_spec.freq = data->sampleRate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = data->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = 512;  // Reduced from 1920 to 512 for lower latency (~10.7ms at 48kHz vs 40ms)
		wanted_spec.callback = sdlAudioCallback;
		wanted_spec.userdata = this;

		if (SDL_OpenAudio(&wanted_spec, &obtained_spec) < 0)
		{
			printf("can't open audio.\n");
			return;
		}

		SDL_PauseAudio(1);

		m_sAudioFmt.bitsPerSample = data->bitsPerSample;
		m_sAudioFmt.channels = data->channels;
		m_sAudioFmt.sampleRate = data->sampleRate;
		m_bAudioInited = true;

		if (m_bDumpAudio) {
			m_fileWav = fopen("airplay-audio.wav", "wb");
		}
	}
	if (m_queueAudio.size() > 2) {  // Reduced from 5 to 2 for lower latency (~21ms vs 200ms initial delay)
		SDL_PauseAudio(0);
	}
}

void CSDLPlayer::unInitAudio()
{
	SDL_CloseAudio();
	m_bAudioInited = false;
	memset(&m_sAudioFmt, 0, sizeof(m_sAudioFmt));

	{
		CAutoLock oLock(m_mutexAudio, "unInitAudio");
		while (!m_queueAudio.empty())
		{
			SAudioFrame* pAudioFrame = m_queueAudio.front();
			delete[] pAudioFrame->data;
			delete pAudioFrame;
			m_queueAudio.pop();
		}
	}

	if (m_fileWav != NULL) {
		fclose(m_fileWav);
		m_fileWav = NULL;
	}
}

void CSDLPlayer::sdlAudioCallback(void* userdata, Uint8* stream, int len)
{
	CSDLPlayer* pThis = (CSDLPlayer*)userdata;
	int needLen = len;
	int streamPos = 0;
	memset(stream, 0, len);

	CAutoLock oLock(pThis->m_mutexAudio, "sdlAudioCallback");
	while (!pThis->m_queueAudio.empty())
	{
		SAudioFrame* pAudioFrame = pThis->m_queueAudio.front();
		int pos = pAudioFrame->dataTotal - pAudioFrame->dataLeft;
		int readLen = min(pAudioFrame->dataLeft, needLen);

		//SDL_MixAudio(stream + streamPos, pAudioFrame->data + pos, readLen, 100);
		memcpy(stream + streamPos, pAudioFrame->data + pos, readLen);

		pAudioFrame->dataLeft -= readLen;
		needLen -= readLen;
		streamPos += readLen;
		if (pAudioFrame->dataLeft <= 0) {
			pThis->m_queueAudio.pop();
			delete[] pAudioFrame->data;
			delete pAudioFrame;
		}
		if (needLen <= 0) {
			break;
		}
	}
}

void CSDLPlayer::showWindow()
{
	if (m_hwnd != NULL && !m_bWindowVisible) {
		ShowWindow(m_hwnd, SW_SHOW);
		SetForegroundWindow(m_hwnd);
		m_bWindowVisible = true;
		SDL_WM_SetCaption("AirPlay Server - Connected", NULL);
	}
}

void CSDLPlayer::hideWindow()
{
	if (m_hwnd != NULL && m_bWindowVisible) {
		ShowWindow(m_hwnd, SW_HIDE);
		m_bWindowVisible = false;
	}
	else if (m_hwnd != NULL) {
		// Initial hide
		ShowWindow(m_hwnd, SW_HIDE);
		m_bWindowVisible = false;
	}
}

void CSDLPlayer::requestShowWindow()
{
	SDL_Event evt;
	evt.type = SDL_USEREVENT;
	evt.user.type = SDL_USEREVENT;
	evt.user.code = SHOW_WINDOW_CODE;
	evt.user.data1 = NULL;
	evt.user.data2 = NULL;
	SDL_PushEvent(&evt);
}

void CSDLPlayer::requestHideWindow()
{
	SDL_Event evt;
	evt.type = SDL_USEREVENT;
	evt.user.type = SDL_USEREVENT;
	evt.user.code = HIDE_WINDOW_CODE;
	evt.user.data1 = NULL;
	evt.user.data2 = NULL;
	SDL_PushEvent(&evt);
}

void CSDLPlayer::requestToggleFullscreen()
{
	SDL_Event evt;
	evt.type = SDL_USEREVENT;
	evt.user.type = SDL_USEREVENT;
	evt.user.code = TOGGLE_FULLSCREEN_CODE;
	evt.user.data1 = NULL;
	evt.user.data2 = NULL;
	SDL_PushEvent(&evt);
}

void CSDLPlayer::resizeToVideoSize()
{
	// Resize window to exactly match video resolution for 1:1 pixel mapping (no upscaling blur)
	if (m_videoWidth <= 0 || m_videoHeight <= 0) {
		return;
	}

	if (m_bFullscreen) {
		return;  // Don't resize in fullscreen mode
	}

	if (m_windowWidth == m_videoWidth && m_windowHeight == m_videoHeight) {
		return;  // Already 1:1
	}

	// Resize window to match video exactly
	resizeWindow(m_videoWidth, m_videoHeight);

	// Also update the Windows window size
	if (m_hwnd != NULL) {
		// Calculate window size including borders
		RECT rect = { 0, 0, m_videoWidth, m_videoHeight };
		AdjustWindowRectEx(&rect, GetWindowLong(m_hwnd, GWL_STYLE), FALSE, GetWindowLong(m_hwnd, GWL_EXSTYLE));
		int windowWidth = rect.right - rect.left;
		int windowHeight = rect.bottom - rect.top;

		// Center on screen
		HMONITOR hMonitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
		if (hMonitor != NULL) {
			MONITORINFO mi;
			mi.cbSize = sizeof(MONITORINFO);
			if (GetMonitorInfo(hMonitor, &mi)) {
				int screenWidth = mi.rcWork.right - mi.rcWork.left;
				int screenHeight = mi.rcWork.bottom - mi.rcWork.top;
				int x = mi.rcWork.left + (screenWidth - windowWidth) / 2;
				int y = mi.rcWork.top + (screenHeight - windowHeight) / 2;
				SetWindowPos(m_hwnd, NULL, x, y, windowWidth, windowHeight, SWP_NOZORDER);
			}
		}
	}
}

// Background scaling thread - performs sws_scale off the main thread
// Uses INFINITE wait with proper signaling for minimal latency
DWORD WINAPI CSDLPlayer::ScalingThreadProc(LPVOID param)
{
	CSDLPlayer* pThis = (CSDLPlayer*)param;

	// Set thread to high priority for responsive scaling
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	while (InterlockedCompareExchange(&pThis->m_scalingThreadRunning, 1, 1) == 1) {
		// Wait for signal - use short timeout to stay responsive
		DWORD result = WaitForSingleObject(pThis->m_scalingEvent, 5);  // 5ms max wait

		// Check if we should exit
		if (InterlockedCompareExchange(&pThis->m_scalingThreadRunning, 1, 1) != 1) {
			break;
		}

		// Process all pending frames in a tight loop (drain any backlog)
		while (InterlockedCompareExchange(&pThis->m_srcReady, 0, 1) == 1) {
			// We have source data to scale
			if (pThis->m_swsCtx != NULL && pThis->m_srcYUV[0] != NULL) {
				// Get write buffer index
				LONG writeIdx = InterlockedCompareExchange(&pThis->m_writeBuffer, 0, 0);

				// Perform scaling into back buffer (outside any mutex)
				const uint8_t* srcSlice[3] = { pThis->m_srcYUV[0], pThis->m_srcYUV[1], pThis->m_srcYUV[2] };
				sws_scale(pThis->m_swsCtx, srcSlice, pThis->m_srcPitch, 0, pThis->m_srcHeight,
					pThis->m_scaledYUV[writeIdx], pThis->m_scaledPitch);

				// Swap buffers atomically: write buffer becomes read buffer
				LONG newReadIdx = writeIdx;
				LONG newWriteIdx = 1 - writeIdx;
				InterlockedExchange(&pThis->m_readBuffer, newReadIdx);
				InterlockedExchange(&pThis->m_writeBuffer, newWriteIdx);
				InterlockedExchange(&pThis->m_bufferReady, 1);
			}
		}
	}

	return 0;
}

void CSDLPlayer::initScaler(int srcW, int srcH, int dstW, int dstH)
{
	// Check if scaler needs to be reinitialized (quality preset changed)
	bool forceReinit = (InterlockedCompareExchange(&m_bScalerNeedsReinit, 0, 1) == 1);

	// Check if we need to recreate the scaler
	if (!forceReinit && m_swsCtx != NULL && m_scaledWidth == dstW && m_scaledHeight == dstH &&
		m_srcWidth == srcW && m_srcHeight == srcH) {
		return;  // Already initialized with correct dimensions
	}

	// Free existing scaler
	freeScaler();

	// Determine scaling algorithm based on quality preset
	int scalingAlgorithm;
	EQualityPreset preset = m_imgui.GetQualityPreset();
	switch (preset) {
	case QUALITY_GOOD:
		scalingAlgorithm = SWS_LANCZOS;  // Highest quality - Lanczos resampling
		break;
	case QUALITY_BALANCED:
		scalingAlgorithm = SWS_FAST_BILINEAR;  // Good balance - fast bilinear
		break;
	case QUALITY_FAST:
	default:
		scalingAlgorithm = SWS_POINT;  // Fastest - nearest neighbor
		break;
	}

	// Create new scaler with selected algorithm
	m_swsCtx = sws_getContext(srcW, srcH, AV_PIX_FMT_YUV420P,
		dstW, dstH, AV_PIX_FMT_YUV420P,
		scalingAlgorithm,
		NULL, NULL, NULL);

	if (m_swsCtx == NULL) {
		return;
	}

	m_scaledWidth = dstW;
	m_scaledHeight = dstH;
	m_srcWidth = srcW;
	m_srcHeight = srcH;

	// Allocate scaled YUV buffers with proper alignment (double-buffered)
	m_scaledPitch[0] = ((dstW + 31) >> 5) << 5;        // Y pitch (32-byte aligned for AVX)
	m_scaledPitch[1] = ((dstW / 2 + 31) >> 5) << 5;    // U pitch
	m_scaledPitch[2] = m_scaledPitch[1];               // V pitch

	int ySize = m_scaledPitch[0] * dstH;
	int uvSize = m_scaledPitch[1] * (dstH / 2);

	// Allocate both buffers for double-buffering
	for (int i = 0; i < 2; i++) {
		m_scaledYUV[i][0] = (uint8_t*)_aligned_malloc(ySize, 32);
		m_scaledYUV[i][1] = (uint8_t*)_aligned_malloc(uvSize, 32);
		m_scaledYUV[i][2] = (uint8_t*)_aligned_malloc(uvSize, 32);

		// Initialize to black (Y=0, U=128, V=128)
		if (m_scaledYUV[i][0]) memset(m_scaledYUV[i][0], 0, ySize);
		if (m_scaledYUV[i][1]) memset(m_scaledYUV[i][1], 128, uvSize);
		if (m_scaledYUV[i][2]) memset(m_scaledYUV[i][2], 128, uvSize);
	}

	// Initialize buffer indices
	m_writeBuffer = 0;
	m_readBuffer = 0;
	m_bufferReady = 0;
	m_bScalerNeedsReinit = 0;
}

void CSDLPlayer::freeScaler()
{
	if (m_swsCtx != NULL) {
		sws_freeContext(m_swsCtx);
		m_swsCtx = NULL;
	}

	// Free double-buffered scaled YUV planes
	for (int i = 0; i < 2; i++) {
		if (m_scaledYUV[i][0] != NULL) {
			_aligned_free(m_scaledYUV[i][0]);
			m_scaledYUV[i][0] = NULL;
		}
		if (m_scaledYUV[i][1] != NULL) {
			_aligned_free(m_scaledYUV[i][1]);
			m_scaledYUV[i][1] = NULL;
		}
		if (m_scaledYUV[i][2] != NULL) {
			_aligned_free(m_scaledYUV[i][2]);
			m_scaledYUV[i][2] = NULL;
		}
	}

	m_scaledWidth = 0;
	m_scaledHeight = 0;
	m_srcWidth = 0;
	m_srcHeight = 0;
	m_scaledPitch[0] = 0;
	m_scaledPitch[1] = 0;
	m_scaledPitch[2] = 0;
}
