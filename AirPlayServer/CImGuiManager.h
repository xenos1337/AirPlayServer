#pragma once
#include "imgui.h"
#include "SDL.h"

class CSDLPlayer;

// Quality presets for video rendering
enum EQualityPreset
{
	QUALITY_GOOD = 0,      // 30fps, high quality scaling (SWS_LANCZOS)
	QUALITY_BALANCED = 1,  // 60fps, normal quality scaling (SWS_FAST_BILINEAR)
	QUALITY_FAST = 2       // 60fps, low quality scaling (SWS_POINT)
};

class CImGuiManager
{
public:
	CImGuiManager();
	~CImGuiManager();

	bool Init(SDL_Surface* surface);
	void Shutdown();
	void NewFrame(SDL_Surface* surface);
	void Render(SDL_Surface* surface);
	void ProcessEvent(SDL_Event* event);

	// UI rendering
	void RenderHomeScreen(const char* deviceName, bool isConnected, const char* connectedDeviceName);
	void RenderOverlay(bool* pShowUI, const char* deviceName, bool isConnected, const char* connectedDeviceName,
		int videoWidth = 0, int videoHeight = 0, float fps = 0.0f, float bitrateMbps = 0.0f,
		unsigned long long totalFrames = 0, unsigned long long droppedFrames = 0);

	// Input handling
	bool WantCaptureMouse() const { return ImGui::GetIO().WantCaptureMouse; }
	bool WantCaptureKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }
	
	// Get edited device name
	const char* GetDeviceName() const;

	// Get quality preset
	EQualityPreset GetQualityPreset() const { return m_qualityPreset; }

	// Check if UI was just hidden (need to clear surface)
	bool WasUIJustHidden() {
		bool result = m_bUIVisibilityChanged;
		m_bUIVisibilityChanged = false;  // Clear flag after reading
		return result;
	}

private:
	bool m_bInitialized;
	ImGuiContext* m_pContext;
	
	// Device name editing
	char m_deviceNameBuffer[256];
	bool m_bEditingDeviceName;
	
	// UI state
	bool m_bShowUI;
	bool m_bUIVisibilityChanged;  // Flag to track when UI is hidden (need to clear surface)

	// Quality preset
	EQualityPreset m_qualityPreset;
	bool m_bNeedSyncTabs;  // Flag to sync tab selection once when switching views
	bool m_bLastWasOverlay;  // Track if last rendered was overlay (true) or home (false)

	void SetupStyle();
	void RenderDrawData(ImDrawData* drawData, SDL_Surface* surface);
};

