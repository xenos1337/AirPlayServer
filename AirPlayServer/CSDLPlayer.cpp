#include "CSDLPlayer.h"
#include <stdio.h>
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
	, m_yuv(NULL)
	, m_bAudioInited(false)
	, m_bDumpAudio(false)
	, m_fileWav(NULL)
	, m_sAudioFmt()
	, m_displayRect()
	, m_videoWidth(0)
	, m_videoHeight(0)
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
	m_mutexAudio = CreateMutex(NULL, FALSE, NULL);
	m_mutexVideo = CreateMutex(NULL, FALSE, NULL);
	s_instance = this;
}

CSDLPlayer::~CSDLPlayer()
{
	// Restore original window procedure before cleanup
	if (m_hwnd != NULL && m_originalWndProc != NULL) {
		SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, (LONG_PTR)m_originalWndProc);
	}
	
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
	
	/* Main loop - poll events and render ImGui */
	while (!bEndLoop) {
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
					// Video source size changed - recreate overlay
					{
						CAutoLock oLock(m_mutexVideo, "recreateOverlay");
						if (m_yuv != NULL) {
							SDL_FreeYUVOverlay(m_yuv);
							m_yuv = NULL;
						}
						m_videoWidth = width;
						m_videoHeight = height;
						m_yuv = SDL_CreateYUVOverlay(m_videoWidth, m_videoHeight, SDL_IYUV_OVERLAY, m_surface);
						
						// Initialize overlay to black to prevent green flicker
						if (m_yuv != NULL) {
							SDL_LockYUVOverlay(m_yuv);
							memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);
							memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * (m_yuv->h >> 1));
							memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * (m_yuv->h >> 1));
							SDL_UnlockYUVOverlay(m_yuv);
						}
					}
					calculateDisplayRect();
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
		
		// Render ImGui every frame (outside of event polling)
		m_imgui.NewFrame(m_surface);
		if (m_bConnected) {
			m_imgui.RenderOverlay(&bShowUI, m_serverName, m_bConnected, m_connectedDeviceName);
		} else {
			m_imgui.RenderHomeScreen(m_serverName, m_bConnected, m_connectedDeviceName);
		}
		m_imgui.Render(m_surface);
		
		// Update display - flip surface to show video + ImGui composite
		SDL_Flip(m_surface);
		
		// Small delay to avoid maxing out CPU and prevent video processing delays
		SDL_Delay(16); // 30 FPS
	}
}

