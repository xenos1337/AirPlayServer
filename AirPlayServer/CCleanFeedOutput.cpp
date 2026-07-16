#include "CCleanFeedOutput.h"
#include "SDL_syswm.h"

#include <ShObjIdl.h>

#include <math.h>
#include <stdio.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace
{
	const char* const CLEAN_FEED_WINDOW_TITLE = "AirPlay Receiver - Clean Feed";

	int ScaleCoordinate(int value, int sourceSize, int targetSize)
	{
		if (sourceSize <= 0 || targetSize <= 0) return 0;
		return (int)(((long long)value * targetSize + sourceSize / 2) / sourceSize);
	}
}

CCleanFeedOutput::CCleanFeedOutput()
	: m_mainWindow(NULL)
	, m_mainHwnd(NULL)
	, m_window(NULL)
	, m_renderer(NULL)
	, m_videoTexture(NULL)
	, m_enabled(false)
	, m_textureHasFrame(false)
	, m_visible(false)
	, m_taskbarTabHidden(false)
	, m_settingsCaptureExclusion(false)
	, m_temporaryCaptureExclusion(false)
	, m_privacyModeCaptureExclusion(false)
	, m_captureExclusionRequested(false)
	, m_captureExclusionFailed(false)
	, m_videoWidth(0)
	, m_videoHeight(0)
{
}

CCleanFeedOutput::~CCleanFeedOutput()
{
	Shutdown();
}

bool CCleanFeedOutput::Init(SDL_Window* mainWindow, HWND mainHwnd)
{
	m_mainWindow = mainWindow;
	m_mainHwnd = mainHwnd;
	return m_mainWindow != NULL && m_mainHwnd != NULL;
}

void CCleanFeedOutput::Shutdown()
{
	m_settingsCaptureExclusion = false;
	m_temporaryCaptureExclusion = false;
	m_privacyModeCaptureExclusion = false;
	UpdateMainWindowCaptureExclusion();
	DestroySurface();
	m_mainWindow = NULL;
	m_mainHwnd = NULL;
	m_enabled = false;
	m_settingsCaptureExclusion = false;
	m_temporaryCaptureExclusion = false;
	m_privacyModeCaptureExclusion = false;
	m_captureExclusionRequested = false;
	m_captureExclusionFailed = false;
}

void CCleanFeedOutput::ApplySettings(bool enabled, bool excludeMainWindowFromCapture)
{
	if (m_enabled != enabled) {
		m_enabled = enabled;
		if (!m_enabled) {
			Hide();
			DestroySurface();
		}
	}

	m_settingsCaptureExclusion = m_enabled && excludeMainWindowFromCapture;
	UpdateMainWindowCaptureExclusion();
	if (m_enabled) {
		EnsureSurface();
	}
}

bool CCleanFeedOutput::SetTemporaryMainWindowCaptureExclusion(bool exclude)
{
	m_temporaryCaptureExclusion = exclude;
	bool applied = UpdateMainWindowCaptureExclusion();
	if (exclude && !applied) {
		// Do not leave a failed temporary request latched. A later unrelated
		// settings update must not unexpectedly reveal a PIN flow that failed.
		m_temporaryCaptureExclusion = false;
		UpdateMainWindowCaptureExclusion();
	}
	return applied;
}

bool CCleanFeedOutput::SetPrivacyModeMainWindowCaptureExclusion(bool exclude)
{
	m_privacyModeCaptureExclusion = exclude;
	bool applied = UpdateMainWindowCaptureExclusion();
	if (exclude && !applied) {
		m_privacyModeCaptureExclusion = false;
		UpdateMainWindowCaptureExclusion();
	}
	return applied;
}

