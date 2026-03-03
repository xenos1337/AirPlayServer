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
	, m_bShowUI(false)
	, m_qualityPreset(QUALITY_BALANCED)  // Default to balanced (60fps, linear filtering)
	, m_bNeedSyncTabs(true)
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

	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();

	float screenW = io.DisplaySize.x > 0 ? io.DisplaySize.x : 1920.0f;
	float screenH = io.DisplaySize.y > 0 ? io.DisplaySize.y : 1080.0f;
	float scale = m_dpiScale;

	ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(screenW, screenH), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(1.0f);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

	ImGui::Begin("##HomeScreen", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar);

	ImVec4 labelColor = ImVec4(0.50f, 0.52f, 0.56f, 1.0f);

	// ---- Vertically centered content ----
	// Measure total content height to center it
	float lineH = ImGui::GetTextLineHeightWithSpacing();
	float inputH = ImGui::GetFrameHeight();
	float totalContentH = lineH * 2.0f + 20.0f * scale + inputH + 60.0f * scale + lineH + lineH;
	float startY = (screenH - totalContentH) * 0.5f;
	if (startY < 20.0f * scale) startY = 20.0f * scale;
	ImGui::Dummy(ImVec2(0, startY));

	// ---- Title ----
	const char* title = "AirPlay Receiver";
	float titleW = ImGui::CalcTextSize(title).x;
	ImGui::SetCursorPosX((screenW - titleW) * 0.5f);
	ImGui::Text("%s", title);

	ImGui::Dummy(ImVec2(0, 8.0f * scale));

	// ---- Status with dot ----
	{
		const char* statusLabel = isServerRunning ? "Ready for AirPlay" : "Server not running";
		ImVec4 dotColor = isServerRunning ? ImVec4(0.30f, 0.78f, 0.48f, 1.0f) : ImVec4(0.90f, 0.35f, 0.35f, 1.0f);

		float statusW = ImGui::CalcTextSize(statusLabel).x;
		float dotR = 4.0f * scale;
		float gap = 8.0f;
		float totalW = dotR * 2.0f + gap + statusW;
		float startX = (screenW - totalW) * 0.5f;

		ImVec2 winPos = ImGui::GetWindowPos();
		ImVec2 cursorScreen = ImGui::GetCursorScreenPos();
		float dotCenterX = winPos.x + startX + dotR;
		float dotCenterY = cursorScreen.y + ImGui::GetTextLineHeight() * 0.5f;
		ImGui::GetWindowDrawList()->AddCircleFilled(
			ImVec2(dotCenterX, dotCenterY), dotR,
			ImGui::ColorConvertFloat4ToU32(dotColor));

		ImGui::SetCursorPosX(startX + dotR * 2.0f + gap);
		ImGui::TextColored(labelColor, "%s", statusLabel);
	}

	ImGui::Dummy(ImVec2(0, 20.0f * scale));

	// ---- Device Name Input ----
	{
		float inputW = 320.0f * scale;
		if (inputW > screenW - 40.0f) inputW = screenW - 40.0f;
		float inputX = (screenW - inputW) * 0.5f;

		if (!m_bEditingDeviceName && strlen(m_deviceNameBuffer) == 0) {
			if (deviceName) {
				strncpy_s(m_deviceNameBuffer, sizeof(m_deviceNameBuffer), deviceName, _TRUNCATE);
			}
		}

		ImGui::SetCursorPosX(inputX);
		ImGui::PushItemWidth(inputW);
		if (ImGui::InputText("##DeviceName", m_deviceNameBuffer, sizeof(m_deviceNameBuffer),
			ImGuiInputTextFlags_EnterReturnsTrue)) {
			m_bEditingDeviceName = false;
		}
		ImGui::PopItemWidth();
		m_bEditingDeviceName = ImGui::IsItemActive();
	}

	ImGui::Dummy(ImVec2(0, 16.0f * scale));

	// ---- Hint ----
	{
		char hint[512];
		snprintf(hint, sizeof(hint), "Select \"%s\" on your Apple device to start streaming",
			m_deviceNameBuffer[0] ? m_deviceNameBuffer : deviceName);
		float hintW = ImGui::CalcTextSize(hint).x;
		ImGui::SetCursorPosX((screenW - hintW) * 0.5f);
		ImGui::TextColored(ImVec4(0.38f, 0.40f, 0.43f, 1.0f), "%s", hint);
	}

	ImGui::End();
	ImGui::PopStyleVar(3);
}