void CSDLPlayer::outputVideo(SFgVideoFrame* data) 
{
	if (data->width == 0 || data->height == 0) {
		return;
	}

	// Check if video source dimensions changed
	if ((int)data->width != m_videoWidth || (int)data->height != m_videoHeight) {
		{
			CAutoLock oLock(m_mutexVideo, "unInitVideo");
			if (NULL != m_yuv) {
				SDL_FreeYUVOverlay(m_yuv);
				m_yuv = NULL;
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

	CAutoLock oLock(m_mutexVideo, "outputVideo");
	if (m_yuv == NULL) {
		return;
	}

	// Convert YUV to RGB and draw to surface (allows ImGui compositing)
	CAutoLock surfaceLock(m_mutexVideo, "outputVideoSurface");
	
	if (m_surface == NULL) return;
	
	// Lock surface for direct pixel access
	if (SDL_MUSTLOCK(m_surface)) {
		SDL_LockSurface(m_surface);
	}
	
	// Get YUV plane pointers
	const Uint8* yPlane = data->data;
	const Uint8* uPlane = data->data + data->dataLen[0];
	const Uint8* vPlane = data->data + data->dataLen[0] + data->dataLen[1];
	
	// Calculate scaling factors
	float scaleX = (float)data->width / m_displayRect.w;
	float scaleY = (float)data->height / m_displayRect.h;
	
	// Draw scaled video to display rect area
	for (int dy = 0; dy < m_displayRect.h; dy++) {
		int screenY = m_displayRect.y + dy;
		if (screenY < 0 || screenY >= m_surface->h) continue;
		
		int srcY = (int)(dy * scaleY);
		if (srcY >= (int)data->height) srcY = data->height - 1;
		
		Uint32* dstRow = (Uint32*)((Uint8*)m_surface->pixels + screenY * m_surface->pitch);
		
		for (int dx = 0; dx < m_displayRect.w; dx++) {
			int screenX = m_displayRect.x + dx;
			if (screenX < 0 || screenX >= m_surface->w) continue;
			
			int srcX = (int)(dx * scaleX);
			if (srcX >= (int)data->width) srcX = data->width - 1;
			
			// Get YUV values (U and V are subsampled 2x2)
			int y = yPlane[srcY * data->pitch[0] + srcX];
			int u = uPlane[(srcY / 2) * data->pitch[1] + (srcX / 2)];
			int v = vPlane[(srcY / 2) * data->pitch[2] + (srcX / 2)];
			
			// Convert to RGB
			Uint8 r, g, b;
			YUVToRGB(y, u, v, &r, &g, &b);
			
			// Write pixel
			dstRow[screenX] = SDL_MapRGB(m_surface->format, r, g, b);
		}
	}
	
	if (SDL_MUSTLOCK(m_surface)) {
		SDL_UnlockSurface(m_surface);
	}
	
	// Note: Don't flip here - the main loop will flip after ImGui is drawn
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
		m_queueAudio.push(dataClone);
	}
}

void CSDLPlayer::initVideo(int width, int height)
{
	m_windowWidth = width;
	m_windowHeight = height;
	
	// Create resizable window with software surface
	m_surface = SDL_SetVideoMode(width, height, 32, SDL_SWSURFACE | SDL_RESIZABLE);
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
	
	// Free old overlay before recreating surface
	if (m_yuv != NULL) {
		SDL_FreeYUVOverlay(m_yuv);
		m_yuv = NULL;
	}
	
	// Recreate surface with new size
	m_surface = SDL_SetVideoMode(width, height, 32, SDL_SWSURFACE | SDL_RESIZABLE);
	
	// Recreate YUV overlay for the new surface (if we have video)
	if (m_videoWidth > 0 && m_videoHeight > 0 && m_surface != NULL) {
		m_yuv = SDL_CreateYUVOverlay(m_videoWidth, m_videoHeight, SDL_IYUV_OVERLAY, m_surface);
		
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
	
	// Recalculate display rect for new window size
	calculateDisplayRect();
	
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
	
	// Free old overlay before recreating surface
	if (m_yuv != NULL) {
		SDL_FreeYUVOverlay(m_yuv);
		m_yuv = NULL;
	}
	
	// Recreate surface with new size
	m_surface = SDL_SetVideoMode(width, height, 32, SDL_SWSURFACE | SDL_RESIZABLE);
	
	// Recreate YUV overlay for the new surface (if we have video)
	if (m_videoWidth > 0 && m_videoHeight > 0 && m_surface != NULL) {
		m_yuv = SDL_CreateYUVOverlay(m_videoWidth, m_videoHeight, SDL_IYUV_OVERLAY, m_surface);
		
		// Initialize overlay to black (Y=0, U=128, V=128) to prevent green flicker
		if (m_yuv != NULL) {
			SDL_LockYUVOverlay(m_yuv);
			memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);           // Y = 0
			memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * (m_yuv->h >> 1));  // U = 128
			memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * (m_yuv->h >> 1));  // V = 128
			SDL_UnlockYUVOverlay(m_yuv);
		}
	}
	
	// Recalculate display rect
	calculateDisplayRect();
	
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
		
		m_surface = SDL_SetVideoMode(screenWidth, screenHeight, 32, SDL_SWSURFACE | SDL_NOFRAME);
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
		
		if (m_videoWidth > 0 && m_videoHeight > 0 && m_surface != NULL) {
			m_yuv = SDL_CreateYUVOverlay(m_videoWidth, m_videoHeight, SDL_IYUV_OVERLAY, m_surface);
			if (m_yuv != NULL) {
				SDL_LockYUVOverlay(m_yuv);
				memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);
				memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * (m_yuv->h >> 1));
				memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * (m_yuv->h >> 1));
				SDL_UnlockYUVOverlay(m_yuv);
			}
		}
		
		calculateDisplayRect();
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
		
		m_surface = SDL_SetVideoMode(width, height, 32, SDL_SWSURFACE | SDL_RESIZABLE);
		m_windowWidth = width;
		m_windowHeight = height;
		
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
		
		if (m_videoWidth > 0 && m_videoHeight > 0 && m_surface != NULL) {
			m_yuv = SDL_CreateYUVOverlay(m_videoWidth, m_videoHeight, SDL_IYUV_OVERLAY, m_surface);
			if (m_yuv != NULL) {
				SDL_LockYUVOverlay(m_yuv);
				memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);
				memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * (m_yuv->h >> 1));
				memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * (m_yuv->h >> 1));
				SDL_UnlockYUVOverlay(m_yuv);
			}
		}
		
		calculateDisplayRect();
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
		wanted_spec.samples = 1920;
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
	if (m_queueAudio.size() > 5) {
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