bool CCleanFeedOutput::EnsureSurface()
{
	if (m_window != NULL && m_renderer != NULL) {
		return true;
	}
	if (m_mainWindow == NULL || m_mainHwnd == NULL) {
		return false;
	}

	m_window = SDL_CreateWindow(CLEAN_FEED_WINDOW_TITLE,
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1, 1,
		SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
	if (m_window == NULL) {
		printf("Could not create clean-feed window: %s\n", SDL_GetError());
		return false;
	}

	SDL_SysWMinfo wmInfo = {};
	SDL_VERSION(&wmInfo.version);
	if (SDL_GetWindowWMInfo(m_window, &wmInfo) && wmInfo.subsystem == SDL_SYSWM_WINDOWS) {
		HWND captureHwnd = wmInfo.info.win.window;
		LONG_PTR exStyle = GetWindowLongPtrW(captureHwnd, GWL_EXSTYLE);
		// Discord can omit transparent/no-activate popups as overlay windows.
		// Advertise this as a normal top-level application window instead. It is
		// still kept behind the main receiver window, and its SDL input events are
		// ignored by CSDLPlayer.
		exStyle |= WS_EX_APPWINDOW;
		exStyle &= ~(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW);
		SetWindowLongPtrW(captureHwnd, GWL_EXSTYLE, exStyle);
		SetWindowPos(captureHwnd, NULL, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}

	m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
	if (m_renderer == NULL) {
		printf("Could not create clean-feed renderer: %s\n", SDL_GetError());
		DestroySurface();
		return false;
	}
	SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
	return true;
}

void CCleanFeedOutput::DestroySurface()
{
	if (m_videoTexture != NULL) {
		SDL_DestroyTexture(m_videoTexture);
		m_videoTexture = NULL;
	}
	if (m_renderer != NULL) {
		SDL_DestroyRenderer(m_renderer);
		m_renderer = NULL;
	}
	if (m_window != NULL) {
		SDL_DestroyWindow(m_window);
		m_window = NULL;
	}
	m_textureHasFrame = false;
	m_visible = false;
	m_taskbarTabHidden = false;
	m_videoWidth = 0;
	m_videoHeight = 0;
}

bool CCleanFeedOutput::HideTaskbarTab(HWND hwnd)
{
	if (hwnd == NULL) {
		return false;
	}

	// Keep WS_EX_APPWINDOW so Discord can enumerate this normal application
	// window, then remove only the Shell taskbar tab. This does not change the
	// window's visibility, title, or capture eligibility.
	HRESULT initializeResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	bool shouldUninitialize = SUCCEEDED(initializeResult);
	if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE) {
		return false;
	}

	ITaskbarList* taskbar = NULL;
	HRESULT result = CoCreateInstance(CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&taskbar));
	if (SUCCEEDED(result)) {
		result = taskbar->HrInit();
		if (SUCCEEDED(result)) {
			result = taskbar->DeleteTab(hwnd);
		}
		taskbar->Release();
	}

	if (shouldUninitialize) {
		CoUninitialize();
	}
	return SUCCEEDED(result);
}

bool CCleanFeedOutput::UpdateMainWindowCaptureExclusion()
{
	if (m_mainHwnd == NULL) {
		return false;
	}
	bool exclude = m_settingsCaptureExclusion || m_temporaryCaptureExclusion ||
		m_privacyModeCaptureExclusion;
	if (exclude == m_captureExclusionRequested) {
		return !exclude || !m_captureExclusionFailed;
	}

	DWORD affinity = exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
	if (SetWindowDisplayAffinity(m_mainHwnd, affinity)) {
		m_captureExclusionRequested = exclude;
		m_captureExclusionFailed = false;
		return true;
	}

	if (exclude) {
		m_captureExclusionFailed = true;
		printf("Could not exclude receiver window from capture (error %lu)\n", GetLastError());
	}
	return false;
}

void CCleanFeedOutput::InvalidateVideoTexture()
{
	if (m_videoTexture != NULL) {
		SDL_DestroyTexture(m_videoTexture);
		m_videoTexture = NULL;
	}
	m_textureHasFrame = false;
	m_videoWidth = 0;
	m_videoHeight = 0;
}

