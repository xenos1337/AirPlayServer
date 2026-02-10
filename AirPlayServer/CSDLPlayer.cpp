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
#pragma comment(lib, "winmm.lib")

// GPU handles YUV→RGB conversion via SDL2's D3D11 pixel shader (BT.709 mode)

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

CSDLPlayer::CSDLPlayer()
	: m_window(NULL)
	, m_renderer(NULL)
	, m_videoTexture(NULL)
	, m_bAudioInited(false)
	, m_bDumpAudio(false)
	, m_fileWav(NULL)
	, m_filePerfLog(NULL)
	, m_sAudioFmt()
	, m_displayRect()
	, m_videoWidth(0)
	, m_videoHeight(0)
	, m_lastFramePTS(0)
	, m_lastFrameTime(0)
	, m_windowWidth(800)
	, m_windowHeight(600)
	, m_server()
	, m_hwnd(NULL)
	, m_bWindowVisible(false)
	, m_bResizing(false)
	, m_pendingResizeWidth(800)
	, m_pendingResizeHeight(600)
	, m_bFullscreen(false)
	, m_windowedX(0)
	, m_windowedY(0)
	, m_windowedW(800)
	, m_windowedH(600)
	, m_lastMouseMoveTime(0)
	, m_bCursorHidden(false)
	, m_audioDeviceID(0)
{
	ZeroMemory(&m_sAudioFmt, sizeof(SFgAudioFrame));
	ZeroMemory(&m_displayRect, sizeof(SDL_Rect));
	ZeroMemory(m_serverName, sizeof(m_serverName));
	ZeroMemory(m_connectedDeviceName, sizeof(m_connectedDeviceName));
	m_bConnected = false;
	m_bDisconnecting = false;
	m_dwDisconnectStartTime = 0;
	m_mutexAudio = CreateMutex(NULL, FALSE, NULL);
	m_mutexVideo = CreateMutex(NULL, FALSE, NULL);
	m_audioVolume = SDL_MIX_MAXVOLUME / 2;  // Half volume by default
	m_localVolume = SDL_MIX_MAXVOLUME;     // Full local volume by default

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

	// Initialize video statistics
	m_totalFrames = 0;
	m_droppedFrames = 0;
	m_fpsStartTime = 0;
	m_fpsFrameCount = 0;
	m_currentFPS = 0.0f;
	m_totalBytes = 0;
	m_bitrateStartTime = 0;
	m_currentBitrateMbps = 0.0f;

	// Initialize performance monitoring
	m_bShowPerfGraphs = false;
	QueryPerformanceFrequency(&m_qpcFreq);
	m_qpcFrameStart.QuadPart = 0;
	m_qpcPerfLastUpdate.QuadPart = 0;
	m_qpcPerfLogStart.QuadPart = 0;
	m_qpcFrameArrival = 0;
	memset(m_perfFps, 0, sizeof(m_perfFps));
	memset(m_perfDisplayFps, 0, sizeof(m_perfDisplayFps));
	memset(m_perfFrameTime, 0, sizeof(m_perfFrameTime));
	memset(m_perfLatency, 0, sizeof(m_perfLatency));
	memset(m_perfBitrate, 0, sizeof(m_perfBitrate));
	memset(m_perfAudioQueue, 0, sizeof(m_perfAudioQueue));
	m_perfIdx = 0;
	m_perfAccumFrameTime = 0.0f;
	m_perfAccumLatency = 0.0f;
	m_perfAccumCount = 0;

	// Initialize YUV double buffer
	for (int i = 0; i < 2; i++)
		for (int p = 0; p < 3; p++)
			m_yuvBuffer[i][p] = NULL;
	m_yuvPitch[0] = m_yuvPitch[1] = m_yuvPitch[2] = 0;
	m_yuvWriteIdx = 0;
	m_yuvReadIdx = 0;
	m_yuvReady = 0;

	// Initialize frame pacing (default 60fps target)
	m_qpcLastNewFrame.QuadPart = 0;
	m_targetFrameIntervalMs = 16.667;
	m_displayFPS = 0.0f;
	m_displayFrameCount = 0;
	m_displayFpsStartTime = 0;
	m_connectionStartTime = 0;
}

CSDLPlayer::~CSDLPlayer()
{
	// Close perf log if still open
	if (m_filePerfLog != NULL) {
		fclose(m_filePerfLog);
		m_filePerfLog = NULL;
	}

	// Free YUV double buffers
	for (int i = 0; i < 2; i++) {
		for (int p = 0; p < 3; p++) {
			if (m_yuvBuffer[i][p] != NULL) {
				_aligned_free(m_yuvBuffer[i][p]);
				m_yuvBuffer[i][p] = NULL;
			}
		}
	}

	unInit();

	CloseHandle(m_mutexAudio);
	CloseHandle(m_mutexVideo);
}

