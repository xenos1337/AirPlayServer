#include "CSDLPlayer.h"
#include <stdio.h>
#include <malloc.h>  // For _aligned_malloc/_aligned_free
#include <math.h>    // For powf() in volume conversion
#include <stdlib.h>
#include <new>
#include "CAutoLock.h"
#include "DebugLogger.h"
#include "FgUtf8Utils.h"

// Windows Core Audio API for querying system audio device format
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <bcrypt.h>

// Link against required libraries
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "bcrypt.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
	constexpr float MIN_VIDEO_ZOOM = 1.0f;
	constexpr float MAX_VIDEO_ZOOM = 5.0f;
	constexpr float VIDEO_ZOOM_STEP = 1.10f;
	constexpr float VIDEO_PAN_DRAG_THRESHOLD = 4.0f;
	constexpr unsigned int MIN_ADVERTISED_WIDTH = 640;
	constexpr unsigned int MIN_ADVERTISED_HEIGHT = 360;
	constexpr unsigned int MAX_ADVERTISED_WIDTH = 7680;
	constexpr unsigned int MAX_ADVERTISED_HEIGHT = 4320;
	constexpr DWORD PIN_APPROVAL_TIMEOUT_MS = 60000;
	const wchar_t VIDEO_WINDOW_CLASS[] = L"AirPlayCoreVideo";

	enum EPinApprovalState
	{
		PIN_APPROVAL_IDLE = 0,
		PIN_APPROVAL_WAITING,
		PIN_APPROVAL_SHOWING_CODE
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

	// The video child window belongs to the render thread but its parent belongs
	// to the caller, so their input queues are attached. Waiting must keep
	// servicing sent messages or window creation and destruction can deadlock.
	DWORD WaitServicingSentMessages(DWORD count, const HANDLE* handles)
	{
		for (;;) {
			DWORD result = MsgWaitForMultipleObjects(count, handles, FALSE, INFINITE, QS_SENDMESSAGE);
			if (result != WAIT_OBJECT_0 + count) {
				return result;
			}
			MSG message;
			PeekMessageW(&message, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		}
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
	: m_callbacks()
	, m_topLevel(NULL)
	, m_hwnd(NULL)
	, m_renderThread(NULL)
	, m_renderReady(NULL)
	, m_quitRequested(0)
	, m_mutexConfig(NULL)
	, m_config()
	, m_frameConfig()
	, m_mutexStats(NULL)
	, m_stats()
	, m_window(NULL)
	, m_renderer(NULL)
	, m_videoTexture(NULL)
	, m_videoTextureHasFrame(false)
	, m_displayRect()
	, m_rotationAngle(0)
	, m_appliedRotation(0)
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
	, m_capturePrivacyActive(0)
	, m_videoWidth(0)
	, m_videoHeight(0)
	, m_lastFramePTS(0)
	, m_lastFrameTime(0)
	, m_windowWidth(0)
	, m_windowHeight(0)
	, m_sAudioFmt()
	, m_bAudioInited(false)
	, m_audioDeviceID(0)
	, m_filePerfLog(NULL)
	, m_server()
	, m_mutexPinApproval(NULL)
	, m_eventPinApproval(NULL)
	, m_pinApprovalState(PIN_APPROVAL_IDLE)
	, m_pinApprovalGeneration(0)
	, m_pendingPinEnabled(false)
	, m_pendingDisplayWidth(0)
	, m_pendingDisplayHeight(0)
	, m_receiverSettingsChangeTime(0)
	, m_receiverSettingsPendingRestart(false)
	, m_activePinEnabled(false)
	, m_activeDisplayWidth(0)
	, m_activeDisplayHeight(0)
	, m_restartRequested(0)
	, m_serverThread(NULL)
	, m_serverWake(NULL)
	, m_mutexServer(NULL)
	, m_serverRequest()
	, m_serverStartRequested(false)
	, m_serverStopRequested(false)
	, m_serverStopTimedOut(false)
	, m_advertisingChangeRequested(false)
	, m_advertising(true)
	, m_serverState(AP_SERVER_OFFLINE)
	, m_serverStartRequestTime(0)
	, m_appliedQualityPreset(-1)
	, m_videoReadyPending(false)
	, m_bResizing(false)
	, m_lastMouseMoveTime(0)
	, m_bCursorHidden(false)
	, m_panCursor(NULL)
{
	ZeroMemory(&m_sAudioFmt, sizeof(SFgAudioFrame));
	ZeroMemory(&m_displayRect, sizeof(SDL_Rect));
	ZeroMemory(m_serverName, sizeof(m_serverName));
	ZeroMemory(m_rendererName, sizeof(m_rendererName));
	ZeroMemory(m_pendingServerName, sizeof(m_pendingServerName));
	ZeroMemory(m_connectedDeviceName, sizeof(m_connectedDeviceName));
	SecureZeroMemory(m_sessionAirPlayPin, sizeof(m_sessionAirPlayPin));
	SecureZeroMemory(m_pendingPinRemote, sizeof(m_pendingPinRemote));
	SecureZeroMemory(m_pendingPinCode, sizeof(m_pendingPinCode));
	m_bConnected = false;
	m_shuttingDown = 0;
	m_mutexAudio = CreateMutex(NULL, FALSE, NULL);
	m_mutexVideo = CreateMutex(NULL, FALSE, NULL);
	m_mutexConfig = CreateMutex(NULL, FALSE, NULL);
	m_mutexStats = CreateMutex(NULL, FALSE, NULL);
	m_mutexServer = CreateMutex(NULL, FALSE, NULL);
	m_mutexPinApproval = CreateMutex(NULL, FALSE, NULL);
	m_eventPinApproval = CreateEvent(NULL, TRUE, FALSE, NULL);
	// Until the sender reports its volume, play at the middle of its slider
	// (-15 dB) rather than half amplitude, which is close to full loudness.
	m_senderGain = powf(10.0f, AIRPLAY_MIN_DB / 2.0f / 20.0f);
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
	m_resampleCorrection = 0.0;
	m_needsResampling = false;

	m_peakLevel = 0.0f;
	m_deviceVolumeNormalized = 0.5f;

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
	stop();

	if (m_renderReady != NULL) CloseHandle(m_renderReady);
	if (m_serverWake != NULL) CloseHandle(m_serverWake);
	CloseHandle(m_mutexServer);
	CloseHandle(m_mutexAudio);
	CloseHandle(m_mutexVideo);
	CloseHandle(m_mutexConfig);
	CloseHandle(m_mutexStats);
	if (m_eventPinApproval != NULL) CloseHandle(m_eventPinApproval);
	if (m_mutexPinApproval != NULL) CloseHandle(m_mutexPinApproval);
}

bool CSDLPlayer::start(HWND topLevel, const ApCallbacks& callbacks, const ApConfig& config)
{
	if (m_renderThread != NULL || topLevel == NULL) {
		return false;
	}

	m_topLevel = topLevel;
	m_callbacks = callbacks;
	setConfig(config);

	m_renderReady = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_serverWake = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (m_renderReady == NULL || m_serverWake == NULL) {
		return false;
	}
	m_serverThread = CreateThread(NULL, 0, serverThreadProc, this, 0, NULL);
	if (m_serverThread == NULL) {
		return false;
	}
	m_renderThread = CreateThread(NULL, 0, renderThreadProc, this, 0, NULL);
	if (m_renderThread == NULL) {
		return false;
	}

	HANDLE handles[2] = { m_renderReady, m_renderThread };
	if (WaitServicingSentMessages(2, handles) == WAIT_OBJECT_0) {
		return true;
	}

	// The render thread only exits before signaling when initialization failed.
	CloseHandle(m_renderThread);
	m_renderThread = NULL;
	return false;
}

void CSDLPlayer::stop()
{
	InterlockedExchange(&m_shuttingDown, 1);
	cancelPinApproval();
	if (m_renderThread != NULL) {
		InterlockedExchange(&m_quitRequested, 1);
		WaitServicingSentMessages(1, &m_renderThread);
		CloseHandle(m_renderThread);
		m_renderThread = NULL;
	}
	stopServerThread();
}

DWORD WINAPI CSDLPlayer::serverThreadProc(LPVOID parameter)
{
	((CSDLPlayer*)parameter)->serverThread();
	return 0;
}

// Starting and stopping the receiver talks to the Bonjour service, which can
// block for a long time when it is hung. Doing it here keeps the render thread
// pumping messages; the UI thread waits on that thread through the child window.
void CSDLPlayer::serverThread()
{
	for (;;) {
		WaitForSingleObject(m_serverWake, INFINITE);

		bool stopRequested;
		bool startRequested;
		bool advertisingRequested;
		bool advertising;
		SServerRequest request;
		{
			CAutoLock oLock(m_mutexServer, "serverRequest");
			stopRequested = m_serverStopRequested;
			startRequested = m_serverStartRequested;
			advertisingRequested = m_advertisingChangeRequested;
			advertising = m_advertising;
			request = m_serverRequest;
			m_serverStartRequested = false;
			m_advertisingChangeRequested = false;
		}
		if (stopRequested) {
			break;
		}
		if (!startRequested) {
			if (advertisingRequested) {
				m_server.setAdvertising(advertising);
			}
			continue;
		}

		cancelPinApproval();
		// The PIN is short-lived and memory-only; settings only remember
		// whether it is required at all.
		if (request.pinEnabled && !generateSessionAirPlayPin()) {
			printf("Could not generate the AirPlay session PIN\n");
			m_server.stop();
		} else {
			if (!request.pinEnabled) {
				SecureZeroMemory(m_sessionAirPlayPin, sizeof(m_sessionAirPlayPin));
			}
			m_server.start(this, request.name[0] != '\0' ? request.name : NULL,
				request.pinEnabled ? m_sessionAirPlayPin : NULL,
				request.displayWidth, request.displayHeight);
			// A fresh registration advertises; keep it hidden during a session.
			if (!advertising) {
				m_server.setAdvertising(false);
			}
		}
		// Another request may have arrived meanwhile; it keeps the state at
		// "starting" until it has been handled too.
		CAutoLock oLock(m_mutexServer, "serverState");
		if (!m_serverStartRequested) {
			InterlockedExchange(&m_serverState, m_server.isRunning() ? AP_SERVER_RUNNING : AP_SERVER_OFFLINE);
		}
	}

	m_server.stop();
	InterlockedExchange(&m_serverState, AP_SERVER_OFFLINE);
}

void CSDLPlayer::requestServerStart(bool pinEnabled, unsigned int displayWidth, unsigned int displayHeight)
{
	{
		CAutoLock oLock(m_mutexServer, "requestServerStart");
		strncpy_s(m_serverRequest.name, sizeof(m_serverRequest.name), m_serverName, _TRUNCATE);
		m_serverRequest.pinEnabled = pinEnabled;
		m_serverRequest.displayWidth = displayWidth;
		m_serverRequest.displayHeight = displayHeight;
		m_serverStartRequested = true;
		InterlockedExchange(&m_serverState, AP_SERVER_STARTING);
	}
	m_activePinEnabled = pinEnabled;
	m_activeDisplayWidth = displayWidth;
	m_activeDisplayHeight = displayHeight;
	m_serverStartRequestTime = GetTickCount();
	SetEvent(m_serverWake);
}

// While a device streams, the receiver is not advertised, so other devices
// do not offer it; ones that remember it are refused by the protocol library.
void CSDLPlayer::requestAdvertising(bool enabled)
{
	{
		CAutoLock oLock(m_mutexServer, "requestAdvertising");
		m_advertising = enabled;
		m_advertisingChangeRequested = true;
	}
	SetEvent(m_serverWake);
}

void CSDLPlayer::stopServerThread()
{
	if (m_serverThread == NULL || m_serverStopTimedOut) {
		return;
	}
	{
		CAutoLock oLock(m_mutexServer, "stopServerThread");
		m_serverStopRequested = true;
	}
	SetEvent(m_serverWake);

	// A hung Bonjour service can keep the thread inside a call indefinitely.
	// Give up after a while instead of hanging the exit; the owner then leaks
	// this object rather than freeing memory the thread still uses.
	if (WaitForSingleObject(m_serverThread, SERVER_STOP_TIMEOUT_MS) != WAIT_OBJECT_0) {
		printf("Receiver did not stop in time\n");
		m_serverStopTimedOut = true;
		return;
	}
	CloseHandle(m_serverThread);
	m_serverThread = NULL;
}

bool CSDLPlayer::canDestroy() const
{
	return m_serverThread == NULL;
}

bool CSDLPlayer::isShuttingDown()
{
	return InterlockedCompareExchange(&m_shuttingDown, 0, 0) != 0;
}

DWORD WINAPI CSDLPlayer::renderThreadProc(LPVOID parameter)
{
	((CSDLPlayer*)parameter)->renderThread();
	return 0;
}

void CSDLPlayer::renderThread()
{
	if (!initRenderer()) {
		shutdownRenderer();
		return;
	}
	SetEvent(m_renderReady);
	loopEvents();
	shutdownRenderer();
}

// SDL subclasses this window and passes every message on to this procedure
// after its own handling.
LRESULT CALLBACK CSDLPlayer::videoWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		CREATESTRUCTW* create = (CREATESTRUCTW*)lParam;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
	}
	CSDLPlayer* player = (CSDLPlayer*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

	switch (message) {
	case WM_SETCURSOR:
		// SDL has already applied its cursor. DefWindowProc would ask the WPF
		// parent next, which resets it to the arrow.
		if (LOWORD(lParam) == HTCLIENT) {
			return TRUE;
		}
		break;
	case WM_KEYUP:
		// SDL only tracks keyboard focus for foreground windows, which a child
		// never is. Shortcuts are handled by the WPF window.
		if (player != NULL && player->m_callbacks.keyUp != NULL && !player->isShuttingDown()) {
			int modifiers = (GetKeyState(VK_CONTROL) < 0 ? AP_MOD_CTRL : 0) |
				(GetKeyState(VK_SHIFT) < 0 ? AP_MOD_SHIFT : 0) |
				((GetKeyState(VK_MENU) < 0 || GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
					? AP_MOD_ALT : 0);
			player->m_callbacks.keyUp((int)wParam, modifiers);
		}
		return 0;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool CSDLPlayer::initRenderer()
{
	// The child must match the DPI awareness of its WPF parent, otherwise
	// Windows refuses mixed-mode parenting.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// Request 1ms Windows timer resolution (for accurate Sleep/SDL_Delay)
	timeBeginPeriod(1);

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return false;
	}
	m_panCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);

	// Enable best filtering for smooth scaling (default quality preset = Balanced)
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

	// Use JPEG/full-range color matrix for GPU YUV→RGB conversion
	// AirPlay sends full-range YUV (0-255); BT.709 mode assumes limited-range (16-235) causing oversaturation
	SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_JPEG);

	HINSTANCE instance = (HINSTANCE)&__ImageBase;
	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = videoWindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
	windowClass.lpszClassName = VIDEO_WINDOW_CLASS;
	if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		printf("Could not register video window class (error %lu)\n", GetLastError());
		return false;
	}

	// Hidden until the WPF HwndHost shows it. WS_EX_NOPARENTNOTIFY keeps
	// creation and destruction from sending synchronous messages to the parent.
	m_hwnd = CreateWindowExW(WS_EX_NOPARENTNOTIFY, VIDEO_WINDOW_CLASS, L"",
		WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 1, 1,
		m_topLevel, NULL, instance, this);
	if (m_hwnd == NULL) {
		printf("Could not create video window (error %lu)\n", GetLastError());
		return false;
	}

	m_window = SDL_CreateWindowFrom(m_hwnd);
	if (m_window == NULL) {
		printf("Could not wrap video window: %s\n", SDL_GetError());
		return false;
	}

	// Create GPU-accelerated renderer (no VSync - minimizes frame latency).
	// Prefer Direct3D 11: SDL's default Direct3D 9 renderer resets the device on
	// every resize, which drops the video texture and flashes black.
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
	m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
	if (m_renderer == NULL) {
		printf("Direct3D 11 renderer unavailable (%s); using the default\n", SDL_GetError());
		SDL_SetHint(SDL_HINT_RENDER_DRIVER, "");
		m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
	}
	if (m_renderer == NULL) {
		printf("Could not create renderer: %s\n", SDL_GetError());
		return false;
	}
	SDL_RendererInfo rendererInfo = {};
	if (SDL_GetRendererInfo(m_renderer, &rendererInfo) == 0 && rendererInfo.name != NULL) {
		strncpy_s(m_rendererName, sizeof(m_rendererName), rendererInfo.name, _TRUNCATE);
	}

	RECT client = {};
	GetClientRect(m_hwnd, &client);
	m_windowWidth = client.right - client.left;
	m_windowHeight = client.bottom - client.top;
	calculateDisplayRect();

	m_cleanFeed.Init(m_window, m_hwnd, m_topLevel);

	ApConfig config;
	{
		CAutoLock oLock(m_mutexConfig, "initConfig");
		config = m_config;
	}
	m_frameConfig = config;
	setServerName(config.deviceName);
	strncpy_s(m_pendingServerName, sizeof(m_pendingServerName), m_serverName, _TRUNCATE);
	m_activePinEnabled = m_pendingPinEnabled = config.pinEnabled != 0;
	getAdvertisedDisplaySize(config, m_activeDisplayWidth, m_activeDisplayHeight);
	m_pendingDisplayWidth = m_activeDisplayWidth;
	m_pendingDisplayHeight = m_activeDisplayHeight;

	requestServerStart(m_activePinEnabled, m_activeDisplayWidth, m_activeDisplayHeight);
	return true;
}

void CSDLPlayer::shutdownRenderer()
{
	// Stop callback producers, then release any connection payload queued after
	// the final event poll.
	stopServerThread();
	if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
		SDL_FilterEvents(KeepNonConnectionStateEvents, NULL);
	}

	stopPanning();
	if (m_filePerfLog != NULL) {
		fclose(m_filePerfLog);
		m_filePerfLog = NULL;
	}

	if (m_videoTexture != NULL) {
		SDL_DestroyTexture(m_videoTexture);
		m_videoTexture = NULL;
	}
	m_videoTextureHasFrame = false;

	for (int i = 0; i < 2; i++) {
		for (int p = 0; p < 3; p++) {
			if (m_yuvBuffer[i][p] != NULL) {
				_aligned_free(m_yuvBuffer[i][p]);
				m_yuvBuffer[i][p] = NULL;
			}
		}
	}
	m_yuvPitch[0] = m_yuvPitch[1] = m_yuvPitch[2] = 0;
	m_videoWidth = 0;
	m_videoHeight = 0;

	m_cleanFeed.Shutdown();

	if (m_renderer != NULL) {
		SDL_DestroyRenderer(m_renderer);
		m_renderer = NULL;
	}
	if (m_window != NULL) {
		SDL_DestroyWindow(m_window);
		m_window = NULL;
	}
	if (m_hwnd != NULL) {
		DestroyWindow(m_hwnd);
		m_hwnd = NULL;
	}

	unInitAudio();
	if (m_panCursor != NULL) {
		SDL_FreeCursor(m_panCursor);
		m_panCursor = NULL;
	}
	SDL_Quit();

	// Restore default Windows timer resolution
	timeEndPeriod(1);
}

