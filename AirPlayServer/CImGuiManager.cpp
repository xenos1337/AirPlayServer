#include "CImGuiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "SDL.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

CImGuiManager::CImGuiManager()
	: m_bInitialized(false)
	, m_pContext(NULL)
	, m_pRenderer(NULL)
	, m_bEditingDeviceName(false)
	, m_bShowUI(true)
	, m_qualityPreset(QUALITY_BALANCED)  // Default to balanced (60fps, linear filtering)
	, m_bNeedSyncTabs(false)
	, m_bLastWasOverlay(false)
	, m_deviceVolume(0.5f)     // Default 50% (will be updated by device)
	, m_localVolume(1.0f)      // Default 100% local volume
	, m_bAutoAdjust(false)     // Auto-adjust off by default
	, m_currentAudioLevel(0.0f)
	, m_dpiScale(1.0f)
{
	memset(m_deviceNameBuffer, 0, sizeof(m_deviceNameBuffer));
}

CImGuiManager::~CImGuiManager()
{
	Shutdown();
}

bool CImGuiManager::Init(SDL_Window* window, SDL_Renderer* renderer)
{
	if (m_bInitialized) {
		return true;
	}

	if (!window || !renderer) {
		return false;
	}

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	m_pContext = ImGui::CreateContext();
	ImGui::SetCurrentContext(m_pContext);

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

	// Configure font atlas for better quality
	io.Fonts->TexGlyphPadding = 1;  // Padding between glyphs for crisp rendering

	// Query system DPI for scaling UI elements and fonts
	{
		HDC hdc = GetDC(NULL);
		int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
		ReleaseDC(NULL, hdc);
		m_dpiScale = (float)dpi / 96.0f;
		if (m_dpiScale < 1.0f) m_dpiScale = 1.0f;
	}

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	SetupStyle();

	// Scale all style sizes by DPI factor (padding, rounding, scrollbar, etc.)
	ImGui::GetStyle().ScaleAllSizes(m_dpiScale);

	// Initialize font and build font atlas
	// Try to load fonts in order: Segoe UI Variable -> Segoe UI -> Arial -> Default
	ImFont* font = NULL;
	const char* fontPaths[] = {
		"C:\\Windows\\Fonts\\segoeuiv.ttf",      // Segoe UI Variable
		"C:\\Windows\\Fonts\\SegoeUIVariable.ttf", // Segoe UI Variable (alternate name)
		"C:\\Windows\\Fonts\\segoeui.ttf",        // Segoe UI
		"C:\\Windows\\Fonts\\arial.ttf"          // Arial
	};

	float fontSize = 16.0f * m_dpiScale;  // Scale font to match system DPI

	// Try each font path - check file existence first to avoid unnecessary errors
	for (int i = 0; i < 4 && font == NULL; i++) {
		// Check if file exists before trying to load (Windows-specific check)
		DWORD fileAttributes = GetFileAttributesA(fontPaths[i]);
		if (fileAttributes != INVALID_FILE_ATTRIBUTES && !(fileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			ImFontConfig fontConfig;
			fontConfig.Flags = ImFontFlags_NoLoadError;
			// High quality font rendering with oversampling
			fontConfig.OversampleH = 3;
			fontConfig.OversampleV = 2;
			fontConfig.PixelSnapH = false;
			fontConfig.PixelSnapV = false;
			fontConfig.RasterizerMultiply = 1.0f;
			fontConfig.GlyphOffset.x = 0.0f;
			fontConfig.GlyphOffset.y = 0.0f;

			font = io.Fonts->AddFontFromFileTTF(fontPaths[i], fontSize, &fontConfig, NULL);
		}
	}

	// If all custom fonts failed, use default ImGui font
	if (font == NULL) {
		font = io.Fonts->AddFontDefault();
	}

	// Ensure we have a valid font (safety check)
	if (font == NULL && io.Fonts->Fonts.Size > 0) {
		font = io.Fonts->Fonts[0];
	}

	// Set as default font if we have one
	if (font != NULL) {
		io.FontDefault = font;
	}

	// Initialize SDL2 + SDL2Renderer ImGui backends
	// These handle input, font atlas texture upload, and GPU-accelerated rendering
	ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
	ImGui_ImplSDLRenderer2_Init(renderer);

	m_pRenderer = renderer;
	m_bInitialized = true;
	return true;
}

void CImGuiManager::Shutdown()
{
	if (!m_bInitialized) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);

	// Shutdown ImGui backends
	ImGui_ImplSDLRenderer2_Shutdown();
	ImGui_ImplSDL2_Shutdown();

	ImGui::DestroyContext(m_pContext);
	m_pContext = NULL;
	m_bInitialized = false;
}

void CImGuiManager::NewFrame()
{
	if (!m_bInitialized) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);

	// Start new ImGui frame using SDL2 backends
	ImGui_ImplSDLRenderer2_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
}

void CImGuiManager::ProcessEvent(SDL_Event* event)
{
	if (!m_bInitialized || !event) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);

	// Forward SDL2 events to ImGui backend
	// The SDL2 backend handles all input mapping automatically:
	// mouse, keyboard, text input, mouse wheel, window events, etc.
	ImGui_ImplSDL2_ProcessEvent(event);
}

