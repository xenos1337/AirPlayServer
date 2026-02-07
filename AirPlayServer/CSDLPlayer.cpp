#include "CSDLPlayer.h"
#include <stdio.h>
#include <malloc.h>  // For _aligned_malloc/_aligned_free
#include <math.h>    // For powf() in volume conversion
#include "CAutoLock.h"

// Windows Core Audio API for querying system audio device format
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

// Link against required libraries
#pragma comment(lib, "ole32.lib")

// Static instance pointer for window procedure callback
CSDLPlayer* CSDLPlayer::s_instance = NULL;

// Query the Windows default audio device's sample rate using WASAPI
// Returns 0 on failure, otherwise the sample rate (e.g., 44100, 48000, 96000)
static DWORD GetSystemAudioSampleRate()
{
	DWORD sampleRate = 0;
	HRESULT hr;

	// Initialize COM (required for WASAPI)
	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	bool comInitialized = SUCCEEDED(hr) || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;

	if (!comInitialized) {
		return 0;
	}

	IMMDeviceEnumerator* pEnumerator = NULL;
	IMMDevice* pDevice = NULL;
	IAudioClient* pAudioClient = NULL;
	WAVEFORMATEX* pwfx = NULL;

	// Create device enumerator
	hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

	if (FAILED(hr)) {
		goto cleanup;
	}

	// Get default audio endpoint (speakers)
	hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
	if (FAILED(hr)) {
		goto cleanup;
	}

	// Activate audio client
	hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
	if (FAILED(hr)) {
		goto cleanup;
	}

	// Get the device's mix format (native format)
	hr = pAudioClient->GetMixFormat(&pwfx);
	if (FAILED(hr)) {
		goto cleanup;
	}

	sampleRate = pwfx->nSamplesPerSec;

cleanup:
	if (pwfx) CoTaskMemFree(pwfx);
	if (pAudioClient) pAudioClient->Release();
	if (pDevice) pDevice->Release();
	if (pEnumerator) pEnumerator->Release();

	// Only uninitialize COM if we successfully initialized it (not if it was already initialized)
	if (hr != RPC_E_CHANGED_MODE) {
		CoUninitialize();
	}

	return sampleRate;
}

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
	, m_pendingResizeWidth(800)
	, m_pendingResizeHeight(600)
	, m_bFullscreen(false)
	, m_windowedRect()
	, m_windowedStyle(0)
	, m_windowedExStyle(0)
	, m_lastMouseMoveTime(0)
	, m_bCursorHidden(false)
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
	m_audioVolume = SDL_MIX_MAXVOLUME / 2;  // Half volume by default

	// Initialize audio quality tracking
	m_audioUnderrunCount = 0;
	m_audioDroppedFrames = 0;
	m_audioFadeOut = false;
	m_audioFadeOutSamples = 0;

	// Initialize audio resampling
	m_systemSampleRate = 0;
	m_streamSampleRate = 0;
	m_resampleBuffer = NULL;
	m_resampleBufferSize = 0;
	m_resamplePos = 0.0;
	m_needsResampling = false;

	// Initialize dynamic limiter
	m_limiterGain = 1.0f;
	m_peakLevel = 0.0f;
	m_deviceVolumeNormalized = 0.5f;
	m_autoAdjustEnabled = false;

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

	// Initialize deferred cleanup pointers
	m_pendingFreeCtx = NULL;
	m_pendingFreeYUV[0][0] = NULL;
	m_pendingFreeYUV[0][1] = NULL;
	m_pendingFreeYUV[0][2] = NULL;
	m_pendingFreeYUV[1][0] = NULL;
	m_pendingFreeYUV[1][1] = NULL;
	m_pendingFreeYUV[1][2] = NULL;
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

	// RGB converter members (no longer used - scaling thread outputs BGRA directly)
	m_rgbConvertCtx = NULL;
	m_rgbConvertWidth = 0;
	m_rgbConvertHeight = 0;
}

