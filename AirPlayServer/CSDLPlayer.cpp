#include "CSDLPlayer.h"
#include <stdio.h>
#include <malloc.h>  // For _aligned_malloc/_aligned_free
#include <math.h>    // For powf() in volume conversion
#include <stdlib.h>
#include <new>
#include "CAutoLock.h"
#include "resource.h"

// Windows Core Audio API for querying system audio device format
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <bcrypt.h>

// Link against required libraries
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "bcrypt.lib")

// GPU handles YUV→RGB conversion through SDL2's renderer-specific YUV shader

namespace {
	constexpr float MIN_VIDEO_ZOOM = 1.0f;
	constexpr float MAX_VIDEO_ZOOM = 5.0f;
	constexpr float VIDEO_ZOOM_STEP = 1.10f;
	constexpr float VIDEO_PAN_DRAG_THRESHOLD = 4.0f;
	constexpr DWORD DISCONNECT_NOTICE_MS = 1400;
	constexpr DWORD DISCONNECT_FADE_IN_MS = 160;
	constexpr DWORD DISCONNECT_FADE_OUT_MS = 320;
	constexpr int MIN_WINDOW_WIDTH = 560;
	constexpr int MIN_WINDOW_HEIGHT = 420;
	constexpr int PIP_LONG_EDGE = 420;
	constexpr int PIP_MIN_LONG_EDGE = 240;
	constexpr int PIP_EDGE_MARGIN = 24;
	constexpr DWORD NATIVE_RESIZE_FRAME_INTERVAL_MS = 16;
	constexpr UINT_PTR NATIVE_RESIZE_TIMER_ID = 0x41525052;
	const wchar_t NATIVE_RESIZE_PLAYER_PROPERTY[] = L"AirPlayReceiver.NativeResizePlayer";
	constexpr DWORD PIN_APPROVAL_TIMEOUT_MS = 60000;
	constexpr DWORD PIN_PRIVACY_DELAY_MS = 1000;
	enum EPinApprovalState
	{
		PIN_APPROVAL_IDLE = 0,
		PIN_APPROVAL_WAITING,
		PIN_APPROVAL_PREPARING_CODE,
		PIN_APPROVAL_SHOWING_CODE,
		PIN_APPROVAL_CAPTURE_FAILED
	};

	struct SConnectionStateChange
	{
		bool connected;
		bool hasDeviceName;
		char deviceName[256];
	};

	int SDLCALL KeepNonConnectionStateEvents(void*, SDL_Event* event)
	{
		if (event != NULL && event->type == SDL_USEREVENT &&
			event->user.code == CONNECTION_STATE_CHANGED_CODE) {
			delete (SConnectionStateChange*)event->user.data1;
			event->user.data1 = NULL;
			return 0;
		}
		return 1;
	}

	float ClampFloat(float value, float minimum, float maximum)
	{
		if (value < minimum) return minimum;
		if (value > maximum) return maximum;
		return value;
	}

	bool IsEventForDifferentWindow(const SDL_Event& event, Uint32 mainWindowId)
	{
		if (mainWindowId == 0) return false;
		Uint32 eventWindowId = 0;
		switch (event.type) {
		case SDL_WINDOWEVENT: eventWindowId = event.window.windowID; break;
		case SDL_KEYDOWN:
		case SDL_KEYUP: eventWindowId = event.key.windowID; break;
		case SDL_TEXTEDITING: eventWindowId = event.edit.windowID; break;
		case SDL_TEXTINPUT: eventWindowId = event.text.windowID; break;
		case SDL_MOUSEMOTION: eventWindowId = event.motion.windowID; break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: eventWindowId = event.button.windowID; break;
		case SDL_MOUSEWHEEL: eventWindowId = event.wheel.windowID; break;
		case SDL_DROPFILE:
		case SDL_DROPTEXT:
		case SDL_DROPBEGIN:
		case SDL_DROPCOMPLETE: eventWindowId = event.drop.windowID; break;
		default: return false;
		}
		return eventWindowId != 0 && eventWindowId != mainWindowId;
	}

	LRESULT CALLBACK NativeResizeWindowProc(HWND window, UINT message,
		WPARAM wParam, LPARAM lParam)
	{
		CSDLPlayer* player = (CSDLPlayer*)GetPropW(window,
			NATIVE_RESIZE_PLAYER_PROPERTY);
		if (player == NULL || player->m_originalWindowProc == NULL) {
			return DefWindowProcW(window, message, wParam, lParam);
		}
		if (player->m_bPictureInPicture && message == WM_NCHITTEST) {
			return player->pictureInPictureHitTest(window, lParam);
		}
		if (player->m_bPictureInPicture && message == WM_SIZING) {
			player->constrainPictureInPictureRect(wParam, (RECT*)lParam);
			return TRUE;
		}

		if (message == WM_ENTERSIZEMOVE) {
			// DefWindowProc enters a modal sizing loop after this message. A native
			// timer keeps the SDL renderer alive while that loop owns the thread.
			player->m_nativeResizeActive = true;
			player->m_lastNativeResizeRenderTime = 0;
			SetTimer(window, NATIVE_RESIZE_TIMER_ID,
				NATIVE_RESIZE_FRAME_INTERVAL_MS, NULL);
		} else if (message == WM_EXITSIZEMOVE) {
			KillTimer(window, NATIVE_RESIZE_TIMER_ID);
			player->m_nativeResizeActive = false;
			player->m_nativeResizeRendering = false;
			player->m_lastNativeResizeRenderTime = 0;
		}

		if (message == WM_TIMER && wParam == NATIVE_RESIZE_TIMER_ID) {
			player->renderDuringNativeResize();
			return 0;
		}

		LRESULT result = CallWindowProcW(player->m_originalWindowProc,
			window, message, wParam, lParam);
		if (player->m_nativeResizeActive &&
			(message == WM_SIZE || message == WM_PAINT)) {
			player->renderDuringNativeResize();
		}
		return result;
	}

	void ApplyNativeWindowTheme(HWND window)
	{
		if (window == NULL) return;
		HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
		if (dwm == NULL) return;
		typedef HRESULT(WINAPI* DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
		DwmSetWindowAttributeFn setAttribute = (DwmSetWindowAttributeFn)GetProcAddress(dwm, "DwmSetWindowAttribute");
		if (setAttribute != NULL) {
			const DWORD useImmersiveDarkMode = 20;
			const DWORD captionColorAttribute = 35;
			const DWORD textColorAttribute = 36;
			BOOL enabled = TRUE;
			COLORREF captionColor = RGB(10, 13, 18);
			COLORREF textColor = RGB(238, 241, 246);
			setAttribute(window, useImmersiveDarkMode, &enabled, sizeof(enabled));
			setAttribute(window, captionColorAttribute, &captionColor, sizeof(captionColor));
			setAttribute(window, textColorAttribute, &textColor, sizeof(textColor));
		}
		FreeLibrary(dwm);
	}

	void ApplyNativeWindowIcon(HWND window)
	{
		if (window == NULL) return;

		HINSTANCE instance = GetModuleHandleW(NULL);
		HICON largeIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON),
			IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
			LR_DEFAULTCOLOR | LR_SHARED);
		HICON smallIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON),
			IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
			LR_DEFAULTCOLOR | LR_SHARED);

		if (largeIcon != NULL) SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)largeIcon);
		if (smallIcon != NULL) SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
	}
}

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
	, m_videoTextureHasFrame(false)
	, m_bAudioInited(false)
	, m_bDumpAudio(false)
	, m_fileWav(NULL)
	, m_filePerfLog(NULL)
	, m_sAudioFmt()
	, m_displayRect()
	, m_rotationAngle(0)
	, m_zoomLevel(MIN_VIDEO_ZOOM)
	, m_zoomPanX(0.0f)
	, m_zoomPanY(0.0f)
	, m_bPanning(false)
	, m_bLeftButtonDown(false)
	, m_bPanMoved(false)
	, m_panStartX(0.0f)
	, m_panStartY(0.0f)
	, m_panLastX(0.0f)
	, m_panLastY(0.0f)
	, m_leftClickCount(0)
	, m_zoomResetPending(0)
	, m_videoWidth(0)
	, m_videoHeight(0)
	, m_lastFramePTS(0)
	, m_lastFrameTime(0)
	, m_windowWidth(800)
	, m_windowHeight(600)
	, m_server()
	, m_mutexPinApproval(NULL)
	, m_eventPinApproval(NULL)
	, m_pinApprovalState(PIN_APPROVAL_IDLE)
	, m_pinApprovalGeneration(0)
	, m_pinPrivacyDelayStart(0)
	, m_pinCaptureExclusionActive(false)
	, m_pinCaptureExclusionReleasePending(false)
	, m_capturePrivacyActive(false)
	, m_hwnd(NULL)
	, m_originalWindowProc(NULL)
	, m_nativeResizeActive(false)
	, m_nativeResizeRendering(false)
	, m_lastNativeResizeRenderTime(0)
	, m_bWindowVisible(false)
	, m_bMainWindowMinimized(false)
	, m_bResizing(false)
	, m_pendingResizeWidth(800)
	, m_pendingResizeHeight(600)
	, m_bFullscreen(false)
	, m_windowedX(0)
	, m_windowedY(0)
	, m_windowedW(800)
	, m_windowedH(600)
	, m_bPictureInPicture(false)
	, m_pipRestoreMaximized(false)
	, m_pipRestoreX(0)
	, m_pipRestoreY(0)
	, m_pipRestoreW(800)
	, m_pipRestoreH(600)
	, m_lastMouseMoveTime(0)
	, m_bCursorHidden(false)
	, m_panCursor(NULL)
	, m_audioDeviceID(0)
{
	ZeroMemory(&m_sAudioFmt, sizeof(SFgAudioFrame));
	ZeroMemory(&m_displayRect, sizeof(SDL_Rect));
	ZeroMemory(m_serverName, sizeof(m_serverName));
	ZeroMemory(m_connectedDeviceName, sizeof(m_connectedDeviceName));
	SecureZeroMemory(m_sessionAirPlayPin, sizeof(m_sessionAirPlayPin));
	SecureZeroMemory(m_pendingPinRemote, sizeof(m_pendingPinRemote));
	SecureZeroMemory(m_pendingPinCode, sizeof(m_pendingPinCode));
	m_bConnected = false;
	m_shuttingDown = 0;
	m_bDisconnecting = false;
	m_dwDisconnectStartTime = 0;
	m_mutexAudio = CreateMutex(NULL, FALSE, NULL);
	m_mutexVideo = CreateMutex(NULL, FALSE, NULL);
	m_mutexPinApproval = CreateMutex(NULL, FALSE, NULL);
	m_eventPinApproval = CreateEvent(NULL, TRUE, FALSE, NULL);
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
	m_lastBitrateTotalBytes = 0;
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
	stopServerForShutdown();

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
	if (m_eventPinApproval != NULL) CloseHandle(m_eventPinApproval);
	if (m_mutexPinApproval != NULL) CloseHandle(m_mutexPinApproval);
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
	m_panCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);

	/* Clean up on exit */
	atexit(SDL_Quit);

	// Enable best filtering for smooth scaling (default quality preset = Balanced)
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

	// Use JPEG/full-range color matrix for GPU YUV→RGB conversion
	// AirPlay sends full-range YUV (0-255); BT.709 mode assumes limited-range (16-235) causing oversaturation
	SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_JPEG);

	initVideo(m_windowWidth, m_windowHeight);
	if (m_window == NULL || m_renderer == NULL) {
		return false;
	}

	// Get the window handle for show/hide operations
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	if (SDL_GetWindowWMInfo(m_window, &wmInfo)) {
		m_hwnd = wmInfo.info.win.window;
		ApplyNativeWindowTheme(m_hwnd);
		ApplyNativeWindowIcon(m_hwnd);
		installNativeResizeHook();
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

	if (!m_cleanFeed.Init(m_window, m_hwnd)) {
		printf("Clean-feed output is unavailable because the main HWND could not be resolved\n");
	}
	syncScreenCastOutput();

	// Start with window visible to show home screen
	showWindow();

	// Auto-start the server (server name should be set before init() is called).
	// The PIN is short-lived and memory-only; settings only remember whether it
	// is required at all.
	if (m_imgui.IsAirPlayPinEnabled() && !generateSessionAirPlayPin()) {
		printf("Could not generate the AirPlay session PIN\n");
		return false;
	}
	m_server.start(this, strlen(m_serverName) > 0 ? m_serverName : NULL,
		m_imgui.IsAirPlayPinEnabled() ? m_sessionAirPlayPin : NULL);

	return true;
}