void CSDLPlayer::setConfig(const ApConfig& config)
{
	CAutoLock oLock(m_mutexConfig, "setConfig");
	m_config = config;
	m_config.deviceName[sizeof(m_config.deviceName) - 1] = '\0';
}

int CSDLPlayer::rotate()
{
	LONG current;
	LONG next;
	do {
		current = InterlockedCompareExchange(&m_rotationAngle, 0, 0);
		next = (current + 90) % 360;
	} while (InterlockedCompareExchange(&m_rotationAngle, next, current) != current);
	return (int)next;
}

void CSDLPlayer::resetView()
{
	InterlockedExchange(&m_zoomResetPending, 1);
}

void CSDLPlayer::setCapturePrivacy(bool enabled)
{
	InterlockedExchange(&m_capturePrivacyActive, enabled ? 1 : 0);
}

void CSDLPlayer::getStats(ApStats* stats)
{
	CAutoLock oLock(m_mutexStats, "getStats");
	*stats = m_stats;
}

void CSDLPlayer::getMonitorSize(HWND window, unsigned int& width, unsigned int& height)
{
	HMONITOR monitor = window != NULL
		? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
		: MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFOEXW monitorInfo = {};
	monitorInfo.cbSize = sizeof(monitorInfo);
	DEVMODEW displayMode = {};
	displayMode.dmSize = sizeof(displayMode);
	if (monitor != NULL && GetMonitorInfoW(monitor, &monitorInfo) &&
		EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode) &&
		displayMode.dmPelsWidth > 0 && displayMode.dmPelsHeight > 0) {
		width = displayMode.dmPelsWidth;
		height = displayMode.dmPelsHeight;
		return;
	}

	int systemWidth = GetSystemMetrics(SM_CXSCREEN);
	int systemHeight = GetSystemMetrics(SM_CYSCREEN);
	width = systemWidth > 0 ? (unsigned int)systemWidth : 1920U;
	height = systemHeight > 0 ? (unsigned int)systemHeight : 1080U;
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
		isShuttingDown() || pin == NULL || strlen(pin) != 4) {
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
	LONG generation = InterlockedIncrement(&m_pinApprovalGeneration);
	char remoteCopy[sizeof(m_pendingPinRemote)];
	char pinCopy[sizeof(m_pendingPinCode)];
	memcpy(remoteCopy, m_pendingPinRemote, sizeof(remoteCopy));
	memcpy(pinCopy, m_pendingPinCode, sizeof(pinCopy));
	ResetEvent(m_eventPinApproval);
	ReleaseMutex(m_mutexPinApproval);

	if (m_callbacks.pinRequest != NULL && !isShuttingDown()) {
		m_callbacks.pinRequest((int)generation, remoteCopy, pinCopy);
	}
	SecureZeroMemory(pinCopy, sizeof(pinCopy));

	DWORD waitResult = WaitForSingleObject(m_eventPinApproval, PIN_APPROVAL_TIMEOUT_MS);

	if (WaitForSingleObject(m_mutexPinApproval, INFINITE) != WAIT_OBJECT_0) {
		return false;
	}
	bool current = m_pinApprovalGeneration == generation;
	bool approved = waitResult == WAIT_OBJECT_0 && current &&
		m_pinApprovalState == PIN_APPROVAL_SHOWING_CODE;
	bool timedOut = !approved && current && m_pinApprovalState != PIN_APPROVAL_IDLE;
	if (timedOut) {
		m_pinApprovalState = PIN_APPROVAL_IDLE;
		SecureZeroMemory(m_pendingPinRemote, sizeof(m_pendingPinRemote));
		SecureZeroMemory(m_pendingPinCode, sizeof(m_pendingPinCode));
	}
	ReleaseMutex(m_mutexPinApproval);

	if (timedOut && m_callbacks.pinClosed != NULL && !isShuttingDown()) {
		m_callbacks.pinClosed((int)generation);
	}
	return approved;
}