bool CCleanFeedOutput::EnsureVideoTexture(int width, int height)
{
	if (!EnsureSurface() || width <= 0 || height <= 0) {
		return false;
	}
	if (m_videoTexture != NULL && m_videoWidth == width && m_videoHeight == height) {
		return true;
	}

	InvalidateVideoTexture();
	m_videoTexture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_IYUV,
		SDL_TEXTUREACCESS_STREAMING, width, height);
	if (m_videoTexture == NULL) {
		printf("Could not create clean-feed texture: %s\n", SDL_GetError());
		return false;
	}
	m_videoWidth = width;
	m_videoHeight = height;
	return true;
}

bool CCleanFeedOutput::UploadYUV(int width, int height,
	const Uint8* yPlane, int yPitch,
	const Uint8* uPlane, int uPitch,
	const Uint8* vPlane, int vPitch)
{
	if (!m_enabled || !EnsureVideoTexture(width, height) ||
		yPlane == NULL || uPlane == NULL || vPlane == NULL) {
		return false;
	}

	if (SDL_UpdateYUVTexture(m_videoTexture, NULL,
		yPlane, yPitch, uPlane, uPitch, vPlane, vPitch) != 0) {
		printf("SDL_UpdateYUVTexture failed for clean feed: %s\n", SDL_GetError());
		return false;
	}
	m_textureHasFrame = true;
	return true;
}

