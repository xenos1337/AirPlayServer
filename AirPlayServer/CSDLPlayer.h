#pragma once
#include <Windows.h>
#include "Airplay2Head.h"
#include <queue>
#include "SDL.h"
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
#define WINDOW_RESIZE_CODE 5

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

	// Audio volume control (volume in dB: 0.0 = max, -144.0 = mute)
	void setVolume(float dbVolume);

	// Window visibility control
	void showWindow();
	void hideWindow();
	void requestShowWindow();  // Thread-safe: posts event to show window
	void requestHideWindow();  // Thread-safe: posts event to hide window
	void requestToggleFullscreen();  // Thread-safe: posts event to toggle fullscreen

	// SDL2 window, renderer, and textures (GPU-accelerated)
	SDL_Window* m_window;
	SDL_Renderer* m_renderer;
	SDL_Texture* m_videoTexture;   // IYUV streaming texture (GPU does BT.709 colorspace + scaling)
	SDL_Rect m_displayRect;      // Where to display the video (centered with letterbox)

	// Video source dimensions (from AirPlay stream)
	int m_videoWidth;
	int m_videoHeight;

	// Frame timing for smooth playback
	unsigned long long m_lastFramePTS;      // PTS of last rendered frame
	DWORD m_lastFrameTime;                  // System time when last frame was rendered

	// Video statistics
	unsigned long long m_totalFrames;       // Total frames received
	unsigned long long m_droppedFrames;     // Frames dropped due to queue full
	DWORD m_fpsStartTime;                   // Start time for FPS calculation
	unsigned int m_fpsFrameCount;           // Frame count for FPS calculation
	float m_currentFPS;                     // Current FPS
	unsigned long long m_totalBytes;        // Total bytes received for bitrate calculation
	DWORD m_bitrateStartTime;                // Start time for bitrate calculation
	float m_currentBitrateMbps;             // Current bitrate in Mbps

	// Window dimensions
	int m_windowWidth;
	int m_windowHeight;

	SFgAudioFrame m_sAudioFmt;
	bool m_bAudioInited;
	SAudioFrameQueue m_queueAudio;
	HANDLE m_mutexAudio;
	HANDLE m_mutexVideo;
	SDL_AudioDeviceID m_audioDeviceID;  // SDL2 audio device
	volatile int m_audioVolume;  // SDL volume (0-128, where 128 = SDL_MIX_MAXVOLUME)
	volatile int m_localVolume;  // Local volume from UI slider (0-128, SDL scale)

	// Audio quality improvements
	static const int AUDIO_BUFFER_SAMPLES = 1024;   // ~21ms at 48kHz (balanced latency/quality)
	static const int AUDIO_QUEUE_MAX_FRAMES = 20;   // Max frames before dropping (~400ms buffer)
	static const int AUDIO_QUEUE_START_THRESHOLD = 3;  // Frames needed before starting playback
	int m_audioUnderrunCount;                       // Track underruns for diagnostics
	int m_audioDroppedFrames;                       // Track dropped frames for diagnostics
	bool m_audioFadeOut;                            // Fade out on underrun to prevent pops
	int m_audioFadeOutSamples;                      // Remaining samples in fade-out

	// Audio resampling (for matching system device sample rate)
	DWORD m_systemSampleRate;                       // System audio device sample rate
	DWORD m_streamSampleRate;                       // Incoming stream sample rate
	uint8_t* m_resampleBuffer;                      // Buffer for resampled audio
	int m_resampleBufferSize;                       // Size of resample buffer
	double m_resamplePos;                           // Fractional position for linear interpolation
	bool m_needsResampling;                         // Flag indicating resampling is needed

	// Dynamic limiter (normalize loud sounds)
	float m_limiterGain;                            // Current limiter gain (0.0 to 1.0)
	float m_peakLevel;                              // Peak level for UI display (0.0 to 1.0)
	float m_deviceVolumeNormalized;                 // Device volume normalized to 0.0-1.0 for UI
	bool m_autoAdjustEnabled;                       // Normalize feature enabled
	static constexpr float LIMITER_THRESHOLD = 0.5f;   // Start limiting above 50% level (more aggressive)
	static constexpr float LIMITER_RATIO = 0.6f;       // Output level when limiting (60% = quieter)
	static constexpr float LIMITER_ATTACK = 0.002f;    // Fast attack (2ms)
	static constexpr float LIMITER_RELEASE = 0.100f;   // Slower release (100ms) for smoother sound

	SDL_Event m_evtVideoSizeChange;

	bool m_bDumpAudio;
	FILE* m_fileWav;

	// Performance CSV log (written every frame while connected)
	FILE* m_filePerfLog;
	LARGE_INTEGER m_qpcPerfLogStart;      // QPC at first logged frame (t=0 reference)

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
	void resizeWindow(int width, int height);  // Handle window resize

	// Window handle for show/hide
	HWND m_hwnd;
	bool m_bWindowVisible;

	void handleLiveResize(int width, int height);
	void requestResize(int width, int height);  // Thread-safe: posts event to resize window
	volatile bool m_bResizing;  // Prevent re-entrancy during resize (volatile for thread safety)
	volatile int m_pendingResizeWidth;   // Pending resize dimensions
	volatile int m_pendingResizeHeight;

	// Fullscreen toggle
	void toggleFullscreen();
	bool m_bFullscreen;
	int m_windowedX;          // Saved windowed position
	int m_windowedY;
	int m_windowedW;          // Saved windowed size
	int m_windowedH;

	// Cursor auto-hide after inactivity
	DWORD m_lastMouseMoveTime;  // Time of last mouse movement
	bool m_bCursorHidden;       // Whether cursor is currently hidden
	static const DWORD CURSOR_HIDE_DELAY_MS = 5000;  // Hide after 5 seconds

	// Performance monitoring (F1 toggles graph overlay)
	bool m_bShowPerfGraphs;
	LARGE_INTEGER m_qpcFreq;              // QueryPerformanceCounter frequency
	LARGE_INTEGER m_qpcFrameStart;        // QPC at start of render frame
	volatile LONGLONG m_qpcFrameArrival;  // QPC when latest frame arrived in outputVideo
	static const int PERF_HISTORY = 30;   // 30 seconds of history (1 sample/sec)
	float m_perfFps[PERF_HISTORY];            // Source FPS history (avg per second)
	float m_perfDisplayFps[PERF_HISTORY];    // Display FPS history (actual GPU uploads per second)
	float m_perfFrameTime[PERF_HISTORY];     // Render frame time in ms (avg per second)
	float m_perfLatency[PERF_HISTORY];       // Decode-to-display latency in ms (avg per second)
	float m_perfBitrate[PERF_HISTORY];       // Bitrate in Mbps (per second)
	float m_perfAudioQueue[PERF_HISTORY];    // Audio queue depth in frames (sampled per second)
	int m_perfIdx;                           // Current write index in circular buffers

	// 1-second accumulators for perf graph sampling
	LARGE_INTEGER m_qpcPerfLastUpdate;    // QPC when last perf sample was written
	float m_perfAccumFrameTime;           // Accumulated frame times this second
	float m_perfAccumLatency;             // Accumulated latency this second
	int m_perfAccumCount;                 // Number of frames accumulated this second

	// Double-buffered YUV420P planes for lockless producer-consumer pattern
	// Callback thread copies raw YUV planes into write buffer
	// Main thread uploads to GPU via SDL_UpdateYUVTexture (GPU does BT.709 conversion)
	uint8_t* m_yuvBuffer[2][3];      // [buffer_index][plane] - two sets of Y, U, V planes
	int m_yuvPitch[3];                // Pitches for each YUV plane (32-byte aligned)
	volatile LONG m_yuvWriteIdx;      // Index of buffer being written by producer (0 or 1)
	volatile LONG m_yuvReadIdx;       // Index of buffer ready for consumer (0 or 1)
	volatile LONG m_yuvReady;         // Flag: 1 = new YUV frame available

	// Frame pacing for smooth output (absorbs bursty TCP/WiFi delivery)
	// Instead of displaying frames immediately on arrival (bursty), upload to GPU at fixed intervals
	LARGE_INTEGER m_qpcLastNewFrame;    // QPC when last new frame was uploaded to GPU
	double m_targetFrameIntervalMs;      // Target display interval (16.67ms=60fps, 33.33ms=30fps)
	float m_displayFPS;                  // Actual display FPS (frames uploaded to GPU per second)
	unsigned int m_displayFrameCount;    // Counter for display FPS calculation
	DWORD m_displayFpsStartTime;         // Start time for display FPS calculation
	DWORD m_connectionStartTime;         // GetTickCount when connection started (for uptime)
};
