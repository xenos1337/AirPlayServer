#pragma once
#include <Windows.h>
#include "Airplay2Head.h"
#include <queue>
#include "SDL.h"
#undef main
#include "AirPlayCore.h"
#include "CAirServer.h"
#include "CCleanFeedOutput.h"

typedef struct SAudioFrame {
	unsigned long long pts;
	unsigned int dataTotal;
	unsigned int dataLeft;
	unsigned char* data;
} SAudioFrame;

typedef std::queue<SAudioFrame*> SAudioFrameQueue;

#define VIDEO_SIZE_CHANGED_CODE 1
#define CONNECTION_STATE_CHANGED_CODE 2

enum EQualityPreset
{
	QUALITY_GOOD = 0,      // 30fps, best filtering - maximum quality per frame
	QUALITY_BALANCED = 1,  // 60fps, best filtering - smooth + high quality (default)
	QUALITY_FAST = 2       // 60fps, linear filtering - lowest latency
};

// Owns the receiver, audio output and the SDL renderer. SDL runs entirely on a
// private render thread that draws into a child window of the WPF main window.
class CSDLPlayer
{
public:
	CSDLPlayer();
	~CSDLPlayer();

	bool start(HWND topLevel, const ApCallbacks& callbacks, const ApConfig& config);
	void stop();
	HWND viewWindow() const { return m_hwnd; }
	// False if the receiver thread could not be stopped; the object must then
	// be leaked because that thread may still use it.
	bool canDestroy() const;

	// UI thread commands
	void setConfig(const ApConfig& config);
	int rotate();
	void resetView();
	void setCapturePrivacy(bool enabled);
	void getStats(ApStats* stats);
	void answerPinApproval(LONG generation, bool allow);
	void dismissPinApproval(LONG generation);
	// Re-registers the receiver with Bonjour once no device is connected.
	void requestRestart();
	// Ends the connected sender's session.
	void requestDisconnect();

	static void getMonitorSize(HWND window, unsigned int& width, unsigned int& height);

	// Receiver callbacks (network threads)
	void setConnected(bool connected, const char* deviceName = NULL);
	// Waits for the UI's allow/deny choice. An allowed PIN stays visible until
	// the UI dismisses it.
	bool requestPinApproval(const char* remoteAddress, const char* pin);
	void outputVideo(SFgVideoFrame* data);
	void outputAudio(SFgAudioFrame* data);
	// Audio volume control (volume in dB: 0.0 = max, -144.0 = mute)
	void setVolume(float dbVolume);

private:
	static DWORD WINAPI renderThreadProc(LPVOID parameter);
	static LRESULT CALLBACK videoWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	void renderThread();
	bool initRenderer();
	void shutdownRenderer();
	void loopEvents();
	void renderFrame();
	void updateStatsSnapshot();
	bool isShuttingDown();

	void initAudio(SFgAudioFrame* data);
	void unInitAudio();
	static void sdlAudioCallback(void* userdata, Uint8* stream, int len);

	void applyConnectionState(bool connected, const char* deviceName);
	void applyReceiverSettings();
	void requestServerStart(bool pinEnabled, unsigned int displayWidth, unsigned int displayHeight);
	void stopServerThread();
	void requestAdvertising(bool enabled);
	static DWORD WINAPI serverThreadProc(LPVOID parameter);
	void serverThread();
	void resetSessionStatistics();
	void clearSessionVideoFrame();
	bool generateSessionAirPlayPin();
	void cancelPinApproval();
	void setServerName(const char* serverName);
	void getAdvertisedDisplaySize(const ApConfig& config, unsigned int& width, unsigned int& height) const;
	void applyQualityPreset(int preset);

	void calculateDisplayRect();  // Calculate fitted, zoomed display rect
	SDL_Rect calculateFittedVideoBounds() const;  // Visible video bounds at 1x, after rotation
	SDL_Rect calculateZoomedVideoBounds() const;  // Visible video bounds after zoom/pan
	bool recreateVideoTexture();  // Create and initialize texture from the latest CPU YUV buffer
	void recreateCleanFeedTexture();
	SDL_Rect calculateScreenCastCaptureBounds(bool cropToVideo) const;
	void windowToRendererCoordinates(float windowX, float windowY, float& rendererX, float& rendererY) const;
	void applyWheelZoom(float wheelDelta, float mouseX, float mouseY);
	void applyDragPan(float deltaX, float deltaY);
	void stopPanning();
	void resetZoom();
	void resetGesture();
	bool isCursorOverView() const;

	ApCallbacks m_callbacks;
	HWND m_topLevel;
	HWND m_hwnd;                 // Child window owned by the render thread
	HANDLE m_renderThread;
	HANDLE m_renderReady;        // Signaled once initRenderer() has succeeded
	volatile LONG m_quitRequested;