void CSDLPlayer::answerPinApproval(LONG generation, bool allow)
{
	if (m_mutexPinApproval == NULL || m_eventPinApproval == NULL ||
		WaitForSingleObject(m_mutexPinApproval, INFINITE) != WAIT_OBJECT_0) {
		return;
	}
	if (m_pinApprovalState == PIN_APPROVAL_WAITING && m_pinApprovalGeneration == generation) {
		if (allow) {
			m_pinApprovalState = PIN_APPROVAL_SHOWING_CODE;
		} else {
			m_pinApprovalState = PIN_APPROVAL_IDLE;
			SecureZeroMemory(m_pendingPinRemote, sizeof(m_pendingPinRemote));
			SecureZeroMemory(m_pendingPinCode, sizeof(m_pendingPinCode));
		}
		SetEvent(m_eventPinApproval);
	}
	ReleaseMutex(m_mutexPinApproval);
}

void CSDLPlayer::dismissPinApproval(LONG generation)
{
	if (m_mutexPinApproval == NULL || m_eventPinApproval == NULL ||
		WaitForSingleObject(m_mutexPinApproval, INFINITE) != WAIT_OBJECT_0) {
		return;
	}
	if (m_pinApprovalState != PIN_APPROVAL_IDLE && m_pinApprovalGeneration == generation) {
		m_pinApprovalState = PIN_APPROVAL_IDLE;
		SecureZeroMemory(m_pendingPinRemote, sizeof(m_pendingPinRemote));
		SecureZeroMemory(m_pendingPinCode, sizeof(m_pendingPinCode));
		SetEvent(m_eventPinApproval);
	}
	ReleaseMutex(m_mutexPinApproval);
}