void CCleanFeedOutput::PositionBehindMain(const SDL_Rect& captureBounds,
	int mainRenderWidth, int mainRenderHeight)
{
	if (m_window == NULL || m_mainHwnd == NULL ||
		captureBounds.w <= 0 || captureBounds.h <= 0 ||
		mainRenderWidth <= 0 || mainRenderHeight <= 0) {
		return;
	}

	RECT mainClient = {};
	if (!GetClientRect(m_mainHwnd, &mainClient)) {
		return;
	}
	int clientWidth = mainClient.right - mainClient.left;
	int clientHeight = mainClient.bottom - mainClient.top;
	if (clientWidth <= 0 || clientHeight <= 0) {
		return;
	}

	int left = ScaleCoordinate(captureBounds.x, mainRenderWidth, clientWidth);
	int top = ScaleCoordinate(captureBounds.y, mainRenderHeight, clientHeight);
	int right = ScaleCoordinate(captureBounds.x + captureBounds.w, mainRenderWidth, clientWidth);
	int bottom = ScaleCoordinate(captureBounds.y + captureBounds.h, mainRenderHeight, clientHeight);
	int width = right - left;
	int height = bottom - top;
	if (width < 1 || height < 1) {
		return;
	}

	POINT screenPoint = { 0, 0 };
	if (!ClientToScreen(m_mainHwnd, &screenPoint)) {
		return;
	}

	// Resize through SDL first. Directly resizing the HWND leaves SDL's D3D
	// renderer at its original 1x1 backbuffer on some systems, which makes OBS
	// receive a single averaged pixel stretched across the capture source.
	int mainWindowWidth = 0;
	int mainWindowHeight = 0;
	SDL_GetWindowSize(m_mainWindow, &mainWindowWidth, &mainWindowHeight);
	if (mainWindowWidth <= 0 || mainWindowHeight <= 0) {
		return;
	}
	int logicalLeft = ScaleCoordinate(captureBounds.x, mainRenderWidth, mainWindowWidth);
	int logicalTop = ScaleCoordinate(captureBounds.y, mainRenderHeight, mainWindowHeight);
	int logicalRight = ScaleCoordinate(captureBounds.x + captureBounds.w,
		mainRenderWidth, mainWindowWidth);
	int logicalBottom = ScaleCoordinate(captureBounds.y + captureBounds.h,
		mainRenderHeight, mainWindowHeight);
	int logicalWidth = logicalRight - logicalLeft;
	int logicalHeight = logicalBottom - logicalTop;
	if (logicalWidth < 1 || logicalHeight < 1) {
		return;
	}

	SDL_SysWMinfo wmInfo = {};
	SDL_VERSION(&wmInfo.version);
	if (!SDL_GetWindowWMInfo(m_window, &wmInfo) || wmInfo.subsystem != SDL_SYSWM_WINDOWS) {
		return;
	}

	// Move to the main window's monitor before SDL translates the requested
	// logical dimensions for that monitor's DPI.
	SetWindowPos(wmInfo.info.win.window, m_mainHwnd,
		screenPoint.x + left, screenPoint.y + top, 0, 0,
		SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	int currentLogicalWidth = 0;
	int currentLogicalHeight = 0;
	SDL_GetWindowSize(m_window, &currentLogicalWidth, &currentLogicalHeight);
	if (currentLogicalWidth != logicalWidth || currentLogicalHeight != logicalHeight) {
		SDL_SetWindowSize(m_window, logicalWidth, logicalHeight);
	}

	m_visible = SetWindowPos(wmInfo.info.win.window, m_mainHwnd,
		screenPoint.x + left, screenPoint.y + top, 0, 0,
		SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW) != FALSE;
	if (m_visible && !m_taskbarTabHidden) {
		m_taskbarTabHidden = HideTaskbarTab(wmInfo.info.win.window);
	}
}

void CCleanFeedOutput::Render(const SDL_Rect& mainDestination, const SDL_Rect& captureBounds,
	int mainRenderWidth, int mainRenderHeight, int rotation,
	bool connected, bool mainWindowVisible, bool mainWindowMinimized,
	bool capturePrivacyActive)
{
	if (!m_enabled || !connected || !mainWindowVisible || mainWindowMinimized ||
		(!capturePrivacyActive && (!m_textureHasFrame || m_videoTexture == NULL)) ||
		captureBounds.w <= 0 || captureBounds.h <= 0) {
		Hide();
		return;
	}
	if (!EnsureSurface()) {
		return;
	}

	PositionBehindMain(captureBounds, mainRenderWidth, mainRenderHeight);
	if (!m_visible) {
		return;
	}

	int outputWidth = 0;
	int outputHeight = 0;
	if (SDL_GetRendererOutputSize(m_renderer, &outputWidth, &outputHeight) != 0 ||
		outputWidth <= 0 || outputHeight <= 0) {
		return;
	}
	if ((outputWidth == 1 && captureBounds.w > 1) ||
		(outputHeight == 1 && captureBounds.h > 1)) {
		// SDL processes a native resize lazily on a few D3D backends. Never
		// present that stale 1x1 backbuffer; it will be correct on the next frame.
		SDL_RenderFlush(m_renderer);
		if (SDL_GetRendererOutputSize(m_renderer, &outputWidth, &outputHeight) != 0 ||
			outputWidth <= 1 || outputHeight <= 1) {
			return;
		}
	}

	float scaleX = (float)outputWidth / (float)captureBounds.w;
	float scaleY = (float)outputHeight / (float)captureBounds.h;
	SDL_Rect destination = {};
	destination.x = (int)lroundf((float)(mainDestination.x - captureBounds.x) * scaleX);
	destination.y = (int)lroundf((float)(mainDestination.y - captureBounds.y) * scaleY);
	destination.w = (int)lroundf((float)mainDestination.w * scaleX);
	destination.h = (int)lroundf((float)mainDestination.h * scaleY);

	SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
	SDL_RenderClear(m_renderer);
	if (!capturePrivacyActive && SDL_RenderCopyEx(m_renderer, m_videoTexture, NULL, &destination,
		(double)rotation, NULL, SDL_FLIP_NONE) != 0) {
		printf("SDL_RenderCopyEx failed for clean feed: %s\n", SDL_GetError());
		return;
	}
	SDL_RenderPresent(m_renderer);
}

void CCleanFeedOutput::HandleRendererReset()
{
	InvalidateVideoTexture();
}

void CCleanFeedOutput::Hide()
{
	if (m_window != NULL && m_visible) {
		SDL_HideWindow(m_window);
	}
	m_visible = false;
	m_taskbarTabHidden = false;
}
