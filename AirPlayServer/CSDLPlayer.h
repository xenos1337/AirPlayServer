#pragma once
#include <Windows.h>
#include "Airplay2Head.h"
#include <queue>
#include "SDL.h"
#include "SDL_thread.h"
#include "SDL_syswm.h"
#undef main 
#include "CAirServer.h"
#include "CImGuiManager.h"

typedef void sdlAudioCallback(void* userdata, Uint8* stream, int len);

typedef struct SAudioFrame {
	unsigned long long pts;
	unsigned int dataTotal;
	unsigned int dataLeft;
	unsigned char* data;
} SAudioFrame;

typedef std::queue<SAudioFrame*> SAudioFrameQueue;
typedef std::queue<SFgVideoFrame*> SFgVideoFrameQueue;

#define VIDEO_SIZE_CHANGED_CODE 1
#define SHOW_WINDOW_CODE 2
#define HIDE_WINDOW_CODE 3
#define TOGGLE_FULLSCREEN_CODE 4
#define DOUBLE_CLICK_THRESHOLD_MS 400

class CSDLPlayer
{
public:
	CSDLPlayer();
	~CSDLPlayer();

	bool init();
	void unInit();
	void loopEvents();
	void setServerName(const char* serverName);
	void setConnected(bool connected, const char* deviceName = NULL);

	void outputVideo(SFgVideoFrame* data);
	void outputAudio(SFgAudioFrame* data);

	void initVideo(int width, int height);
	void unInitVideo();
	void initAudio(SFgAudioFrame* data);
	void unInitAudio();
	static void sdlAudioCallback(void* userdata, Uint8* stream, int len);

	// Window visibility control
	void showWindow();
	void hideWindow();
	void requestShowWindow();  // Thread-safe: posts event to show window
	void requestHideWindow();  // Thread-safe: posts event to hide window
	void requestToggleFullscreen();  // Thread-safe: posts event to toggle fullscreen

	SDL_Surface* m_surface;
	SDL_Overlay* m_yuv;
	SDL_Rect m_displayRect;    // Where to display the video (centered with letterbox)
	
	// Video source dimensions (from AirPlay stream)
	int m_videoWidth;
	int m_videoHeight;
	
	// Window dimensions
	int m_windowWidth;
	int m_windowHeight;

	SFgAudioFrame m_sAudioFmt;
	bool m_bAudioInited;
	SAudioFrameQueue m_queueAudio;
	HANDLE m_mutexAudio;
	HANDLE m_mutexVideo;

	SDL_Event m_evtVideoSizeChange;

	bool m_bDumpAudio;
	FILE* m_fileWav;

	CAirServer m_server;
	char m_serverName[256];  // Server name for AirPlay display
	CImGuiManager m_imgui;  // ImGui manager for UI overlay
	
	// Connection state for UI
	bool m_bConnected;
	char m_connectedDeviceName[256];
	
	// Disconnect transition (to render black screen briefly)
	bool m_bDisconnecting;
	DWORD m_dwDisconnectStartTime;
	
	void calculateDisplayRect();  // Calculate centered letterboxed display rect
	void clearToBlack();          // Fill surface with black
	void resizeWindow(int width, int height);  // Handle window resize

	// Window handle for show/hide
	HWND m_hwnd;
	bool m_bWindowVisible;
	
	// Window subclassing for live resize
	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	WNDPROC m_originalWndProc;
	static CSDLPlayer* s_instance;  // For accessing instance from static callback
	void handleLiveResize(int width, int height);
	bool m_bResizing;  // Prevent re-entrancy during resize
	
	// Fullscreen toggle on double-click
	void toggleFullscreen();
	bool m_bFullscreen;
	RECT m_windowedRect;      // Saved windowed position/size
	LONG m_windowedStyle;     // Saved windowed style
	LONG m_windowedExStyle;   // Saved windowed extended style
};