void CSDLPlayer::cancelPinApproval()
{
	if (m_mutexPinApproval == NULL || m_eventPinApproval == NULL) {
		return;
	}
	if (WaitForSingleObject(m_mutexPinApproval, INFINITE) != WAIT_OBJECT_0) {
		return;
	}
	LONG previousGeneration = m_pinApprovalGeneration;
	bool wasActive = m_pinApprovalState != PIN_APPROVAL_IDLE;
	m_pinApprovalState = PIN_APPROVAL_IDLE;
	InterlockedIncrement(&m_pinApprovalGeneration);
	SecureZeroMemory(m_pendingPinRemote, sizeof(m_pendingPinRemote));
	SecureZeroMemory(m_pendingPinCode, sizeof(m_pendingPinCode));
	SetEvent(m_eventPinApproval);
	ReleaseMutex(m_mutexPinApproval);

	if (wasActive && m_callbacks.pinClosed != NULL && !isShuttingDown()) {
		m_callbacks.pinClosed((int)previousGeneration);
	}
}

void CSDLPlayer::setServerName(const char* serverName)
{
	if (serverName != NULL && strlen(serverName) > 0) {
		if (!CFgUtf8Utils::IsReceiverNameValid(serverName)) {
			serverName = "AirPlay Receiver";
		}
		strncpy_s(m_serverName, sizeof(m_serverName), serverName, _TRUNCATE);
	} else {
		m_serverName[0] = '\0';
	}
}

