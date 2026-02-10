#pragma once
#include "imgui.h"
#include "SDL.h"

class CSDLPlayer;

// All performance data passed to the perf overlay (F1)
struct SPerfData
{
	// Graph histories (circular buffers, 1 sample/sec, 30s window)
	const float* sourceFpsHistory;
	const float* displayFpsHistory;
	const float* frameTimeHistory;
	const float* latencyHistory;
	const float* bitrateHistory;
	const float* audioQueueHistory;
	int historySize;
	int currentIdx;

	// Current values (for labels)
	float sourceFps;
	float displayFps;
	float frameTimeMs;
	float latencyMs;
	float bitrateMbps;
	float targetFps;       // From quality preset (30 or 60)

	// Video
	int videoWidth;
	int videoHeight;

	// Counters
	unsigned long long totalFrames;
	unsigned long long droppedFrames;
	unsigned long long totalBytes;
	int audioUnderruns;
	int audioDropped;
	int audioQueueSize;
	float connectionTimeSec;  // Time since connect in seconds
};

// Quality presets for video rendering
enum EQualityPreset
{
	QUALITY_GOOD = 0,      // 30fps, best filtering - maximum quality per frame
	QUALITY_BALANCED = 1,  // 60fps, best filtering - smooth + high quality (default)
	QUALITY_FAST = 2       // 60fps, linear filtering - lowest latency
};

class CImGuiManager
{
public:
	CImGuiManager();
	~CImGuiManager();

	bool Init(SDL_Window* window, SDL_Renderer* renderer);
	void Shutdown();
	void NewFrame();
	void Render();
	void ProcessEvent(SDL_Event* event);

	// UI rendering
	void RenderHomeScreen(const char* deviceName, bool isConnected, const char* connectedDeviceName, bool isServerRunning = true);
	void RenderDisconnectMessage(const char* deviceName);
	void RenderOverlay(bool* pShowUI, const char* deviceName, bool isConnected, const char* connectedDeviceName,
		int videoWidth = 0, int videoHeight = 0, float fps = 0.0f, float bitrateMbps = 0.0f,
		unsigned long long totalFrames = 0, unsigned long long droppedFrames = 0,
		unsigned long long totalBytes = 0);
	void RenderPerfGraphs(const SPerfData& perf);

	// Input handling
	bool WantCaptureMouse() const { return ImGui::GetIO().WantCaptureMouse; }
	bool WantCaptureKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }

	// Get edited device name
	const char* GetDeviceName() const;

	// Get quality preset
	EQualityPreset GetQualityPreset() const { return m_qualityPreset; }

	// Audio controls
	bool IsAutoAdjustEnabled() const { return m_bAutoAdjust; }
	void SetDeviceVolume(float volume) { m_deviceVolume = volume; }  // From AirPlay device (0.0-1.0)
	void SetCurrentAudioLevel(float level) { m_currentAudioLevel = level; }  // For UI display
	float GetLocalVolume() const { return m_localVolume; }  // Local gain multiplier (0.0-1.0)

	// Settings persistence
	void LoadSettings(const char* iniPath);
	void SaveSettings(const char* iniPath);

private:
	bool m_bInitialized;
	ImGuiContext* m_pContext;
	SDL_Renderer* m_pRenderer;

	// Device name editing
	char m_deviceNameBuffer[256];
	bool m_bEditingDeviceName;

	// UI state
	bool m_bShowUI;

	// Quality preset
	EQualityPreset m_qualityPreset;
	bool m_bNeedSyncTabs;  // Flag to sync tab selection once when switching views
	bool m_bLastWasOverlay;  // Track if last rendered was overlay (true) or home (false)

	// Audio controls
	float m_deviceVolume;        // Volume from AirPlay device (0.0 to 1.0)
	float m_localVolume;         // Local volume gain multiplier (0.0 to 1.0)
	bool m_bAutoAdjust;          // Auto-adjust (normalize loud sounds) enabled
	float m_currentAudioLevel;   // Current audio level for meter display (0.0 to 1.0)

	// DPI scaling
	float m_dpiScale;            // System DPI scale factor (1.0 = 96dpi, 1.25 = 120dpi, etc.)

	void SetupStyle();
};