bool CSDLPlayer::init()
{
	// Request 1ms Windows timer resolution (for accurate Sleep/SDL_Delay)
	timeBeginPeriod(1);

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return false;
	}

	/* Clean up on exit */
	atexit(SDL_Quit);

	// Enable best filtering for smooth scaling (default quality preset = Balanced)
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

	// Use JPEG/full-range color matrix for GPU YUV→RGB conversion
	// AirPlay sends full-range YUV (0-255); BT.709 mode assumes limited-range (16-235) causing oversaturation
	SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_JPEG);

	initVideo(m_windowWidth, m_windowHeight);

	// Get the window handle for show/hide operations
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	if (SDL_GetWindowWMInfo(m_window, &wmInfo)) {
		m_hwnd = wmInfo.info.win.window;
	}

	// Initialize ImGui with SDL2 backends
	if (!m_imgui.Init(m_window, m_renderer)) {
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

	// Start with window visible to show home screen
	showWindow();

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
		m_bDisconnecting = true;
		m_dwDisconnectStartTime = GetTickCount();

		// Close perf log on disconnect
		if (m_filePerfLog != NULL) {
			fclose(m_filePerfLog);
			m_filePerfLog = NULL;
			printf("Performance log saved to airplay_perf.csv\n");
		}

		// Reset statistics and perf graphs on disconnect
		m_totalFrames = 0;
		m_droppedFrames = 0;
		m_fpsStartTime = 0;
		m_fpsFrameCount = 0;
		m_currentFPS = 0.0f;
		m_totalBytes = 0;
		m_bitrateStartTime = 0;
		m_currentBitrateMbps = 0.0f;
		memset(m_perfFps, 0, sizeof(m_perfFps));
		memset(m_perfDisplayFps, 0, sizeof(m_perfDisplayFps));
		memset(m_perfFrameTime, 0, sizeof(m_perfFrameTime));
		memset(m_perfLatency, 0, sizeof(m_perfLatency));
		memset(m_perfBitrate, 0, sizeof(m_perfBitrate));
		memset(m_perfAudioQueue, 0, sizeof(m_perfAudioQueue));
		m_perfIdx = 0;
		m_perfAccumFrameTime = 0.0f;
		m_perfAccumLatency = 0.0f;
		m_perfAccumCount = 0;
		m_qpcPerfLastUpdate.QuadPart = 0;
		m_qpcPerfLogStart.QuadPart = 0;
		m_qpcLastNewFrame.QuadPart = 0;
		m_displayFPS = 0.0f;
		m_displayFrameCount = 0;
		m_displayFpsStartTime = 0;
		m_connectionStartTime = 0;
	} else if (!m_bConnected && connected) {
		// Open perf log on connect
		{
			char logPath[MAX_PATH] = { 0 };
			GetModuleFileNameA(NULL, logPath, MAX_PATH);
			char* lastSlash = strrchr(logPath, '\\');
			if (lastSlash) {
				strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - logPath), "airplay_perf.csv");
			} else {
				strcpy_s(logPath, MAX_PATH, "airplay_perf.csv");
			}
			m_filePerfLog = fopen(logPath, "w");
			if (m_filePerfLog) {
				fprintf(m_filePerfLog,
					"time_ms,frame_time_ms,source_fps,latency_ms,new_frame,video_w,video_h,bitrate_mbps,total_frames,dropped_frames,audio_queue,audio_underruns\n");
				fflush(m_filePerfLog);
			}
			m_qpcPerfLogStart.QuadPart = 0;
		}

		// Reset statistics and perf graphs when connecting
		m_totalFrames = 0;
		m_droppedFrames = 0;
		m_fpsStartTime = 0;
		m_fpsFrameCount = 0;
		m_currentFPS = 0.0f;
		m_totalBytes = 0;
		m_bitrateStartTime = 0;
		m_currentBitrateMbps = 0.0f;
		memset(m_perfFps, 0, sizeof(m_perfFps));
		memset(m_perfDisplayFps, 0, sizeof(m_perfDisplayFps));
		memset(m_perfFrameTime, 0, sizeof(m_perfFrameTime));
		memset(m_perfLatency, 0, sizeof(m_perfLatency));
		memset(m_perfBitrate, 0, sizeof(m_perfBitrate));
		memset(m_perfAudioQueue, 0, sizeof(m_perfAudioQueue));
		m_perfIdx = 0;
		m_perfAccumFrameTime = 0.0f;
		m_perfAccumLatency = 0.0f;
		m_perfAccumCount = 0;
		m_qpcPerfLastUpdate.QuadPart = 0;
		m_qpcPerfLogStart.QuadPart = 0;
		m_qpcLastNewFrame.QuadPart = 0;
		m_displayFPS = 0.0f;
		m_displayFrameCount = 0;
		m_displayFpsStartTime = 0;
		m_connectionStartTime = GetTickCount();
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

	// Restore default Windows timer resolution
	timeEndPeriod(1);

	SDL_Quit();
}