void CImGuiManager::RenderOverlay(bool* pShowUI, const char* deviceName, bool isConnected, const char* connectedDeviceName,
	int videoWidth, int videoHeight, float fps, float bitrateMbps,
	unsigned long long totalFrames, unsigned long long droppedFrames,
	unsigned long long totalBytes)
{
	if (!m_bInitialized) {
		return;
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
	ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.88f);
	ImGui::Begin("##OverlayControls", pShowUI,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoTitleBar);

	ImVec4 labelColor = ImVec4(0.50f, 0.52f, 0.56f, 1.0f);
	ImVec4 accentColor = ImVec4(0.35f, 0.61f, 0.95f, 1.0f);

	// Device name
	ImGui::TextColored(labelColor, "Device");
	ImGui::SameLine();
	ImGui::Text("%s", deviceName ? deviceName : "Unknown");

	if (isConnected) {
		ImGui::TextColored(ImVec4(0.30f, 0.78f, 0.48f, 1.0f), "Connected");
		if (connectedDeviceName) {
			ImGui::SameLine();
			ImGui::TextColored(labelColor, "from %s", connectedDeviceName);
		}

		if (videoWidth > 0 && videoHeight > 0) {
			ImGui::Spacing();
			ImGui::TextColored(labelColor, "Video");
			ImGui::SameLine(70.0f * m_dpiScale);
			ImGui::Text("%d x %d", videoWidth, videoHeight);

			ImGui::TextColored(labelColor, "FPS");
			ImGui::SameLine(70.0f * m_dpiScale);
			if (fps > 0.0f) {
				ImGui::Text("%.1f", fps);
			} else {
				ImGui::TextColored(labelColor, "...");
			}

			if (bitrateMbps > 0.0f) {
				ImGui::TextColored(labelColor, "Bitrate");
				ImGui::SameLine(70.0f * m_dpiScale);
				ImGui::Text("%.1f Mbps", bitrateMbps);
			}

			if (droppedFrames > 0) {
				ImGui::TextColored(labelColor, "Dropped");
				ImGui::SameLine(70.0f * m_dpiScale);
				ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.25f, 1.0f), "%llu / %llu", droppedFrames, totalFrames);
			}
		}
	} else {
		ImGui::TextColored(labelColor, "Waiting for connection...");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Quality preset selection
	ImGui::TextColored(labelColor, "Quality");
	if (ImGui::BeginTabBar("QualityTabs", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
		// Use m_bNeedSyncTabs as one-shot to select the saved preset on first render
		ImGuiTabItemFlags goodFlags = (m_bNeedSyncTabs && m_qualityPreset == QUALITY_GOOD) ? ImGuiTabItemFlags_SetSelected : 0;
		ImGuiTabItemFlags balancedFlags = (m_bNeedSyncTabs && m_qualityPreset == QUALITY_BALANCED) ? ImGuiTabItemFlags_SetSelected : 0;
		ImGuiTabItemFlags fastFlags = (m_bNeedSyncTabs && m_qualityPreset == QUALITY_FAST) ? ImGuiTabItemFlags_SetSelected : 0;
		m_bNeedSyncTabs = false;

		if (ImGui::BeginTabItem("Good", NULL, goodFlags)) {
			m_qualityPreset = QUALITY_GOOD;
			ImGui::TextColored(labelColor, "30fps, Lanczos");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Balanced", NULL, balancedFlags)) {
			m_qualityPreset = QUALITY_BALANCED;
			ImGui::TextColored(labelColor, "60fps, Bilinear");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Fast", NULL, fastFlags)) {
			m_qualityPreset = QUALITY_FAST;
			ImGui::TextColored(labelColor, "60fps, Nearest");
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Audio section
	ImGui::PushItemWidth(140.0f * m_dpiScale);
	int localPct = (int)(m_localVolume * 100.0f);
	if (ImGui::SliderInt("##OvVolume", &localPct, 0, 100, "Volume: %d%%")) {
		m_localVolume = localPct / 100.0f;
	}
	ImGui::PopItemWidth();

	// Device volume bar
	int volumePercent = (int)(m_deviceVolume * 100.0f);
	ImGui::TextColored(labelColor, "Device: %d%%", volumePercent);
	{
		float barWidth = 140.0f * m_dpiScale;
		float barHeight = 4.0f * m_dpiScale;
		float rounding = 2.0f * m_dpiScale;
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		drawList->AddRectFilled(pos, ImVec2(pos.x + barWidth, pos.y + barHeight),
			IM_COL32(35, 36, 40, 255), rounding);
		drawList->AddRectFilled(pos, ImVec2(pos.x + barWidth * m_deviceVolume, pos.y + barHeight),
			IM_COL32(90, 156, 242, 200), rounding);

		ImGui::Dummy(ImVec2(barWidth, barHeight + 4.0f));
	}

	// Audio level meter
	ImGui::TextColored(labelColor, "Level");
	ImGui::SameLine();
	{
		float barWidth = 100.0f * m_dpiScale;
		float barHeight = 4.0f * m_dpiScale;
		float rounding = 2.0f * m_dpiScale;
		ImVec2 pos = ImGui::GetCursorScreenPos();
		pos.y += (ImGui::GetTextLineHeight() - barHeight) * 0.5f;
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		drawList->AddRectFilled(pos, ImVec2(pos.x + barWidth, pos.y + barHeight),
			IM_COL32(35, 36, 40, 255), rounding);

		float level = m_currentAudioLevel;
		ImU32 levelColor;
		if (level < 0.5f) {
			levelColor = IM_COL32(77, 199, 122, 220);
		} else if (level < 0.75f) {
			levelColor = IM_COL32(230, 190, 60, 220);
		} else {
			levelColor = IM_COL32(220, 80, 70, 220);
		}
		drawList->AddRectFilled(pos, ImVec2(pos.x + barWidth * level, pos.y + barHeight),
			levelColor, rounding);

		ImGui::Dummy(ImVec2(barWidth, 0));
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::TextColored(ImVec4(0.32f, 0.33f, 0.36f, 1.0f), "F1: Perf | H: UI | F: Fullscreen");

	ImGui::End();
}

// Helper: draw a filled-area graph using ImDrawList
// data is a circular buffer, offset is the write position (oldest data)
static void DrawFilledGraph(ImDrawList* drawList, ImVec2 pos, ImVec2 size,
	const float* data, int count, int offset, float minVal, float maxVal,
	ImU32 lineColor, ImU32 fillColor)
{
	// Background
	drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(18, 19, 22, 220), 3.0f);

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
	drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(50, 52, 58, 160), 3.0f);
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
	ImVec4 labelColor = ImVec4(0.50f, 0.52f, 0.56f, 1.0f);
	ImVec4 valueColor = ImVec4(0.93f, 0.94f, 0.95f, 1.0f);
	ImVec4 warnColor = ImVec4(0.95f, 0.60f, 0.25f, 1.0f);
	ImVec4 goodColor = ImVec4(0.30f, 0.78f, 0.48f, 1.0f);

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
	ImGui::TextColored(ImVec4(0.32f, 0.33f, 0.36f, 1.0f), "30s window | F1: Close");

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

	// Soft, modern rounding
	style.WindowRounding = 8.0f;
	style.ChildRounding = 6.0f;
	style.FrameRounding = 6.0f;
	style.PopupRounding = 8.0f;
	style.ScrollbarRounding = 6.0f;
	style.GrabRounding = 6.0f;
	style.TabRounding = 6.0f;
	style.TreeLinesRounding = 3.0f;
	style.DragDropTargetRounding = 6.0f;

	// Comfortable padding
	style.WindowPadding = ImVec2(12.0f, 12.0f);
	style.FramePadding = ImVec2(10.0f, 6.0f);
	style.CellPadding = ImVec2(6.0f, 4.0f);
	style.ItemSpacing = ImVec2(8.0f, 6.0f);
	style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
	style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
	style.SeparatorTextPadding = ImVec2(8.0f, 4.0f);
	style.DisplayWindowPadding = ImVec2(12.0f, 12.0f);
	style.DisplaySafeAreaPadding = ImVec2(4.0f, 4.0f);

	// Borders
	style.WindowBorderSize = 1.0f;
	style.ChildBorderSize = 1.0f;
	style.PopupBorderSize = 1.0f;
	style.FrameBorderSize = 0.0f;
	style.TabBorderSize = 0.0f;
	style.TabBarBorderSize = 1.0f;
	style.DragDropTargetBorderSize = 2.0f;
	style.ImageBorderSize = 0.0f;
	style.SeparatorTextBorderSize = 1.0f;

	// Sizing
	style.IndentSpacing = 20.0f;
	style.ColumnsMinSpacing = 6.0f;
	style.ScrollbarSize = 12.0f;
	style.ScrollbarPadding = 0.0f;
	style.GrabMinSize = 10.0f;
	style.LogSliderDeadzone = 4.0f;
	style.TabMinWidthBase = 32.0f;
	style.TabMinWidthShrink = 0.0f;
	style.TabCloseButtonMinWidthSelected = 0.0f;
	style.TabCloseButtonMinWidthUnselected = 0.0f;
	style.TabBarOverlineSize = 2.0f;
	style.TreeLinesSize = 1.0f;
	style.DragDropTargetPadding = 4.0f;

	// Window
	style.WindowMinSize = ImVec2(32.0f, 32.0f);
	style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
	style.WindowMenuButtonPosition = ImGuiDir_Left;
	style.WindowBorderHoverPadding = 4.0f;

	// Table
	style.TableAngledHeadersAngle = 35.0f;
	style.TableAngledHeadersTextAlign = ImVec2(0.0f, 0.0f);

	// Alignment
	style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
	style.SelectableTextAlign = ImVec2(0.0f, 0.0f);
	style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
	style.ColorButtonPosition = ImGuiDir_Right;

	// Anti-aliasing
	style.AntiAliasedLines = true;
	style.AntiAliasedLinesUseTex = true;
	style.AntiAliasedFill = true;
	style.CurveTessellationTol = 1.25f;
	style.CircleTessellationMaxError = 0.30f;

	style.Alpha = 1.0f;
	style.DisabledAlpha = 0.50f;

	// === Windows 11 Dark Theme — Blue Accent ===
	ImVec4* c = style.Colors;

	// Accent palette
	ImVec4 accent      = ImVec4(0.35f, 0.61f, 0.95f, 1.00f);  // #5A9CF2
	ImVec4 accentHover = ImVec4(0.45f, 0.69f, 0.98f, 1.00f);
	ImVec4 accentDim   = ImVec4(0.25f, 0.48f, 0.80f, 1.00f);

	// Surface hierarchy
	ImVec4 bg0 = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);  // Deepest
	ImVec4 bg1 = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);  // Window
	ImVec4 bg2 = ImVec4(0.16f, 0.17f, 0.18f, 1.00f);  // Card / child
	ImVec4 bg3 = ImVec4(0.20f, 0.21f, 0.23f, 1.00f);  // Frame / input

	// Text
	ImVec4 textPri = ImVec4(0.93f, 0.94f, 0.95f, 1.00f);
	ImVec4 textDim = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);

	// Border
	ImVec4 border = ImVec4(0.22f, 0.23f, 0.25f, 0.65f);

	// --- Text ---
	c[ImGuiCol_Text]                   = textPri;
	c[ImGuiCol_TextDisabled]           = textDim;
	c[ImGuiCol_TextLink]               = accent;
	c[ImGuiCol_TextSelectedBg]         = ImVec4(accent.x, accent.y, accent.z, 0.30f);

	// --- Windows ---
	c[ImGuiCol_WindowBg]               = ImVec4(bg1.x, bg1.y, bg1.z, 0.97f);
	c[ImGuiCol_ChildBg]                = ImVec4(bg2.x, bg2.y, bg2.z, 0.55f);
	c[ImGuiCol_PopupBg]                = ImVec4(bg1.x, bg1.y, bg1.z, 0.97f);

	// --- Borders ---
	c[ImGuiCol_Border]                 = border;
	c[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

	// --- Frames (inputs, sliders) ---
	c[ImGuiCol_FrameBg]                = ImVec4(bg3.x, bg3.y, bg3.z, 0.70f);
	c[ImGuiCol_FrameBgHovered]         = ImVec4(bg3.x + 0.06f, bg3.y + 0.06f, bg3.z + 0.06f, 0.85f);
	c[ImGuiCol_FrameBgActive]          = ImVec4(accent.x, accent.y, accent.z, 0.25f);

	// --- Title bar ---
	c[ImGuiCol_TitleBg]                = bg0;
	c[ImGuiCol_TitleBgActive]          = ImVec4(bg1.x + 0.04f, bg1.y + 0.04f, bg1.z + 0.04f, 1.00f);
	c[ImGuiCol_TitleBgCollapsed]       = ImVec4(bg0.x, bg0.y, bg0.z, 0.60f);

	// --- Menu bar ---
	c[ImGuiCol_MenuBarBg]              = bg0;

	// --- Scrollbar ---
	c[ImGuiCol_ScrollbarBg]            = ImVec4(bg0.x, bg0.y, bg0.z, 0.40f);
	c[ImGuiCol_ScrollbarGrab]          = ImVec4(0.34f, 0.35f, 0.37f, 1.00f);
	c[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.44f, 0.45f, 0.47f, 1.00f);
	c[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.54f, 0.55f, 0.57f, 1.00f);

	// --- Interactive elements — accent colored ---
	c[ImGuiCol_CheckMark]              = accent;
	c[ImGuiCol_SliderGrab]             = accent;
	c[ImGuiCol_SliderGrabActive]       = accentHover;

	// --- Buttons ---
	c[ImGuiCol_Button]                 = ImVec4(bg3.x, bg3.y, bg3.z, 0.80f);
	c[ImGuiCol_ButtonHovered]          = ImVec4(accent.x, accent.y, accent.z, 0.55f);
	c[ImGuiCol_ButtonActive]           = ImVec4(accent.x, accent.y, accent.z, 0.78f);

	// --- Headers (selectable, tree nodes) ---
	c[ImGuiCol_Header]                 = ImVec4(accent.x, accent.y, accent.z, 0.18f);
	c[ImGuiCol_HeaderHovered]          = ImVec4(accent.x, accent.y, accent.z, 0.32f);
	c[ImGuiCol_HeaderActive]           = ImVec4(accent.x, accent.y, accent.z, 0.45f);

	// --- Separators ---
	c[ImGuiCol_Separator]              = ImVec4(border.x, border.y, border.z, 0.40f);
	c[ImGuiCol_SeparatorHovered]       = ImVec4(accent.x, accent.y, accent.z, 0.50f);
	c[ImGuiCol_SeparatorActive]        = accent;

	// --- Resize grip ---
	c[ImGuiCol_ResizeGrip]             = ImVec4(accent.x, accent.y, accent.z, 0.12f);
	c[ImGuiCol_ResizeGripHovered]      = ImVec4(accent.x, accent.y, accent.z, 0.35f);
	c[ImGuiCol_ResizeGripActive]       = ImVec4(accent.x, accent.y, accent.z, 0.65f);

	// --- Input ---
	c[ImGuiCol_InputTextCursor]        = accent;

	// --- Tabs — accent-highlighted active tab ---
	c[ImGuiCol_Tab]                    = ImVec4(bg2.x, bg2.y, bg2.z, 0.90f);
	c[ImGuiCol_TabHovered]             = ImVec4(accent.x, accent.y, accent.z, 0.40f);
	c[ImGuiCol_TabSelected]            = ImVec4(accent.x, accent.y, accent.z, 0.22f);
	c[ImGuiCol_TabSelectedOverline]    = accent;
	c[ImGuiCol_TabDimmed]              = ImVec4(bg0.x, bg0.y, bg0.z, 0.90f);
	c[ImGuiCol_TabDimmedSelected]      = ImVec4(bg2.x + 0.04f, bg2.y + 0.04f, bg2.z + 0.04f, 1.00f);
	c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(accent.x, accent.y, accent.z, 0.00f);

	// --- Plots ---
	c[ImGuiCol_PlotLines]              = accent;
	c[ImGuiCol_PlotLinesHovered]       = accentHover;
	c[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
	c[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

	// --- Tables ---
	c[ImGuiCol_TableHeaderBg]          = bg2;
	c[ImGuiCol_TableBorderStrong]      = border;
	c[ImGuiCol_TableBorderLight]       = ImVec4(border.x, border.y, border.z, 0.30f);
	c[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	c[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.025f);

	// --- Tree lines ---
	c[ImGuiCol_TreeLines]              = ImVec4(border.x, border.y, border.z, 0.50f);

	// --- Drag and drop ---
	c[ImGuiCol_DragDropTarget]         = accent;
	c[ImGuiCol_DragDropTargetBg]       = ImVec4(accent.x, accent.y, accent.z, 0.12f);

	// --- Navigation ---
	c[ImGuiCol_NavCursor]              = accent;
	c[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
	c[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.15f, 0.15f, 0.15f, 0.20f);

	// --- Modal ---
	c[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);

	// --- Misc ---
	c[ImGuiCol_UnsavedMarker]          = ImVec4(0.95f, 0.40f, 0.30f, 1.00f);
}

void CImGuiManager::RenderDisconnectMessage(const char* deviceName)
{
	if (!m_bInitialized) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);

	// Centered floating card for disconnect message
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * m_dpiScale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * m_dpiScale, 16.0f * m_dpiScale));

	ImGui::Begin("##Disconnect", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs);

	ImGui::Dummy(ImVec2(0, 4.0f));

	const char* discLabel = "Disconnected";
	float discW = ImGui::CalcTextSize(discLabel).x;
	float winW = ImGui::GetContentRegionAvail().x;
	if (winW > discW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (winW - discW) * 0.5f);
	ImGui::Text("%s", discLabel);

	if (deviceName && strlen(deviceName) > 0) {
		float devW = ImGui::CalcTextSize(deviceName).x;
		if (winW > devW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (winW - devW) * 0.5f);
		ImGui::TextColored(ImVec4(0.50f, 0.52f, 0.56f, 1.0f), "%s", deviceName);
	}

	ImGui::Dummy(ImVec2(0, 4.0f));

	ImGui::End();
	ImGui::PopStyleVar(2);
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

	// Load overlay visibility (default: hidden)
	int showUI = GetPrivateProfileIntA("General", "ShowOverlay", 0, iniPath);
	m_bShowUI = (showUI != 0);

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

	// Save overlay visibility
	WritePrivateProfileStringA("General", "ShowOverlay", m_bShowUI ? "1" : "0", iniPath);

	// Save auto-adjust
	WritePrivateProfileStringA("Audio", "AutoAdjust", m_bAutoAdjust ? "1" : "0", iniPath);

	// Save local volume (stored as 0-100 integer)
	char volBuf[16];
	sprintf_s(volBuf, sizeof(volBuf), "%d", (int)(m_localVolume * 100.0f));
	WritePrivateProfileStringA("Audio", "LocalVolume", volBuf, iniPath);
}