void CImGuiManager::RenderHomeScreen(const char* deviceName, bool isConnected, const char* connectedDeviceName, bool isServerRunning)
{
	if (!m_bInitialized) {
		return;
	}

	// Detect if we're switching from overlay to home screen
	if (m_bLastWasOverlay) {
		m_bNeedSyncTabs = true;
		m_bLastWasOverlay = false;
	}

	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();

	// Fill entire window (no black margins on home screen)
	float screenWidth = io.DisplaySize.x;
	float screenHeight = io.DisplaySize.y;

	float windowWidth = (screenWidth > 0) ? screenWidth : 1920.0f;
	float windowHeight = (screenHeight > 0) ? screenHeight : 1080.0f;

	ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(1.0f);

	// Push minimal styling for home screen (no rounding for edge-to-edge fill)
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

	ImGui::Begin("AirPlay Server", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoTitleBar);

	// Calculate content area for centering
	float contentWidth = windowWidth - 16.0f;
	float padding = 8.0f;

	// Title section - centered
	float titleWidth = ImGui::CalcTextSize("AirPlay Receiver").x;
	ImGui::SetCursorPosX((windowWidth - titleWidth) * 0.5f - padding);
	ImGui::Text("AirPlay Receiver");

	ImGui::Dummy(ImVec2(0, 4.0f));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0, 4.0f));

	// Device name input - centered
	ImGui::SetCursorPosX((windowWidth - contentWidth * 0.4f) * 0.5f - padding);
	ImGui::Text("Device Name:");

	// Copy device name to buffer if not editing
	if (!m_bEditingDeviceName && strlen(m_deviceNameBuffer) == 0) {
		if (deviceName) {
			strncpy_s(m_deviceNameBuffer, sizeof(m_deviceNameBuffer), deviceName, _TRUNCATE);
		}
	}

	ImGui::SetCursorPosX((windowWidth - contentWidth * 0.4f) * 0.5f - padding);
	ImGui::PushItemWidth(contentWidth * 0.4f);
	if (ImGui::InputText("##DeviceName", m_deviceNameBuffer, sizeof(m_deviceNameBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
		m_bEditingDeviceName = false;
	}
	ImGui::PopItemWidth();
	m_bEditingDeviceName = ImGui::IsItemActive();

	ImGui::Dummy(ImVec2(0, 8.0f));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0, 8.0f));

	// Quality preset selection - centered tabs
	float qualityLabelWidth = ImGui::CalcTextSize("Quality Preset:").x;
	ImGui::SetCursorPosX((windowWidth - qualityLabelWidth) * 0.5f - padding);
	ImGui::Text("Quality Preset:");
	ImGui::Dummy(ImVec2(0, 4.0f));

	if (ImGui::BeginTabBar("QualityPresetTabs", ImGuiTabBarFlags_None)) {
		// Only use SetSelected once when syncing from overlay, then clear flag
		ImGuiTabItemFlags goodFlags = (m_bNeedSyncTabs && m_qualityPreset == QUALITY_GOOD) ? ImGuiTabItemFlags_SetSelected : 0;
		ImGuiTabItemFlags balancedFlags = (m_bNeedSyncTabs && m_qualityPreset == QUALITY_BALANCED) ? ImGuiTabItemFlags_SetSelected : 0;
		ImGuiTabItemFlags fastFlags = (m_bNeedSyncTabs && m_qualityPreset == QUALITY_FAST) ? ImGuiTabItemFlags_SetSelected : 0;
		m_bNeedSyncTabs = false;  // Clear flag after applying

		if (ImGui::BeginTabItem("Good Quality", NULL, goodFlags)) {
			m_qualityPreset = QUALITY_GOOD;
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "30 FPS - Maximum quality per frame");
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Best filtering, highest fidelity");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Balanced", NULL, balancedFlags)) {
			m_qualityPreset = QUALITY_BALANCED;
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "60 FPS - Smooth + high quality");
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Best filtering, full frame rate");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Fast Speed", NULL, fastFlags)) {
			m_qualityPreset = QUALITY_FAST;
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "60 FPS - Lowest latency");
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Linear filtering, minimal processing");
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::Dummy(ImVec2(0, 4.0f));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0, 4.0f));

	// Audio section
	float audioLabelWidth = ImGui::CalcTextSize("Audio").x;
	ImGui::SetCursorPosX((windowWidth - audioLabelWidth) * 0.5f - padding);
	ImGui::Text("Audio");

	// Center the volume slider
	float sliderWidth = contentWidth * 0.5f;
	ImGui::SetCursorPosX((windowWidth - sliderWidth) * 0.5f - padding);
	ImGui::PushItemWidth(sliderWidth);
	int localPct = (int)(m_localVolume * 100.0f);
	if (ImGui::SliderInt("##Volume", &localPct, 0, 100, "%d%%")) {
		m_localVolume = localPct / 100.0f;
	}
	ImGui::PopItemWidth();

	ImGui::Dummy(ImVec2(0, 4.0f));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0, 4.0f));

	// Network status indicator
	const char* svcText = isServerRunning ? "[Services Active]" : "[Not Running]";
	ImVec4 svcColor = isServerRunning ? ImVec4(0.2f, 0.8f, 0.3f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
	float svcWidth = ImGui::CalcTextSize(svcText).x;
	ImGui::SetCursorPosX((windowWidth - svcWidth) * 0.5f - padding);
	ImGui::TextColored(svcColor, "%s", svcText);

	ImGui::Dummy(ImVec2(0, 4.0f));

	// Connection status - centered
	const char* statusText = isConnected ? "Connected" : "Waiting for connection...";
	ImVec4 statusColor = isConnected ? ImVec4(0.2f, 0.9f, 0.3f, 1.0f) : ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
	float statusWidth = ImGui::CalcTextSize(statusText).x;
	ImGui::SetCursorPosX((windowWidth - statusWidth) * 0.5f - padding);
	ImGui::TextColored(statusColor, "%s", statusText);

	if (isConnected && connectedDeviceName) {
		char connText[512];
		snprintf(connText, sizeof(connText), "Streaming from: %s", connectedDeviceName);
		float connWidth = ImGui::CalcTextSize(connText).x;
		ImGui::SetCursorPosX((windowWidth - connWidth) * 0.5f - padding);
		ImGui::Text("%s", connText);
	}

	ImGui::Dummy(ImVec2(0, 4.0f));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0, 4.0f));

	// Instructions - centered
	const char* instruction1 = "This device is ready to receive AirPlay connections.";
	float inst1Width = ImGui::CalcTextSize(instruction1).x;
	ImGui::SetCursorPosX((windowWidth - inst1Width) * 0.5f - padding);
	ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", instruction1);

	char instruction2[512];
	snprintf(instruction2, sizeof(instruction2), "Look for \"%s\" in your device's AirPlay menu.",
		m_deviceNameBuffer[0] ? m_deviceNameBuffer : deviceName);
	float inst2Width = ImGui::CalcTextSize(instruction2).x;
	ImGui::SetCursorPosX((windowWidth - inst2Width) * 0.5f - padding);
	ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", instruction2);

	ImGui::Dummy(ImVec2(0, 8.0f));

	// Keyboard shortcuts reference
	const char* shortcuts = "F1: Perf | H: Toggle UI | F: Fullscreen | Esc: Exit FS";
	float shortcutsWidth = ImGui::CalcTextSize(shortcuts).x;
	ImGui::SetCursorPosX((windowWidth - shortcutsWidth) * 0.5f - padding);
	ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "%s", shortcuts);

	ImGui::End();
	ImGui::PopStyleVar(5);
}