void CSDLPlayer::loopEvents()
{
	SDL_Event event;

	BOOL bEndLoop = FALSE;
	bool bShowUI = true;

	EQualityPreset lastQualityPreset = (EQualityPreset)-1;  // Force initial preset application

	// Initialize cursor hide timer
	m_lastMouseMoveTime = GetTickCount();

	/* Main loop - poll events, upload YUV to GPU, render ImGui */
	while (!bEndLoop) {
		// Record frame start time with high-precision QPC
		LARGE_INTEGER qpcFrameStart;
		QueryPerformanceCounter(&qpcFrameStart);
		m_qpcFrameStart = qpcFrameStart;

		// Process all pending events
		while (SDL_PollEvent(&event)) {
			// Forward SDL2 events to ImGui first
			m_imgui.ProcessEvent(&event);

			// Track mouse movement for cursor auto-hide
			if (event.type == SDL_MOUSEMOTION) {
				m_lastMouseMoveTime = GetTickCount();
				if (m_bCursorHidden) {
					m_bCursorHidden = false;
					SDL_ShowCursor(SDL_ENABLE);
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
					if (width != (unsigned int)m_videoWidth || height != (unsigned int)m_videoHeight) {
						m_bResizing = true;

						{
							CAutoLock oLock(m_mutexVideo, "recreateTexture");
							if (m_videoTexture != NULL) {
								SDL_DestroyTexture(m_videoTexture);
								m_videoTexture = NULL;
							}
							m_videoWidth = width;
							m_videoHeight = height;
						}

						// Allocate YUV double buffers for the new video dimensions
						{
							CAutoLock oLock(m_mutexVideo, "allocYUVBuffers");

							// Free old buffers
							for (int i = 0; i < 2; i++) {
								for (int p = 0; p < 3; p++) {
									if (m_yuvBuffer[i][p] != NULL) {
										_aligned_free(m_yuvBuffer[i][p]);
										m_yuvBuffer[i][p] = NULL;
									}
								}
							}

							int uvWidth = (width + 1) / 2;
							int uvHeight = (height + 1) / 2;
							m_yuvPitch[0] = ((width + 31) >> 5) << 5;      // Y pitch, 32-byte aligned
							m_yuvPitch[1] = ((uvWidth + 31) >> 5) << 5;    // U pitch
							m_yuvPitch[2] = m_yuvPitch[1];                  // V pitch

							for (int i = 0; i < 2; i++) {
								m_yuvBuffer[i][0] = (uint8_t*)_aligned_malloc(m_yuvPitch[0] * height, 32);
								m_yuvBuffer[i][1] = (uint8_t*)_aligned_malloc(m_yuvPitch[1] * uvHeight, 32);
								m_yuvBuffer[i][2] = (uint8_t*)_aligned_malloc(m_yuvPitch[2] * uvHeight, 32);

								// Initialize to black (Y=0, U=128, V=128)
								if (m_yuvBuffer[i][0]) memset(m_yuvBuffer[i][0], 0, m_yuvPitch[0] * height);
								if (m_yuvBuffer[i][1]) memset(m_yuvBuffer[i][1], 128, m_yuvPitch[1] * uvHeight);
								if (m_yuvBuffer[i][2]) memset(m_yuvBuffer[i][2], 128, m_yuvPitch[2] * uvHeight);
							}

							m_yuvWriteIdx = 0;
							m_yuvReadIdx = 0;
							m_yuvReady = 0;
						}

						// Resize window to match video aspect ratio at 80% of monitor height
						if (!m_bFullscreen) {
							SDL_DisplayMode dm;
							if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
								int targetH = (int)(dm.h * 0.80f);
								int targetW = (int)((long long)targetH * width / height);
								// Clamp width to 95% of monitor width if too wide
								int maxW = (int)(dm.w * 0.95f);
								if (targetW > maxW) {
									targetW = maxW;
									targetH = (int)((long long)targetW * height / width);
								}
								SDL_SetWindowSize(m_window, targetW, targetH);
								SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
								m_windowWidth = targetW;
								m_windowHeight = targetH;
								// Save as windowed dimensions for fullscreen restore
								m_windowedW = targetW;
								m_windowedH = targetH;
								SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
							}
						}

						// Calculate display rect for the new video
						calculateDisplayRect();

						// Create IYUV streaming texture (GPU does BT.709 conversion + scaling)
						m_videoTexture = SDL_CreateTexture(m_renderer,
							SDL_PIXELFORMAT_IYUV,
							SDL_TEXTUREACCESS_STREAMING,
							m_videoWidth, m_videoHeight);

						m_bResizing = false;
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
			case SDL_WINDOWEVENT: {
				if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
					event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					int newW = event.window.data1;
					int newH = event.window.data2;
					if (newW >= 100 && newH >= 100 &&
						(newW != m_windowWidth || newH != m_windowHeight)) {
						m_windowWidth = newW;
						m_windowHeight = newH;
						calculateDisplayRect();
					}
				}
				else if (event.window.event == SDL_WINDOWEVENT_EXPOSED) {
					// Window exposed - will be redrawn in the render loop
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
				case SDLK_F1: {
					// F1 toggles performance graphs overlay
					m_bShowPerfGraphs = !m_bShowPerfGraphs;
					break;
				}
				}
				break;
			}
			case SDL_MOUSEBUTTONDOWN: {
				// Double-click toggles fullscreen
				if (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2) {
					toggleFullscreen();
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
				m_server.stop();
				SDL_Delay(100);
				bEndLoop = TRUE;
				break;
			}
			}
		} // End of event polling loop

		// Handle disconnect transition (show black screen briefly)
		if (m_bDisconnecting) {
			DWORD elapsed = GetTickCount() - m_dwDisconnectStartTime;
			if (elapsed >= 300) {
				m_bDisconnecting = false;
			}
		}

		// Apply quality preset as SDL render scale quality hint
		EQualityPreset currentPreset = m_imgui.GetQualityPreset();
		if (currentPreset != lastQualityPreset) {
			lastQualityPreset = currentPreset;
			switch (currentPreset) {
			case QUALITY_GOOD:
				SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
				m_targetFrameIntervalMs = 33.333;  // 30fps - maximum quality per frame
				break;
			case QUALITY_BALANCED:
				SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
				m_targetFrameIntervalMs = 16.667;  // 60fps - smooth + high quality
				break;
			case QUALITY_FAST:
				SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
				m_targetFrameIntervalMs = 16.667;  // 60fps - lowest latency
				break;
			}
			// Recreate texture with new filter mode (hint only applies at texture creation)
			if (m_videoTexture != NULL && m_videoWidth > 0 && m_videoHeight > 0) {
				SDL_DestroyTexture(m_videoTexture);
				m_videoTexture = SDL_CreateTexture(m_renderer,
					SDL_PIXELFORMAT_IYUV,
					SDL_TEXTUREACCESS_STREAMING,
					m_videoWidth, m_videoHeight);
			}
		}

		// === GPU RENDER PIPELINE ===

		// 1. Clear renderer to black
		SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
		SDL_RenderClear(m_renderer);

		// 2. Upload new YUV frame with FIXED-INTERVAL PACING
		// Instead of uploading immediately (which mirrors bursty TCP delivery),
		// only upload when the target frame interval has elapsed.
		// This absorbs bursts: frames queue in double buffer, latest wins.
		LONGLONG frameArrivalQpc = 0;  // For latency measurement
		{
			LARGE_INTEGER qpcUploadCheck;
			QueryPerformanceCounter(&qpcUploadCheck);
			double msSinceLastNew = (m_qpcLastNewFrame.QuadPart > 0)
				? (double)(qpcUploadCheck.QuadPart - m_qpcLastNewFrame.QuadPart) * 1000.0 / (double)m_qpcFreq.QuadPart
				: m_targetFrameIntervalMs;  // First frame: always display immediately

			// 90% tolerance to avoid missing frames due to timing jitter
			bool intervalReady = (msSinceLastNew >= m_targetFrameIntervalMs * 0.90);

			if (intervalReady && InterlockedCompareExchange(&m_yuvReady, 0, 1) == 1) {
				LONG readIdx = InterlockedCompareExchange(&m_yuvReadIdx, 0, 0);
				if (readIdx >= 0 && readIdx <= 1 &&
					m_yuvBuffer[readIdx][0] != NULL && m_videoTexture != NULL) {

					// Capture frame arrival timestamp before upload
					frameArrivalQpc = InterlockedCompareExchange64(&m_qpcFrameArrival, 0, 0);

					// Upload raw YUV planes to GPU (GPU shader does colorspace conversion + scaling)
					SDL_UpdateYUVTexture(m_videoTexture, NULL,
						m_yuvBuffer[readIdx][0], m_yuvPitch[0],
						m_yuvBuffer[readIdx][1], m_yuvPitch[1],
						m_yuvBuffer[readIdx][2], m_yuvPitch[2]);
					m_lastFrameTime = GetTickCount();

					// Record when this new frame was displayed (for pacing)
					QueryPerformanceCounter(&m_qpcLastNewFrame);

					// Track display FPS (actual frames uploaded to GPU per second)
					m_displayFrameCount++;
					DWORD displayNow = SDL_GetTicks();
					if (m_displayFpsStartTime == 0) {
						m_displayFpsStartTime = displayNow;
					} else if (displayNow - m_displayFpsStartTime >= 1000) {
						m_displayFPS = (float)m_displayFrameCount * 1000.0f / (float)(displayNow - m_displayFpsStartTime);
						m_displayFrameCount = 0;
						m_displayFpsStartTime = displayNow;
					}
				}
			}
		}

		// 3. Render video texture (GPU handles scaling)
		// Only render when connected or during disconnect transition (prevents stale frame behind home screen)
		if (m_videoTexture != NULL && m_videoWidth > 0 && (m_bConnected || m_bDisconnecting)) {
			SDL_RenderCopy(m_renderer, m_videoTexture, NULL, &m_displayRect);
		}

		// 4. Render ImGui overlay on top
		m_imgui.NewFrame();
		if (m_bConnected) {
			// Connected - show overlay with controls and video statistics
			m_imgui.RenderOverlay(&bShowUI, m_serverName, m_bConnected, m_connectedDeviceName,
				m_videoWidth, m_videoHeight, m_displayFPS, m_currentBitrateMbps,
				m_totalFrames, m_droppedFrames, m_totalBytes);
		} else if (m_bDisconnecting) {
			// Disconnecting - show disconnect message
			m_imgui.RenderDisconnectMessage(m_connectedDeviceName);
		} else {
			// Disconnected - show home screen
			m_imgui.RenderHomeScreen(m_serverName, m_bConnected, m_connectedDeviceName, m_server.isRunning());
		}

		// 4b. Render performance graphs if F1 toggled on
		if (m_bShowPerfGraphs) {
			float liveFrameTime = (m_perfAccumCount > 0) ? m_perfAccumFrameTime / (float)m_perfAccumCount : 0.0f;
			float liveLatency = (m_perfAccumCount > 0) ? m_perfAccumLatency / (float)m_perfAccumCount : 0.0f;

			int audioQueueNow = 0;
			{
				CAutoLock oLock(m_mutexAudio, "perfGraphAudioQ");
				audioQueueNow = (int)m_queueAudio.size();
			}

			SPerfData perf = {};
			perf.sourceFpsHistory = m_perfFps;
			perf.displayFpsHistory = m_perfDisplayFps;
			perf.frameTimeHistory = m_perfFrameTime;
			perf.latencyHistory = m_perfLatency;
			perf.bitrateHistory = m_perfBitrate;
			perf.audioQueueHistory = m_perfAudioQueue;
			perf.historySize = PERF_HISTORY;
			perf.currentIdx = m_perfIdx;
			perf.sourceFps = m_currentFPS;
			perf.displayFps = m_displayFPS;
			perf.frameTimeMs = liveFrameTime;
			perf.latencyMs = liveLatency;
			perf.bitrateMbps = m_currentBitrateMbps;
			perf.targetFps = (float)(1000.0 / m_targetFrameIntervalMs);
			perf.videoWidth = m_videoWidth;
			perf.videoHeight = m_videoHeight;
			perf.totalFrames = m_totalFrames;
			perf.droppedFrames = m_droppedFrames;
			perf.totalBytes = m_totalBytes;
			perf.audioUnderruns = m_audioUnderrunCount;
			perf.audioDropped = m_audioDroppedFrames;
			perf.audioQueueSize = audioQueueNow;
			perf.connectionTimeSec = (m_connectionStartTime > 0) ? (float)(GetTickCount() - m_connectionStartTime) / 1000.0f : 0.0f;

			m_imgui.RenderPerfGraphs(perf);
		}

		// ImGui::Render() + GPU draw via SDL2Renderer backend
		m_imgui.Render();

		// 5. Present (no VSync - immediate display for lowest latency)
		SDL_RenderPresent(m_renderer);

		// 6. Accumulate performance metrics, write to circular buffer every 1 second
		{
			LARGE_INTEGER qpcNow;
			QueryPerformanceCounter(&qpcNow);

			// Frame time this frame
			double frameTimeMs = (double)(qpcNow.QuadPart - qpcFrameStart.QuadPart) * 1000.0 / (double)m_qpcFreq.QuadPart;
			m_perfAccumFrameTime += (float)frameTimeMs;

			// Decode-to-display latency this frame
			double latencyMs = 0.0;
			if (frameArrivalQpc > 0 && m_qpcFreq.QuadPart > 0) {
				latencyMs = (double)(qpcNow.QuadPart - frameArrivalQpc) * 1000.0 / (double)m_qpcFreq.QuadPart;
				if (latencyMs >= 0.0 && latencyMs < 1000.0) {
					m_perfAccumLatency += (float)latencyMs;
				} else {
					latencyMs = 0.0;
				}
			}
			m_perfAccumCount++;

			// Initialize timer on first frame
			if (m_qpcPerfLastUpdate.QuadPart == 0) {
				m_qpcPerfLastUpdate = qpcNow;
			}

			// Write one sample per second (averages) for graphs
			double elapsedSec = (double)(qpcNow.QuadPart - m_qpcPerfLastUpdate.QuadPart) / (double)m_qpcFreq.QuadPart;
			if (elapsedSec >= 1.0) {
				if (m_perfAccumCount > 0) {
					m_perfFps[m_perfIdx] = m_currentFPS;
					m_perfDisplayFps[m_perfIdx] = m_displayFPS;
					m_perfFrameTime[m_perfIdx] = m_perfAccumFrameTime / (float)m_perfAccumCount;
					m_perfLatency[m_perfIdx] = m_perfAccumLatency / (float)m_perfAccumCount;
					m_perfBitrate[m_perfIdx] = m_currentBitrateMbps;
					// Sample audio queue depth (with lock)
					{
						CAutoLock oLock(m_mutexAudio, "perfAudioQueue");
						m_perfAudioQueue[m_perfIdx] = (float)m_queueAudio.size();
					}
				} else {
					m_perfFps[m_perfIdx] = 0.0f;
					m_perfDisplayFps[m_perfIdx] = 0.0f;
					m_perfFrameTime[m_perfIdx] = 0.0f;
					m_perfLatency[m_perfIdx] = 0.0f;
					m_perfBitrate[m_perfIdx] = 0.0f;
					m_perfAudioQueue[m_perfIdx] = 0.0f;
				}
				m_perfIdx = (m_perfIdx + 1) % PERF_HISTORY;
				m_perfAccumFrameTime = 0.0f;
				m_perfAccumLatency = 0.0f;
				m_perfAccumCount = 0;
				m_qpcPerfLastUpdate = qpcNow;

				// Flush CSV log every second so data isn't lost on crash
				if (m_filePerfLog != NULL) {
					fflush(m_filePerfLog);
				}
			}

			// 7. Write per-frame CSV log (every render frame while connected)
			if (m_filePerfLog != NULL && m_bConnected) {
				if (m_qpcPerfLogStart.QuadPart == 0) {
					m_qpcPerfLogStart = qpcNow;
				}
				double timeSinceStartMs = (double)(qpcNow.QuadPart - m_qpcPerfLogStart.QuadPart) * 1000.0 / (double)m_qpcFreq.QuadPart;

				int audioQueueSize = 0;
				{
					CAutoLock oLock(m_mutexAudio, "perfLog");
					audioQueueSize = (int)m_queueAudio.size();
				}

				fprintf(m_filePerfLog,
					"%.3f,%.3f,%.1f,%.3f,%d,%d,%d,%.2f,%llu,%llu,%d,%d\n",
					timeSinceStartMs,
					frameTimeMs,
					m_currentFPS,
					latencyMs,
					(frameArrivalQpc > 0) ? 1 : 0,
					m_videoWidth, m_videoHeight,
					m_currentBitrateMbps,
					m_totalFrames, m_droppedFrames,
					audioQueueSize,
					m_audioUnderrunCount);
			}
		}

		// Auto-hide cursor after 5 seconds of inactivity
		if (!m_bCursorHidden && m_lastMouseMoveTime > 0) {
			DWORD elapsed = GetTickCount() - m_lastMouseMoveTime;
			if (elapsed >= CURSOR_HIDE_DELAY_MS) {
				m_bCursorHidden = true;
				SDL_ShowCursor(SDL_DISABLE);
			}
		}

		// Sync audio settings between player and UI
		m_autoAdjustEnabled = m_imgui.IsAutoAdjustEnabled();
		m_localVolume = (int)(m_imgui.GetLocalVolume() * SDL_MIX_MAXVOLUME);
		m_imgui.SetDeviceVolume(m_deviceVolumeNormalized);
		m_imgui.SetCurrentAudioLevel(m_peakLevel);

		// Frame-paced render loop: sleep + spin-wait to hit target intervals precisely
		// Connected: pace to target FPS (16.67ms=60fps or 33.33ms=30fps from quality preset)
		// Idle: pace to 60fps to save CPU while keeping ImGui responsive
		{
			double targetMs = m_bConnected ? m_targetFrameIntervalMs : 16.667;
			LARGE_INTEGER qpcEnd;
			QueryPerformanceCounter(&qpcEnd);
			double frameMs = (double)(qpcEnd.QuadPart - qpcFrameStart.QuadPart) * 1000.0 / (double)m_qpcFreq.QuadPart;

			if (frameMs < targetMs) {
				double remainMs = targetMs - frameMs;
				// Sleep for bulk of remaining time (saves CPU), leave 1.5ms for spin-wait precision
				if (remainMs > 2.0) {
					SDL_Delay((int)(remainMs - 1.5));
				}
				// Spin-wait the last ~1.5ms for sub-ms precision (timeBeginPeriod(1) makes Sleep ~1ms)
				do {
					QueryPerformanceCounter(&qpcEnd);
					frameMs = (double)(qpcEnd.QuadPart - qpcFrameStart.QuadPart) * 1000.0 / (double)m_qpcFreq.QuadPart;
				} while (frameMs < targetMs);
			}
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

	// Check if video source dimensions changed
	if ((int)data->width != m_videoWidth || (int)data->height != m_videoHeight) {
		m_evtVideoSizeChange.type = SDL_USEREVENT;
		m_evtVideoSizeChange.user.type = SDL_USEREVENT;
		m_evtVideoSizeChange.user.code = VIDEO_SIZE_CHANGED_CODE;
		m_evtVideoSizeChange.user.data1 = (void*)(uintptr_t)data->width;
		m_evtVideoSizeChange.user.data2 = (void*)(uintptr_t)data->height;
		SDL_PushEvent(&m_evtVideoSizeChange);
		return;
	}

	// CALLBACK THREAD: Copy raw YUV420P planes into double buffer (fast memcpy)
	// GPU does BT.709 conversion via shader during SDL_UpdateYUVTexture
	{
		CAutoLock oLock(m_mutexVideo, "outputVideo");

		if (m_bResizing) {
			return;
		}

		LONG writeIdx = InterlockedCompareExchange(&m_yuvWriteIdx, 0, 0);
		if (writeIdx < 0 || writeIdx > 1 || m_yuvBuffer[writeIdx][0] == NULL) {
			return;  // Buffers not allocated yet
		}

		// Copy YUV planes from source data
		const uint8_t* srcY = data->data;
		const uint8_t* srcU = data->data + data->dataLen[0];
		const uint8_t* srcV = data->data + data->dataLen[0] + data->dataLen[1];

		const int yHeight = data->height;
		const int uvHeight = (data->height + 1) / 2;

		// Y plane
		{
			const uint8_t* sp = srcY;
			uint8_t* dp = m_yuvBuffer[writeIdx][0];
			const int copyW = ((int)data->width < m_yuvPitch[0]) ? (int)data->width : m_yuvPitch[0];
			for (int row = 0; row < yHeight; row++) {
				memcpy(dp, sp, copyW);
				sp += data->pitch[0];
				dp += m_yuvPitch[0];
			}
		}
		// U plane
		{
			const uint8_t* sp = srcU;
			uint8_t* dp = m_yuvBuffer[writeIdx][1];
			const int uvW = ((int)data->width + 1) / 2;
			const int copyW = (uvW < m_yuvPitch[1]) ? uvW : m_yuvPitch[1];
			for (int row = 0; row < uvHeight; row++) {
				memcpy(dp, sp, copyW);
				sp += data->pitch[1];
				dp += m_yuvPitch[1];
			}
		}
		// V plane
		{
			const uint8_t* sp = srcV;
			uint8_t* dp = m_yuvBuffer[writeIdx][2];
			const int uvW = ((int)data->width + 1) / 2;
			const int copyW = (uvW < m_yuvPitch[2]) ? uvW : m_yuvPitch[2];
			for (int row = 0; row < uvHeight; row++) {
				memcpy(dp, sp, copyW);
				sp += data->pitch[2];
				dp += m_yuvPitch[2];
			}
		}

		m_lastFramePTS = data->pts;

		// Record arrival timestamp for decode-to-display latency measurement
		LARGE_INTEGER qpcNow;
		QueryPerformanceCounter(&qpcNow);
		InterlockedExchange64(&m_qpcFrameArrival, qpcNow.QuadPart);

		// Publish: make this buffer available for render thread
		InterlockedExchange(&m_yuvReadIdx, writeIdx);
		InterlockedExchange(&m_yuvWriteIdx, 1 - writeIdx);
		InterlockedExchange(&m_yuvReady, 1);
	}

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
		m_currentBitrateMbps = (float)(bytesDelta * 8) / (1000.0f * 1000.0f);
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
		double step = 1.0 / ratio;

		for (int i = 0; i < outSamples; i++) {
			double srcPos = i * step;
			int srcIdx = (int)srcPos;
			double frac = srcPos - srcIdx;

			if (srcIdx >= inSamples - 1) {
				srcIdx = inSamples - 2;
				if (srcIdx < 0) srcIdx = 0;
				frac = 1.0;
			}

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

		// AUDIO QUEUE LIMITING: Prevent unbounded growth
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

	// Create SDL2 window with resizable support and HiDPI awareness
	m_window = SDL_CreateWindow("AirPlay Server",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		width, height,
		SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

	if (m_window == NULL) {
		printf("Could not create window: %s\n", SDL_GetError());
		return;
	}

	// Create GPU-accelerated renderer (no VSync - minimizes frame latency)
	m_renderer = SDL_CreateRenderer(m_window, -1,
		SDL_RENDERER_ACCELERATED);

	if (m_renderer == NULL) {
		printf("Could not create renderer: %s\n", SDL_GetError());
		return;
	}

	// Enable alpha blending for ImGui overlay
	SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);

	// Calculate display rect (will be 0x0 until video arrives)
	calculateDisplayRect();
}

void CSDLPlayer::resizeWindow(int width, int height)
{
	if (m_bResizing) {
		return;
	}

	m_bResizing = true;

	m_windowWidth = width;
	m_windowHeight = height;

	// Recalculate display rect for new window size
	calculateDisplayRect();

	m_bResizing = false;
}

void CSDLPlayer::handleLiveResize(int width, int height)
{
	if (width <= 0 || height <= 0) {
		return;
	}
	if (width == m_windowWidth && height == m_windowHeight) {
		return;
	}

	m_windowWidth = width;
	m_windowHeight = height;
	calculateDisplayRect();
}

void CSDLPlayer::toggleFullscreen()
{
	if (m_window == NULL) {
		return;
	}

	if (m_bResizing) {
		return;
	}
	m_bResizing = true;

	// Save cursor position before fullscreen toggle (prevents cursor teleport)
	POINT cursorPos;
	GetCursorPos(&cursorPos);

	if (!m_bFullscreen) {
		// Save current window state
		SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
		SDL_GetWindowSize(m_window, &m_windowedW, &m_windowedH);

		// Enter borderless fullscreen (SDL2 handles everything)
		SDL_SetWindowFullscreen(m_window, SDL_WINDOW_FULLSCREEN_DESKTOP);

		// Update window dimensions
		SDL_GetWindowSize(m_window, &m_windowWidth, &m_windowHeight);

		// Recalculate display rect for fullscreen
		calculateDisplayRect();

		m_bFullscreen = true;
	}
	else {
		// Exit fullscreen
		SDL_SetWindowFullscreen(m_window, 0);

		// Restore window position and size
		SDL_SetWindowPosition(m_window, m_windowedX, m_windowedY);
		SDL_SetWindowSize(m_window, m_windowedW, m_windowedH);

		m_windowWidth = m_windowedW;
		m_windowHeight = m_windowedH;

		// Recalculate display rect for windowed mode
		calculateDisplayRect();

		m_bFullscreen = false;
	}

	// Restore cursor position after fullscreen toggle
	SetCursorPos(cursorPos.x, cursorPos.y);

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

	// Use cross-multiplication to compare aspect ratios without float rounding
	long long videoCross = (long long)m_videoWidth * m_windowHeight;
	long long windowCross = (long long)m_windowWidth * m_videoHeight;

	// If the aspect ratios are very close (within 1%), just fill the entire window
	long long diff = videoCross - windowCross;
	if (diff < 0) diff = -diff;
	long long threshold = windowCross / 100;
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
		displayHeight = (int)(((long long)m_windowWidth * m_videoHeight + m_videoWidth / 2) / m_videoWidth);
	} else {
		// Video is taller than window - fit to height (pillarbox left/right)
		displayHeight = m_windowHeight;
		displayWidth = (int)(((long long)m_windowHeight * m_videoWidth + m_videoHeight / 2) / m_videoHeight);
	}

	// Center the display rect
	m_displayRect.x = (m_windowWidth - displayWidth) / 2;
	m_displayRect.y = (m_windowHeight - displayHeight) / 2;
	m_displayRect.w = displayWidth;
	m_displayRect.h = displayHeight;
}

void CSDLPlayer::unInitVideo()
{
	// Destroy video texture
	if (m_videoTexture != NULL) {
		SDL_DestroyTexture(m_videoTexture);
		m_videoTexture = NULL;
	}

	// Free YUV double buffers
	for (int i = 0; i < 2; i++) {
		for (int p = 0; p < 3; p++) {
			if (m_yuvBuffer[i][p] != NULL) {
				_aligned_free(m_yuvBuffer[i][p]);
				m_yuvBuffer[i][p] = NULL;
			}
		}
	}
	m_yuvPitch[0] = m_yuvPitch[1] = m_yuvPitch[2] = 0;

	// Shutdown ImGui before destroying renderer
	m_imgui.Shutdown();

	// Destroy renderer and window
	if (m_renderer != NULL) {
		SDL_DestroyRenderer(m_renderer);
		m_renderer = NULL;
	}

	if (m_window != NULL) {
		SDL_DestroyWindow(m_window);
		m_window = NULL;
	}

	m_videoWidth = 0;
	m_videoHeight = 0;

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
			m_resampleBufferSize = m_systemSampleRate * data->channels * 2;
			m_resampleBuffer = (uint8_t*)malloc(m_resampleBufferSize);

			if (!m_resampleBuffer) {
				m_needsResampling = false;
				outputSampleRate = m_streamSampleRate;
			}
		}

		SDL_AudioSpec wanted_spec, obtained_spec;
		wanted_spec.freq = outputSampleRate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = (Uint8)data->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = AUDIO_BUFFER_SAMPLES;
		wanted_spec.callback = sdlAudioCallback;
		wanted_spec.userdata = this;

		// SDL2: use SDL_OpenAudioDevice for device selection support
		m_audioDeviceID = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &obtained_spec, 0);
		if (m_audioDeviceID == 0)
		{
			printf("Cannot open audio: %s\n", SDL_GetError());
			return;
		}

		SDL_PauseAudioDevice(m_audioDeviceID, 1);

		m_sAudioFmt.bitsPerSample = data->bitsPerSample;
		m_sAudioFmt.channels = data->channels;
		m_sAudioFmt.sampleRate = data->sampleRate;
		m_bAudioInited = true;

		if (m_bDumpAudio) {
			m_fileWav = fopen("airplay-audio.wav", "wb");
		}
	}
	if (m_queueAudio.size() >= AUDIO_QUEUE_START_THRESHOLD) {
		SDL_PauseAudioDevice(m_audioDeviceID, 0);
	}
}

