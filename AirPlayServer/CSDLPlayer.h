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

// FFmpeg for high-quality video scaling
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

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
#define WINDOW_RESIZE_CODE 5
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

	SDL_Surface* m_surface;      // Main screen surface (only touched by main thread)
	SDL_Surface* m_videoBuffer;  // Off-screen buffer for video data (written by callback thread)
	SDL_Overlay* m_yuv;
	SDL_Rect m_displayRect;      // Where to display the video (centered with letterbox)
	
	// Video source dimensions (from AirPlay stream)
	int m_videoWidth;
	int m_videoHeight;
	
	// Frame timing for smooth playback
	unsigned long long m_lastFramePTS;      // PTS of last rendered frame
	DWORD m_lastFrameTime;                  // System time when last frame was rendered
	bool m_hasNewFrame;                     // Flag indicating new frame is available
	
	// Video statistics
	unsigned long long m_totalFrames;       // Total frames received
	unsigned long long m_droppedFrames;     // Frames dropped due to queue full
	DWORD m_fpsStartTime;                   // Start time for FPS calculation
	unsigned int m_fpsFrameCount;           // Frame count for FPS calculation
	float m_currentFPS;                     // Current FPS
	unsigned long long m_totalBytes;        // Total bytes received for bitrate calculation
	DWORD m_bitrateStartTime;                // Start time for bitrate calculation
	float m_currentBitrateMbps;             // Current bitrate in Mbps

	// Frame skip for 30fps mode
	unsigned int m_frameSkipCounter;        // Counter for frame skipping in Good Quality mode
	volatile LONG m_currentQualityPreset;   // Thread-safe copy of quality preset for callback thread
	
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
	void requestResize(int width, int height);  // Thread-safe: posts event to resize window
	volatile bool m_bResizing;  // Prevent re-entrancy during resize (volatile for thread safety)
	volatile int m_pendingResizeWidth;   // Pending resize dimensions
	volatile int m_pendingResizeHeight;
	
	// Fullscreen toggle on double-click
	void toggleFullscreen();
	bool m_bFullscreen;
	RECT m_windowedRect;      // Saved windowed position/size
	LONG m_windowedStyle;     // Saved windowed style
	LONG m_windowedExStyle;   // Saved windowed extended style

	// Cursor auto-hide after inactivity
	DWORD m_lastMouseMoveTime;  // Time of last mouse movement
	bool m_bCursorHidden;       // Whether cursor is currently hidden
	static const DWORD CURSOR_HIDE_DELAY_MS = 5000;  // Hide after 5 seconds

	// 1:1 pixel mode - resize window to match video for crisp rendering
	void resizeToVideoSize();  // Resize window to match video resolution exactly
	bool m_b1to1PixelMode;     // When true, window matches video size for no upscaling

	// High-quality video scaling with double-buffering (for fullscreen/resized windows)
	SwsContext* m_swsCtx;          // FFmpeg scaler context
	int m_scaledWidth;             // Current scaled output width
	int m_scaledHeight;            // Current scaled output height
	volatile LONG m_bScalerNeedsReinit;  // Flag to reinit scaler (thread-safe)

	// Deferred scaler cleanup (to handle cross-thread freeing safely)
	SwsContext* m_pendingFreeCtx;  // Context waiting to be freed (by callback thread)
	uint8_t* m_pendingFreeYUV[2][3];  // Buffers waiting to be freed

	// Double-buffered scaled YUV planes for lockless producer-consumer pattern
	// Buffer 0 = front buffer (being displayed), Buffer 1 = back buffer (being written)
	uint8_t* m_scaledYUV[2][3];    // [buffer_index][plane] - two sets of YUV planes
	int m_scaledPitch[3];          // Scaled YUV pitches (same for both buffers)
	volatile LONG m_writeBuffer;   // Index of buffer currently being written to (0 or 1)
	volatile LONG m_readBuffer;    // Index of buffer ready for reading (0 or 1)
	volatile LONG m_bufferReady;   // Flag: 1 = new frame ready in read buffer

	// Source video buffer for scaling outside mutex
	uint8_t* m_srcYUV[3];          // Source YUV planes (copy of incoming frame)
	int m_srcPitch[3];             // Source pitches
	int m_srcWidth;                // Source width
	int m_srcHeight;               // Source height
	volatile LONG m_srcReady;      // Flag: 1 = source data ready for scaling
	HANDLE m_scalingThread;        // Background thread for scaling
	HANDLE m_scalingEvent;         // Event to signal scaling thread
	volatile LONG m_scalingThreadRunning;  // Flag to control thread lifetime
	static DWORD WINAPI ScalingThreadProc(LPVOID param);  // Scaling thread function

	void initScaler(int srcW, int srcH, int dstW, int dstH);
	void freeScaler();
	void freePendingScalerResources();  // Free deferred scaler resources (called from callback thread)
};

