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

enum EOverlayState
{
	OVERLAY_EXPANDED = 0,
	OVERLAY_LAUNCHER = 1,
	OVERLAY_HIDDEN = 2
};

enum EPinApprovalResult
{
	PIN_APPROVAL_NO_ACTION = 0,
	PIN_APPROVAL_ALLOW,
	PIN_APPROVAL_DENY,
	PIN_APPROVAL_DISMISS
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
	void RecreateRendererDeviceObjects();

	// UI rendering
	void RenderHomeScreen(const char* deviceName, bool isServerRunning = true);
	void RenderDisconnectMessage(const char* deviceName, float visibility = 1.0f);
	void RenderOverlay(const char* deviceName, bool isConnected, const char* connectedDeviceName,
		int videoWidth = 0, int videoHeight = 0, float fps = 0.0f, float bitrateMbps = 0.0f,
		unsigned long long totalFrames = 0, unsigned long long droppedFrames = 0,
		float zoomLevel = 1.0f, int rotationAngle = 0,
		bool* pResetView = NULL, bool* pRotateView = NULL,
		bool capturePrivacyActive = false, bool* pToggleCapturePrivacy = NULL,
		bool captureExclusionAvailable = true, bool cleanFeedReady = true,
		bool pictureInPictureActive = false, bool* pTogglePictureInPicture = NULL);
	void RenderPictureInPictureControls(bool* pExitPictureInPicture);
	void RenderPerfGraphs(const SPerfData& perf, bool* pOpen);
	void SetPictureInPictureMode(bool enabled);

	// Input handling
	bool WantCaptureMouse() const { return ImGui::GetIO().WantCaptureMouse; }
	bool WantCaptureKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }
	bool WantTextInput() const { return ImGui::GetIO().WantTextInput; }

	// Get edited device name
	const char* GetDeviceName() const;
	bool IsAirPlayPinEnabled() const { return m_airPlayPinEnabled; }
	bool ShouldProtectPinFromCapture() const { return m_protectPinFromCapture; }

	// The receiver owns the PIN lifetime; ImGui only presents the approval flow.
	void RequestPinApprovalPopup(bool notify = true);
	EPinApprovalResult RenderPinApprovalPopup(const char* remoteAddress,
		const char* pin, bool awaitingApproval, bool preparingPin, bool showPin,
		bool captureProtectionFailed);

	// Get quality preset
	EQualityPreset GetQualityPreset() const { return m_qualityPreset; }

	// Overlay visibility (persisted in settings)
	EOverlayState GetOverlayState() const { return m_overlayState; }
	bool IsOverlayVisible() const { return m_overlayState == OVERLAY_EXPANDED; }
	void ShowOverlay() { m_overlayState = OVERLAY_EXPANDED; }
	void ToggleOverlay() {
		m_overlayState = m_overlayState == OVERLAY_EXPANDED
			? OVERLAY_HIDDEN : OVERLAY_EXPANDED;
	}

	// Screen-cast settings. The player consumes these directly on the render thread.
	bool IsScreenCastEnabled() const { return m_screenCastEnabled; }
	bool ShouldHideInterfaceFromCapture() const { return m_screenCastHideInterface; }
	bool ShouldCropCleanFeedToVideo() const { return m_screenCastCropToVideo; }

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
	bool m_rendererDeviceResetPending;
	ImGuiContext* m_pContext;
	SDL_Window* m_pWindow;
	SDL_Renderer* m_pRenderer;
	ImFont* m_pFontBody;
	ImFont* m_pFontHeading;
	ImFont* m_pFontTitle;
	ImFont* m_pFontPin;
	ImFont* m_pFontMono;

	// Device name editing
	char m_deviceNameBuffer[256];
	bool m_bEditingDeviceName;
	bool m_airPlayPinEnabled;
	bool m_protectPinFromCapture;
	bool m_pinApprovalPopupRequested;

	// UI state
	EOverlayState m_overlayState;
	ImVec2 m_overlayAnchor;
	bool m_overlayAnchorValid;
	ImVec2 m_overlayExpandedSize;
	bool m_overlayExpandedSizeValid;
	bool m_pictureInPictureMode;

	// Quality preset
	EQualityPreset m_qualityPreset;

	// Screen-cast clean feed
	bool m_screenCastEnabled;
	bool m_screenCastHideInterface;
	bool m_screenCastCropToVideo;

	// Audio controls
	float m_deviceVolume;        // Volume from AirPlay device (0.0 to 1.0)
	float m_localVolume;         // Local volume gain multiplier (0.0 to 1.0)
	bool m_bAutoAdjust;          // Auto-adjust (normalize loud sounds) enabled
	float m_currentAudioLevel;   // Current audio level for meter display (0.0 to 1.0)

	// DPI scaling
	float m_dpiScale;            // System DPI scale factor (1.0 = 96dpi, 1.25 = 120dpi, etc.)

	float GetWindowDpiScale() const;
	void ApplyWindowMinimumSize();
	void ApplyDpiScale(float dpiScale);
	void RebuildFonts();
	void SetupStyle();
	void RenderSettingsPopup();
	void RenderRequirePinSetting(float contentWidth, float scale);
};