void CSDLPlayer::unInitAudio()
{
	if (m_audioDeviceID != 0) {
		SDL_CloseAudioDevice(m_audioDeviceID);
		m_audioDeviceID = 0;
	}
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
	if (m_resampleBuffer != NULL) {
		free(m_resampleBuffer);
		m_resampleBuffer = NULL;
		m_resampleBufferSize = 0;
	}
	m_needsResampling = false;
	m_resamplePos = 0.0;
	m_streamSampleRate = 0;
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
		pThis->m_audioFadeOut = true;
		pThis->m_audioFadeOutSamples = 256;
		pThis->m_peakLevel *= 0.95f;
		return;
	}

	// Track peak level for this buffer
	float bufferPeak = 0.0f;

	// Process audio frames from queue
	while (!pThis->m_queueAudio.empty() && needLen > 0)
	{
		SAudioFrame* pAudioFrame = pThis->m_queueAudio.front();
		int pos = pAudioFrame->dataTotal - pAudioFrame->dataLeft;
		int readLen = min((int)pAudioFrame->dataLeft, needLen);

		Sint16* src = (Sint16*)(pAudioFrame->data + pos);
		Sint16* dst = (Sint16*)(stream + streamPos);
		int numSamples = readLen / 2;

		int volume = pThis->m_audioVolume;
		int localVol = pThis->m_localVolume;
		for (int i = 0; i < numSamples; i++) {
			int absSrc = (src[i] < 0) ? -src[i] : src[i];
			float level = absSrc / 32768.0f;
			if (level > bufferPeak) {
				bufferPeak = level;
			}

			int sample = ((int)src[i] * volume) / SDL_MIX_MAXVOLUME;
			sample = (sample * localVol) / SDL_MIX_MAXVOLUME;

			if (pThis->m_audioFadeOut && pThis->m_audioFadeOutSamples > 0) {
				int fadeProgress = 256 - pThis->m_audioFadeOutSamples;
				sample = (sample * fadeProgress) / 256;
				pThis->m_audioFadeOutSamples--;
				if (pThis->m_audioFadeOutSamples <= 0) {
					pThis->m_audioFadeOut = false;
				}
			}

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
		pThis->m_peakLevel = bufferPeak;
	} else {
		pThis->m_peakLevel = pThis->m_peakLevel * 0.95f + bufferPeak * 0.05f;
	}

	if (needLen > 0) {
		pThis->m_audioUnderrunCount++;
	}
}