void CImGuiManager::RenderOverlay(bool* pShowUI, const char* deviceName, bool isConnected, const char* connectedDeviceName,
	int videoWidth, int videoHeight, float fps, float bitrateMbps,
	unsigned long long totalFrames, unsigned long long droppedFrames,
	unsigned long long totalBytes)
{
	if (!m_bInitialized) {
		return;
	}

	// Detect if we're switching from home screen to overlay
	if (!m_bLastWasOverlay) {
		m_bNeedSyncTabs = true;
		m_bLastWasOverlay = true;
	}

	ImGui::SetCurrentContext(m_pContext);

	// Toggle UI with H key
	ImGuiIO& io = ImGui::GetIO();
	if (ImGui::IsKeyPressed(ImGuiKey_H)) {
		*pShowUI = !*pShowUI;
	}

	if (!*pShowUI) {
		return;
	}

	// Render overlay UI in corner with semi-transparent background
	ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.85f); // Semi-transparent for overlay
	ImGui::Begin("AirPlay Controls", pShowUI, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

	ImGui::Text("Device: %s", deviceName ? deviceName : "Unknown");

	if (isConnected) {
		ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "Connected");
		if (connectedDeviceName) {
			ImGui::Text("From: %s", connectedDeviceName);
		}

		// Display video statistics when streaming
		if (videoWidth > 0 && videoHeight > 0) {
			ImGui::Separator();
			ImGui::Text("Video Information:");
			ImGui::Text("Resolution: %d x %d", videoWidth, videoHeight);

			if (fps > 0.0f) {
				ImGui::Text("Frame Rate: %.2f FPS", fps);
			} else {
				ImGui::Text("Frame Rate: Calculating...");
			}

			if (bitrateMbps > 0.0f) {
				ImGui::Text("Bitrate: %.2f Mbps", bitrateMbps);
			} else {
				ImGui::Text("Bitrate: Calculating...");
			}

			ImGui::Text("Total Frames: %llu", totalFrames);

			if (droppedFrames > 0) {
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Dropped Frames: %llu", droppedFrames);
			} else {
				ImGui::Text("Dropped Frames: %llu", droppedFrames);
			}

			// Calculate aspect ratio
			float aspectRatio = (float)videoWidth / (float)videoHeight;
			ImGui::Text("Aspect Ratio: %.2f:1", aspectRatio);

		}
	} else {
		ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "Waiting...");
	}

	ImGui::Separator();

	// Quality preset selection
	ImGui::Text("Quality:");
	if (ImGui::BeginTabBar("QualityTabs", ImGuiTabBarFlags_None)) {
		// Only use SetSelected once when syncing from home screen, then clear flag
		ImGuiTabItemFlags goodFlags = (m_bNeedSyncTabs && m_qualityPreset == QUALITY_GOOD) ? ImGuiTabItemFlags_SetSelected : 0;
		ImGuiTabItemFlags balancedFlags = (m_bNeedSyncTabs && m_qualityPreset == QUALITY_BALANCED) ? ImGuiTabItemFlags_SetSelected : 0;
		ImGuiTabItemFlags fastFlags = (m_bNeedSyncTabs && m_qualityPreset == QUALITY_FAST) ? ImGuiTabItemFlags_SetSelected : 0;
		m_bNeedSyncTabs = false;  // Clear flag after applying

		if (ImGui::BeginTabItem("Good", NULL, goodFlags)) {
			m_qualityPreset = QUALITY_GOOD;
			ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "30fps, Best");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Balanced", NULL, balancedFlags)) {
			m_qualityPreset = QUALITY_BALANCED;
			ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "60fps, Best");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Fast", NULL, fastFlags)) {
			m_qualityPreset = QUALITY_FAST;
			ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "60fps, Linear");
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::Separator();

	// Audio Controls Section
	ImGui::Text("Audio:");

	// Local volume slider
	ImGui::PushItemWidth(100.0f);
	int localPct = (int)(m_localVolume * 100.0f);
	if (ImGui::SliderInt("Volume", &localPct, 0, 100, "%d%%")) {
		m_localVolume = localPct / 100.0f;
	}
	ImGui::PopItemWidth();

	// Device volume display (read-only - controlled by the streaming device)
	int volumePercent = (int)(m_deviceVolume * 100.0f);
	ImGui::Text("Device: %d%%", volumePercent);

	// Volume bar visualization
	{
		float barWidth = 100.0f;
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		// Background
		drawList->AddRectFilled(pos, ImVec2(pos.x + barWidth, pos.y + 8),
			IM_COL32(40, 40, 40, 255));

		// Volume level (blue)
		drawList->AddRectFilled(pos, ImVec2(pos.x + barWidth * m_deviceVolume, pos.y + 8),
			IM_COL32(80, 140, 200, 255));

		// Border
		drawList->AddRect(pos, ImVec2(pos.x + barWidth, pos.y + 8),
			IM_COL32(100, 100, 100, 255));

		ImGui::Dummy(ImVec2(barWidth, 10));
	}

	// Audio level meter
	ImGui::Text("Level:");
	ImGui::SameLine();
	{
		float barWidth = 80.0f;
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		drawList->AddRectFilled(pos, ImVec2(pos.x + barWidth, pos.y + 10),
			IM_COL32(40, 40, 40, 255));

		float level = m_currentAudioLevel;
		ImU32 levelColor;
		if (level < 0.5f) {
			levelColor = IM_COL32(80, 200, 80, 255);
		} else if (level < 0.75f) {
			levelColor = IM_COL32(200, 200, 80, 255);
		} else {
			levelColor = IM_COL32(200, 80, 80, 255);
		}
		drawList->AddRectFilled(pos, ImVec2(pos.x + barWidth * level, pos.y + 10), levelColor);
		drawList->AddRect(pos, ImVec2(pos.x + barWidth, pos.y + 10),
			IM_COL32(100, 100, 100, 255));

		ImGui::Dummy(ImVec2(barWidth, 10));
	}

	ImGui::Separator();
	ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "F1: Perf | H: Toggle UI | F: Fullscreen | Esc: Exit FS");

	ImGui::End();
}

