#pragma once

#include <Windows.h>
#include "SDL.h"

// A video-only SDL surface kept behind the interactive receiver window. OBS
// captures this surface while the normal window remains available for local UI.
class CCleanFeedOutput
{
public:
	CCleanFeedOutput();
	~CCleanFeedOutput();

	bool Init(SDL_Window* mainWindow, HWND mainHwnd);
	void Shutdown();

	void ApplySettings(bool enabled, bool excludeMainWindowFromCapture);
	// PIN privacy temporarily excludes the receiver independently of the
	// persistent Screen Cast setting. Returns false if Windows rejects it.
	bool SetTemporaryMainWindowCaptureExclusion(bool exclude);
	bool SetPrivacyModeMainWindowCaptureExclusion(bool exclude);
	bool IsEnabled() const { return m_enabled; }
	bool IsReady() const { return m_window != NULL && m_renderer != NULL; }
	bool HasVideoFrame() const { return m_textureHasFrame; }
	bool IsCaptureExclusionAvailable() const { return !m_captureExclusionFailed; }

	void InvalidateVideoTexture();
	bool UploadYUV(int width, int height,
		const Uint8* yPlane, int yPitch,
		const Uint8* uPlane, int uPitch,
		const Uint8* vPlane, int vPitch);

	// Coordinates are in the main renderer's output pixel space. The destination
	// uses the same transform as the visible player, while captureBounds selects
	// only the part intended for OBS.
	void Render(const SDL_Rect& mainDestination, const SDL_Rect& captureBounds,
		int mainRenderWidth, int mainRenderHeight, int rotation,
		bool connected, bool mainWindowVisible, bool mainWindowMinimized,
		bool capturePrivacyActive = false);
	void HandleRendererReset();
	void Hide();

private:
	bool EnsureSurface();
	bool EnsureVideoTexture(int width, int height);
	void DestroySurface();
	bool HideTaskbarTab(HWND hwnd);
	bool UpdateMainWindowCaptureExclusion();
	void PositionBehindMain(const SDL_Rect& captureBounds,
		int mainRenderWidth, int mainRenderHeight);

	SDL_Window* m_mainWindow;
	HWND m_mainHwnd;
	SDL_Window* m_window;
	SDL_Renderer* m_renderer;
	SDL_Texture* m_videoTexture;
	bool m_enabled;
	bool m_textureHasFrame;
	bool m_visible;
	bool m_taskbarTabHidden;
	bool m_settingsCaptureExclusion;
	bool m_temporaryCaptureExclusion;
	bool m_privacyModeCaptureExclusion;
	bool m_captureExclusionRequested;
	bool m_captureExclusionFailed;
	int m_videoWidth;
	int m_videoHeight;
};