void CSDLPlayer::setVolume(float dbVolume)
{
	const float AIRPLAY_MIN_DB = -30.0f;
	const float AIRPLAY_MAX_DB = 0.0f;

	int sdlVolume;

	if (dbVolume <= AIRPLAY_MIN_DB) {
		sdlVolume = 0;
	} else if (dbVolume >= AIRPLAY_MAX_DB) {
		sdlVolume = SDL_MIX_MAXVOLUME;
	} else {
		float normalized = (dbVolume - AIRPLAY_MIN_DB) / (AIRPLAY_MAX_DB - AIRPLAY_MIN_DB);
		sdlVolume = (int)(normalized * SDL_MIX_MAXVOLUME);
		if (sdlVolume < 0) sdlVolume = 0;
		if (sdlVolume > SDL_MIX_MAXVOLUME) sdlVolume = SDL_MIX_MAXVOLUME;
	}

	m_audioVolume = sdlVolume;
}

void CSDLPlayer::showWindow()
{
	if (m_window != NULL && !m_bWindowVisible) {
		SDL_ShowWindow(m_window);
		SDL_RaiseWindow(m_window);
		m_bWindowVisible = true;
		SDL_SetWindowTitle(m_window, "AirPlay Server - Connected");
	}
}

void CSDLPlayer::hideWindow()
{
	if (m_window != NULL) {
		SDL_HideWindow(m_window);
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
	m_pendingResizeWidth = width;
	m_pendingResizeHeight = height;
}