CSDLPlayer::~CSDLPlayer()
{
	// Restore original window procedure before cleanup
	if (m_hwnd != NULL && m_originalWndProc != NULL) {
		SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)m_originalWndProc);
	}

	// Stop scaling thread before freeing resources
	if (m_scalingThread != NULL) {
		InterlockedExchange(&m_scalingThreadRunning, 0);
		if (m_scalingEvent != NULL) {
			SetEvent(m_scalingEvent);  // Wake thread so it can exit
		}
		WaitForSingleObject(m_scalingThread, 2000);
		CloseHandle(m_scalingThread);
		m_scalingThread = NULL;
	}
	if (m_scalingEvent != NULL) {
		CloseHandle(m_scalingEvent);
		m_scalingEvent = NULL;
	}

	freeScaler();

	// Free any pending resources (safe now since server is stopped and no more callbacks)
	freePendingScalerResources();

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

		// Disable SDL 1.2's custom cursor - we handle cursor via WM_SETCURSOR in WindowProc
		SDL_ShowCursor(SDL_DISABLE);

		// Subclass the window for live resize updates and double-click fullscreen
		m_originalWndProc = (WNDPROC)SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)WindowProc);
	}

	// Initialize ImGui
	if (!m_imgui.Init(m_surface)) {
		printf("Failed to initialize ImGui\n");
		// Continue anyway
	}

	// Load persisted settings
	{
		char settingsPath[MAX_PATH] = { 0 };
		GetModuleFileNameA(NULL, settingsPath, MAX_PATH);
		// Replace exe filename with ini filename
		char* lastSlash = strrchr(settingsPath, '\\');
		if (lastSlash) {
			strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - settingsPath), "airplay_settings.ini");
		} else {
			strcpy_s(settingsPath, MAX_PATH, "airplay_settings.ini");
		}
		m_imgui.LoadSettings(settingsPath);

		// Override server name if saved device name exists
		const char* savedName = m_imgui.GetDeviceName();
		if (savedName != NULL && strlen(savedName) > 0) {
			setServerName(savedName);
		}
	}

	// Start background scaling thread
	m_scalingEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  // auto-reset event
	m_scalingThreadRunning = 1;
	m_scalingThread = CreateThread(NULL, 0, ScalingThreadProc, this, 0, NULL);

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

	// Initialize cursor hide timer
	m_lastMouseMoveTime = GetTickCount();
	
	/* Main loop - poll events and render ImGui */
	while (!bEndLoop) {
		// Record frame start time
		frameStartTime = GetTickCount();
		
		// Process all pending events
		while (SDL_PollEvent(&event)) {
			// Process ImGui events first
			m_imgui.ProcessEvent(&event);
			
			// Track mouse movement for cursor auto-hide
			if (event.type == SDL_MOUSEMOTION) {
				m_lastMouseMoveTime = GetTickCount();
				if (m_bCursorHidden) {
					m_bCursorHidden = false;
					// Force WM_SETCURSOR to fire and show the arrow cursor
					SetCursor(LoadCursor(NULL, IDC_ARROW));
				}
			}

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

					// IMPORTANT: Set resize flag FIRST to prevent outputVideo from creating
					// new scalers during the entire resize operation
					m_bResizing = true;

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
					// We do this AFTER the current resize operation completes to avoid race conditions
					// The resize will happen via a deferred call below
					bool needsWindowResize = (m_b1to1PixelMode && !m_bFullscreen);

					// Calculate display rect first (needed for overlay size)
					calculateDisplayRect();

					// Now create overlay at display rect size for high-quality scaling
					// When window matches video (1:1), displayRect matches video size
					// When window differs, displayRect is the scaled size
					{
						CAutoLock oLock(m_mutexVideo, "createOverlay");
						int overlayW = (m_displayRect.w > 0) ? m_displayRect.w : m_videoWidth;
						int overlayH = (m_displayRect.h > 0) ? m_displayRect.h : m_videoHeight;
						// YUV420 requires even dimensions to avoid chroma drift
						overlayW = overlayW & ~1;
						overlayH = overlayH & ~1;

						m_yuv = SDL_CreateYUVOverlay(overlayW, overlayH, SDL_IYUV_OVERLAY, m_surface);

						// Create off-screen video buffer matching window size
						if (m_surface != NULL) {
							m_videoBuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, m_windowWidth, m_windowHeight, 32,
								m_surface->format->Rmask, m_surface->format->Gmask,
								m_surface->format->Bmask, m_surface->format->Amask);
							if (m_videoBuffer != NULL) {
								SDL_SetAlpha(m_videoBuffer, 0, 0);  // Disable alpha blending for direct blit
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

					// Now it's safe for outputVideo to create scalers again
					m_bResizing = false;

					// Now resize window if needed (AFTER m_bResizing is cleared)
					// This prevents the race condition where outputVideo creates a scaler
					// that gets freed by resizeWindow
					if (needsWindowResize) {
						resizeToVideoSize();
					}
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
			} else {
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

			// Save settings before shutdown
			{
				char settingsPath[MAX_PATH] = { 0 };
				GetModuleFileNameA(NULL, settingsPath, MAX_PATH);
				char* lastSlash = strrchr(settingsPath, '\\');
				if (lastSlash) {
					strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - settingsPath), "airplay_settings.ini");
				} else {
					strcpy_s(settingsPath, MAX_PATH, "airplay_settings.ini");
				}
				m_imgui.SaveSettings(settingsPath);
			}

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

		// Handle pending resize from WM_SIZE (deferred to avoid reentrancy)
		int pendingW = m_pendingResizeWidth;
		int pendingH = m_pendingResizeHeight;
		// Only log when there's a pending resize that differs from current
		if (pendingW >= 100 && pendingH >= 100 &&
			(pendingW != m_windowWidth || pendingH != m_windowHeight)) {
			handleLiveResize(pendingW, pendingH);
		}

		// Handle disconnect transition (show black screen briefly)
		if (m_bDisconnecting) {
			DWORD elapsed = GetTickCount() - m_dwDisconnectStartTime;
			if (elapsed >= 800) {  // Show black screen for 800ms
				m_bDisconnecting = false;
			}
		}
		
		// MAIN THREAD: Consume BGRA frames from scaling thread, blit to surface
		// Step 1: When scaling thread produces a new frame, copy BGRA to m_videoBuffer
		// m_videoBuffer is a persistent off-screen surface that holds the last displayed frame
		if (InterlockedCompareExchange(&m_bufferReady, 0, 1) == 1) {
			LONG readIdx = InterlockedCompareExchange(&m_readBuffer, 0, 0);
			if (readIdx >= 0 && readIdx <= 1 &&
				m_scaledYUV[readIdx][0] != NULL &&
				m_scaledWidth > 0 && m_scaledHeight > 0 &&
				m_videoBuffer != NULL) {

				// Copy BGRA from double buffer to m_videoBuffer at displayRect position
				// m_videoBuffer persists between frames (survives SDL_Flip's buffer swap)

				// Clear entire video buffer to black (for letterbox areas)
				SDL_FillRect(m_videoBuffer, NULL, SDL_MapRGB(m_videoBuffer->format, 0, 0, 0));

				// Lock for direct pixel access, then copy BGRA rows
				if (SDL_MUSTLOCK(m_videoBuffer)) SDL_LockSurface(m_videoBuffer);

				int bpp = m_videoBuffer->format->BytesPerPixel;
				uint8_t* dstBase = (uint8_t*)m_videoBuffer->pixels
					+ m_displayRect.y * m_videoBuffer->pitch
					+ m_displayRect.x * bpp;
				const uint8_t* srcBase = m_scaledYUV[readIdx][0];
				int copyBytes = m_scaledWidth * bpp;

				for (int row = 0; row < m_scaledHeight; row++) {
					memcpy(dstBase, srcBase, copyBytes);
					dstBase += m_videoBuffer->pitch;
					srcBase += m_scaledPitch[0];
				}

				if (SDL_MUSTLOCK(m_videoBuffer)) SDL_UnlockSurface(m_videoBuffer);
				m_hasNewFrame = true;
				m_lastFrameTime = GetTickCount();
			}
		}

		// Step 2: Blit m_videoBuffer to m_surface (fast, no YUV conversion, no SDL_UpdateRects)
		// This is flicker-free because only SDL_Flip updates the screen
		if (m_surface != NULL) {
			if (m_videoBuffer != NULL && m_videoWidth > 0) {
				// Blit the persistent video buffer onto the screen's back buffer
				SDL_BlitSurface(m_videoBuffer, NULL, m_surface, NULL);
			} else {
				// No video - fill with black
				SDL_FillRect(m_surface, NULL, SDL_MapRGB(m_surface->format, 0, 0, 0));
			}
		}

		// Step 3: Render ImGui on top of m_surface
		if (m_surface != NULL) {
			m_imgui.WasUIJustHidden();  // Consume the flag to keep state clean

			m_imgui.NewFrame(m_surface);
			if (m_bConnected) {
				// Connected - show overlay with controls and video statistics
				m_imgui.RenderOverlay(&bShowUI, m_serverName, m_bConnected, m_connectedDeviceName,
					m_videoWidth, m_videoHeight, m_currentFPS, m_currentBitrateMbps,
					m_totalFrames, m_droppedFrames, m_totalBytes);
			} else if (m_bDisconnecting) {
				// Disconnecting - show disconnect message instead of blank screen
				m_imgui.RenderDisconnectMessage(m_connectedDeviceName);
			} else {
				// Disconnected - show home screen
				m_imgui.RenderHomeScreen(m_serverName, m_bConnected, m_connectedDeviceName, m_server.isRunning());
			}
			// ImGui renders with alpha blending on top of the video layer
			m_imgui.Render(m_surface);

			// Step 4: Flip to display everything (ONLY screen update per frame)
			SDL_Flip(m_surface);
		}
		
		// Auto-hide cursor after 5 seconds of inactivity
		if (!m_bCursorHidden && m_lastMouseMoveTime > 0) {
			DWORD elapsed = GetTickCount() - m_lastMouseMoveTime;
			if (elapsed >= CURSOR_HIDE_DELAY_MS) {
				m_bCursorHidden = true;
				// Force WM_SETCURSOR to fire and hide the cursor
				SetCursor(NULL);
			}
		}

		// Sync audio settings between player and UI
		m_autoAdjustEnabled = m_imgui.IsAutoAdjustEnabled();
		m_imgui.SetDeviceVolume(m_deviceVolumeNormalized);  // Send device volume to UI
		m_imgui.SetCurrentAudioLevel(m_peakLevel);

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

	// Skip video processing during resize to prevent race conditions
	if (m_bResizing) {
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

	// CALLBACK THREAD: Copy YUV source to staging buffer, signal scaling thread
	// The scaling thread converts YUV->BGRA in the background

	// Diagnostic: log first few frames and every 100th to confirm arrival
	static int dbgFrameCount = 0;
	dbgFrameCount++;
	if (dbgFrameCount <= 3 || dbgFrameCount % 100 == 0) {
		char dbg[256];
		sprintf_s(dbg, "outputVideo: frame #%d, %ux%u, displayRect=%dx%d, bResizing=%d\n",
			dbgFrameCount, data->width, data->height,
			m_displayRect.w, m_displayRect.h, (int)m_bResizing);
		OutputDebugStringA(dbg);
	}

	// Get source YUV plane pointers
	const uint8_t* srcY = data->data;
	const uint8_t* srcU = data->data + data->dataLen[0];
	const uint8_t* srcV = data->data + data->dataLen[0] + data->dataLen[1];

	// SCALING/COPY PATH - under mutex to prevent race with resize
	{
		CAutoLock oLock(m_mutexVideo, "outputVideo");

		// Double-check resize flag inside mutex (may have changed since pre-mutex check)
		if (m_bResizing) {
			return;
		}

		// Free any pending scaler resources (safe inside mutex - no race with freeScaler)
		freePendingScalerResources();

		// Read display rect inside mutex for thread safety
		int dstW = m_displayRect.w;
		int dstH = m_displayRect.h;

		// No even-dimension constraint needed: output is BGRA (packed RGB), not YUV420P.
		// The input YUV420P source dimensions are handled by sws_scale internally.

		// UNIFIED PATH: All frames go through the scaling thread
		// The scaling thread handles both rescaling AND colorspace conversion (YUV->BGRA)
		// Even for 1:1 mode, this converts YUV to BGRA for flicker-free surface blit
		if (dstW > 0 && dstH > 0) {
			initScaler(data->width, data->height, dstW, dstH);

			if (m_swsCtx != NULL &&
				m_srcYUV[0] != NULL && m_srcYUV[1] != NULL && m_srcYUV[2] != NULL &&
				srcY != NULL && srcU != NULL && srcV != NULL &&
				data->pitch[0] > 0 && data->pitch[1] > 0 && data->pitch[2] > 0 &&
				m_scaledWidth == dstW && m_scaledHeight == dstH) {

				// Drop frame if scaling thread hasn't consumed the previous one
				if (InterlockedCompareExchange(&m_srcReady, 0, 0) != 0) {
					m_droppedFrames++;
				} else {
				// Fast memcpy of source planes into staging buffer (~1ms)
				const int yHeight = data->height;
				const int uvHeight = (data->height + 1) / 2;
				const int yBytesToCopy = (int)data->width < m_srcPitch[0] ? (int)data->width : m_srcPitch[0];
				const int uvLogicalWidth = ((int)data->width + 1) / 2;
				const int uvBytesToCopy = uvLogicalWidth < m_srcPitch[1] ? uvLogicalWidth : m_srcPitch[1];

				// Y plane copy
				{
					const uint8_t* sp = srcY;
					uint8_t* dp = m_srcYUV[0];
					for (int row = 0; row < yHeight; row++) {
						memcpy(dp, sp, yBytesToCopy);
						sp += data->pitch[0];
						dp += m_srcPitch[0];
					}
				}
				// U plane copy
				{
					const uint8_t* sp = srcU;
					uint8_t* dp = m_srcYUV[1];
					for (int row = 0; row < uvHeight; row++) {
						memcpy(dp, sp, uvBytesToCopy);
						sp += data->pitch[1];
						dp += m_srcPitch[1];
					}
				}
				// V plane copy
				{
					const uint8_t* sp = srcV;
					uint8_t* dp = m_srcYUV[2];
					for (int row = 0; row < uvHeight; row++) {
						memcpy(dp, sp, uvBytesToCopy);
						sp += data->pitch[2];
						dp += m_srcPitch[2];
					}
				}

				m_lastFramePTS = data->pts;

				// Signal scaling thread to process this frame
				InterlockedExchange(&m_srcReady, 1);
				if (m_scalingEvent != NULL) {
					SetEvent(m_scalingEvent);
				}
				} // end of else (frame not dropped)
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
	dataClone->pts = data->pts;

	// Resample audio if needed (stream rate differs from system rate)
	if (m_needsResampling && m_resampleBuffer && m_streamSampleRate > 0) {
		// Linear interpolation resampling
		int bytesPerSample = 2;  // 16-bit
		int channels = data->channels;
		int inSamples = data->dataLen / (channels * bytesPerSample);

		// Calculate output samples based on sample rate ratio
		double ratio = (double)m_systemSampleRate / (double)m_streamSampleRate;
		int outSamples = (int)(inSamples * ratio + 0.5);

		// Ensure we have enough buffer space
		int outBufferSize = outSamples * channels * bytesPerSample;
		if (outBufferSize > m_resampleBufferSize) {
			m_resampleBuffer = (uint8_t*)realloc(m_resampleBuffer, outBufferSize);
			m_resampleBufferSize = outBufferSize;
		}

		// Perform linear interpolation resampling
		Sint16* inPtr = (Sint16*)data->data;
		Sint16* outPtr = (Sint16*)m_resampleBuffer;
		double step = 1.0 / ratio;  // Input step per output sample

		for (int i = 0; i < outSamples; i++) {
			double srcPos = i * step;
			int srcIdx = (int)srcPos;
			double frac = srcPos - srcIdx;

			// Ensure we don't read past the end
			if (srcIdx >= inSamples - 1) {
				srcIdx = inSamples - 2;
				if (srcIdx < 0) srcIdx = 0;
				frac = 1.0;
			}

			// Linear interpolation for each channel
			for (int ch = 0; ch < channels; ch++) {
				Sint16 s1 = inPtr[srcIdx * channels + ch];
				Sint16 s2 = (srcIdx + 1 < inSamples) ? inPtr[(srcIdx + 1) * channels + ch] : s1;
				outPtr[i * channels + ch] = (Sint16)(s1 + (s2 - s1) * frac);
			}
		}

		int convertedBytes = outSamples * channels * bytesPerSample;
		dataClone->dataTotal = convertedBytes;
		dataClone->dataLeft = convertedBytes;
		dataClone->data = new uint8_t[convertedBytes];
		memcpy(dataClone->data, m_resampleBuffer, convertedBytes);
	} else {
		// No resampling needed, copy directly
		dataClone->dataTotal = data->dataLen;
		dataClone->dataLeft = data->dataLen;
		dataClone->data = new uint8_t[data->dataLen];
		memcpy(dataClone->data, data->data, data->dataLen);
	}

	{
		CAutoLock oLock(m_mutexAudio, "outputAudio");

		// AUDIO QUEUE LIMITING: Prevent unbounded growth causing delay accumulation
		// Use larger queue (20 frames ~400ms) for smoother playback with less dropping
		while (m_queueAudio.size() >= AUDIO_QUEUE_MAX_FRAMES) {
			SAudioFrame* oldFrame = m_queueAudio.front();
			m_queueAudio.pop();
			delete[] oldFrame->data;
			delete oldFrame;
			m_audioDroppedFrames++;
		}

		m_queueAudio.push(dataClone);

		// Reset fade-out state when we have audio data
		m_audioFadeOut = false;
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

	// Acquire mutex FIRST before modifying any shared state
	// This prevents race conditions with the callback thread
	CAutoLock oLock(m_mutexVideo, "resizeWindow");

	// NOW set the resizing flag (inside mutex to ensure callback sees consistent state)
	m_bResizing = true;

	// Update dimensions inside mutex
	m_windowWidth = width;
	m_windowHeight = height;
	
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
			SDL_SetAlpha(m_videoBuffer, 0, 0);  // Disable alpha blending for direct blit
			SDL_FillRect(m_videoBuffer, NULL, SDL_MapRGB(m_videoBuffer->format, 0, 0, 0));
		}
	}

	// Recalculate display rect for new window size (needed for overlay size)
	calculateDisplayRect();

	// Recreate YUV overlay at display rect size for high-quality scaling
	if (m_videoWidth > 0 && m_videoHeight > 0 && m_surface != NULL) {
		int overlayW = (m_displayRect.w > 0) ? m_displayRect.w : m_videoWidth;
		int overlayH = (m_displayRect.h > 0) ? m_displayRect.h : m_videoHeight;
		// YUV420 requires even dimensions to avoid chroma drift
		overlayW = overlayW & ~1;
		overlayH = overlayH & ~1;

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

	// Early validation (don't need mutex for this)
	if (width <= 0 || height <= 0) {
		return;
	}

	// Acquire mutex FIRST before checking or modifying any shared state
	CAutoLock oLock(m_mutexVideo, "handleLiveResize");

	// Check inside mutex to ensure consistent state
	if (width == m_windowWidth && height == m_windowHeight) {
		return;
	}

	// NOW set the resizing flag (inside mutex)
	m_bResizing = true;

	// Update dimensions inside mutex
	m_windowWidth = width;
	m_windowHeight = height;

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

	// If surface creation failed, abort resize
	if (m_surface == NULL) {
		m_bResizing = false;
		return;
	}

	// Recreate video buffer matching new window size
	m_videoBuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32,
		m_surface->format->Rmask, m_surface->format->Gmask,
		m_surface->format->Bmask, m_surface->format->Amask);
	if (m_videoBuffer != NULL) {
		SDL_SetAlpha(m_videoBuffer, 0, 0);  // Disable alpha blending for direct blit
		SDL_FillRect(m_videoBuffer, NULL, SDL_MapRGB(m_videoBuffer->format, 0, 0, 0));
	}

	// Recalculate display rect for new window size (needed for overlay size)
	calculateDisplayRect();

	// Recreate YUV overlay at display rect size for high-quality scaling
	if (m_videoWidth > 0 && m_videoHeight > 0) {
		int overlayW = (m_displayRect.w > 0) ? m_displayRect.w : m_videoWidth;
		int overlayH = (m_displayRect.h > 0) ? m_displayRect.h : m_videoHeight;
		// YUV420 requires even dimensions to avoid chroma drift
		overlayW = overlayW & ~1;
		overlayH = overlayH & ~1;

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

	// Re-acquire window handle after SDL_SetVideoMode (it may create a new window)
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
	if (s_instance == NULL) {
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	// Safety check: if this is for an old window handle, use default proc
	if (hwnd != s_instance->m_hwnd || s_instance->m_originalWndProc == NULL) {
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	switch (msg) {
		case WM_SIZE: {
			// WM_SIZE fires during resize with the new client area size
			// Post event to handle asynchronously to avoid reentrancy issues during modal resize
			if (wParam != SIZE_MINIMIZED) {
				int width = LOWORD(lParam);
				int height = HIWORD(lParam);
				// Minimum size check to prevent issues with tiny windows
				if (width >= 100 && height >= 100) {
					s_instance->requestResize(width, height);
				}
			}
			break;
		}
		case WM_LBUTTONDBLCLK: {
			// Double-click toggles fullscreen (post event to handle asynchronously)
			s_instance->requestToggleFullscreen();
			return 0;
		}
		case WM_SETCURSOR: {
			// Override SDL 1.2's cursor handling - it sets an invisible cursor when SDL_ShowCursor is disabled
			if (LOWORD(lParam) == HTCLIENT) {
				if (s_instance->m_bCursorHidden) {
					SetCursor(NULL);  // Hide cursor in client area
				} else {
					SetCursor(LoadCursor(NULL, IDC_ARROW));  // Standard Windows arrow
				}
				return TRUE;  // Prevent SDL from overriding our cursor
			}
			break;
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
			SDL_SetAlpha(m_videoBuffer, 0, 0);  // Disable alpha blending for direct blit
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
			// YUV420 requires even dimensions to avoid chroma drift
			overlayW = overlayW & ~1;
			overlayH = overlayH & ~1;

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
		int posX = m_windowedRect.left;
		int posY = m_windowedRect.top;

		// Clamp restored window to 80% of screen (high-res sources can exceed screen size)
		HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
		if (hMon != NULL) {
			MONITORINFO mi;
			mi.cbSize = sizeof(MONITORINFO);
			if (GetMonitorInfo(hMon, &mi)) {
				int workW = mi.rcWork.right - mi.rcWork.left;
				int workH = mi.rcWork.bottom - mi.rcWork.top;
				int maxW = (int)(workW * 0.80f);
				int maxH = (int)(workH * 0.80f);
				if (width > maxW || height > maxH) {
					float aspect = (float)width / (float)height;
					float maxAspect = (float)maxW / (float)maxH;
					if (aspect > maxAspect) {
						width = maxW;
						height = (int)(maxW / aspect);
					} else {
						height = maxH;
						width = (int)(maxH * aspect);
					}
				}
				// Center on screen
				posX = mi.rcWork.left + (workW - width) / 2;
				posY = mi.rcWork.top + (workH - height) / 2;
			}
		}

		SetWindowPos(m_hwnd, HWND_NOTOPMOST,
			posX, posY, width, height,
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
				SDL_SetAlpha(m_videoBuffer, 0, 0);  // Disable alpha blending for direct blit
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
			// YUV420 requires even dimensions to avoid chroma drift
			overlayW = overlayW & ~1;
			overlayH = overlayH & ~1;

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
		// No video yet, fill the entire window
		m_displayRect.x = 0;
		m_displayRect.y = 0;
		m_displayRect.w = m_windowWidth;
		m_displayRect.h = m_windowHeight;
		return;
	}

	// Use cross-multiplication to compare aspect ratios without float rounding:
	// videoAspect > windowAspect  iff  videoW * windowH > windowW * videoH
	long long videoCross = (long long)m_videoWidth * m_windowHeight;
	long long windowCross = (long long)m_windowWidth * m_videoHeight;

	// If the aspect ratios are very close (within 1%), just fill the entire window.
	// This avoids thin black bars from integer rounding when the window was sized to match the video.
	long long diff = videoCross - windowCross;
	if (diff < 0) diff = -diff;
	long long threshold = windowCross / 100;  // 1% tolerance
	if (diff <= threshold) {
		m_displayRect.x = 0;
		m_displayRect.y = 0;
		m_displayRect.w = m_windowWidth;
		m_displayRect.h = m_windowHeight;
		return;
	}

	int displayWidth, displayHeight;

	if (videoCross > windowCross) {
		// Video is wider than window - fit to width (letterbox top/bottom)
		displayWidth = m_windowWidth;
		// Round to nearest: (windowW * videoH + videoW/2) / videoW
		displayHeight = (int)(((long long)m_windowWidth * m_videoHeight + m_videoWidth / 2) / m_videoWidth);
	} else {
		// Video is taller than window - fit to height (pillarbox left/right)
		displayHeight = m_windowHeight;
		// Round to nearest: (windowH * videoW + videoH/2) / videoH
		displayWidth = (int)(((long long)m_windowHeight * m_videoWidth + m_videoHeight / 2) / m_videoHeight);
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
		// Query system audio device sample rate (only once)
		if (m_systemSampleRate == 0) {
			m_systemSampleRate = GetSystemAudioSampleRate();
			if (m_systemSampleRate == 0) {
				// Fallback to 48kHz if query fails
				m_systemSampleRate = 48000;
			}
		}

		m_streamSampleRate = data->sampleRate;
		DWORD outputSampleRate = m_systemSampleRate;


		// Set up audio resampler if stream rate differs from system rate
		if (m_streamSampleRate != m_systemSampleRate) {

			m_needsResampling = true;
			m_resamplePos = 0.0;

			// Allocate resample buffer (enough for 1 second of audio)
			m_resampleBufferSize = m_systemSampleRate * data->channels * 2;  // 16-bit samples
			m_resampleBuffer = (uint8_t*)malloc(m_resampleBufferSize);

			if (!m_resampleBuffer) {
				m_needsResampling = false;
				outputSampleRate = m_streamSampleRate;  // Fallback
			}
		}

		SDL_AudioSpec wanted_spec, obtained_spec;
		wanted_spec.freq = outputSampleRate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = data->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = AUDIO_BUFFER_SAMPLES;  // 1024 samples - balanced latency/quality
		wanted_spec.callback = sdlAudioCallback;
		wanted_spec.userdata = this;

		if (SDL_OpenAudio(&wanted_spec, &obtained_spec) < 0)
		{
			printf("Cannot open audio: %s\n", SDL_GetError());
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
	if (m_queueAudio.size() >= AUDIO_QUEUE_START_THRESHOLD) {  // Wait for enough frames before starting
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

	// Reset audio quality tracking
	m_audioUnderrunCount = 0;
	m_audioDroppedFrames = 0;
	m_audioFadeOut = false;
	m_audioFadeOutSamples = 0;

	// Clean up audio resampler
	if (!m_resampleBuffer) {
		free(m_resampleBuffer);
		m_resampleBuffer = NULL;
		m_resampleBufferSize = 0;
	}
	m_needsResampling = false;
	m_resamplePos = 0.0;
	m_streamSampleRate = 0;
	// Note: m_systemSampleRate is intentionally NOT reset - it's cached for the session
}

void CSDLPlayer::sdlAudioCallback(void* userdata, Uint8* stream, int len)
{
	CSDLPlayer* pThis = (CSDLPlayer*)userdata;
	int needLen = len;
	int streamPos = 0;

	// Initialize output buffer to silence
	memset(stream, 0, len);

	// Update normalized device volume for UI display
	pThis->m_deviceVolumeNormalized = (float)pThis->m_audioVolume / (float)SDL_MIX_MAXVOLUME;

	CAutoLock oLock(pThis->m_mutexAudio, "sdlAudioCallback");

	// Check for underrun condition
	if (pThis->m_queueAudio.empty()) {
		pThis->m_audioUnderrunCount++;
		// Signal fade-out for next time we get data (prevents pop when resuming)
		pThis->m_audioFadeOut = true;
		pThis->m_audioFadeOutSamples = 256;  // ~5ms fade at 48kHz stereo
		// Decay peak level when no audio
		pThis->m_peakLevel *= 0.95f;
		return;  // Output silence
	}

	// Track peak level for this buffer
	float bufferPeak = 0.0f;

	// Process audio frames from queue
	while (!pThis->m_queueAudio.empty() && needLen > 0)
	{
		SAudioFrame* pAudioFrame = pThis->m_queueAudio.front();
		int pos = pAudioFrame->dataTotal - pAudioFrame->dataLeft;
		int readLen = min((int)pAudioFrame->dataLeft, needLen);

		// Get source and destination pointers
		Sint16* src = (Sint16*)(pAudioFrame->data + pos);
		Sint16* dst = (Sint16*)(stream + streamPos);
		int numSamples = readLen / 2;  // 16-bit samples

		// Simple volume scaling using integer math (avoids float conversion crackling)
		int volume = pThis->m_audioVolume;
		for (int i = 0; i < numSamples; i++) {
			// Track peak level from INPUT signal (before volume scaling)
			int absSrc = (src[i] < 0) ? -src[i] : src[i];
			float level = absSrc / 32768.0f;
			if (level > bufferPeak) {
				bufferPeak = level;
			}

			// Scale by device volume
			int sample = ((int)src[i] * volume) / SDL_MIX_MAXVOLUME;

			// Apply fade-in if recovering from underrun
			if (pThis->m_audioFadeOut && pThis->m_audioFadeOutSamples > 0) {
				int fadeProgress = 256 - pThis->m_audioFadeOutSamples;
				sample = (sample * fadeProgress) / 256;
				pThis->m_audioFadeOutSamples--;
				if (pThis->m_audioFadeOutSamples <= 0) {
					pThis->m_audioFadeOut = false;
				}
			}

			// Clamp to 16-bit range
			if (sample > 32767) sample = 32767;
			else if (sample < -32768) sample = -32768;

			dst[i] = (Sint16)sample;
		}

		pAudioFrame->dataLeft -= readLen;
		needLen -= readLen;
		streamPos += readLen;

		if (pAudioFrame->dataLeft <= 0) {
			pThis->m_queueAudio.pop();
			delete[] pAudioFrame->data;
			delete pAudioFrame;
		}
	}

	// Update peak level for UI display (with smoothing)
	if (bufferPeak > pThis->m_peakLevel) {
		pThis->m_peakLevel = bufferPeak;  // Fast attack
	} else {
		pThis->m_peakLevel = pThis->m_peakLevel * 0.95f + bufferPeak * 0.05f;  // Slow decay
	}

	// If we still need data but queue is empty, we had a partial underrun
	if (needLen > 0) {
		pThis->m_audioUnderrunCount++;
	}
}

void CSDLPlayer::setVolume(float dbVolume)
{
	// Convert AirPlay volume (dB) to SDL volume (0-128)
	//
	// AirPlay volume range (actual from iOS):
	//   0.0 dB   = Maximum volume (slider at 100%)
	//   -30.0 dB = Minimum volume (slider at 0%) - THIS IS THE FLOOR, NOT -144!
	//
	// SDL volume: 0 = mute, 128 = max (SDL_MIX_MAXVOLUME)
	//
	// IMPORTANT: AirPlay sends -30 dB when muted, NOT -144 dB!
	// So we must treat -30 dB (and below) as mute.

	const float AIRPLAY_MIN_DB = -30.0f;  // iPhone sends this at 0% volume
	const float AIRPLAY_MAX_DB = 0.0f;    // iPhone sends this at 100% volume

	int sdlVolume;

	if (dbVolume <= AIRPLAY_MIN_DB) {
		// MUTE: -30 dB or lower = complete silence
		sdlVolume = 0;
	} else if (dbVolume >= AIRPLAY_MAX_DB) {
		// MAX: 0 dB or higher = full volume
		sdlVolume = SDL_MIX_MAXVOLUME;
	} else {
		// SCALE: Map -30 dB to 0 dB -> 0 to 128 (linear in dB space)
		// This gives a natural volume curve that matches the iPhone slider
		//
		// Formula: sdlVolume = ((dbVolume - minDB) / (maxDB - minDB)) * 128
		//          sdlVolume = ((dbVolume + 30) / 30) * 128
		float normalized = (dbVolume - AIRPLAY_MIN_DB) / (AIRPLAY_MAX_DB - AIRPLAY_MIN_DB);
		sdlVolume = (int)(normalized * SDL_MIX_MAXVOLUME);

		// Clamp to valid range (safety)
		if (sdlVolume < 0) sdlVolume = 0;
		if (sdlVolume > SDL_MIX_MAXVOLUME) sdlVolume = SDL_MIX_MAXVOLUME;
	}

	m_audioVolume = sdlVolume;
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

void CSDLPlayer::requestResize(int width, int height)
{
	// Store pending resize dimensions - will be handled in main loop
	// Don't set m_bResizing here - let handleLiveResize do it when it actually runs
	m_pendingResizeWidth = width;
	m_pendingResizeHeight = height;
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

	// Determine target size: video resolution clamped to screen work area
	int targetW = m_videoWidth;
	int targetH = m_videoHeight;

	if (m_hwnd != NULL) {
		HMONITOR hMonitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
		if (hMonitor != NULL) {
			MONITORINFO mi;
			mi.cbSize = sizeof(MONITORINFO);
			if (GetMonitorInfo(hMonitor, &mi)) {
				int workW = mi.rcWork.right - mi.rcWork.left;
				int workH = mi.rcWork.bottom - mi.rcWork.top;

				// Cap window to 80% of screen when video exceeds screen size
				int maxW = workW * 4 / 5;  // 80% using integer math
				int maxH = workH * 4 / 5;

				if (targetW > maxW || targetH > maxH) {
					// Use cross-multiplication to find constraining dimension
					// videoW/videoH > maxW/maxH  iff  videoW*maxH > maxW*videoH
					if ((long long)m_videoWidth * maxH > (long long)maxW * m_videoHeight) {
						// Constrained by width
						targetW = maxW;
						targetH = (int)(((long long)maxW * m_videoHeight + m_videoWidth / 2) / m_videoWidth);
					} else {
						// Constrained by height
						targetH = maxH;
						targetW = (int)(((long long)maxH * m_videoWidth + m_videoHeight / 2) / m_videoHeight);
					}
				}
			}
		}
	}

	resizeWindow(targetW, targetH);

	// Also update the Windows window size
	if (m_hwnd != NULL) {
		// Calculate window size including borders
		RECT rect = { 0, 0, targetW, targetH };
		AdjustWindowRectEx(&rect, GetWindowLong(m_hwnd, GWL_STYLE), FALSE, GetWindowLong(m_hwnd, GWL_EXSTYLE));
		int windowWidth = rect.right - rect.left;
		int windowHeight = rect.bottom - rect.top;

		// Clamp total window size (including borders) to work area
		HMONITOR hMonitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
		if (hMonitor != NULL) {
			MONITORINFO mi;
			mi.cbSize = sizeof(MONITORINFO);
			if (GetMonitorInfo(hMonitor, &mi)) {
				int workW = mi.rcWork.right - mi.rcWork.left;
				int workH = mi.rcWork.bottom - mi.rcWork.top;
				if (windowWidth > workW) windowWidth = workW;
				if (windowHeight > workH) windowHeight = workH;

				// Center on screen
				int x = mi.rcWork.left + (workW - windowWidth) / 2;
				int y = mi.rcWork.top + (workH - windowHeight) / 2;
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

		// Process pending frame if source is ready (lockless producer-consumer)
		// Protocol: callback thread only writes m_srcYUV when m_srcReady==0
		//           scaling thread only reads m_srcYUV when m_srcReady==1
		//           scaling thread clears m_srcReady after finishing the read
		if (InterlockedCompareExchange(&pThis->m_srcReady, 1, 1) == 1) {
			if (pThis->m_swsCtx != NULL && pThis->m_srcYUV[0] != NULL) {
				// Get write buffer index
				LONG writeIdx = InterlockedCompareExchange(&pThis->m_writeBuffer, 0, 0);

				// Perform scaling+conversion outside any mutex (the expensive operation)
				// YUV420P input -> BGRA output (scaling thread handles both rescaling and colorspace)
				const uint8_t* srcSlice[4] = { pThis->m_srcYUV[0], pThis->m_srcYUV[1], pThis->m_srcYUV[2], NULL };
				sws_scale(pThis->m_swsCtx, srcSlice, pThis->m_srcPitch, 0, pThis->m_srcHeight,
					pThis->m_scaledYUV[writeIdx], pThis->m_scaledPitch);

				// Clear source ready AFTER scaling is complete (allows callback to write next frame)
				InterlockedExchange(&pThis->m_srcReady, 0);

				// Swap buffers atomically: write buffer becomes read buffer
				InterlockedExchange(&pThis->m_readBuffer, writeIdx);
				InterlockedExchange(&pThis->m_writeBuffer, 1 - writeIdx);
				InterlockedExchange(&pThis->m_bufferReady, 1);
			} else {
				// Invalid state, clear the flag
				InterlockedExchange(&pThis->m_srcReady, 0);
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

	// Only log when actually reinitializing

	// Free existing scaler
	freeScaler();

	// Determine scaling algorithm based on quality preset
	// SWS_ACCURATE_RND: precise rounding in color conversion (avoids banding)
	// SWS_FULL_CHR_H_INT: full chroma horizontal interpolation (YUV420 subsamples chroma 2x,
	//   this flag upsamples chroma to full resolution before RGB conversion for sharper color edges)
	int scalingAlgorithm;
	int qualityFlags = SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
	EQualityPreset preset = m_imgui.GetQualityPreset();
	switch (preset) {
	case QUALITY_GOOD:
		scalingAlgorithm = SWS_LANCZOS | qualityFlags;
		break;
	case QUALITY_BALANCED:
		scalingAlgorithm = SWS_BILINEAR | qualityFlags;
		break;
	case QUALITY_FAST:
	default:
		scalingAlgorithm = SWS_FAST_BILINEAR;  // Skip quality flags for max speed
		break;
	}

	// Create new scaler: YUV420P input -> BGRA output (handles both scaling and colorspace)
	// BGRA matches Windows SDL 1.2 surface format (Rmask=0x00FF0000 on little-endian)
	m_swsCtx = sws_getContext(srcW, srcH, AV_PIX_FMT_YUV420P,
		dstW, dstH, AV_PIX_FMT_BGRA,
		scalingAlgorithm,
		NULL, NULL, NULL);

	if (m_swsCtx == NULL) {
		OutputDebugStringA("initScaler: sws_getContext FAILED for YUV420P->BGRA\n");
		return;
	}

	// Set colorspace for accurate color reproduction
	// The FFmpeg H.264 decoder in this project outputs full-range YUV (Y:0-255, UV:0-255)
	// srcRange=1: full range input (no Y expansion needed)
	// dstRange=1: full range output (RGB 0-255)
	// BT.709 for HD (>=720p), BT.601 for SD - matches H.264 color matrix
	{
		int srcRange, dstRange, brightness, contrast, saturation;
		const int* inv_table;
		const int* table;
		sws_getColorspaceDetails(m_swsCtx, (int**)&inv_table, &srcRange,
			(int**)&table, &dstRange, &brightness, &contrast, &saturation);

		int colorspace = (srcH >= 720) ? SWS_CS_ITU709 : SWS_CS_ITU601;
		inv_table = sws_getCoefficients(colorspace);
		table = sws_getCoefficients(colorspace);

		sws_setColorspaceDetails(m_swsCtx, inv_table, 1, table, 1,
			brightness, contrast, saturation);
	}

	m_scaledWidth = dstW;
	m_scaledHeight = dstH;
	m_srcWidth = srcW;
	m_srcHeight = srcH;

	// Allocate BGRA output buffers (double-buffered, single plane per buffer)
	// BGRA = 4 bytes per pixel, 32-byte aligned pitch for SIMD
	m_scaledPitch[0] = ((dstW * 4 + 31) >> 5) << 5;  // BGRA pitch (32-byte aligned)
	m_scaledPitch[1] = 0;
	m_scaledPitch[2] = 0;

	int rgbSize = m_scaledPitch[0] * dstH;

	for (int i = 0; i < 2; i++) {
		m_scaledYUV[i][0] = (uint8_t*)_aligned_malloc(rgbSize, 32);
		m_scaledYUV[i][1] = NULL;  // Not used for BGRA
		m_scaledYUV[i][2] = NULL;  // Not used for BGRA

		if (m_scaledYUV[i][0]) memset(m_scaledYUV[i][0], 0, rgbSize);
	}

	// Allocate source staging buffer for scaling thread
	int srcUVWidth = (srcW + 1) / 2;
	int srcUVHeight = (srcH + 1) / 2;
	m_srcPitch[0] = ((srcW + 31) >> 5) << 5;
	m_srcPitch[1] = ((srcUVWidth + 31) >> 5) << 5;
	m_srcPitch[2] = m_srcPitch[1];
	m_srcYUV[0] = (uint8_t*)_aligned_malloc(m_srcPitch[0] * srcH, 32);
	m_srcYUV[1] = (uint8_t*)_aligned_malloc(m_srcPitch[1] * srcUVHeight, 32);
	m_srcYUV[2] = (uint8_t*)_aligned_malloc(m_srcPitch[2] * srcUVHeight, 32);

	// Initialize source buffers to black
	if (m_srcYUV[0]) memset(m_srcYUV[0], 0, m_srcPitch[0] * srcH);
	if (m_srcYUV[1]) memset(m_srcYUV[1], 128, m_srcPitch[1] * srcUVHeight);
	if (m_srcYUV[2]) memset(m_srcYUV[2], 128, m_srcPitch[2] * srcUVHeight);

	// Initialize buffer indices
	m_writeBuffer = 0;
	m_readBuffer = 0;
	m_bufferReady = 0;
	m_bScalerNeedsReinit = 0;
}

// Free pending scaler resources - now just clears the pending pointers (actual free is in freeScaler)
void CSDLPlayer::freePendingScalerResources()
{
	// Free any pending context
	if (m_pendingFreeCtx != NULL) {
		SwsContext* ctx = m_pendingFreeCtx;
		m_pendingFreeCtx = NULL;
		sws_freeContext(ctx);
	}

	// Free any pending buffers
	for (int i = 0; i < 2; i++) {
		if (m_pendingFreeYUV[i][0] != NULL) {
			_aligned_free(m_pendingFreeYUV[i][0]);
			m_pendingFreeYUV[i][0] = NULL;
		}
		if (m_pendingFreeYUV[i][1] != NULL) {
			_aligned_free(m_pendingFreeYUV[i][1]);
			m_pendingFreeYUV[i][1] = NULL;
		}
		if (m_pendingFreeYUV[i][2] != NULL) {
			_aligned_free(m_pendingFreeYUV[i][2]);
			m_pendingFreeYUV[i][2] = NULL;
		}
	}
}

void CSDLPlayer::freeScaler()
{
	// SAFETY: Clear dimensions FIRST to prevent any other thread from thinking the scaler is valid
	m_scaledWidth = 0;
	m_scaledHeight = 0;
	m_srcWidth = 0;
	m_srcHeight = 0;

	// Move context to pending (will be freed by callback thread)
	if (m_swsCtx != NULL) {

		// If there's already a pending context, accept the leak (safer than crash)
		if (m_pendingFreeCtx != NULL) {
		}
		m_pendingFreeCtx = m_swsCtx;
		m_swsCtx = NULL;
	}

	// Move buffers to pending (will be freed by callback thread)
	for (int i = 0; i < 2; i++) {
		// If there are already pending buffers, free them (safe from any thread)
		if (m_pendingFreeYUV[i][0] != NULL) _aligned_free(m_pendingFreeYUV[i][0]);
		if (m_pendingFreeYUV[i][1] != NULL) _aligned_free(m_pendingFreeYUV[i][1]);
		if (m_pendingFreeYUV[i][2] != NULL) _aligned_free(m_pendingFreeYUV[i][2]);

		// Move current to pending
		m_pendingFreeYUV[i][0] = m_scaledYUV[i][0];
		m_pendingFreeYUV[i][1] = m_scaledYUV[i][1];
		m_pendingFreeYUV[i][2] = m_scaledYUV[i][2];
		m_scaledYUV[i][0] = NULL;
		m_scaledYUV[i][1] = NULL;
		m_scaledYUV[i][2] = NULL;
	}

	m_scaledPitch[0] = 0;
	m_scaledPitch[1] = 0;
	m_scaledPitch[2] = 0;

	// Wait for scaling thread to finish processing before freeing source buffers
	// The scaling thread clears m_srcReady when done reading m_srcYUV
	if (m_scalingThread != NULL) {
		int waitCount = 0;
		while (InterlockedCompareExchange(&m_srcReady, 0, 0) != 0 && waitCount < 100) {
			Sleep(1);
			waitCount++;
		}
	}

	// Free source staging buffers (safe now - scaling thread is done with them)
	for (int i = 0; i < 3; i++) {
		if (m_srcYUV[i] != NULL) {
			_aligned_free(m_srcYUV[i]);
			m_srcYUV[i] = NULL;
		}
		m_srcPitch[i] = 0;
	}
	m_srcReady = 0;
}

// initRGBConverter/freeRGBConverter are no longer used.
// The scaling thread now outputs BGRA directly, eliminating the need for
// a separate YUV->RGB converter in the render loop.
void CSDLPlayer::initRGBConverter(int w, int h)
{
	(void)w; (void)h;
}

void CSDLPlayer::freeRGBConverter()
{
}
