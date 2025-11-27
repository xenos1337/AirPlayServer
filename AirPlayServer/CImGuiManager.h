#pragma once
#include "imgui.h"
#include "SDL.h"

class CSDLPlayer;

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
	void RenderOverlay(bool* pShowUI, const char* deviceName, bool isConnected, const char* connectedDeviceName);

	// Input handling
	bool WantCaptureMouse() const { return ImGui::GetIO().WantCaptureMouse; }
	bool WantCaptureKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }
	
	// Get edited device name
	const char* GetDeviceName() const;

private:
	bool m_bInitialized;
	ImGuiContext* m_pContext;
	
	// Device name editing
	char m_deviceNameBuffer[256];
	bool m_bEditingDeviceName;
	
	// UI state
	bool m_bShowUI;
	
	void SetupStyle();
	void RenderDrawData(ImDrawData* drawData, SDL_Surface* surface);
};