bool CSDLPlayer::generateSessionAirPlayPin()
{
	unsigned int randomValue = 0;
	if (BCryptGenRandom(NULL, (PUCHAR)&randomValue, sizeof(randomValue),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
		SecureZeroMemory(m_sessionAirPlayPin, sizeof(m_sessionAirPlayPin));
		return false;
	}

	unsigned int pin = (randomValue % 9999U) + 1U;
	sprintf_s(m_sessionAirPlayPin, sizeof(m_sessionAirPlayPin), "%04u", pin);
	return true;
}

bool CSDLPlayer::requestPinApproval(const char* remoteAddress, const char* pin)
{
	if (m_mutexPinApproval == NULL || m_eventPinApproval == NULL ||
		InterlockedCompareExchange(&m_shuttingDown, 0, 0) != 0 || pin == NULL ||
		strlen(pin) != 4) {
		return false;
	}

	if (WaitForSingleObject(m_mutexPinApproval, INFINITE) != WAIT_OBJECT_0) {
		return false;
	}
	if (m_pinApprovalState != PIN_APPROVAL_IDLE) {
		ReleaseMutex(m_mutexPinApproval);
		return false;
	}

	strncpy_s(m_pendingPinRemote, sizeof(m_pendingPinRemote),
		(remoteAddress != NULL && remoteAddress[0] != '\0') ? remoteAddress : "Unknown device",
		_TRUNCATE);
	strncpy_s(m_pendingPinCode, sizeof(m_pendingPinCode), pin, _TRUNCATE);
	m_pinApprovalState = PIN_APPROVAL_WAITING;
	m_pinPrivacyDelayStart = 0;
	InterlockedIncrement(&m_pinApprovalGeneration);
	ResetEvent(m_eventPinApproval);
	ReleaseMutex(m_mutexPinApproval);

	requestShowWindow();
	DWORD waitResult = WaitForSingleObject(m_eventPinApproval, PIN_APPROVAL_TIMEOUT_MS);

	if (WaitForSingleObject(m_mutexPinApproval, INFINITE) != WAIT_OBJECT_0) {
		return false;
	}
	bool approved = waitResult == WAIT_OBJECT_0 &&
		m_pinApprovalState == PIN_APPROVAL_SHOWING_CODE;
	if (!approved) {
		m_pinApprovalState = PIN_APPROVAL_IDLE;
		SecureZeroMemory(m_pendingPinRemote, sizeof(m_pendingPinRemote));
		SecureZeroMemory(m_pendingPinCode, sizeof(m_pendingPinCode));
	}
	ReleaseMutex(m_mutexPinApproval);
	return approved;
}

void CSDLPlayer::cancelPinApproval()
{
	if (m_mutexPinApproval == NULL || m_eventPinApproval == NULL) {
		return;
	}
	if (WaitForSingleObject(m_mutexPinApproval, INFINITE) != WAIT_OBJECT_0) {
		return;
	}
	m_pinApprovalState = PIN_APPROVAL_IDLE;
	m_pinPrivacyDelayStart = 0;
	InterlockedIncrement(&m_pinApprovalGeneration);
	SecureZeroMemory(m_pendingPinRemote, sizeof(m_pendingPinRemote));
	SecureZeroMemory(m_pendingPinCode, sizeof(m_pendingPinCode));
	SetEvent(m_eventPinApproval);
	ReleaseMutex(m_mutexPinApproval);
}

void CSDLPlayer::renderPinApprovalPopup(LONG& lastGeneration)
{
	LONG state = PIN_APPROVAL_IDLE;
	LONG generation = 0;
	char remoteAddress[sizeof(m_pendingPinRemote)] = { 0 };
	char pin[sizeof(m_pendingPinCode)] = { 0 };

	if (m_mutexPinApproval != NULL &&
		WaitForSingleObject(m_mutexPinApproval, INFINITE) == WAIT_OBJECT_0) {
		if (m_pinApprovalState == PIN_APPROVAL_PREPARING_CODE &&
			GetTickCount() - m_pinPrivacyDelayStart >= PIN_PRIVACY_DELAY_MS) {
			m_pinApprovalState = PIN_APPROVAL_SHOWING_CODE;
			m_pinPrivacyDelayStart = 0;
			// Capture exclusion has now been active for a full second. Reopen the
			// dialog and show the real PIN locally while exclusion remains active.
			m_imgui.RequestPinApprovalPopup(false);
			SetEvent(m_eventPinApproval);
		}
		state = m_pinApprovalState;
		generation = m_pinApprovalGeneration;
		strncpy_s(remoteAddress, sizeof(remoteAddress), m_pendingPinRemote, _TRUNCATE);
		strncpy_s(pin, sizeof(pin), m_pendingPinCode, _TRUNCATE);
		ReleaseMutex(m_mutexPinApproval);
	}

	bool awaitingApproval = state == PIN_APPROVAL_WAITING;
	bool preparingPin = state == PIN_APPROVAL_PREPARING_CODE;
	bool showPin = state == PIN_APPROVAL_SHOWING_CODE;
	bool captureProtectionFailed = state == PIN_APPROVAL_CAPTURE_FAILED;
	if ((awaitingApproval || preparingPin || showPin || captureProtectionFailed) &&
		generation != lastGeneration) {
		m_imgui.RequestPinApprovalPopup();
		lastGeneration = generation;
	}
	if (state == PIN_APPROVAL_IDLE && m_pinCaptureExclusionActive) {
		// Release only after this PIN-free frame has been presented. Removing
		// exclusion now could let capture software sample the previous PIN frame.
		m_pinCaptureExclusionReleasePending = true;
	}

	EPinApprovalResult result = m_imgui.RenderPinApprovalPopup(remoteAddress, pin,
		awaitingApproval, preparingPin, showPin, captureProtectionFailed);
	if (result == PIN_APPROVAL_NO_ACTION || m_mutexPinApproval == NULL ||
		m_eventPinApproval == NULL ||
		WaitForSingleObject(m_mutexPinApproval, INFINITE) != WAIT_OBJECT_0) {
		return;
	}

	if (result == PIN_APPROVAL_ALLOW && m_pinApprovalState == PIN_APPROVAL_WAITING &&
		m_pinApprovalGeneration == generation) {
		if (m_imgui.ShouldProtectPinFromCapture()) {
			if (m_cleanFeed.SetTemporaryMainWindowCaptureExclusion(true)) {
				m_pinCaptureExclusionActive = true;
				m_pinCaptureExclusionReleasePending = false;
				m_pinApprovalState = PIN_APPROVAL_PREPARING_CODE;
				m_pinPrivacyDelayStart = GetTickCount();
			} else {
				m_pinApprovalState = PIN_APPROVAL_CAPTURE_FAILED;
				m_pinPrivacyDelayStart = 0;
			}
		} else {
			m_pinApprovalState = PIN_APPROVAL_SHOWING_CODE;
			SetEvent(m_eventPinApproval);
		}
	} else if ((result == PIN_APPROVAL_DENY && m_pinApprovalState == PIN_APPROVAL_WAITING) ||
		(result == PIN_APPROVAL_DISMISS &&
			(m_pinApprovalState == PIN_APPROVAL_PREPARING_CODE ||
			 m_pinApprovalState == PIN_APPROVAL_SHOWING_CODE ||
			 m_pinApprovalState == PIN_APPROVAL_CAPTURE_FAILED))) {
		m_pinApprovalState = PIN_APPROVAL_IDLE;
		m_pinPrivacyDelayStart = 0;
		if (m_pinCaptureExclusionActive) {
			m_pinCaptureExclusionReleasePending = true;
		}
		SecureZeroMemory(m_pendingPinRemote, sizeof(m_pendingPinRemote));
		SecureZeroMemory(m_pendingPinCode, sizeof(m_pendingPinCode));
		SetEvent(m_eventPinApproval);
	}
	ReleaseMutex(m_mutexPinApproval);
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
	if (InterlockedCompareExchange(&m_shuttingDown, 0, 0) != 0) {
		return;
	}
	if (connected) {
		cancelPinApproval();
	}

	SConnectionStateChange* change = new (std::nothrow) SConnectionStateChange();
	if (change == NULL) {
		printf("Could not allocate connection-state change\n");
		return;
	}
	change->connected = connected;
	change->hasDeviceName = deviceName != NULL;
	change->deviceName[0] = '\0';
	if (deviceName != NULL) {
		strncpy_s(change->deviceName, sizeof(change->deviceName), deviceName, _TRUNCATE);
	}

	SDL_Event event = {};
	event.type = SDL_USEREVENT;
	event.user.type = SDL_USEREVENT;
	event.user.code = CONNECTION_STATE_CHANGED_CODE;
	event.user.data1 = change;
	event.user.data2 = NULL;
	if (InterlockedCompareExchange(&m_shuttingDown, 0, 0) != 0) {
		delete change;
		return;
	}
	if (SDL_PushEvent(&event) <= 0) {
		printf("Could not queue connection-state change: %s\n", SDL_GetError());
		delete change;
	}
}

void CSDLPlayer::stopServerForShutdown()
{
	// Block new callback payloads before stopping their producer, then remove any
	// payload already queued after the render thread's final event poll.
	InterlockedExchange(&m_shuttingDown, 1);
	cancelPinApproval();
	m_server.stop();
	if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
		SDL_FilterEvents(KeepNonConnectionStateEvents, NULL);
	}
}