	HANDLE m_mutexConfig;
	ApConfig m_config;
	ApConfig m_frameConfig;      // Render thread copy of m_config for the current frame

	HANDLE m_mutexStats;
	ApStats m_stats;

	// SDL2 window, renderer, and textures (GPU-accelerated)
	SDL_Window* m_window;
	SDL_Renderer* m_renderer;
	char m_rendererName[32];       // For diagnostics
	SDL_Texture* m_videoTexture;   // IYUV streaming texture (GPU does colorspace + scaling)
	bool m_videoTextureHasFrame;   // Never draw an IYUV texture before its first successful upload
	SDL_Rect m_displayRect;      // Where to display the video (centered with letterbox)
	volatile LONG m_rotationAngle;  // Video rotation in degrees (0, 90, 180, 270), written by the UI thread
	LONG m_appliedRotation;      // Rotation the current display rect was computed for
	float m_zoomLevel;           // View zoom: 1.0 = fit to window
	float m_zoomPanX;            // Normalized horizontal focal offset (-1.0 to 1.0)
	float m_zoomPanY;            // Normalized vertical focal offset (-1.0 to 1.0)
	bool m_bPanning;             // Left-button pan interaction is active
	bool m_bLeftButtonDown;      // Tracks a click through button-up
	bool m_bPanMoved;            // Movement exceeded the double-click drag threshold
	float m_panStartX;           // Pan/click start in renderer pixels
	float m_panStartY;
	float m_panLastX;            // Previous pan position in renderer pixels
	float m_panLastY;
	Uint8 m_leftClickCount;      // Click count captured on button-down
	volatile LONG m_zoomResetPending;  // Resets rotation and zoom on the render thread
	volatile LONG m_capturePrivacyActive;  // Clean feed shows black while set

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
	unsigned long long m_lastBitrateTotalBytes; // Session-local baseline for bitrate deltas
	DWORD m_bitrateStartTime;                // Start time for bitrate calculation
	float m_currentBitrateMbps;             // Current bitrate in Mbps

	// Child window dimensions
	int m_windowWidth;
	int m_windowHeight;

	SFgAudioFrame m_sAudioFmt;
	bool m_bAudioInited;
	SAudioFrameQueue m_queueAudio;
	HANDLE m_mutexAudio;
	HANDLE m_mutexVideo;
	SDL_AudioDeviceID m_audioDeviceID;  // SDL2 audio device
	static constexpr float AIRPLAY_MIN_DB = -30.0f;  // Bottom of the sender's volume slider
	volatile float m_senderGain;  // Amplitude from the sender's volume (0.0-1.0)
	volatile int m_localVolume;  // Local volume from UI slider (0-128, SDL scale)

	// Audio quality improvements
	static const int AUDIO_BUFFER_SAMPLES = 1024;   // ~21ms at 48kHz (balanced latency/quality)
	static const int AUDIO_QUEUE_MAX_FRAMES = 20;   // Max frames before dropping (~400ms buffer)
	static const int AUDIO_QUEUE_TARGET_FRAMES = 8; // Closed-loop resampler target (~170ms at 48kHz)
	static const int AUDIO_QUEUE_START_THRESHOLD = AUDIO_QUEUE_TARGET_FRAMES;
	static constexpr double AUDIO_RESAMPLE_PROPORTIONAL_GAIN = 0.001; // 1000ppm per frame
	static constexpr double AUDIO_RESAMPLE_INTEGRAL_GAIN = 0.0000005; // 0.5ppm per frame per packet
	static constexpr double AUDIO_RESAMPLE_MAX_CORRECTION = 0.005; // Never alter pitch by more than 0.5%
	int m_audioUnderrunCount;                       // Track underruns for diagnostics
	int m_audioDroppedFrames;                       // Track dropped frames for diagnostics
	bool m_audioFadeOut;                            // Fade out on underrun to prevent pops
	int m_audioFadeOutSamples;                      // Remaining samples in fade-out

	// Audio resampling (for matching system device sample rate)
	DWORD m_systemSampleRate;                       // System audio device sample rate
	DWORD m_streamSampleRate;                       // Incoming stream sample rate
	uint8_t* m_resampleBuffer;                      // Buffer for resampled audio
	int m_resampleBufferSize;                       // Size of resample buffer
	double m_resamplePos;                           // Fractional output sample carried between input frames
	double m_resampleCorrection;                    // Learned sender/device clock correction
	bool m_needsResampling;                         // Flag indicating resampling is needed

	float m_peakLevel;                              // Peak level for UI display (0.0 to 1.0)
	float m_deviceVolumeNormalized;                 // Device volume normalized to 0.0-1.0 for UI