// Helper: draw a filled-area graph using ImDrawList
// data is a circular buffer, offset is the write position (oldest data)
static void DrawFilledGraph(ImDrawList* drawList, ImVec2 pos, ImVec2 size,
	const float* data, int count, int offset, float minVal, float maxVal,
	ImU32 lineColor, ImU32 fillColor)
{
	// Background
	drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(15, 15, 15, 200));

	if (count <= 1) return;

	float range = maxVal - minVal;
	if (range <= 0.0f) range = 1.0f;

	// Build data points
	float stepX = size.x / (float)(count - 1);

	// Draw filled quads from bottom to data line
	for (int i = 0; i < count - 1; i++) {
		int idx0 = (offset + i) % count;
		int idx1 = (offset + i + 1) % count;

		float v0 = (data[idx0] - minVal) / range;
		float v1 = (data[idx1] - minVal) / range;
		if (v0 < 0.0f) v0 = 0.0f; if (v0 > 1.0f) v0 = 1.0f;
		if (v1 < 0.0f) v1 = 0.0f; if (v1 > 1.0f) v1 = 1.0f;

		float x0 = pos.x + stepX * (float)i;
		float x1 = pos.x + stepX * (float)(i + 1);
		float y0 = pos.y + size.y - v0 * size.y;
		float y1 = pos.y + size.y - v1 * size.y;
		float yBot = pos.y + size.y;

		// Filled quad (two triangles)
		drawList->AddQuadFilled(
			ImVec2(x0, y0), ImVec2(x1, y1),
			ImVec2(x1, yBot), ImVec2(x0, yBot),
			fillColor);
	}

	// Draw line on top
	for (int i = 0; i < count - 1; i++) {
		int idx0 = (offset + i) % count;
		int idx1 = (offset + i + 1) % count;

		float v0 = (data[idx0] - minVal) / range;
		float v1 = (data[idx1] - minVal) / range;
		if (v0 < 0.0f) v0 = 0.0f; if (v0 > 1.0f) v0 = 1.0f;
		if (v1 < 0.0f) v1 = 0.0f; if (v1 > 1.0f) v1 = 1.0f;

		float x0 = pos.x + stepX * (float)i;
		float x1 = pos.x + stepX * (float)(i + 1);
		float y0 = pos.y + size.y - v0 * size.y;
		float y1 = pos.y + size.y - v1 * size.y;

		drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), lineColor, 2.0f);
	}

	// Border
	drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(60, 60, 60, 200));
}