void CSDLPlayer::applyConnectionState(bool connected, const char* deviceName)
{
	if (m_bConnected == connected) {
		if (connected && deviceName != NULL) {
			strncpy_s(m_connectedDeviceName, sizeof(m_connectedDeviceName), deviceName, _TRUNCATE);
		}
		return;
	}
	if (m_pinCaptureExclusionActive) {
		// Connection completion/cancellation retires the PIN. Keep exclusion
		// through the next PIN-free presentation before removing it.
		m_pinCaptureExclusionReleasePending = true;
	}

	// This method only runs on the SDL thread. Retire every staged and displayed
	// pixel before changing session ownership so reconnecting at the same source
	// resolution can never expose the previous sender's final frame.
	clearSessionVideoFrame();

	if (m_bConnected && !connected) {
		// Transitioning from connected to disconnected
		setPictureInPictureMode(false);
		m_cleanFeed.Hide();
		setCapturePrivacyMode(false);
		m_bDisconnecting = true;
		m_bShowPerfGraphs = false;
		m_dwDisconnectStartTime = GetTickCount();

		// Close perf log on disconnect
		if (m_filePerfLog != NULL) {
			fclose(m_filePerfLog);
			m_filePerfLog = NULL;
			printf("Performance log saved to airplay_perf.csv\n");
		}

		// Reset view and statistics on disconnect
		InterlockedExchange(&m_zoomResetPending, 1);
		m_totalFrames = 0;
		m_droppedFrames = 0;
		m_fpsStartTime = 0;
		m_fpsFrameCount = 0;
		m_currentFPS = 0.0f;
		m_totalBytes = 0;
		m_lastBitrateTotalBytes = 0;
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
		m_bDisconnecting = false;
		InterlockedExchange(&m_zoomResetPending, 1);

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
		m_lastBitrateTotalBytes = 0;
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
	} else if (connected) {
		m_connectedDeviceName[0] = '\0';
	}
}

void CSDLPlayer::unInit()
{
	stopServerForShutdown();
	unInitVideo();
	unInitAudio();
	if (m_panCursor != NULL) {
		SDL_FreeCursor(m_panCursor);
		m_panCursor = NULL;
	}

	// Restore default Windows timer resolution
	timeEndPeriod(1);

	SDL_Quit();
}