void CSDLPlayer::setConnected(bool connected, const char* deviceName)
{
	if (isShuttingDown()) {
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
	if (isShuttingDown()) {
		delete change;
		return;
	}
	if (SDL_PushEvent(&event) <= 0) {
		printf("Could not queue connection-state change: %s\n", SDL_GetError());
		delete change;
	}
}

void CSDLPlayer::resetSessionStatistics()
{
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
}

void CSDLPlayer::applyConnectionState(bool connected, const char* deviceName)
{
	bool stateChanged = m_bConnected != connected;
	if (stateChanged) {
		// Retire every staged and displayed pixel before changing session
		// ownership so reconnecting at the same source resolution can never
		// expose the previous sender's final frame.
		clearSessionVideoFrame();

		if (!connected) {
			unInitAudio();
			m_cleanFeed.Hide();

			if (m_filePerfLog != NULL) {
				fclose(m_filePerfLog);
				m_filePerfLog = NULL;
			}
		} else if (DebugLogger::Enabled()) {
			// A row per rendered frame is only useful for diagnosis, so the
			// per-session CSV sits next to the --debug log.
			std::string perfPath = DebugLogger::Path();
			size_t extension = perfPath.rfind(".log");
			if (extension != std::string::npos) {
				perfPath.erase(extension);
			}
			perfPath += "-perf.csv";
			m_filePerfLog = fopen(perfPath.c_str(), "w");
			if (m_filePerfLog) {
				fprintf(m_filePerfLog,
					"time_ms,frame_time_ms,source_fps,latency_ms,new_frame,video_w,video_h,bitrate_mbps,total_frames,dropped_frames,audio_queue,audio_underruns\n");
				fflush(m_filePerfLog);
			}
		}

		InterlockedExchange(&m_zoomResetPending, 1);
		resetSessionStatistics();
		m_connectionStartTime = connected ? GetTickCount() : 0;
		m_videoReadyPending = connected;
		requestAdvertising(!connected);
		m_bConnected = connected;
		if (connected && deviceName == NULL) {
			m_connectedDeviceName[0] = '\0';
		}
	}

	// The last sender name is kept after a disconnect for the UI's notice.
	bool nameChanged = false;
	if (deviceName != NULL && (stateChanged || connected) &&
		strcmp(m_connectedDeviceName, deviceName) != 0) {
		strncpy_s(m_connectedDeviceName, sizeof(m_connectedDeviceName), deviceName, _TRUNCATE);
		nameChanged = true;
	}

	if ((stateChanged || nameChanged) && m_callbacks.connectionChanged != NULL && !isShuttingDown()) {
		m_callbacks.connectionChanged(m_bConnected ? 1 : 0, m_connectedDeviceName);
	}
}

void CSDLPlayer::applyReceiverSettings()
{
	// Apply receiver identity/security changes together. The PIN itself is
	// generated in memory on every restart and is never persisted.
	const char* desiredName = (m_frameConfig.deviceName[0] != '\0' &&
		CFgUtf8Utils::IsReceiverNameValid(m_frameConfig.deviceName))
		? m_frameConfig.deviceName : m_serverName;
	bool desiredPinEnabled = m_frameConfig.pinEnabled != 0;
	unsigned int desiredDisplayWidth = 0;
	unsigned int desiredDisplayHeight = 0;
	getAdvertisedDisplaySize(m_frameConfig, desiredDisplayWidth, desiredDisplayHeight);
	bool settingsDiffer = strcmp(desiredName, m_serverName) != 0 ||
		desiredPinEnabled != m_activePinEnabled ||
		desiredDisplayWidth != m_activeDisplayWidth ||
		desiredDisplayHeight != m_activeDisplayHeight;

	if (!settingsDiffer) {
		m_receiverSettingsPendingRestart = false;
	} else if (!m_receiverSettingsPendingRestart ||
		strcmp(desiredName, m_pendingServerName) != 0 ||
		desiredPinEnabled != m_pendingPinEnabled ||
		desiredDisplayWidth != m_pendingDisplayWidth ||
		desiredDisplayHeight != m_pendingDisplayHeight) {
		strncpy_s(m_pendingServerName, sizeof(m_pendingServerName), desiredName, _TRUNCATE);
		m_pendingPinEnabled = desiredPinEnabled;
		m_pendingDisplayWidth = desiredDisplayWidth;
		m_pendingDisplayHeight = desiredDisplayHeight;
		m_receiverSettingsChangeTime = GetTickCount();
		m_receiverSettingsPendingRestart = true;
	}

	if (m_receiverSettingsPendingRestart && !m_bConnected &&
		GetTickCount() - m_receiverSettingsChangeTime >= 1000) {
		setServerName(m_pendingServerName);
		requestServerStart(m_pendingPinEnabled, m_pendingDisplayWidth, m_pendingDisplayHeight);
		m_receiverSettingsPendingRestart = false;
		InterlockedExchange(&m_restartRequested, 0);
	}

	// An explicit restart (after Bonjour restarts) re-registers the receiver
	// with its current settings.
	if (!m_bConnected && InterlockedExchange(&m_restartRequested, 0) == 1) {
		requestServerStart(m_activePinEnabled, m_activeDisplayWidth, m_activeDisplayHeight);
	}

	// A registration that failed while Bonjour was busy or restarting is
	// retried on its own instead of leaving the receiver offline.
	if (!m_bConnected &&
		InterlockedCompareExchange(&m_serverState, 0, 0) == AP_SERVER_OFFLINE &&
		GetTickCount() - m_serverStartRequestTime >= SERVER_RETRY_INTERVAL_MS) {
		requestServerStart(m_activePinEnabled, m_activeDisplayWidth, m_activeDisplayHeight);
	}
}

void CSDLPlayer::requestRestart()
{
	InterlockedExchange(&m_restartRequested, 1);
}

void CSDLPlayer::applyQualityPreset(int preset)
{
	if (preset == m_appliedQualityPreset) {
		return;
	}
	m_appliedQualityPreset = preset;
	switch (preset) {
	case QUALITY_GOOD:
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
		m_targetFrameIntervalMs = 33.333;  // 30fps - maximum quality per frame
		break;
	case QUALITY_FAST:
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
		m_targetFrameIntervalMs = 16.667;  // 60fps - lowest latency
		break;
	default:
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
		m_targetFrameIntervalMs = 16.667;  // 60fps - smooth + high quality
		break;
	}
	// Recreate texture with new filter mode (hint only applies at texture creation)
	if (m_videoTexture != NULL && m_videoWidth > 0 && m_videoHeight > 0) {
		recreateVideoTexture();
		m_cleanFeed.InvalidateVideoTexture();
		recreateCleanFeedTexture();
	}
}

void CSDLPlayer::resetGesture()
{
	stopPanning();
	m_bLeftButtonDown = false;
	m_bPanMoved = false;
	m_leftClickCount = 0;
}

bool CSDLPlayer::isCursorOverView() const
{
	if (m_hwnd == NULL || !IsWindowVisible(m_hwnd)) {
		return false;
	}
	POINT cursor = {};
	RECT bounds = {};
	return GetCursorPos(&cursor) && GetWindowRect(m_hwnd, &bounds) &&
		PtInRect(&bounds, cursor);
}

void CSDLPlayer::loopEvents()
{
	SDL_Event event;

	// Initialize cursor hide timer
	m_lastMouseMoveTime = GetTickCount();

	while (InterlockedCompareExchange(&m_quitRequested, 0, 0) == 0) {
		// Record frame start time with high-precision QPC
		LARGE_INTEGER qpcFrameStart;
		QueryPerformanceCounter(&qpcFrameStart);
		m_qpcFrameStart = qpcFrameStart;

		{
			CAutoLock oLock(m_mutexConfig, "frameConfig");
			m_frameConfig = m_config;
		}
		int localVolume = m_frameConfig.localVolume;
		if (localVolume < 0) localVolume = 0;
		if (localVolume > 100) localVolume = 100;
		m_localVolume = (int)(localVolume / 100.0f * SDL_MIX_MAXVOLUME);

		// Process all pending events
		while (SDL_PollEvent(&event)) {
			if (IsEventForDifferentWindow(event,
				m_window != NULL ? SDL_GetWindowID(m_window) : 0)) {
				// The clean-feed target is intentionally a separate SDL window. It
				// must never feed input or resize notifications into the video view.
				continue;
			}

			// Track pointer activity for cursor auto-hide.
			if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEWHEEL ||
				event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
				m_lastMouseMoveTime = GetTickCount();
				if (m_bCursorHidden) {
					m_bCursorHidden = false;
					SDL_ShowCursor(SDL_ENABLE);
				}
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

						// Calculate display rect for the new video
						calculateDisplayRect();

						// D3D9 can cache a newly bound IYUV texture across the pending
						// window-resize reset. Initialize all planes before the texture is
						// ever eligible for drawing, otherwise that cached binding is green.
						recreateVideoTexture();
						recreateCleanFeedTexture();

						m_bResizing = false;
						m_videoReadyPending = true;
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
						resetGesture();
						m_windowWidth = newW;
						m_windowHeight = newH;
						calculateDisplayRect();
					}
				}
				else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
					resetGesture();
				}
				break;
			}
			case SDL_MOUSEBUTTONDOWN: {
				if (event.button.button != SDL_BUTTON_LEFT) {
					break;
				}

				// A child window only receives wheel and key input while focused.
				SetFocus(m_hwnd);

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

				bool doubleClicked = m_bLeftButtonDown &&
					m_leftClickCount == 2 && !m_bPanMoved;
				resetGesture();

				if (doubleClicked && m_callbacks.doubleClick != NULL && !isShuttingDown()) {
					m_callbacks.doubleClick();
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
			}
		} // End of event polling loop

		// Connection changes and the UI request view resets; geometry is only
		// touched by the render thread.
		if (InterlockedExchange(&m_zoomResetPending, 0) == 1) {
			InterlockedExchange(&m_rotationAngle, 0);
			resetZoom();
			m_appliedRotation = 0;
			calculateDisplayRect();
		}
		LONG requestedRotation = InterlockedCompareExchange(&m_rotationAngle, 0, 0);
		if (requestedRotation != m_appliedRotation) {
			m_appliedRotation = requestedRotation;
			calculateDisplayRect();
		}

		bool screenCastEnabled = m_frameConfig.screenCast != 0;
		m_cleanFeed.ApplySettings(screenCastEnabled);
		if (screenCastEnabled && m_videoTextureHasFrame && !m_cleanFeed.HasVideoFrame()) {
			recreateCleanFeedTexture();
		}

		applyReceiverSettings();
		applyQualityPreset(m_frameConfig.quality);

		renderFrame();

		// Hide the pointer only over an idle, unobstructed cast.
		bool allowCursorHide = m_bConnected && !m_bPanning && isCursorOverView();
		DWORD cursorIdleMs = m_lastMouseMoveTime > 0
			? GetTickCount() - m_lastMouseMoveTime : 0;
		if (allowCursorHide && cursorIdleMs >= CURSOR_HIDE_DELAY_MS && !m_bCursorHidden) {
			m_bCursorHidden = true;
			SDL_ShowCursor(SDL_DISABLE);
		} else if (!allowCursorHide && m_bCursorHidden) {
			m_bCursorHidden = false;
			SDL_ShowCursor(SDL_ENABLE);
		}
		if (m_bPanning && m_panCursor != NULL) {
			SDL_SetCursor(m_panCursor);
		}

		updateStatsSnapshot();

		// Frame-paced render loop: sleep + spin-wait to hit target intervals precisely
		// Connected: pace to target FPS (16.67ms=60fps or 33.33ms=30fps from quality preset)
		// Idle: pace to 60fps
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