void CImGuiManager::RenderPerfGraphs(const SPerfData& perf)
{
	if (!m_bInitialized) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();

	// Position in top-right corner
	float windowWidth = 340.0f * m_dpiScale;
	float margin = 10.0f;
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - windowWidth - margin, margin), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(windowWidth, 0), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.88f);

	ImGui::Begin("Performance", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	float graphWidth = windowWidth - 16.0f * m_dpiScale;
	float graphHeight = 36.0f * m_dpiScale;
	ImU32 scaleColor = IM_COL32(100, 100, 100, 160);

	// --- Source FPS graph (blue, 0-80) ---
	{
		char label[64];
		snprintf(label, sizeof(label), "Source FPS: %.1f", perf.sourceFps);
		ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", label);
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(graphWidth, graphHeight));
		DrawFilledGraph(drawList, pos, ImVec2(graphWidth, graphHeight),
			perf.sourceFpsHistory, perf.historySize, perf.currentIdx, 0.0f, 80.0f,
			IM_COL32(100, 200, 255, 255), IM_COL32(100, 200, 255, 40));
		drawList->AddText(ImVec2(pos.x + 2, pos.y), scaleColor, "80");
		drawList->AddText(ImVec2(pos.x + 2, pos.y + graphHeight - 12), scaleColor, "0");
	}

	ImGui::Spacing();

	// --- Display FPS graph (cyan, 0-80) ---
	{
		char label[64];
		snprintf(label, sizeof(label), "Display FPS: %.1f  (target: %.0f)", perf.displayFps, perf.targetFps);
		ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.9f, 1.0f), "%s", label);
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(graphWidth, graphHeight));
		DrawFilledGraph(drawList, pos, ImVec2(graphWidth, graphHeight),
			perf.displayFpsHistory, perf.historySize, perf.currentIdx, 0.0f, 80.0f,
			IM_COL32(50, 255, 230, 255), IM_COL32(50, 255, 230, 40));
		drawList->AddText(ImVec2(pos.x + 2, pos.y), scaleColor, "80");
		drawList->AddText(ImVec2(pos.x + 2, pos.y + graphHeight - 12), scaleColor, "0");
		// Draw target FPS line
		float targetNorm = perf.targetFps / 80.0f;
		if (targetNorm > 0.0f && targetNorm <= 1.0f) {
			float yTarget = pos.y + graphHeight - targetNorm * graphHeight;
			drawList->AddLine(ImVec2(pos.x, yTarget), ImVec2(pos.x + graphWidth, yTarget),
				IM_COL32(255, 255, 255, 60), 1.0f);
		}
	}

	ImGui::Spacing();

	// --- Frame Time graph (green, 0-33ms) ---
	{
		char label[64];
		snprintf(label, sizeof(label), "Frame Time: %.2f ms", perf.frameTimeMs);
		ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", label);
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(graphWidth, graphHeight));
		DrawFilledGraph(drawList, pos, ImVec2(graphWidth, graphHeight),
			perf.frameTimeHistory, perf.historySize, perf.currentIdx, 0.0f, 33.0f,
			IM_COL32(100, 255, 100, 255), IM_COL32(100, 255, 100, 40));
		drawList->AddText(ImVec2(pos.x + 2, pos.y), scaleColor, "33ms");
		drawList->AddText(ImVec2(pos.x + 2, pos.y + graphHeight - 12), scaleColor, "0");
	}

	ImGui::Spacing();

	// --- Decode-to-Display Latency graph (orange, 0-33ms) ---
	{
		char label[64];
		snprintf(label, sizeof(label), "Decode-to-Display: %.2f ms", perf.latencyMs);
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", label);
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(graphWidth, graphHeight));
		DrawFilledGraph(drawList, pos, ImVec2(graphWidth, graphHeight),
			perf.latencyHistory, perf.historySize, perf.currentIdx, 0.0f, 33.0f,
			IM_COL32(255, 200, 80, 255), IM_COL32(255, 200, 80, 40));
		drawList->AddText(ImVec2(pos.x + 2, pos.y), scaleColor, "33ms");
		drawList->AddText(ImVec2(pos.x + 2, pos.y + graphHeight - 12), scaleColor, "0");
	}

	ImGui::Spacing();

	// --- Bitrate graph (purple, 0-50 Mbps) ---
	{
		char label[64];
		snprintf(label, sizeof(label), "Bitrate: %.2f Mbps", perf.bitrateMbps);
		ImGui::TextColored(ImVec4(0.8f, 0.5f, 1.0f, 1.0f), "%s", label);
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(graphWidth, graphHeight));
		DrawFilledGraph(drawList, pos, ImVec2(graphWidth, graphHeight),
			perf.bitrateHistory, perf.historySize, perf.currentIdx, 0.0f, 50.0f,
			IM_COL32(200, 130, 255, 255), IM_COL32(200, 130, 255, 40));
		drawList->AddText(ImVec2(pos.x + 2, pos.y), scaleColor, "50");
		drawList->AddText(ImVec2(pos.x + 2, pos.y + graphHeight - 12), scaleColor, "0");
	}

	ImGui::Spacing();

	// --- Audio Buffer graph (yellow, 0-20 frames) ---
	{
		char label[64];
		snprintf(label, sizeof(label), "Audio Buffer: %d frames", perf.audioQueueSize);
		ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "%s", label);
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(graphWidth, graphHeight));
		DrawFilledGraph(drawList, pos, ImVec2(graphWidth, graphHeight),
			perf.audioQueueHistory, perf.historySize, perf.currentIdx, 0.0f, 20.0f,
			IM_COL32(255, 230, 80, 255), IM_COL32(255, 230, 80, 40));
		drawList->AddText(ImVec2(pos.x + 2, pos.y), scaleColor, "20");
		drawList->AddText(ImVec2(pos.x + 2, pos.y + graphHeight - 12), scaleColor, "0");
	}

	ImGui::Spacing();
	ImGui::Separator();

	// === STATS PANEL ===
	ImVec4 labelColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
	ImVec4 valueColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
	ImVec4 warnColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
	ImVec4 goodColor = ImVec4(0.3f, 0.9f, 0.4f, 1.0f);

	// Video info
	if (perf.videoWidth > 0 && perf.videoHeight > 0) {
		// Resolution with aspect ratio
		float ar = (float)perf.videoWidth / (float)perf.videoHeight;
		const char* arLabel = "";
		if (ar > 1.76f && ar < 1.78f) arLabel = "16:9";
		else if (ar > 1.59f && ar < 1.61f) arLabel = "16:10";
		else if (ar > 1.32f && ar < 1.34f) arLabel = "4:3";
		else if (ar > 2.32f && ar < 2.35f) arLabel = "21:9";

		ImGui::TextColored(labelColor, "Video");
		ImGui::SameLine(80.0f * m_dpiScale);
		ImGui::TextColored(valueColor, "%dx%d %s", perf.videoWidth, perf.videoHeight, arLabel);

		// Data transferred
		ImGui::TextColored(labelColor, "Data");
		ImGui::SameLine(80.0f * m_dpiScale);
		if (perf.totalBytes > 1073741824ULL) {
			ImGui::TextColored(valueColor, "%.2f GB", (float)perf.totalBytes / 1073741824.0f);
		} else {
			ImGui::TextColored(valueColor, "%.1f MB", (float)perf.totalBytes / 1048576.0f);
		}
	}

	// Frame counters
	ImGui::TextColored(labelColor, "Frames");
	ImGui::SameLine(80.0f * m_dpiScale);
	if (perf.droppedFrames > 0) {
		ImGui::TextColored(warnColor, "%llu  (%llu dropped)", perf.totalFrames, perf.droppedFrames);
	} else {
		ImGui::TextColored(valueColor, "%llu", perf.totalFrames);
	}

	// Audio stats
	ImGui::TextColored(labelColor, "Audio");
	ImGui::SameLine(80.0f * m_dpiScale);
	if (perf.audioUnderruns > 0 || perf.audioDropped > 0) {
		ImGui::TextColored(warnColor, "%d underruns, %d dropped", perf.audioUnderruns, perf.audioDropped);
	} else {
		ImGui::TextColored(goodColor, "OK");
	}

	// Connection time
	if (perf.connectionTimeSec > 0.0f) {
		ImGui::TextColored(labelColor, "Uptime");
		ImGui::SameLine(80.0f * m_dpiScale);
		int totalSec = (int)perf.connectionTimeSec;
		int hours = totalSec / 3600;
		int mins = (totalSec % 3600) / 60;
		int secs = totalSec % 60;
		if (hours > 0) {
			ImGui::TextColored(valueColor, "%dh %02dm %02ds", hours, mins, secs);
		} else if (mins > 0) {
			ImGui::TextColored(valueColor, "%dm %02ds", mins, secs);
		} else {
			ImGui::TextColored(valueColor, "%ds", secs);
		}
	}

	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.35f, 1.0f), "30s window | F1: Close");

	ImGui::End();
}