	SDL_Event m_evtVideoSizeChange;

	// Performance CSV log (written every frame while connected)
	FILE* m_filePerfLog;
	LARGE_INTEGER m_qpcPerfLogStart;      // QPC at first logged frame (t=0 reference)

	CAirServer m_server;
	char m_serverName[256];  // Server name for AirPlay display
	CCleanFeedOutput m_cleanFeed;  // Video-only surface for OBS/Display Capture
	char m_sessionAirPlayPin[5];  // Temporary 4-digit PIN; never persisted
	HANDLE m_mutexPinApproval;
	HANDLE m_eventPinApproval;
	LONG m_pinApprovalState;
	volatile LONG m_pinApprovalGeneration;
	char m_pendingPinRemote[64];
	char m_pendingPinCode[5];

	// Connection state
	bool m_bConnected;
	char m_connectedDeviceName[256];
	volatile LONG m_shuttingDown;  // Blocks callback events once server teardown starts

	// Receiver restart debounce. Name, PIN-policy and display changes restart
	// the service only after they settle, and never during an active session.
	char m_pendingServerName[256];
	bool m_pendingPinEnabled;
	unsigned int m_pendingDisplayWidth;
	unsigned int m_pendingDisplayHeight;
	DWORD m_receiverSettingsChangeTime;
	bool m_receiverSettingsPendingRestart;
	bool m_activePinEnabled;
	unsigned int m_activeDisplayWidth;
	unsigned int m_activeDisplayHeight;
	volatile LONG m_restartRequested;

	// Receiver start/stop runs on its own thread (see serverThread).
	struct SServerRequest
	{
		char name[256];
		bool pinEnabled;
		unsigned int displayWidth;
		unsigned int displayHeight;
	};
	static const DWORD SERVER_STOP_TIMEOUT_MS = 5000;
	static const DWORD SERVER_RETRY_INTERVAL_MS = 10000;
	HANDLE m_serverThread;
	HANDLE m_serverWake;
	HANDLE m_mutexServer;
	SServerRequest m_serverRequest;
	bool m_serverStartRequested;
	bool m_serverStopRequested;
	bool m_serverStopTimedOut;
	bool m_advertisingChangeRequested;
	bool m_disconnectRequested;
	bool m_advertising;
	volatile LONG m_serverState;  // AP_SERVER_*
	DWORD m_serverStartRequestTime;  // Render thread only

	int m_appliedQualityPreset;
	bool m_videoReadyPending;  // Announce the next uploaded frame (new session or size)

	volatile bool m_bResizing;  // Blocks outputVideo while the render thread reallocates buffers

	// Cursor auto-hide after inactivity
	DWORD m_lastMouseMoveTime;  // Time of last mouse movement
	bool m_bCursorHidden;       // Whether cursor is currently hidden
	SDL_Cursor* m_panCursor;    // Move cursor while an active drag owns the view
	static const DWORD CURSOR_HIDE_DELAY_MS = 5000;  // Hide after 5 seconds

	// Performance monitoring
	LARGE_INTEGER m_qpcFreq;              // QueryPerformanceCounter frequency
	LARGE_INTEGER m_qpcFrameStart;        // QPC at start of render frame
	volatile LONGLONG m_qpcFrameArrival;  // QPC when latest frame arrived in outputVideo
	static const int PERF_HISTORY = AP_HISTORY_SIZE;   // 30 seconds of history (1 sample/sec)
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
	// Render thread uploads to GPU via SDL_UpdateYUVTexture
	uint8_t* m_yuvBuffer[2][3];      // [buffer_index][plane] - two sets of Y, U, V planes
	int m_yuvPitch[3];                // Pitches for each YUV plane (32-byte aligned)
	volatile LONG m_yuvWriteIdx;      // Index of buffer being written by producer (0 or 1)
	volatile LONG m_yuvReadIdx;       // Index of buffer ready for consumer (0 or 1)
	volatile LONG m_yuvReady;         // Flag: 1 = new YUV frame available

	// Frame pacing for smooth output (absorbs bursty TCP/WiFi delivery)
	LARGE_INTEGER m_qpcLastNewFrame;    // QPC when last new frame was uploaded to GPU
	double m_targetFrameIntervalMs;      // Target display interval (16.67ms=60fps, 33.33ms=30fps)
	float m_displayFPS;                  // Actual display FPS (frames uploaded to GPU per second)
	unsigned int m_displayFrameCount;    // Counter for display FPS calculation
	DWORD m_displayFpsStartTime;         // Start time for display FPS calculation
	DWORD m_connectionStartTime;         // GetTickCount when connection started (for uptime)
};