void CSDLPlayer::renderFrame()
{
	LARGE_INTEGER qpcFrameStart = m_qpcFrameStart;
	bool announceVideoReady = false;

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
					if (m_videoReadyPending && m_bConnected) {
						m_videoReadyPending = false;
						announceVideoReady = true;
					}
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
	if (m_videoTexture != NULL && m_videoTextureHasFrame && m_videoWidth > 0 &&
		m_bConnected) {
		if (SDL_RenderCopyEx(m_renderer, m_videoTexture, NULL, &m_displayRect,
			(double)m_appliedRotation, NULL, SDL_FLIP_NONE) != 0) {
			printf("SDL_RenderCopyEx failed: %s\n", SDL_GetError());
			m_videoTextureHasFrame = false;
			InterlockedExchange(&m_yuvReady, 1);
		}
	}

	// Capture-only privacy keeps the local receiver visible while forcing the
	// separate clean-feed capture target to a safe black frame.
	if (m_cleanFeed.IsEnabled()) {
		int renderWidth = m_windowWidth;
		int renderHeight = m_windowHeight;
		int outputWidth = 0;
		int outputHeight = 0;
		if (SDL_GetRendererOutputSize(m_renderer, &outputWidth, &outputHeight) == 0 &&
			outputWidth > 0 && outputHeight > 0) {
			renderWidth = outputWidth;
			renderHeight = outputHeight;
		}
		bool viewVisible = IsWindowVisible(m_hwnd) != FALSE;
		bool minimized = IsIconic(m_topLevel) != FALSE;
		m_cleanFeed.Render(m_displayRect,
			calculateScreenCastCaptureBounds(m_frameConfig.screenCastCrop != 0),
			renderWidth, renderHeight, (int)m_appliedRotation,
			m_bConnected, viewVisible, minimized,
			InterlockedCompareExchange(&m_capturePrivacyActive, 0, 0) != 0);
	}

	// 4. Present (no VSync - immediate display for lowest latency)
	SDL_RenderPresent(m_renderer);

	// The UI keeps its connecting screen up until this first frame is shown.
	if (announceVideoReady && m_callbacks.videoSizeChanged != NULL && !isShuttingDown()) {
		m_callbacks.videoSizeChanged(m_videoWidth, m_videoHeight);
	}

	// 5. Accumulate performance metrics, write to circular buffer every 1 second
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

	// 6. Write per-frame CSV log (every render frame while connected)
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

void CSDLPlayer::updateStatsSnapshot()
{
	int audioQueueNow = 0;
	{
		CAutoLock oLock(m_mutexAudio, "statsAudioQueue");
		audioQueueNow = (int)m_queueAudio.size();
	}

	CAutoLock oLock(m_mutexStats, "updateStats");
	ApStats& stats = m_stats;
	stats.totalFrames = m_totalFrames;
	stats.droppedFrames = m_droppedFrames;
	stats.totalBytes = m_totalBytes;
	memcpy(stats.sourceFpsHistory, m_perfFps, sizeof(stats.sourceFpsHistory));
	memcpy(stats.displayFpsHistory, m_perfDisplayFps, sizeof(stats.displayFpsHistory));
	memcpy(stats.frameTimeHistory, m_perfFrameTime, sizeof(stats.frameTimeHistory));
	memcpy(stats.latencyHistory, m_perfLatency, sizeof(stats.latencyHistory));
	memcpy(stats.bitrateHistory, m_perfBitrate, sizeof(stats.bitrateHistory));
	memcpy(stats.audioQueueHistory, m_perfAudioQueue, sizeof(stats.audioQueueHistory));
	stats.historyIndex = m_perfIdx;
	stats.sourceFps = m_currentFPS;
	stats.displayFps = m_displayFPS;
	stats.frameTimeMs = (m_perfAccumCount > 0) ? m_perfAccumFrameTime / (float)m_perfAccumCount : 0.0f;
	stats.latencyMs = (m_perfAccumCount > 0) ? m_perfAccumLatency / (float)m_perfAccumCount : 0.0f;
	stats.bitrateMbps = m_currentBitrateMbps;
	stats.targetFps = (float)(1000.0 / m_targetFrameIntervalMs);
	stats.videoWidth = m_videoWidth;
	stats.videoHeight = m_videoHeight;
	stats.audioUnderruns = m_audioUnderrunCount;
	stats.audioDropped = m_audioDroppedFrames;
	stats.audioQueueSize = audioQueueNow;
	stats.connectionSeconds = (m_connectionStartTime > 0)
		? (float)(GetTickCount() - m_connectionStartTime) / 1000.0f : 0.0f;
	stats.zoom = m_zoomLevel;
	stats.rotation = (int)m_appliedRotation;
	stats.cleanFeedReady = m_cleanFeed.IsReady() ? 1 : 0;
	stats.serverState = (int)InterlockedCompareExchange(&m_serverState, 0, 0);
	stats.senderVolume = m_deviceVolumeNormalized;
	stats.inputLevel = m_peakLevel;
	stats.audioStreamRate = m_bAudioInited ? (int)m_streamSampleRate : 0;
	stats.audioDeviceRate = m_bAudioInited
		? (int)(m_needsResampling ? m_systemSampleRate : m_streamSampleRate) : 0;
	strncpy_s(stats.renderer, sizeof(stats.renderer), m_rendererName, _TRUNCATE);
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

		// Wait for render thread to complete the resize so we can use this frame
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

	SAudioFrame* dataClone = new SAudioFrame();
	dataClone->pts = data->pts;

	// Resample audio if needed (stream rate differs from system rate)
	if (m_needsResampling && m_resampleBuffer && m_streamSampleRate > 0) {
		// Linear interpolation resampling
		int bytesPerSample = 2;  // 16-bit
		int channels = data->channels;
		int inSamples = data->dataLen / (channels * bytesPerSample);

		// Keep the producer clock locked to the device clock. A fixed nominal
		// ratio eventually drains or fills the queue because the two clocks are
		// never exactly equal. Producing slightly more audio at low queue depths
		// (and slightly less at high depths) keeps the queue near its target.
		int queueDepth = 0;
		{
			CAutoLock oLock(m_mutexAudio, "resampleQueueDepth");
			queueDepth = (int)m_queueAudio.size();
		}
		double queueError = (double)(AUDIO_QUEUE_TARGET_FRAMES - queueDepth);
		m_resampleCorrection += queueError * AUDIO_RESAMPLE_INTEGRAL_GAIN;
		if (m_resampleCorrection > AUDIO_RESAMPLE_MAX_CORRECTION) {
			m_resampleCorrection = AUDIO_RESAMPLE_MAX_CORRECTION;
		} else if (m_resampleCorrection < -AUDIO_RESAMPLE_MAX_CORRECTION) {
			m_resampleCorrection = -AUDIO_RESAMPLE_MAX_CORRECTION;
		}
		double correction = m_resampleCorrection +
			queueError * AUDIO_RESAMPLE_PROPORTIONAL_GAIN;
		if (correction > AUDIO_RESAMPLE_MAX_CORRECTION) {
			correction = AUDIO_RESAMPLE_MAX_CORRECTION;
		} else if (correction < -AUDIO_RESAMPLE_MAX_CORRECTION) {
			correction = -AUDIO_RESAMPLE_MAX_CORRECTION;
		}
		double ratio = ((double)m_systemSampleRate / (double)m_streamSampleRate) *
			(1.0 + correction);

		// Carry the fractional output count across calls instead of rounding each
		// input frame independently, which introduces another persistent rate error.
		double exactOutSamples = (double)inSamples * ratio + m_resamplePos;
		int outSamples = (int)floor(exactOutSamples);
		m_resamplePos = exactOutSamples - (double)outSamples;

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

void CSDLPlayer::getAdvertisedDisplaySize(const ApConfig& config, unsigned int& width,
	unsigned int& height) const
{
	if (!config.matchMonitor) {
		int customWidth = config.customWidth;
		int customHeight = config.customHeight;
		if (customWidth < (int)MIN_ADVERTISED_WIDTH) customWidth = MIN_ADVERTISED_WIDTH;
		if (customWidth > (int)MAX_ADVERTISED_WIDTH) customWidth = MAX_ADVERTISED_WIDTH;
		if (customHeight < (int)MIN_ADVERTISED_HEIGHT) customHeight = MIN_ADVERTISED_HEIGHT;
		if (customHeight > (int)MAX_ADVERTISED_HEIGHT) customHeight = MAX_ADVERTISED_HEIGHT;
		width = (unsigned int)(customWidth & ~1);
		height = (unsigned int)(customHeight & ~1);
		return;
	}

	getMonitorSize(m_topLevel, width, height);
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
	bool rotated = (m_appliedRotation == 90 || m_appliedRotation == 270);
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
	bool rotated = (m_appliedRotation == 90 || m_appliedRotation == 270);

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

SDL_Rect CSDLPlayer::calculateScreenCastCaptureBounds(bool cropToVideo) const
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

	if (!cropToVideo || m_videoWidth <= 0 || m_videoHeight <= 0) {
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
	SDL_SetCursor(SDL_GetDefaultCursor());
}

void CSDLPlayer::resetZoom()
{
	resetGesture();
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
			m_resampleCorrection = 0.0;

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
	}
	int audioQueueDepth = 0;
	{
		CAutoLock oLock(m_mutexAudio, "initAudioQueueDepth");
		audioQueueDepth = (int)m_queueAudio.size();
	}
	if (audioQueueDepth >= AUDIO_QUEUE_START_THRESHOLD) {
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
	m_resampleCorrection = 0.0;
	m_streamSampleRate = 0;
}

void CSDLPlayer::sdlAudioCallback(void* userdata, Uint8* stream, int len)
{
	CSDLPlayer* pThis = (CSDLPlayer*)userdata;
	int needLen = len;
	int streamPos = 0;

	// Initialize output buffer to silence
	memset(stream, 0, len);

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

		float gain = pThis->m_senderGain * (float)pThis->m_localVolume / (float)SDL_MIX_MAXVOLUME;
		for (int i = 0; i < numSamples; i++) {
			int absSrc = (src[i] < 0) ? -src[i] : src[i];
			float level = absSrc / 32768.0f;
			if (level > bufferPeak) {
				bufferPeak = level;
			}

			int sample = (int)lrintf((float)src[i] * gain);

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
	// The sender reports its volume slider as attenuation: 0 dB is full,
	// AIRPLAY_MIN_DB the bottom of the slider and -144 dB muted. Apply it as
	// real attenuation; treating the dB value as a linear amplitude made most
	// of the slider almost as loud as full volume.
	if (dbVolume < AIRPLAY_MIN_DB) {
		m_senderGain = 0.0f;
		m_deviceVolumeNormalized = 0.0f;
		return;
	}
	if (dbVolume > 0.0f) {
		dbVolume = 0.0f;
	}
	m_senderGain = powf(10.0f, dbVolume / 20.0f);
	m_deviceVolumeNormalized = (dbVolume - AIRPLAY_MIN_DB) / -AIRPLAY_MIN_DB;
}