void CImGuiManager::Render()
{
	if (!m_bInitialized) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);
	ImGui::Render();

	// GPU-accelerated rendering via SDL2Renderer backend
	ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), m_pRenderer);
}

void CImGuiManager::SetupStyle()
{
	ImGui::SetCurrentContext(m_pContext);
	ImGuiStyle& style = ImGui::GetStyle();

	// Modern styling: minimal rounding, tight spacing
	style.WindowRounding = 4.0f;
	style.ChildRounding = 4.0f;
	style.FrameRounding = 3.0f;
	style.PopupRounding = 4.0f;
	style.ScrollbarRounding = 3.0f;
	style.GrabRounding = 3.0f;
	style.TabRounding = 3.0f;
	style.TreeLinesRounding = 2.0f;
	style.DragDropTargetRounding = 4.0f;

	// Minimal padding and spacing
	style.WindowPadding = ImVec2(6.0f, 6.0f);
	style.FramePadding = ImVec2(4.0f, 2.0f);
	style.CellPadding = ImVec2(2.0f, 1.0f);
	style.ItemSpacing = ImVec2(4.0f, 2.0f);
	style.ItemInnerSpacing = ImVec2(2.0f, 2.0f);
	style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
	style.SeparatorTextPadding = ImVec2(4.0f, 2.0f);
	style.DisplayWindowPadding = ImVec2(8.0f, 8.0f);
	style.DisplaySafeAreaPadding = ImVec2(2.0f, 2.0f);

	// Borders and sizes
	style.WindowBorderSize = 1.0f;
	style.ChildBorderSize = 1.0f;
	style.PopupBorderSize = 1.0f;
	style.FrameBorderSize = 0.0f;
	style.TabBorderSize = 0.0f;
	style.TabBarBorderSize = 1.0f;
	style.DragDropTargetBorderSize = 2.0f;
	style.ImageBorderSize = 0.0f;
	style.SeparatorTextBorderSize = 1.0f;

	// Spacing and sizing
	style.IndentSpacing = 14.0f;
	style.ColumnsMinSpacing = 4.0f;
	style.ScrollbarSize = 10.0f;
	style.ScrollbarPadding = 0.0f;
	style.GrabMinSize = 6.0f;
	style.LogSliderDeadzone = 4.0f;
	style.TabMinWidthBase = 20.0f;
	style.TabMinWidthShrink = 0.0f;
	style.TabCloseButtonMinWidthSelected = 0.0f;
	style.TabCloseButtonMinWidthUnselected = 0.0f;
	style.TabBarOverlineSize = 2.0f;
	style.TreeLinesSize = 1.0f;
	style.DragDropTargetPadding = 4.0f;

	// Window settings
	style.WindowMinSize = ImVec2(32.0f, 32.0f);
	style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
	style.WindowMenuButtonPosition = ImGuiDir_Left;
	style.WindowBorderHoverPadding = 4.0f;

	// Table settings
	style.TableAngledHeadersAngle = 35.0f;
	style.TableAngledHeadersTextAlign = ImVec2(0.0f, 0.0f);

	// Button and selectable alignment
	style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
	style.SelectableTextAlign = ImVec2(0.0f, 0.0f);
	style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);

	// Color button position
	style.ColorButtonPosition = ImGuiDir_Right;

	// Anti-aliasing
	style.AntiAliasedLines = true;
	style.AntiAliasedLinesUseTex = true;
	style.AntiAliasedFill = true;
	style.CurveTessellationTol = 1.25f;
	style.CircleTessellationMaxError = 0.30f;

	// Alpha
	style.Alpha = 1.0f;
	style.DisabledAlpha = 0.60f;

	// Apply modern theme colors - all colors included
	ImVec4* colors = style.Colors;

	// Text colors
	colors[ImGuiCol_Text]                   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
	colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
	colors[ImGuiCol_TextLink]               = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
	colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.45f, 0.45f, 0.45f, 0.35f);

	// Window colors
	colors[ImGuiCol_WindowBg]               = ImVec4(0.08f, 0.08f, 0.08f, 0.95f);
	colors[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_PopupBg]                = ImVec4(0.10f, 0.10f, 0.10f, 0.95f);

	// Border colors
	colors[ImGuiCol_Border]                 = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
	colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

	// Frame colors
	colors[ImGuiCol_FrameBg]                = ImVec4(0.18f, 0.18f, 0.18f, 0.54f);
	colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.35f, 0.35f, 0.35f, 0.40f);
	colors[ImGuiCol_FrameBgActive]          = ImVec4(0.45f, 0.45f, 0.45f, 0.67f);

	// Title bar colors
	colors[ImGuiCol_TitleBg]                = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
	colors[ImGuiCol_TitleBgActive]          = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
	colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

	// Menu bar
	colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);

	// Scrollbar colors
	colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
	colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
	colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
	colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);

	// Checkbox and slider
	colors[ImGuiCol_CheckMark]              = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
	colors[ImGuiCol_SliderGrab]             = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
	colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.65f, 0.65f, 0.65f, 1.00f);

	// Button colors
	colors[ImGuiCol_Button]                 = ImVec4(0.30f, 0.30f, 0.30f, 0.40f);
	colors[ImGuiCol_ButtonHovered]          = ImVec4(0.40f, 0.40f, 0.40f, 0.80f);
	colors[ImGuiCol_ButtonActive]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

	// Header colors (for selectable, tree nodes, etc.)
	colors[ImGuiCol_Header]                 = ImVec4(0.30f, 0.30f, 0.30f, 0.31f);
	colors[ImGuiCol_HeaderHovered]          = ImVec4(0.40f, 0.40f, 0.40f, 0.80f);
	colors[ImGuiCol_HeaderActive]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

	// Separator colors
	colors[ImGuiCol_Separator]              = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
	colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.45f, 0.45f, 0.45f, 0.78f);
	colors[ImGuiCol_SeparatorActive]        = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);

	// Resize grip colors
	colors[ImGuiCol_ResizeGrip]             = ImVec4(0.40f, 0.40f, 0.40f, 0.20f);
	colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.50f, 0.50f, 0.50f, 0.67f);
	colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.60f, 0.60f, 0.60f, 0.95f);

	// Input text
	colors[ImGuiCol_InputTextCursor]        = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);

	// Tab colors
	colors[ImGuiCol_Tab]                    = ImVec4(0.20f, 0.20f, 0.20f, 0.86f);
	colors[ImGuiCol_TabHovered]             = ImVec4(0.35f, 0.35f, 0.35f, 0.80f);
	colors[ImGuiCol_TabSelected]            = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
	colors[ImGuiCol_TabSelectedOverline]    = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
	colors[ImGuiCol_TabDimmed]              = ImVec4(0.10f, 0.10f, 0.10f, 0.97f);
	colors[ImGuiCol_TabDimmedSelected]       = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
	colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.40f, 0.40f, 0.40f, 0.00f);

	// Plot colors
	colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
	colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
	colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
	colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

	// Table colors
	colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
	colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
	colors[ImGuiCol_TableBorderLight]       = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
	colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);

	// Tree lines
	colors[ImGuiCol_TreeLines]              = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);

	// Drag and drop
	colors[ImGuiCol_DragDropTarget]         = ImVec4(0.80f, 0.80f, 0.80f, 0.90f);
	colors[ImGuiCol_DragDropTargetBg]       = ImVec4(0.40f, 0.40f, 0.40f, 0.20f);

	// Navigation
	colors[ImGuiCol_NavCursor]              = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
	colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
	colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);

	// Modal window
	colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

	// Unsaved marker
	colors[ImGuiCol_UnsavedMarker]          = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
}

