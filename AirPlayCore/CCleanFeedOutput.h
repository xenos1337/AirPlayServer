#pragma once

#include <Windows.h>
#include "SDL.h"

// A video-only SDL surface kept behind the receiver window. OBS captures this
// surface while the normal window remains available for local UI.
class CCleanFeedOutput
{
public:
	CCleanFeedOutput();
	~CCleanFeedOutput();

	// videoWindow/videoHwnd is the view the clean feed mirrors; topLevelHwnd is
	// the receiver window it is kept directly behind.
	bool Init(SDL_Window* videoWindow, HWND videoHwnd, HWND topLevelHwnd);
	void Shutdown();

	void ApplySettings(bool enabled);
	bool IsEnabled() const { return m_enabled; }
	bool IsReady() const { return m_window != NULL && m_renderer != NULL; }
	bool HasVideoFrame() const { return m_textureHasFrame; }

	void InvalidateVideoTexture();
	bool UploadYUV(int width, int height,
		const Uint8* yPlane, int yPitch,
		const Uint8* uPlane, int uPitch,
		const Uint8* vPlane, int vPitch);

	// Coordinates are in the video renderer's output pixel space. The destination
	// uses the same transform as the visible player, while captureBounds selects
	// only the part intended for OBS.
	void Render(const SDL_Rect& mainDestination, const SDL_Rect& captureBounds,
		int mainRenderWidth, int mainRenderHeight, int rotation,
		bool connected, bool videoVisible, bool mainWindowMinimized,
		bool capturePrivacyActive);
	void HandleRendererReset();
	void Hide();

private:
	bool EnsureSurface();
	bool EnsureVideoTexture(int width, int height);
	void DestroySurface();
	bool HideTaskbarTab(HWND hwnd);
	void PositionBehindMain(const SDL_Rect& captureBounds,
		int mainRenderWidth, int mainRenderHeight);

	SDL_Window* m_videoWindow;
	HWND m_videoHwnd;
	HWND m_topLevelHwnd;
	SDL_Window* m_window;
	SDL_Renderer* m_renderer;
	SDL_Texture* m_videoTexture;
	bool m_enabled;
	bool m_textureHasFrame;
	bool m_visible;
	bool m_taskbarTabHidden;
	int m_videoWidth;
	int m_videoHeight;
};