void CSDLPlayer::loopEvents()
{
	SDL_Event event;

	BOOL bEndLoop = FALSE;

	EQualityPreset lastQualityPreset = (EQualityPreset)-1;  // Force initial preset application

	// Receiver name and PIN-policy changes restart Bonjour only after typing settles.
	// The pending change is held while a device is connected, then applied as
	// soon as the receiver is idle so an active stream is never interrupted.
	char pendingServerName[256] = { 0 };
	bool pendingPinEnabled = m_imgui.IsAirPlayPinEnabled();
	strncpy_s(pendingServerName, sizeof(pendingServerName), m_serverName, _TRUNCATE);
	DWORD receiverSettingsChangeTime = 0;
	bool receiverSettingsPendingRestart = false;
	bool activePinEnabled = pendingPinEnabled;
	LONG lastPinApprovalGeneration = 0;

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
			if (IsEventForDifferentWindow(event,
				m_window != NULL ? SDL_GetWindowID(m_window) : 0)) {
				// The clean-feed target is intentionally a separate SDL window. It
				// must never feed input or resize notifications into the UI window.
				continue;
			}

			// Forward SDL2 events to ImGui first
			m_imgui.ProcessEvent(&event);

			// Track pointer activity for cursor auto-hide.
			if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEWHEEL ||
				event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
				m_lastMouseMoveTime = GetTickCount();
				if (m_bCursorHidden) {
					m_bCursorHidden = false;
					SDL_ShowCursor(SDL_ENABLE);
				}
			}

			// A pan that started outside ImGui must keep receiving motion and its
			// release even if the captured pointer crosses the overlay.
			bool activePanMouseEvent = m_bPanning &&
				(event.type == SDL_MOUSEMOTION ||
					(event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT));

			// Skip application event processing if ImGui wants to capture it.
			if (!activePanMouseEvent && m_imgui.WantCaptureMouse() &&
				(event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN ||
					event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEWHEEL)) {
				if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) &&
					event.button.button == SDL_BUTTON_LEFT) {
					m_bLeftButtonDown = false;
					m_bPanMoved = false;
					m_leftClickCount = 0;
				}
				continue;
			}
			bool globalShortcut = false;
			if (event.type == SDL_KEYUP) {
				SDL_Keycode key = event.key.keysym.sym;
				SDL_Keymod modifiers = (SDL_Keymod)event.key.keysym.mod;
				bool hasCommandModifier = (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) != 0;
				bool privacyShortcut = key == SDLK_h && m_bConnected &&
					(modifiers & KMOD_CTRL) != 0 && (modifiers & KMOD_SHIFT) != 0;
				if (privacyShortcut) {
					globalShortcut = true;
				} else if (key == SDLK_F1 && m_bConnected) {
					globalShortcut = true;
				} else if (!m_imgui.WantTextInput() && !hasCommandModifier &&
					(key == SDLK_ESCAPE || key == SDLK_f || key == SDLK_p || key == SDLK_r ||
						(key == SDLK_h && m_bConnected))) {
					globalShortcut = true;
				}
			}
			if (!globalShortcut && m_imgui.WantCaptureKeyboard() &&
				(event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)) {
				continue;
			}

			switch (event.type) {
			case SDL_USEREVENT: {
				if (event.user.code == VIDEO_SIZE_CHANGED_CODE) {
					unsigned int width = (unsigned int)(uintptr_t)event.user.data1;
					unsigned int height = (unsigned int)(uintptr_t)event.user.data2;
					if (width != (unsigned int)m_videoWidth || height != (unsigned int)m_videoHeight) {
						m_bResizing = true;
						resetZoom();

						{
							CAutoLock oLock(m_mutexVideo, "updateVideoDimensions");
							if (m_videoTexture != NULL) {
								SDL_DestroyTexture(m_videoTexture);
								m_videoTexture = NULL;
							}
							m_videoTextureHasFrame = false;
							m_cleanFeed.InvalidateVideoTexture();
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

						// Fit on the monitor that currently owns the window while retaining
						// the user's chosen window center whenever the usable bounds allow it.
						if (!m_bFullscreen && !m_bPictureInPicture &&
							!m_imgui.IsScreenCastEnabled()) {
							resizeWindowForVideo((int)width, (int)height);
						} else if (m_bPictureInPicture) {
							resizePictureInPictureToAspect();
						}

						// Calculate display rect for the new video
						calculateDisplayRect();

						// D3D9 can cache a newly bound IYUV texture across the pending
						// window-resize reset. Initialize all planes before the texture is
						// ever eligible for drawing, otherwise that cached binding is green.
						recreateVideoTexture();
						recreateCleanFeedTexture();

						m_bResizing = false;
					}
				}
				else if (event.user.code == CONNECTION_STATE_CHANGED_CODE) {
					SConnectionStateChange* change =
						(SConnectionStateChange*)event.user.data1;
					if (change != NULL) {
						applyConnectionState(change->connected,
							change->hasDeviceName ? change->deviceName : NULL);
						delete change;
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
					if (newW >= 100 && newH >= 100) {
						// Renderer scaling can change across resize/DPI transitions. End
						// the gesture so its next delta cannot mix coordinate spaces.
						stopPanning();
						m_bLeftButtonDown = false;
						m_bPanMoved = false;
						m_leftClickCount = 0;
						m_bMainWindowMinimized = false;
						m_windowWidth = newW;
						m_windowHeight = newH;
						// Recalculate even when a programmatic resize pre-populated the
						// cached size; the renderer output is authoritative only now.
						calculateDisplayRect();
					}
				}
				else if (event.window.event == SDL_WINDOWEVENT_EXPOSED) {
					// Window exposed - will be redrawn in the render loop
				}
				else if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
					// SDL2 delivers a close request as a window event. Re-queue the
					// existing quit path so settings and clean-feed resources are
					// consistently released before the main HWND is destroyed.
					SDL_Event quitEvent = {};
					quitEvent.type = SDL_QUIT;
					SDL_PushEvent(&quitEvent);
				}
				else if (event.window.event == SDL_WINDOWEVENT_MINIMIZED ||
					event.window.event == SDL_WINDOWEVENT_HIDDEN) {
					m_bMainWindowMinimized = true;
					m_cleanFeed.Hide();
				}
				else if (event.window.event == SDL_WINDOWEVENT_RESTORED ||
					event.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
					event.window.event == SDL_WINDOWEVENT_SHOWN) {
					m_bMainWindowMinimized = false;
				}
				else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
					stopPanning();
					m_bLeftButtonDown = false;
					m_bPanMoved = false;
					m_leftClickCount = 0;
				}
				break;
			}
			case SDL_KEYUP: {
				bool hasCommandModifier =
					(event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) != 0;
				switch (event.key.keysym.sym)
				{
				case SDLK_ESCAPE: {
					// ESC exits fullscreen
					if (!hasCommandModifier && m_bFullscreen) {
						toggleFullscreen();
					}
					break;
				}
				case SDLK_f: {
					// F key also toggles fullscreen
					if (!hasCommandModifier) {
						toggleFullscreen();
					}
					break;
				}
				case SDLK_F1: {
					// F1 toggles performance graphs overlay
					if (m_bConnected && !m_bPictureInPicture) {
						m_bShowPerfGraphs = !m_bShowPerfGraphs;
					}
					break;
				}
				case SDLK_p: {
					if (!hasCommandModifier && m_bConnected) {
						setPictureInPictureMode(!m_bPictureInPicture);
					}
					break;
				}
				case SDLK_h: {
					bool privacyShortcut = m_bConnected &&
						(event.key.keysym.mod & KMOD_CTRL) != 0 &&
						(event.key.keysym.mod & KMOD_SHIFT) != 0;
					if (privacyShortcut) {
						setCapturePrivacyMode(!m_capturePrivacyActive);
					} else if (!hasCommandModifier && m_bConnected) {
						// Diagnostics and controls are mutually exclusive. H returns to
						// controls from diagnostics, then toggles controls normally.
						if (m_bShowPerfGraphs) {
							m_bShowPerfGraphs = false;
							m_imgui.ShowOverlay();
						} else {
							m_imgui.ToggleOverlay();
						}
					}
					break;
				}
				case SDLK_r: {
					// R rotates video 90 degrees clockwise
					if (!hasCommandModifier) {
						m_rotationAngle = (m_rotationAngle + 90) % 360;
						if (m_bPictureInPicture) resizePictureInPictureToAspect();
						else calculateDisplayRect();
					}
					break;
				}
				}
				break;
			}
			case SDL_MOUSEBUTTONDOWN: {
				if (event.button.button != SDL_BUTTON_LEFT) {
					break;
				}

				float rendererX = 0.0f;
				float rendererY = 0.0f;
				windowToRendererCoordinates((float)event.button.x, (float)event.button.y,
					rendererX, rendererY);

				stopPanning();
				m_bLeftButtonDown = true;
				m_bPanMoved = false;
				m_leftClickCount = event.button.clicks;
				m_panStartX = rendererX;
				m_panStartY = rendererY;
				m_panLastX = rendererX;
				m_panLastY = rendererY;

				SDL_Rect videoBounds = calculateZoomedVideoBounds();
				bool overVideo = rendererX >= (float)videoBounds.x &&
					rendererX < (float)(videoBounds.x + videoBounds.w) &&
					rendererY >= (float)videoBounds.y &&
					rendererY < (float)(videoBounds.y + videoBounds.h);
				if (overVideo && m_bConnected && m_videoTexture != NULL &&
					m_videoWidth > 0 && m_videoHeight > 0 &&
					m_zoomLevel > MIN_VIDEO_ZOOM + 0.0001f) {
					m_bPanning = true;
					SDL_CaptureMouse(SDL_TRUE);
					if (m_panCursor != NULL) SDL_SetCursor(m_panCursor);
				}
				break;
			}
			case SDL_MOUSEMOTION: {
				if (!m_bLeftButtonDown) {
					break;
				}

				float rendererX = 0.0f;
				float rendererY = 0.0f;
				windowToRendererCoordinates((float)event.motion.x, (float)event.motion.y,
					rendererX, rendererY);

				float totalX = rendererX - m_panStartX;
				float totalY = rendererY - m_panStartY;
				if (m_bPanning) {
					if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
						stopPanning();
						m_bLeftButtonDown = false;
						m_leftClickCount = 0;
						break;
					}
				}

				bool crossedDragThreshold = totalX * totalX + totalY * totalY >=
					VIDEO_PAN_DRAG_THRESHOLD * VIDEO_PAN_DRAG_THRESHOLD;
				if (!m_bPanMoved && crossedDragThreshold) {
					m_bPanMoved = true;
					if (m_bPanning) {
						// Apply the full displacement once the gesture is intentional,
						// avoiding click jitter without losing the first drag pixels.
						applyDragPan(totalX, totalY);
						m_panLastX = rendererX;
						m_panLastY = rendererY;
					}
				} else if (m_bPanning && m_bPanMoved) {
					float deltaX = rendererX - m_panLastX;
					float deltaY = rendererY - m_panLastY;
					m_panLastX = rendererX;
					m_panLastY = rendererY;
					applyDragPan(deltaX, deltaY);
				}
				break;
			}
			case SDL_MOUSEBUTTONUP: {
				if (event.button.button != SDL_BUTTON_LEFT) {
					break;
				}

				if (m_bLeftButtonDown) {
					float rendererX = 0.0f;
					float rendererY = 0.0f;
					windowToRendererCoordinates((float)event.button.x, (float)event.button.y,
						rendererX, rendererY);
					float totalX = rendererX - m_panStartX;
					float totalY = rendererY - m_panStartY;
					if (totalX * totalX + totalY * totalY >=
						VIDEO_PAN_DRAG_THRESHOLD * VIDEO_PAN_DRAG_THRESHOLD) {
						m_bPanMoved = true;
					}
				}

				bool toggleOnDoubleClick = m_bLeftButtonDown &&
					m_leftClickCount == 2 && !m_bPanMoved;
				stopPanning();
				m_bLeftButtonDown = false;
				m_bPanMoved = false;
				m_leftClickCount = 0;

				if (toggleOnDoubleClick) {
					toggleFullscreen();
				}
				break;
			}
			case SDL_RENDER_TARGETS_RESET: {
				// D3D9 reports this after a resize reset. Re-upload the latest CPU
				// frame so all three YUV planes and the cached draw state are current.
				if (m_videoTexture != NULL && m_videoTextureHasFrame) {
					m_videoTextureHasFrame = false;
					InterlockedExchange(&m_yuvReady, 1);
				}
				m_cleanFeed.HandleRendererReset();
				break;
			}
			case SDL_RENDER_DEVICE_RESET: {
				// A full renderer reset invalidates both video and ImGui font textures.
				// Retire backend resources in their owning ImGui context before retrying.
				m_imgui.RecreateRendererDeviceObjects();
				m_videoTextureHasFrame = false;
				m_cleanFeed.HandleRendererReset();
				recreateVideoTexture();
				recreateCleanFeedTexture();
				break;
			}
			case SDL_MOUSEWHEEL: {
				if (m_bConnected && m_videoTexture != NULL && m_videoWidth > 0 && m_videoHeight > 0) {
					float wheelDelta = event.wheel.preciseY;
					if (wheelDelta == 0.0f) {
						wheelDelta = (float)event.wheel.y;
					}
					if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
						wheelDelta = -wheelDelta;
					}

					// Keep pointer anchoring correct when window and renderer pixels differ.
					float mouseX = 0.0f;
					float mouseY = 0.0f;
					windowToRendererCoordinates((float)event.wheel.mouseX,
						(float)event.wheel.mouseY, mouseX, mouseY);

					applyWheelZoom(wheelDelta, mouseX, mouseY);
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

				// Stop callback producers and release any queued payloads before SDL exits.
				stopServerForShutdown();
				bEndLoop = TRUE;
				break;
			}
			}
		} // End of event polling loop
		if (bEndLoop) {
			break;
		}

		// Connection callbacks run off the SDL thread. Apply their requested view
		// reset here so renderer geometry is only touched by the render thread.
		if (InterlockedExchange(&m_zoomResetPending, 0) == 1) {
			m_rotationAngle = 0;
			resetZoom();
			calculateDisplayRect();
		}

		// The ImGui settings are persisted and may have changed on the previous
		// frame. Keep the companion surface and main-window affinity in sync.
		syncScreenCastOutput();

		// Handle disconnect transition (show black screen briefly)
		if (m_bDisconnecting) {
			DWORD elapsed = GetTickCount() - m_dwDisconnectStartTime;
			if (elapsed >= DISCONNECT_NOTICE_MS) {
				m_bDisconnecting = false;
			}
		}

		// Apply receiver identity/security changes together. The PIN itself is
		// generated in memory on every restart and is never persisted.
		{
			const char* currentName = m_imgui.GetDeviceName();
			const char* desiredName = (currentName && currentName[0] != '\0')
				? currentName : m_serverName;
			bool desiredPinEnabled = m_imgui.IsAirPlayPinEnabled();
			bool settingsDiffer = strcmp(desiredName, m_serverName) != 0 ||
				desiredPinEnabled != activePinEnabled;

			if (!settingsDiffer) {
				receiverSettingsPendingRestart = false;
			} else if (!receiverSettingsPendingRestart ||
				strcmp(desiredName, pendingServerName) != 0 ||
				desiredPinEnabled != pendingPinEnabled) {
				strncpy_s(pendingServerName, sizeof(pendingServerName), desiredName, _TRUNCATE);
				pendingPinEnabled = desiredPinEnabled;
				receiverSettingsChangeTime = GetTickCount();
				receiverSettingsPendingRestart = true;
			}

			if (receiverSettingsPendingRestart && !m_bConnected &&
				GetTickCount() - receiverSettingsChangeTime >= 1000) {
				setServerName(pendingServerName);
				cancelPinApproval();
				bool pinReady = !pendingPinEnabled || generateSessionAirPlayPin();
				if (!pinReady) {
					printf("Could not generate the AirPlay session PIN\n");
					receiverSettingsPendingRestart = false;
				} else {
					if (!pendingPinEnabled) {
						SecureZeroMemory(m_sessionAirPlayPin, sizeof(m_sessionAirPlayPin));
					}
					m_server.restart(m_serverName,
						pendingPinEnabled ? m_sessionAirPlayPin : NULL);
					activePinEnabled = pendingPinEnabled;
					receiverSettingsPendingRestart = false;
				}
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
				recreateVideoTexture();
				m_cleanFeed.InvalidateVideoTexture();
				recreateCleanFeedTexture();
			}
		}

		// === GPU RENDER PIPELINE ===

		// 1. Clear renderer to black
		SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
		SDL_RenderClear(m_renderer);

		// A device reset can make creation fail transiently. Flush the queued clear
		// so SDL activates/resets the renderer before retrying texture creation.
		if (m_videoTexture == NULL && m_videoWidth > 0 && m_videoHeight > 0) {
			if (SDL_RenderFlush(m_renderer) != 0) {
				printf("SDL_RenderFlush failed while recovering texture: %s\n", SDL_GetError());
			}
			recreateVideoTexture();
		}

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
			bool intervalReady = !m_videoTextureHasFrame ||
				(msSinceLastNew >= m_targetFrameIntervalMs * 0.90);

			if (intervalReady && InterlockedCompareExchange(&m_yuvReady, 0, 0) == 1) {
				CAutoLock oLock(m_mutexVideo, "uploadVideoTexture");
				LONG readIdx = InterlockedCompareExchange(&m_yuvReadIdx, 0, 0);
				if (readIdx >= 0 && readIdx <= 1 && m_videoTexture != NULL &&
					m_yuvBuffer[readIdx][0] != NULL &&
					m_yuvBuffer[readIdx][1] != NULL &&
					m_yuvBuffer[readIdx][2] != NULL) {

					// Capture frame arrival timestamp before upload
					frameArrivalQpc = InterlockedCompareExchange64(&m_qpcFrameArrival, 0, 0);

					// Upload raw YUV planes to GPU (GPU shader does colorspace conversion + scaling)
					int updateResult = SDL_UpdateYUVTexture(m_videoTexture, NULL,
						m_yuvBuffer[readIdx][0], m_yuvPitch[0],
						m_yuvBuffer[readIdx][1], m_yuvPitch[1],
						m_yuvBuffer[readIdx][2], m_yuvPitch[2]);
					if (updateResult != 0) {
						printf("SDL_UpdateYUVTexture failed: %s\n", SDL_GetError());
						// Keep m_yuvReady set so this frame is retried next iteration.
						frameArrivalQpc = 0;
					} else {
						m_videoTextureHasFrame = true;
						// Clean-feed failures are deliberately isolated: the receiver must
						// keep playing even if OBS output needs to recreate a texture.
						if (m_cleanFeed.IsEnabled()) {
							m_cleanFeed.UploadYUV(m_videoWidth, m_videoHeight,
								m_yuvBuffer[readIdx][0], m_yuvPitch[0],
								m_yuvBuffer[readIdx][1], m_yuvPitch[1],
								m_yuvBuffer[readIdx][2], m_yuvPitch[2]);
						}
						InterlockedExchange(&m_yuvReady, 0);
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
		}

		// 3. Render video texture (GPU handles scaling)
		// Disconnect notices render over a clean background, never a stale cast frame.
		if (m_videoTexture != NULL && m_videoTextureHasFrame && m_videoWidth > 0 &&
			m_bConnected) {
			if (SDL_RenderCopyEx(m_renderer, m_videoTexture, NULL, &m_displayRect,
				(double)m_rotationAngle, NULL, SDL_FLIP_NONE) != 0) {
				printf("SDL_RenderCopyEx failed: %s\n", SDL_GetError());
				m_videoTextureHasFrame = false;
				InterlockedExchange(&m_yuvReady, 1);
			}
		}

		// A release requested before this UI frame may run after it is presented.
		// A release requested by a button during this frame waits for the next one,
		// because ImGui draw data for the closing PIN popup may still contain digits.
		bool releasePinCaptureAfterPresent = m_pinCaptureExclusionReleasePending;

		// 4. Render ImGui overlay on top
		m_imgui.NewFrame();
		if (m_bConnected) {
			if (m_bPictureInPicture) {
				bool exitPictureInPicture = false;
				m_imgui.RenderPictureInPictureControls(&exitPictureInPicture);
				if (exitPictureInPicture) {
					setPictureInPictureMode(false);
				}
			} else if (!m_bShowPerfGraphs) {
				// Controls and diagnostics are mutually exclusive so the cast remains visible.
				bool resetView = false;
				bool rotateView = false;
				bool toggleCapturePrivacy = false;
				bool togglePictureInPicture = false;
				m_imgui.RenderOverlay(m_serverName, m_bConnected, m_connectedDeviceName,
					m_videoWidth, m_videoHeight, m_displayFPS, m_currentBitrateMbps,
					m_totalFrames, m_droppedFrames, m_zoomLevel, m_rotationAngle,
					&resetView, &rotateView, m_capturePrivacyActive, &toggleCapturePrivacy,
					m_cleanFeed.IsCaptureExclusionAvailable(), m_cleanFeed.IsReady(),
					m_bPictureInPicture, &togglePictureInPicture);
				if (toggleCapturePrivacy) {
					setCapturePrivacyMode(!m_capturePrivacyActive);
				}
				if (togglePictureInPicture) {
					setPictureInPictureMode(true);
				}
				if (rotateView) {
					m_rotationAngle = (m_rotationAngle + 90) % 360;
					calculateDisplayRect();
				}
				if (resetView) {
					m_rotationAngle = 0;
					resetZoom();
					calculateDisplayRect();
				}
			}
		} else if (m_bDisconnecting) {
			// Disconnecting - show disconnect message
			DWORD elapsed = GetTickCount() - m_dwDisconnectStartTime;
			float visibility = 1.0f;
			if (elapsed < DISCONNECT_FADE_IN_MS) {
				visibility = (float)elapsed / (float)DISCONNECT_FADE_IN_MS;
			} else if (elapsed > DISCONNECT_NOTICE_MS - DISCONNECT_FADE_OUT_MS) {
				visibility = (float)(DISCONNECT_NOTICE_MS - elapsed) / (float)DISCONNECT_FADE_OUT_MS;
			}
			m_imgui.RenderDisconnectMessage(m_connectedDeviceName, ClampFloat(visibility, 0.0f, 1.0f));
		} else {
			// Disconnected - show home screen
			m_imgui.RenderHomeScreen(m_serverName, m_server.isRunning());
		}
		renderPinApprovalPopup(lastPinApprovalGeneration);

		// 4b. Render performance graphs if F1 toggled on
		if (m_bShowPerfGraphs && m_bConnected) {
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

			m_imgui.RenderPerfGraphs(perf, &m_bShowPerfGraphs);
		}

		// Capture-only privacy keeps the local receiver visible while forcing the
		// separate clean-feed capture target to a safe black frame.
		if (m_cleanFeed.IsEnabled()) {
			int renderWidth = m_windowWidth;
			int renderHeight = m_windowHeight;
			if (m_renderer != NULL) {
				int outputWidth = 0;
				int outputHeight = 0;
				if (SDL_GetRendererOutputSize(m_renderer, &outputWidth, &outputHeight) == 0 &&
					outputWidth > 0 && outputHeight > 0) {
					renderWidth = outputWidth;
					renderHeight = outputHeight;
				}
			}
			m_cleanFeed.Render(m_displayRect, calculateScreenCastCaptureBounds(),
				renderWidth, renderHeight, m_rotationAngle,
				m_bConnected, m_bWindowVisible, m_bMainWindowMinimized,
				m_capturePrivacyActive);
		}

		// ImGui::Render() + GPU draw via SDL2Renderer backend
		m_imgui.Render();

		// 5. Present (no VSync - immediate display for lowest latency)
		SDL_RenderPresent(m_renderer);
		if (releasePinCaptureAfterPresent) {
			// The currently presented frame contains no PIN, so capture exclusion
			// can now be removed without exposing a retained PIN frame.
			m_cleanFeed.SetTemporaryMainWindowCaptureExclusion(false);
			m_pinCaptureExclusionActive = false;
			m_pinCaptureExclusionReleasePending = false;
		}

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

		// Keep the pointer visible anywhere controls are usable. During an
		// unobstructed cast, enforce hidden state every frame because the ImGui
		// SDL backend may restore the OS cursor while starting a new frame.
		bool allowCursorHide = m_bConnected &&
			m_imgui.GetOverlayState() == OVERLAY_HIDDEN && !m_bShowPerfGraphs &&
			!m_bPanning && !m_imgui.WantCaptureMouse();
		DWORD cursorIdleMs = m_lastMouseMoveTime > 0
			? GetTickCount() - m_lastMouseMoveTime : 0;
		if (allowCursorHide && cursorIdleMs >= CURSOR_HIDE_DELAY_MS) {
			m_bCursorHidden = true;
			SDL_ShowCursor(SDL_DISABLE);
		} else if (!allowCursorHide && m_bCursorHidden) {
			m_bCursorHidden = false;
			SDL_ShowCursor(SDL_ENABLE);
		}
		if (m_bPanning && m_panCursor != NULL) {
			SDL_SetCursor(m_panCursor);
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

		// Wait for main thread to complete the resize so we can use this frame
		// instead of dropping it (prevents black screen during resolution changes)
		DWORD waitStart = GetTickCount();
		while (m_bResizing || (int)data->width != m_videoWidth || (int)data->height != m_videoHeight) {
			Sleep(1);
			if (GetTickCount() - waitStart > 500) {
				return;  // Timeout - give up on this frame
			}
		}
		// Fall through to copy this frame into the newly allocated buffers
	}

	// CALLBACK THREAD: Copy raw YUV420P planes into double buffer (fast memcpy)
	// GPU does BT.709 conversion via shader during SDL_UpdateYUVTexture
	{
		CAutoLock oLock(m_mutexVideo, "outputVideo");

		if (m_bResizing) {
			return;
		}

		LONG writeIdx = InterlockedCompareExchange(&m_yuvWriteIdx, 0, 0);
		if (writeIdx < 0 || writeIdx > 1 ||
			m_yuvBuffer[writeIdx][0] == NULL ||
			m_yuvBuffer[writeIdx][1] == NULL ||
			m_yuvBuffer[writeIdx][2] == NULL) {
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
	// dataTotalLen is post-decode YUV memory (roughly 1.5 bytes per pixel),
	// not what crossed the network. Report the compressed H.264 payload instead.
	m_totalBytes += data->encodedDataLen;

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
	DWORD bitrateElapsedMs = currentTime - m_bitrateStartTime;
	if (bitrateElapsedMs >= 1000) {
		unsigned long long bytesDelta = m_totalBytes >= m_lastBitrateTotalBytes
			? m_totalBytes - m_lastBitrateTotalBytes : m_totalBytes;
		m_currentBitrateMbps = (float)((double)bytesDelta * 8.0 /
			((double)bitrateElapsedMs * 1000.0));
		m_lastBitrateTotalBytes = m_totalBytes;
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
	m_window = SDL_CreateWindow("AirPlay Receiver",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		width, height,
		SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

	if (m_window == NULL) {
		printf("Could not create window: %s\n", SDL_GetError());
		return;
	}
	SDL_SetWindowMinimumSize(m_window, MIN_WINDOW_WIDTH, MIN_WINDOW_HEIGHT);

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

void CSDLPlayer::resizeWindowForVideo(int width, int height)
{
	if (m_window == NULL || width <= 0 || height <= 0) {
		return;
	}

	int displayIndex = SDL_GetWindowDisplayIndex(m_window);
	SDL_Rect usableBounds = {};
	bool haveBounds = displayIndex >= 0 &&
		SDL_GetDisplayUsableBounds(displayIndex, &usableBounds) == 0;
	if (!haveBounds && displayIndex >= 0) {
		haveBounds = SDL_GetDisplayBounds(displayIndex, &usableBounds) == 0;
	}
	if (!haveBounds || usableBounds.w <= 0 || usableBounds.h <= 0) {
		printf("Could not get usable bounds for window display: %s\n", SDL_GetError());
		return;
	}

	int borderTop = 0;
	int borderLeft = 0;
	int borderBottom = 0;
	int borderRight = 0;
	SDL_GetWindowBordersSize(m_window, &borderTop, &borderLeft,
		&borderBottom, &borderRight);
	int borderWidth = borderLeft + borderRight;
	int borderHeight = borderTop + borderBottom;

	int maxWidth = (int)((long long)usableBounds.w * 95 / 100) - borderWidth;
	int maxHeight = (int)((long long)usableBounds.h * 80 / 100) - borderHeight;
	if (maxWidth < 1) maxWidth = 1;
	if (maxHeight < 1) maxHeight = 1;

	int targetWidth = maxWidth;
	int targetHeight = (int)((long long)targetWidth * height / width);
	if (targetHeight > maxHeight) {
		targetHeight = maxHeight;
		targetWidth = (int)((long long)targetHeight * width / height);
	}

	int maximumClientWidth = usableBounds.w - borderWidth;
	int maximumClientHeight = usableBounds.h - borderHeight;
	if (maximumClientWidth < 1) maximumClientWidth = 1;
	if (maximumClientHeight < 1) maximumClientHeight = 1;
	int minimumWidth = MIN_WINDOW_WIDTH < maximumClientWidth
		? MIN_WINDOW_WIDTH : maximumClientWidth;
	int minimumHeight = MIN_WINDOW_HEIGHT < maximumClientHeight
		? MIN_WINDOW_HEIGHT : maximumClientHeight;
	if (targetWidth < minimumWidth) targetWidth = minimumWidth;
	if (targetHeight < minimumHeight) targetHeight = minimumHeight;
	if (targetWidth > maximumClientWidth) targetWidth = maximumClientWidth;
	if (targetHeight > maximumClientHeight) targetHeight = maximumClientHeight;

	int oldX = 0;
	int oldY = 0;
	int oldWidth = 0;
	int oldHeight = 0;
	SDL_GetWindowPosition(m_window, &oldX, &oldY);
	SDL_GetWindowSize(m_window, &oldWidth, &oldHeight);
	long long centerXTwice = ((long long)oldX - borderLeft) * 2 +
		oldWidth + borderWidth;
	long long centerYTwice = ((long long)oldY - borderTop) * 2 +
		oldHeight + borderHeight;

	SDL_SetWindowSize(m_window, targetWidth, targetHeight);

	// SDL applies minimum-size and DPI constraints synchronously. Cache the
	// resulting client size, not the requested size, so fullscreen restore and
	// renderer geometry cannot diverge from the native window.
	SDL_GetWindowSize(m_window, &targetWidth, &targetHeight);
	SDL_GetWindowBordersSize(m_window, &borderTop, &borderLeft,
		&borderBottom, &borderRight);
	int outerWidth = targetWidth + borderLeft + borderRight;
	int outerHeight = targetHeight + borderTop + borderBottom;
	int targetOuterX = (int)((centerXTwice - outerWidth) / 2);
	int targetOuterY = (int)((centerYTwice - outerHeight) / 2);
	int maximumOuterX = usableBounds.x + usableBounds.w - outerWidth;
	int maximumOuterY = usableBounds.y + usableBounds.h - outerHeight;
	if (maximumOuterX < usableBounds.x) maximumOuterX = usableBounds.x;
	if (maximumOuterY < usableBounds.y) maximumOuterY = usableBounds.y;
	if (targetOuterX < usableBounds.x) targetOuterX = usableBounds.x;
	if (targetOuterX > maximumOuterX) targetOuterX = maximumOuterX;
	if (targetOuterY < usableBounds.y) targetOuterY = usableBounds.y;
	if (targetOuterY > maximumOuterY) targetOuterY = maximumOuterY;
	SDL_SetWindowPosition(m_window, targetOuterX + borderLeft,
		targetOuterY + borderTop);

	m_windowWidth = targetWidth;
	m_windowHeight = targetHeight;
	m_windowedW = targetWidth;
	m_windowedH = targetHeight;
	SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
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

void CSDLPlayer::installNativeResizeHook()
{
	if (m_hwnd == NULL || m_originalWindowProc != NULL) {
		return;
	}

	if (!SetPropW(m_hwnd, NATIVE_RESIZE_PLAYER_PROPERTY, (HANDLE)this)) {
		return;
	}

	SetLastError(0);
	LONG_PTR previous = SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC,
		(LONG_PTR)NativeResizeWindowProc);
	if (previous == 0 && GetLastError() != 0) {
		RemovePropW(m_hwnd, NATIVE_RESIZE_PLAYER_PROPERTY);
		return;
	}
	m_originalWindowProc = (WNDPROC)previous;
}

void CSDLPlayer::removeNativeResizeHook()
{
	if (m_hwnd == NULL) {
		return;
	}

	KillTimer(m_hwnd, NATIVE_RESIZE_TIMER_ID);
	m_nativeResizeActive = false;
	m_nativeResizeRendering = false;
	m_lastNativeResizeRenderTime = 0;
	if (m_originalWindowProc != NULL) {
		SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)m_originalWindowProc);
		m_originalWindowProc = NULL;
	}
	RemovePropW(m_hwnd, NATIVE_RESIZE_PLAYER_PROPERTY);
}

void CSDLPlayer::renderDuringNativeResize()
{
	if (!m_nativeResizeActive || m_nativeResizeRendering || m_window == NULL ||
		m_renderer == NULL || !m_bWindowVisible || m_bMainWindowMinimized) {
		return;
	}

	DWORD now = GetTickCount();
	if (m_lastNativeResizeRenderTime != 0 &&
		now - m_lastNativeResizeRenderTime < NATIVE_RESIZE_FRAME_INTERVAL_MS) {
		return;
	}
	m_lastNativeResizeRenderTime = now;
	m_nativeResizeRendering = true;

	int width = 0;
	int height = 0;
	SDL_GetWindowSize(m_window, &width, &height);
	if (width > 0 && height > 0) {
		handleLiveResize(width, height);
	}

	// The normal event loop is paused by Windows during a border drag. Upload
	// the newest decoded frame here so resizing does not turn the video into a
	// frozen screenshot.
	if (m_videoTexture != NULL &&
		InterlockedCompareExchange(&m_yuvReady, 0, 0) == 1) {
		CAutoLock oLock(m_mutexVideo, "nativeResizeUpload");
		LONG readIdx = InterlockedCompareExchange(&m_yuvReadIdx, 0, 0);
		if (readIdx >= 0 && readIdx <= 1 &&
			m_yuvBuffer[readIdx][0] != NULL &&
			m_yuvBuffer[readIdx][1] != NULL &&
			m_yuvBuffer[readIdx][2] != NULL &&
			SDL_UpdateYUVTexture(m_videoTexture, NULL,
				m_yuvBuffer[readIdx][0], m_yuvPitch[0],
				m_yuvBuffer[readIdx][1], m_yuvPitch[1],
				m_yuvBuffer[readIdx][2], m_yuvPitch[2]) == 0) {
			m_videoTextureHasFrame = true;
			InterlockedExchange(&m_yuvReady, 0);
		}
	}

	SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
	SDL_RenderClear(m_renderer);
	if (m_bConnected && m_videoTexture != NULL && m_videoTextureHasFrame &&
		m_videoWidth > 0 && m_videoHeight > 0) {
		SDL_RenderCopyEx(m_renderer, m_videoTexture, NULL, &m_displayRect,
			(double)m_rotationAngle, NULL, SDL_FLIP_NONE);
	}

	// WM_ENTERSIZEMOVE pauses loopEvents(), so presenting only the video here
	// would erase the ImGui home screen while the window is being dragged.
	// Draw the same static UI layer from the native timer to keep the receiver
	// visually stable during both moves and resizes.
	m_imgui.NewFrame();
	if (m_bConnected) {
		if (m_bPictureInPicture) {
			m_imgui.RenderPictureInPictureControls(NULL);
		} else if (!m_bShowPerfGraphs) {
			bool ignoredReset = false;
			bool ignoredRotate = false;
			m_imgui.RenderOverlay(m_serverName, m_bConnected, m_connectedDeviceName,
				m_videoWidth, m_videoHeight, m_displayFPS, m_currentBitrateMbps,
				m_totalFrames, m_droppedFrames, m_zoomLevel, m_rotationAngle,
				&ignoredReset, &ignoredRotate, m_capturePrivacyActive, NULL,
				m_cleanFeed.IsCaptureExclusionAvailable(), m_cleanFeed.IsReady());
		}
	} else if (m_bDisconnecting) {
		m_imgui.RenderDisconnectMessage(m_connectedDeviceName);
	} else {
		m_imgui.RenderHomeScreen(m_serverName, m_server.isRunning());
	}
	m_imgui.Render();
	SDL_RenderPresent(m_renderer);

	m_nativeResizeRendering = false;
}

void CSDLPlayer::setCapturePrivacyMode(bool enabled)
{
	if (enabled && !m_bConnected) {
		return;
	}
	if (m_capturePrivacyActive == enabled) {
		return;
	}

	if (!m_cleanFeed.SetPrivacyModeMainWindowCaptureExclusion(enabled)) {
		return;
	}
	m_capturePrivacyActive = enabled;
	// Keep controls available locally so privacy can be disabled without a
	// global hotkey. Capture software cannot see this excluded main window.
	m_imgui.ShowOverlay();
}

double CSDLPlayer::pictureInPictureAspectRatio() const
{
	int width = m_videoWidth > 0 ? m_videoWidth : 16;
	int height = m_videoHeight > 0 ? m_videoHeight : 9;
	if (m_rotationAngle == 90 || m_rotationAngle == 270) {
		int swap = width;
		width = height;
		height = swap;
	}
	return height > 0 ? (double)width / (double)height : 16.0 / 9.0;
}

void CSDLPlayer::constrainPictureInPictureRect(WPARAM sizingEdge, RECT* windowRect) const
{
	if (!m_bPictureInPicture || windowRect == NULL) {
		return;
	}

	double aspect = pictureInPictureAspectRatio();
	if (aspect <= 0.0) {
		return;
	}
	int width = windowRect->right - windowRect->left;
	int height = windowRect->bottom - windowRect->top;
	if (width <= 0 || height <= 0) {
		return;
	}

	bool leftEdge = sizingEdge == WMSZ_LEFT || sizingEdge == WMSZ_TOPLEFT ||
		sizingEdge == WMSZ_BOTTOMLEFT;
	bool rightEdge = sizingEdge == WMSZ_RIGHT || sizingEdge == WMSZ_TOPRIGHT ||
		sizingEdge == WMSZ_BOTTOMRIGHT;
	bool topEdge = sizingEdge == WMSZ_TOP || sizingEdge == WMSZ_TOPLEFT ||
		sizingEdge == WMSZ_TOPRIGHT;
	bool bottomEdge = sizingEdge == WMSZ_BOTTOM || sizingEdge == WMSZ_BOTTOMLEFT ||
		sizingEdge == WMSZ_BOTTOMRIGHT;
	bool corner = (leftEdge || rightEdge) && (topEdge || bottomEdge);

	int widthFromHeight = (int)((double)height * aspect + 0.5);
	int heightFromWidth = (int)((double)width / aspect + 0.5);
	bool driveFromWidth = !topEdge && !bottomEdge;
	if (corner) {
		driveFromWidth = abs(heightFromWidth - height) <= abs(widthFromHeight - width);
	}
	int adjustedWidth = driveFromWidth ? width : widthFromHeight;
	int adjustedHeight = driveFromWidth ? heightFromWidth : height;
	int minimumWidth = aspect >= 1.0
		? PIP_MIN_LONG_EDGE : (int)((double)PIP_MIN_LONG_EDGE * aspect + 0.5);
	int minimumHeight = aspect >= 1.0
		? (int)((double)PIP_MIN_LONG_EDGE / aspect + 0.5) : PIP_MIN_LONG_EDGE;
	if (adjustedWidth < minimumWidth || adjustedHeight < minimumHeight) {
		adjustedWidth = minimumWidth;
		adjustedHeight = minimumHeight;
	}

	if (leftEdge) windowRect->left = windowRect->right - adjustedWidth;
	else if (rightEdge) windowRect->right = windowRect->left + adjustedWidth;
	else {
		int centerX = (windowRect->left + windowRect->right) / 2;
		windowRect->left = centerX - adjustedWidth / 2;
		windowRect->right = windowRect->left + adjustedWidth;
	}
	if (topEdge) windowRect->top = windowRect->bottom - adjustedHeight;
	else if (bottomEdge) windowRect->bottom = windowRect->top + adjustedHeight;
	else {
		int centerY = (windowRect->top + windowRect->bottom) / 2;
		windowRect->top = centerY - adjustedHeight / 2;
		windowRect->bottom = windowRect->top + adjustedHeight;
	}
}

LRESULT CSDLPlayer::pictureInPictureHitTest(HWND window, LPARAM position) const
{
	RECT bounds = {};
	if (!m_bPictureInPicture || !GetWindowRect(window, &bounds)) {
		return HTCLIENT;
	}
	int x = (int)(short)LOWORD(position) - bounds.left;
	int y = (int)(short)HIWORD(position) - bounds.top;
	int width = bounds.right - bounds.left;
	int height = bounds.bottom - bounds.top;
	int resizeGrip = GetSystemMetrics(SM_CXSIZEFRAME);
	if (resizeGrip < 7) resizeGrip = 7;

	bool left = x >= 0 && x < resizeGrip;
	bool right = x < width && x >= width - resizeGrip;
	bool top = y >= 0 && y < resizeGrip;
	bool bottom = y < height && y >= height - resizeGrip;
	if (top && left) return HTTOPLEFT;
	if (top && right) return HTTOPRIGHT;
	if (bottom && left) return HTBOTTOMLEFT;
	if (bottom && right) return HTBOTTOMRIGHT;
	if (left) return HTLEFT;
	if (right) return HTRIGHT;
	if (top) return HTTOP;
	if (bottom) return HTBOTTOM;

	// Keep the hover-only close button interactive. The rest of the invisible
	// top strip acts as a caption so the frameless PiP remains draggable.
	if (y >= resizeGrip && y < 42 && x < width - 48) {
		return HTCAPTION;
	}
	return HTCLIENT;
}

void CSDLPlayer::resizePictureInPictureToAspect()
{
	if (!m_bPictureInPicture || m_window == NULL) {
		return;
	}
	double aspect = pictureInPictureAspectRatio();
	int minimumWidth = aspect >= 1.0
		? PIP_MIN_LONG_EDGE : (int)((double)PIP_MIN_LONG_EDGE * aspect + 0.5);
	int minimumHeight = aspect >= 1.0
		? (int)((double)PIP_MIN_LONG_EDGE / aspect + 0.5) : PIP_MIN_LONG_EDGE;
	SDL_SetWindowMinimumSize(m_window, minimumWidth, minimumHeight);
	int oldWidth = 0;
	int oldHeight = 0;
	int oldX = 0;
	int oldY = 0;
	SDL_GetWindowSize(m_window, &oldWidth, &oldHeight);
	SDL_GetWindowPosition(m_window, &oldX, &oldY);
	int longEdge = oldWidth > oldHeight ? oldWidth : oldHeight;
	if (longEdge < PIP_MIN_LONG_EDGE) longEdge = PIP_MIN_LONG_EDGE;
	int targetWidth = longEdge;
	int targetHeight = (int)((double)targetWidth / aspect + 0.5);
	if (aspect < 1.0) {
		targetHeight = longEdge;
		targetWidth = (int)((double)targetHeight * aspect + 0.5);
	}
	SDL_SetWindowSize(m_window, targetWidth, targetHeight);
	SDL_GetWindowSize(m_window, &targetWidth, &targetHeight);
	SDL_SetWindowPosition(m_window,
		oldX + (oldWidth - targetWidth) / 2,
		oldY + (oldHeight - targetHeight) / 2);
	m_windowWidth = targetWidth;
	m_windowHeight = targetHeight;
	calculateDisplayRect();
}

void CSDLPlayer::setPictureInPictureMode(bool enabled)
{
	if (m_window == NULL || m_bPictureInPicture == enabled ||
		(enabled && !m_bConnected) || m_bResizing) {
		return;
	}

	if (enabled && m_bFullscreen) {
		toggleFullscreen();
		if (m_bFullscreen) {
			return;
		}
	}

	m_bResizing = true;
	stopPanning();
	m_bLeftButtonDown = false;
	m_bPanMoved = false;
	m_leftClickCount = 0;

	if (enabled) {
		Uint32 flags = SDL_GetWindowFlags(m_window);
		m_pipRestoreMaximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
		if (m_pipRestoreMaximized) {
			SDL_RestoreWindow(m_window);
		}
		SDL_GetWindowPosition(m_window, &m_pipRestoreX, &m_pipRestoreY);
		SDL_GetWindowSize(m_window, &m_pipRestoreW, &m_pipRestoreH);
		if (m_pipRestoreW < 1) m_pipRestoreW = MIN_WINDOW_WIDTH;
		if (m_pipRestoreH < 1) m_pipRestoreH = MIN_WINDOW_HEIGHT;

		double aspect = pictureInPictureAspectRatio();
		int targetWidth = PIP_LONG_EDGE;
		int targetHeight = (int)((double)targetWidth / aspect + 0.5);
		if (aspect < 1.0) {
			targetHeight = PIP_LONG_EDGE;
			targetWidth = (int)((double)targetHeight * aspect + 0.5);
		}
		int minimumWidth = aspect >= 1.0
			? PIP_MIN_LONG_EDGE : (int)((double)PIP_MIN_LONG_EDGE * aspect + 0.5);
		int minimumHeight = aspect >= 1.0
			? (int)((double)PIP_MIN_LONG_EDGE / aspect + 0.5) : PIP_MIN_LONG_EDGE;

		m_bPictureInPicture = true;
		m_bShowPerfGraphs = false;
		m_imgui.SetPictureInPictureMode(true);
		SDL_SetWindowBordered(m_window, SDL_FALSE);
		SDL_SetWindowResizable(m_window, SDL_TRUE);
		SDL_SetWindowMinimumSize(m_window, minimumWidth, minimumHeight);
		SDL_SetWindowAlwaysOnTop(m_window, SDL_TRUE);
		SDL_SetWindowSize(m_window, targetWidth, targetHeight);
		SDL_GetWindowSize(m_window, &targetWidth, &targetHeight);

		int displayIndex = SDL_GetWindowDisplayIndex(m_window);
		SDL_Rect usableBounds = {};
		if (displayIndex >= 0 &&
			SDL_GetDisplayUsableBounds(displayIndex, &usableBounds) == 0) {
			int borderTop = 0;
			int borderLeft = 0;
			int borderBottom = 0;
			int borderRight = 0;
			SDL_GetWindowBordersSize(m_window, &borderTop, &borderLeft,
				&borderBottom, &borderRight);
			int outerWidth = targetWidth + borderLeft + borderRight;
			int outerHeight = targetHeight + borderTop + borderBottom;
			int outerX = usableBounds.x + usableBounds.w - outerWidth - PIP_EDGE_MARGIN;
			int outerY = usableBounds.y + usableBounds.h - outerHeight - PIP_EDGE_MARGIN;
			if (outerX < usableBounds.x) outerX = usableBounds.x;
			if (outerY < usableBounds.y) outerY = usableBounds.y;
			SDL_SetWindowPosition(m_window, outerX + borderLeft, outerY + borderTop);
		}
		SDL_SetWindowTitle(m_window, "AirPlay Receiver - Picture in Picture");
	} else {
		m_bPictureInPicture = false;
		SDL_SetWindowAlwaysOnTop(m_window, SDL_FALSE);
		SDL_SetWindowBordered(m_window, SDL_TRUE);
		SDL_SetWindowResizable(m_window, SDL_TRUE);
		m_imgui.SetPictureInPictureMode(false);
		SDL_SetWindowPosition(m_window, m_pipRestoreX, m_pipRestoreY);
		SDL_SetWindowSize(m_window, m_pipRestoreW, m_pipRestoreH);
		if (m_pipRestoreMaximized) {
			SDL_MaximizeWindow(m_window);
		}
		SDL_SetWindowTitle(m_window, "AirPlay Receiver");
		m_windowedX = m_pipRestoreX;
		m_windowedY = m_pipRestoreY;
		m_windowedW = m_pipRestoreW;
		m_windowedH = m_pipRestoreH;
		m_imgui.ShowOverlay();
	}

	SDL_GetWindowSize(m_window, &m_windowWidth, &m_windowHeight);
	calculateDisplayRect();
	m_bResizing = false;
}

void CSDLPlayer::toggleFullscreen()
{
	stopPanning();
	m_bLeftButtonDown = false;
	m_bPanMoved = false;
	m_leftClickCount = 0;

	if (m_window == NULL) {
		return;
	}

	if (m_bResizing) {
		return;
	}
	if (m_bPictureInPicture) {
		setPictureInPictureMode(false);
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
		if (SDL_SetWindowFullscreen(m_window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
			printf("Could not enter fullscreen: %s\n", SDL_GetError());
			SetCursorPos(cursorPos.x, cursorPos.y);
			m_bResizing = false;
			return;
		}

		// Update window dimensions
		SDL_GetWindowSize(m_window, &m_windowWidth, &m_windowHeight);

		// Recalculate display rect for fullscreen
		calculateDisplayRect();

		m_bFullscreen = true;
	}
	else {
		// Exit fullscreen
		if (SDL_SetWindowFullscreen(m_window, 0) != 0) {
			printf("Could not exit fullscreen: %s\n", SDL_GetError());
			SetCursorPos(cursorPos.x, cursorPos.y);
			m_bResizing = false;
			return;
		}

		// Restore window position and size
		SDL_SetWindowPosition(m_window, m_windowedX, m_windowedY);
		SDL_SetWindowSize(m_window, m_windowedW, m_windowedH);

		SDL_GetWindowSize(m_window, &m_windowWidth, &m_windowHeight);
		m_windowedW = m_windowWidth;
		m_windowedH = m_windowHeight;
		SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);

		// Recalculate display rect for windowed mode
		calculateDisplayRect();

		m_bFullscreen = false;
	}

	// Restore cursor position after fullscreen toggle
	SetCursorPos(cursorPos.x, cursorPos.y);

	m_bResizing = false;
}

SDL_Rect CSDLPlayer::calculateFittedVideoBounds() const
{
	SDL_Rect bounds = {};
	int renderWidth = m_windowWidth;
	int renderHeight = m_windowHeight;
	if (m_renderer != NULL) {
		int outputWidth = 0;
		int outputHeight = 0;
		if (SDL_GetRendererOutputSize(m_renderer, &outputWidth, &outputHeight) == 0 &&
			outputWidth > 0 && outputHeight > 0) {
			renderWidth = outputWidth;
			renderHeight = outputHeight;
		}
	}

	if (m_videoWidth <= 0 || m_videoHeight <= 0) {
		bounds.w = renderWidth;
		bounds.h = renderHeight;
		return bounds;
	}

	// For 90/270 rotation, the effective video dimensions are swapped
	bool rotated = (m_rotationAngle == 90 || m_rotationAngle == 270);
	int effectiveW = rotated ? m_videoHeight : m_videoWidth;
	int effectiveH = rotated ? m_videoWidth : m_videoHeight;

	// Use cross-multiplication to compare aspect ratios without float rounding
	long long videoCross = (long long)effectiveW * renderHeight;
	long long windowCross = (long long)renderWidth * effectiveH;

	// If the aspect ratios are very close (within 1%), just fill the entire window
	long long diff = videoCross - windowCross;
	if (diff < 0) diff = -diff;
	long long threshold = windowCross / 100;
	if (diff <= threshold) {
		bounds.w = renderWidth;
		bounds.h = renderHeight;
		return bounds;
	}

	int displayWidth, displayHeight;

	if (videoCross > windowCross) {
		// Video is wider than window - fit to width (letterbox top/bottom)
		displayWidth = renderWidth;
		displayHeight = (int)(((long long)renderWidth * effectiveH + effectiveW / 2) / effectiveW);
	} else {
		// Video is taller than window - fit to height (pillarbox left/right)
		displayHeight = renderHeight;
		displayWidth = (int)(((long long)renderHeight * effectiveW + effectiveH / 2) / effectiveH);
	}

	bounds.x = (renderWidth - displayWidth) / 2;
	bounds.y = (renderHeight - displayHeight) / 2;
	bounds.w = displayWidth;
	bounds.h = displayHeight;
	return bounds;
}

SDL_Rect CSDLPlayer::calculateZoomedVideoBounds() const
{
	SDL_Rect fitted = calculateFittedVideoBounds();
	if (m_videoWidth <= 0 || m_videoHeight <= 0) {
		return fitted;
	}

	float zoom = ClampFloat(m_zoomLevel, MIN_VIDEO_ZOOM, MAX_VIDEO_ZOOM);
	int zoomedWidth = (int)lroundf((float)fitted.w * zoom);
	int zoomedHeight = (int)lroundf((float)fitted.h * zoom);
	if (zoomedWidth < 1) zoomedWidth = 1;
	if (zoomedHeight < 1) zoomedHeight = 1;

	float extraWidth = (float)(zoomedWidth - fitted.w);
	float extraHeight = (float)(zoomedHeight - fitted.h);
	float centerX = (float)fitted.x + (float)fitted.w * 0.5f +
		ClampFloat(m_zoomPanX, -1.0f, 1.0f) * extraWidth * 0.5f;
	float centerY = (float)fitted.y + (float)fitted.h * 0.5f +
		ClampFloat(m_zoomPanY, -1.0f, 1.0f) * extraHeight * 0.5f;

	SDL_Rect zoomed = {};
	zoomed.x = (int)lroundf(centerX - (float)zoomedWidth * 0.5f);
	zoomed.y = (int)lroundf(centerY - (float)zoomedHeight * 0.5f);
	zoomed.w = zoomedWidth;
	zoomed.h = zoomedHeight;
	return zoomed;
}

void CSDLPlayer::calculateDisplayRect()
{
	SDL_Rect visibleBounds = calculateZoomedVideoBounds();
	bool rotated = (m_rotationAngle == 90 || m_rotationAngle == 270);

	// SDL_RenderCopyEx rotates around the destination center. For quarter-turns,
	// convert the desired visible bounds back to the pre-rotation destination.
	m_displayRect.w = rotated ? visibleBounds.h : visibleBounds.w;
	m_displayRect.h = rotated ? visibleBounds.w : visibleBounds.h;
	m_displayRect.x = visibleBounds.x + (visibleBounds.w - m_displayRect.w) / 2;
	m_displayRect.y = visibleBounds.y + (visibleBounds.h - m_displayRect.h) / 2;
}

bool CSDLPlayer::recreateVideoTexture()
{
	if (m_renderer == NULL || m_videoWidth <= 0 || m_videoHeight <= 0) {
		return false;
	}

	CAutoLock oLock(m_mutexVideo, "recreateVideoTexture");

	if (m_videoTexture != NULL) {
		SDL_DestroyTexture(m_videoTexture);
		m_videoTexture = NULL;
	}
	m_videoTextureHasFrame = false;

	m_videoTexture = SDL_CreateTexture(m_renderer,
		SDL_PIXELFORMAT_IYUV,
		SDL_TEXTUREACCESS_STREAMING,
		m_videoWidth, m_videoHeight);
	if (m_videoTexture == NULL) {
		printf("SDL_CreateTexture failed: %s\n", SDL_GetError());
		return false;
	}

	LONG readIdx = InterlockedCompareExchange(&m_yuvReadIdx, 0, 0);
	if (readIdx < 0 || readIdx > 1 ||
		m_yuvBuffer[readIdx][0] == NULL ||
		m_yuvBuffer[readIdx][1] == NULL ||
		m_yuvBuffer[readIdx][2] == NULL) {
		return false;
	}

	if (SDL_UpdateYUVTexture(m_videoTexture, NULL,
		m_yuvBuffer[readIdx][0], m_yuvPitch[0],
		m_yuvBuffer[readIdx][1], m_yuvPitch[1],
		m_yuvBuffer[readIdx][2], m_yuvPitch[2]) != 0) {
		printf("Initial SDL_UpdateYUVTexture failed: %s\n", SDL_GetError());
		InterlockedExchange(&m_yuvReady, 1);
		return false;
	}

	// The texture now has staging data before its first RenderCopy. This is the
	// invariant required for SDL's D3D9 resize reset to restore Y/U/V correctly.
	m_videoTextureHasFrame = true;
	InterlockedExchange(&m_yuvReady, 0);
	return true;
}

void CSDLPlayer::recreateCleanFeedTexture()
{
	if (!m_cleanFeed.IsEnabled() || m_videoWidth <= 0 || m_videoHeight <= 0) {
		return;
	}

	CAutoLock oLock(m_mutexVideo, "recreateCleanFeedTexture");
	LONG readIdx = InterlockedCompareExchange(&m_yuvReadIdx, 0, 0);
	if (readIdx < 0 || readIdx > 1 ||
		m_yuvBuffer[readIdx][0] == NULL ||
		m_yuvBuffer[readIdx][1] == NULL ||
		m_yuvBuffer[readIdx][2] == NULL) {
		return;
	}

	m_cleanFeed.UploadYUV(m_videoWidth, m_videoHeight,
		m_yuvBuffer[readIdx][0], m_yuvPitch[0],
		m_yuvBuffer[readIdx][1], m_yuvPitch[1],
		m_yuvBuffer[readIdx][2], m_yuvPitch[2]);
}

void CSDLPlayer::syncScreenCastOutput()
{
	bool enabled = m_imgui.IsScreenCastEnabled();
	m_cleanFeed.ApplySettings(enabled,
		enabled && m_imgui.ShouldHideInterfaceFromCapture());
	if (enabled && m_videoTextureHasFrame && !m_cleanFeed.HasVideoFrame()) {
		recreateCleanFeedTexture();
	}
}

SDL_Rect CSDLPlayer::calculateScreenCastCaptureBounds() const
{
	SDL_Rect bounds = {};
	int renderWidth = m_windowWidth;
	int renderHeight = m_windowHeight;
	if (m_renderer != NULL) {
		int outputWidth = 0;
		int outputHeight = 0;
		if (SDL_GetRendererOutputSize(m_renderer, &outputWidth, &outputHeight) == 0 &&
			outputWidth > 0 && outputHeight > 0) {
			renderWidth = outputWidth;
			renderHeight = outputHeight;
		}
	}
	if (renderWidth <= 0 || renderHeight <= 0) {
		return bounds;
	}

	if (!m_imgui.ShouldCropCleanFeedToVideo() || m_videoWidth <= 0 || m_videoHeight <= 0) {
		bounds.w = renderWidth;
		bounds.h = renderHeight;
		return bounds;
	}

	SDL_Rect visible = calculateZoomedVideoBounds();
	int left = visible.x > 0 ? visible.x : 0;
	int top = visible.y > 0 ? visible.y : 0;
	int right = visible.x + visible.w;
	int bottom = visible.y + visible.h;
	if (right > renderWidth) right = renderWidth;
	if (bottom > renderHeight) bottom = renderHeight;
	if (right <= left || bottom <= top) {
		return bounds;
	}
	bounds.x = left;
	bounds.y = top;
	bounds.w = right - left;
	bounds.h = bottom - top;
	return bounds;
}

void CSDLPlayer::windowToRendererCoordinates(float windowX, float windowY,
	float& rendererX, float& rendererY) const
{
	rendererX = windowX;
	rendererY = windowY;
	if (m_window == NULL || m_renderer == NULL) {
		return;
	}

	int windowW = 0;
	int windowH = 0;
	int renderW = 0;
	int renderH = 0;
	SDL_GetWindowSize(m_window, &windowW, &windowH);
	if (SDL_GetRendererOutputSize(m_renderer, &renderW, &renderH) == 0 &&
		windowW > 0 && windowH > 0 && renderW > 0 && renderH > 0) {
		rendererX *= (float)renderW / (float)windowW;
		rendererY *= (float)renderH / (float)windowH;
	}
}

void CSDLPlayer::applyWheelZoom(float wheelDelta, float mouseX, float mouseY)
{
	if (wheelDelta == 0.0f || m_videoWidth <= 0 || m_videoHeight <= 0) {
		return;
	}

	float oldZoom = ClampFloat(m_zoomLevel, MIN_VIDEO_ZOOM, MAX_VIDEO_ZOOM);
	float newZoom = ClampFloat(oldZoom * powf(VIDEO_ZOOM_STEP, wheelDelta),
		MIN_VIDEO_ZOOM, MAX_VIDEO_ZOOM);
	if (fabsf(newZoom - oldZoom) < 0.0001f) {
		return;
	}

	SDL_Rect fitted = calculateFittedVideoBounds();
	SDL_Rect current = calculateZoomedVideoBounds();
	float anchorX = ClampFloat(mouseX, (float)current.x, (float)(current.x + current.w));
	float anchorY = ClampFloat(mouseY, (float)current.y, (float)(current.y + current.h));
	float relativeX = current.w > 0 ? (anchorX - (float)current.x) / (float)current.w : 0.5f;
	float relativeY = current.h > 0 ? (anchorY - (float)current.y) / (float)current.h : 0.5f;

	int targetWidth = (int)lroundf((float)fitted.w * newZoom);
	int targetHeight = (int)lroundf((float)fitted.h * newZoom);
	float targetCenterX = anchorX + (0.5f - relativeX) * (float)targetWidth;
	float targetCenterY = anchorY + (0.5f - relativeY) * (float)targetHeight;
	float fittedCenterX = (float)fitted.x + (float)fitted.w * 0.5f;
	float fittedCenterY = (float)fitted.y + (float)fitted.h * 0.5f;
	float maxOffsetX = (float)(targetWidth - fitted.w) * 0.5f;
	float maxOffsetY = (float)(targetHeight - fitted.h) * 0.5f;

	m_zoomLevel = newZoom;
	m_zoomPanX = maxOffsetX > 0.0f
		? ClampFloat((targetCenterX - fittedCenterX) / maxOffsetX, -1.0f, 1.0f)
		: 0.0f;
	m_zoomPanY = maxOffsetY > 0.0f
		? ClampFloat((targetCenterY - fittedCenterY) / maxOffsetY, -1.0f, 1.0f)
		: 0.0f;

	if (m_zoomLevel <= MIN_VIDEO_ZOOM) {
		resetZoom();
	}
	calculateDisplayRect();
}

void CSDLPlayer::applyDragPan(float deltaX, float deltaY)
{
	if (m_zoomLevel <= MIN_VIDEO_ZOOM + 0.0001f ||
		m_videoWidth <= 0 || m_videoHeight <= 0) {
		return;
	}

	SDL_Rect fitted = calculateFittedVideoBounds();
	SDL_Rect current = calculateZoomedVideoBounds();
	float maxOffsetX = (float)(current.w - fitted.w) * 0.5f;
	float maxOffsetY = (float)(current.h - fitted.h) * 0.5f;

	m_zoomPanX = maxOffsetX > 0.0f
		? ClampFloat(m_zoomPanX + deltaX / maxOffsetX, -1.0f, 1.0f)
		: 0.0f;
	m_zoomPanY = maxOffsetY > 0.0f
		? ClampFloat(m_zoomPanY + deltaY / maxOffsetY, -1.0f, 1.0f)
		: 0.0f;
	calculateDisplayRect();
}

void CSDLPlayer::stopPanning()
{
	if (!m_bPanning) {
		return;
	}

	m_bPanning = false;
	SDL_CaptureMouse(SDL_FALSE);
	SDL_SetCursor(SDL_GetDefaultCursor());
}

void CSDLPlayer::resetZoom()
{
	stopPanning();
	m_bLeftButtonDown = false;
	m_bPanMoved = false;
	m_leftClickCount = 0;
	m_zoomLevel = MIN_VIDEO_ZOOM;
	m_zoomPanX = 0.0f;
	m_zoomPanY = 0.0f;
}

void CSDLPlayer::clearSessionVideoFrame()
{
	CAutoLock oLock(m_mutexVideo, "clearSessionVideoFrame");
	m_videoTextureHasFrame = false;
	m_cleanFeed.InvalidateVideoTexture();
	InterlockedExchange(&m_yuvReady, 0);
	InterlockedExchange(&m_yuvWriteIdx, 0);
	InterlockedExchange(&m_yuvReadIdx, 0);
	InterlockedExchange64(&m_qpcFrameArrival, 0);

	if (m_videoWidth <= 0 || m_videoHeight <= 0) {
		return;
	}

	int uvHeight = (m_videoHeight + 1) / 2;
	for (int i = 0; i < 2; ++i) {
		if (m_yuvBuffer[i][0] != NULL) {
			memset(m_yuvBuffer[i][0], 0, m_yuvPitch[0] * m_videoHeight);
		}
		if (m_yuvBuffer[i][1] != NULL) {
			memset(m_yuvBuffer[i][1], 128, m_yuvPitch[1] * uvHeight);
		}
		if (m_yuvBuffer[i][2] != NULL) {
			memset(m_yuvBuffer[i][2], 128, m_yuvPitch[2] * uvHeight);
		}
	}
}

void CSDLPlayer::unInitVideo()
{
	removeNativeResizeHook();

	// Destroy video texture
	if (m_videoTexture != NULL) {
		SDL_DestroyTexture(m_videoTexture);
		m_videoTexture = NULL;
	}
	m_videoTextureHasFrame = false;

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

	// The clean feed owns a separate renderer but must restore the main window's
	// capture affinity before that HWND is destroyed.
	m_cleanFeed.Shutdown();

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
	m_hwnd = NULL;

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
		m_bMainWindowMinimized = false;
		SDL_SetWindowTitle(m_window, "AirPlay Receiver");
	}
}

void CSDLPlayer::hideWindow()
{
	if (m_window != NULL) {
		SDL_HideWindow(m_window);
		m_bWindowVisible = false;
		m_bMainWindowMinimized = true;
		m_cleanFeed.Hide();
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