void CImGuiManager::RenderDisconnectMessage(const char* deviceName)
{
	if (!m_bInitialized) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);

	// Centered semi-transparent window showing disconnect message
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(0.75f);

	ImGui::Begin("##Disconnect", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs);

	ImGui::Dummy(ImVec2(0, 8.0f));
	ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "  Disconnected  ");
	if (deviceName && strlen(deviceName) > 0) {
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  %s  ", deviceName);
	}
	ImGui::Dummy(ImVec2(0, 8.0f));

	ImGui::End();
}

const char* CImGuiManager::GetDeviceName() const
{
	return m_deviceNameBuffer[0] ? m_deviceNameBuffer : NULL;
}

void CImGuiManager::LoadSettings(const char* iniPath)
{
	// Load device name
	char buf[256] = { 0 };
	GetPrivateProfileStringA("General", "DeviceName", "", buf, sizeof(buf), iniPath);
	if (strlen(buf) > 0) {
		strncpy_s(m_deviceNameBuffer, sizeof(m_deviceNameBuffer), buf, _TRUNCATE);
	}

	// Load quality preset
	int preset = GetPrivateProfileIntA("General", "QualityPreset", 1, iniPath);
	if (preset >= QUALITY_GOOD && preset <= QUALITY_FAST) {
		m_qualityPreset = (EQualityPreset)preset;
	}

	// Load auto-adjust
	int autoAdj = GetPrivateProfileIntA("Audio", "AutoAdjust", 0, iniPath);
	m_bAutoAdjust = (autoAdj != 0);

	// Load local volume (stored as 0-100 integer)
	int localVol = GetPrivateProfileIntA("Audio", "LocalVolume", 100, iniPath);
	if (localVol < 0) localVol = 0;
	if (localVol > 100) localVol = 100;
	m_localVolume = localVol / 100.0f;
}

void CImGuiManager::SaveSettings(const char* iniPath)
{
	// Save device name
	WritePrivateProfileStringA("General", "DeviceName",
		m_deviceNameBuffer[0] ? m_deviceNameBuffer : "", iniPath);

	// Save quality preset
	char buf[16];
	sprintf_s(buf, sizeof(buf), "%d", (int)m_qualityPreset);
	WritePrivateProfileStringA("General", "QualityPreset", buf, iniPath);

	// Save auto-adjust
	WritePrivateProfileStringA("Audio", "AutoAdjust", m_bAutoAdjust ? "1" : "0", iniPath);

	// Save local volume (stored as 0-100 integer)
	char volBuf[16];
	sprintf_s(volBuf, sizeof(volBuf), "%d", (int)(m_localVolume * 100.0f));
	WritePrivateProfileStringA("Audio", "LocalVolume", volBuf, iniPath);
}
