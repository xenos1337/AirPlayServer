#include "CImGuiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "SDL.h"
#include "SDL_syswm.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

CImGuiManager::CImGuiManager()
	: m_bInitialized(false)
	, m_pContext(NULL)
	, m_bEditingDeviceName(false)
	, m_bShowUI(true)
	, m_bUIVisibilityChanged(false)
	, m_qualityPreset(QUALITY_BALANCED)  // Default to balanced (60fps, normal quality)
	, m_bNeedSyncTabs(false)
	, m_bLastWasOverlay(false)
{
	memset(m_deviceNameBuffer, 0, sizeof(m_deviceNameBuffer));
}

CImGuiManager::~CImGuiManager()
{
	Shutdown();
}

bool CImGuiManager::Init(SDL_Surface* surface)
{
	if (m_bInitialized) {
		return true;
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
	
	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	SetupStyle();
	
	// Setup Platform/Renderer backends
	// For SDL 1.2, we'll use Win32 for input and a custom renderer
	// Get the window handle from SDL
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	if (SDL_GetWMInfo(&wmInfo) == 1) {
		HWND hwnd = wmInfo.window;
		
		// Initialize Win32 backend for input
		// We'll handle this manually in ProcessEvent
	}
	
	// Initialize font and build font atlas
	// Try to load fonts in order: Segoe UI Variable -> Segoe UI -> Arial -> Default
	ImFont* font = NULL;
	const char* fontPaths[] = {
		"C:\\Windows\\Fonts\\segoeuiv.ttf",      // Segoe UI Variable
		"C:\\Windows\\Fonts\\SegoeUIVariable.ttf", // Segoe UI Variable (alternate name)
		"C:\\Windows\\Fonts\\segoeui.ttf",        // Segoe UI
		"C:\\Windows\\Fonts\\arial.ttf"          // Arial
	};
	
	// Try each font path - check file existence first to avoid unnecessary errors
	for (int i = 0; i < 4 && font == NULL; i++) {
		// Check if file exists before trying to load (Windows-specific check)
		DWORD fileAttributes = GetFileAttributesA(fontPaths[i]);
		if (fileAttributes != INVALID_FILE_ATTRIBUTES && !(fileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			// Create a fresh font config with default constructor, then set flags
			ImFontConfig fontConfig;
			fontConfig.Flags = ImFontFlags_NoLoadError;
			// High quality font rendering with proper oversampling
			fontConfig.OversampleH = 3;  // 3x horizontal oversampling for sharp edges
			fontConfig.OversampleV = 2;  // 2x vertical oversampling
			fontConfig.PixelSnapH = false;  // Disable snap to allow sub-pixel positioning with oversampling
			fontConfig.PixelSnapV = false;
			fontConfig.RasterizerMultiply = 1.0f;  // Normal brightness
			fontConfig.GlyphOffset.x = 0.0f;  // No horizontal offset
			fontConfig.GlyphOffset.y = 0.0f;  // No vertical offset

			// Compact font size
			font = io.Fonts->AddFontFromFileTTF(fontPaths[i], 16.0f, &fontConfig, NULL);
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
	
	// Build font atlas texture
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	// We don't need to create a texture for software rendering
	// Just mark the atlas as built
	io.Fonts->SetTexID((ImTextureID)1);  // Dummy texture ID
	
	m_bInitialized = true;
	return true;
}

void CImGuiManager::Shutdown()
{
	if (!m_bInitialized) {
		return;
	}
	
	ImGui::SetCurrentContext(m_pContext);
	ImGui::DestroyContext(m_pContext);
	m_pContext = NULL;
	m_bInitialized = false;
}

void CImGuiManager::NewFrame(SDL_Surface* surface)
{
	if (!m_bInitialized || !surface) {
		return;
	}
	
	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();
	
	// Setup display size
	io.DisplaySize = ImVec2((float)surface->w, (float)surface->h);
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	
	// Setup time step - ensure DeltaTime is always positive
	static DWORD g_LastTicks = 0;
	DWORD currentTicks = SDL_GetTicks();
	if (g_LastTicks == 0) {
		g_LastTicks = currentTicks;
	}
	DWORD deltaTicks = currentTicks - g_LastTicks;
	// Ensure minimum of 1ms to avoid zero DeltaTime
	if (deltaTicks == 0) {
		deltaTicks = 1;
	}
	io.DeltaTime = deltaTicks / 1000.0f;
	g_LastTicks = currentTicks;
	
	ImGui::NewFrame();
}

void CImGuiManager::ProcessEvent(SDL_Event* event)
{
	if (!m_bInitialized || !event) {
		return;
	}
	
	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();
	
	switch (event->type) {
	case SDL_MOUSEMOTION:
		io.AddMousePosEvent((float)event->motion.x, (float)event->motion.y);
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	{
		int button = -1;
		if (event->button.button == SDL_BUTTON_LEFT) button = 0;
		if (event->button.button == SDL_BUTTON_RIGHT) button = 1;
		if (event->button.button == SDL_BUTTON_MIDDLE) button = 2;
		if (button != -1) {
			io.AddMouseButtonEvent(button, event->type == SDL_MOUSEBUTTONDOWN);
		}
		break;
	}
	// SDL 1.2 doesn't have SDL_MOUSEWHEEL - handle via button events
	// Mouse wheel is handled as button events in SDL 1.2
	case SDL_KEYDOWN:
	case SDL_KEYUP:
	{
		int key = event->key.keysym.sym;
		// Map SDL keys to ImGui keys
		ImGuiKey imgui_key = ImGuiKey_None;
		if (key >= SDLK_a && key <= SDLK_z) {
			imgui_key = (ImGuiKey)(ImGuiKey_A + (key - SDLK_a));
		} else if (key >= SDLK_0 && key <= SDLK_9) {
			imgui_key = (ImGuiKey)(ImGuiKey_0 + (key - SDLK_0));
		} else {
			switch (key) {
			case SDLK_TAB: imgui_key = ImGuiKey_Tab; break;
			case SDLK_LEFT: imgui_key = ImGuiKey_LeftArrow; break;
			case SDLK_RIGHT: imgui_key = ImGuiKey_RightArrow; break;
			case SDLK_UP: imgui_key = ImGuiKey_UpArrow; break;
			case SDLK_DOWN: imgui_key = ImGuiKey_DownArrow; break;
			case SDLK_PAGEUP: imgui_key = ImGuiKey_PageUp; break;
			case SDLK_PAGEDOWN: imgui_key = ImGuiKey_PageDown; break;
			case SDLK_HOME: imgui_key = ImGuiKey_Home; break;
			case SDLK_END: imgui_key = ImGuiKey_End; break;
			case SDLK_INSERT: imgui_key = ImGuiKey_Insert; break;
			case SDLK_DELETE: imgui_key = ImGuiKey_Delete; break;
			case SDLK_BACKSPACE: imgui_key = ImGuiKey_Backspace; break;
			case SDLK_SPACE: imgui_key = ImGuiKey_Space; break;
			case SDLK_RETURN: imgui_key = ImGuiKey_Enter; break;
			case SDLK_ESCAPE: imgui_key = ImGuiKey_Escape; break;
			case SDLK_LCTRL: case SDLK_RCTRL: imgui_key = ImGuiKey_LeftCtrl; break;
			case SDLK_LSHIFT: case SDLK_RSHIFT: imgui_key = ImGuiKey_LeftShift; break;
			case SDLK_LALT: case SDLK_RALT: imgui_key = ImGuiKey_LeftAlt; break;
			case SDLK_LSUPER: case SDLK_RSUPER: imgui_key = ImGuiKey_LeftSuper; break;
			}
		}
		if (imgui_key != ImGuiKey_None) {
			io.AddKeyEvent(imgui_key, event->type == SDL_KEYDOWN);
		}
		
		// Handle text input
		if (event->type == SDL_KEYDOWN && event->key.keysym.unicode && event->key.keysym.unicode < 0x10000) {
			io.AddInputCharacter((unsigned short)event->key.keysym.unicode);
		}
		break;
	}
	}
}

void CImGuiManager::RenderHomeScreen(const char* deviceName, bool isConnected, const char* connectedDeviceName)
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

	// Calculate window size: 75% of screen width and height
	float screenWidth = io.DisplaySize.x;
	float screenHeight = io.DisplaySize.y;

	float windowWidth, windowHeight;
	if (screenWidth > 0 && screenHeight > 0) {
		windowWidth = screenWidth * 0.75f;
		windowHeight = screenHeight * 0.75f;
	} else {
		// Fallback if display size unavailable
		windowWidth = 1440.0f;
		windowHeight = 810.0f;
	}

	// Center the window
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.95f);

	// Push minimal styling for home screen
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));

	ImGui::Begin("AirPlay Server", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

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
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "30 FPS - Best video quality");
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Uses high-quality Lanczos scaling");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Balanced", NULL, balancedFlags)) {
			m_qualityPreset = QUALITY_BALANCED;
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "60 FPS - Good balance");
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Uses fast bilinear scaling");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Fast Speed", NULL, fastFlags)) {
			m_qualityPreset = QUALITY_FAST;
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "60 FPS - Lowest latency");
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Uses nearest-neighbor scaling");
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::Dummy(ImVec2(0, 4.0f));
	ImGui::Separator();
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

	ImGui::End();
	ImGui::PopStyleVar(3);
}

void CImGuiManager::RenderOverlay(bool* pShowUI, const char* deviceName, bool isConnected, const char* connectedDeviceName,
	int videoWidth, int videoHeight, float fps, float bitrateMbps,
	unsigned long long totalFrames, unsigned long long droppedFrames)
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
		bool wasVisible = *pShowUI;
		*pShowUI = !*pShowUI;
		// If we just hid the UI, set flag to trigger surface clear
		if (wasVisible && !*pShowUI) {
			m_bUIVisibilityChanged = true;
		}
	}

	if (!*pShowUI) {
		// Don't render anything when UI is hidden - just return
		// The video will cover the full screen
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
			
			// Calculate total data received
			unsigned long long totalMB = totalFrames > 0 ? (unsigned long long)(bitrateMbps * 1000.0f * 1000.0f / 8.0f * (totalFrames / (fps > 0 ? fps : 30.0f)) / (1024 * 1024)) : 0;
			if (totalMB > 0) {
				ImGui::Text("Data Received: ~%llu MB", totalMB);
			}
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
			ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "30fps, HQ");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Balanced", NULL, balancedFlags)) {
			m_qualityPreset = QUALITY_BALANCED;
			ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "60fps, Normal");
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Fast", NULL, fastFlags)) {
			m_qualityPreset = QUALITY_FAST;
			ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "60fps, Low");
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::Separator();
	ImGui::Text("Press H to hide UI");

	ImGui::End();
}

// Helper function to blend a pixel with alpha
static inline void BlendPixel(Uint32* dst, Uint8 r, Uint8 g, Uint8 b, Uint8 a, SDL_PixelFormat* fmt)
{
	if (a == 0) return;
	
	if (a == 255) {
		*dst = SDL_MapRGB(fmt, r, g, b);
		return;
	}
	
	Uint8 dr, dg, db;
	SDL_GetRGB(*dst, fmt, &dr, &dg, &db);
	
	// Alpha blend
	Uint8 nr = (Uint8)((r * a + dr * (255 - a)) / 255);
	Uint8 ng = (Uint8)((g * a + dg * (255 - a)) / 255);
	Uint8 nb = (Uint8)((b * a + db * (255 - a)) / 255);
	
	*dst = SDL_MapRGB(fmt, nr, ng, nb);
}

// Helper to get pixel pointer
static inline Uint32* GetPixel(SDL_Surface* surface, int x, int y)
{
	if (x < 0 || y < 0 || x >= surface->w || y >= surface->h) return NULL;
	return (Uint32*)((Uint8*)surface->pixels + y * surface->pitch + x * surface->format->BytesPerPixel);
}

// Draw a horizontal line with alpha blending
static void DrawHLine(SDL_Surface* surface, int x1, int x2, int y, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	if (y < 0 || y >= surface->h) return;
	if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
	if (x1 < 0) x1 = 0;
	if (x2 >= surface->w) x2 = surface->w - 1;
	if (x1 > x2) return;
	
	for (int x = x1; x <= x2; x++) {
		Uint32* pixel = GetPixel(surface, x, y);
		if (pixel) BlendPixel(pixel, r, g, b, a, surface->format);
	}
}

// Simple filled rectangle drawing
static void FillRect(SDL_Surface* surface, int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	for (int py = y; py < y + h; py++) {
		DrawHLine(surface, x, x + w - 1, py, r, g, b, a);
	}
}

// Edge function for triangle rasterization (returns positive if point is on the left of the edge)
static inline float EdgeFunction(float ax, float ay, float bx, float by, float cx, float cy)
{
	return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

void CImGuiManager::Render(SDL_Surface* surface)
{
	if (!m_bInitialized || !surface) {
		return;
	}
	
	ImGui::SetCurrentContext(m_pContext);
	ImGui::Render();
	
	ImDrawData* drawData = ImGui::GetDrawData();
	if (!drawData || drawData->CmdListsCount == 0) {
		return;
	}
	
	// Lock surface for drawing
	if (SDL_MUSTLOCK(surface)) {
		SDL_LockSurface(surface);
	}
	
	// Get font texture data for text rendering
	ImGuiIO& io = ImGui::GetIO();
	unsigned char* fontPixels = NULL;
	int fontWidth, fontHeight;
	io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
	
	// Render each draw list
	for (int n = 0; n < drawData->CmdListsCount; n++) {
		const ImDrawList* cmdList = drawData->CmdLists[n];
		const ImDrawVert* vtxBuffer = cmdList->VtxBuffer.Data;
		const ImDrawIdx* idxBuffer = cmdList->IdxBuffer.Data;
		
		for (int cmdIdx = 0; cmdIdx < cmdList->CmdBuffer.Size; cmdIdx++) {
			const ImDrawCmd* pcmd = &cmdList->CmdBuffer[cmdIdx];
			
			if (pcmd->UserCallback) {
				pcmd->UserCallback(cmdList, pcmd);
				continue;
			}
			
			// Get clip rect
			int clipX1 = (int)pcmd->ClipRect.x;
			int clipY1 = (int)pcmd->ClipRect.y;
			int clipX2 = (int)pcmd->ClipRect.z;
			int clipY2 = (int)pcmd->ClipRect.w;
			
			// Check if this is textured (font rendering)
			bool isTextured = (pcmd->GetTexID() != 0) && fontPixels;
			
			// Render triangles
			for (unsigned int i = 0; i < pcmd->ElemCount; i += 3) {
				const ImDrawVert& v0 = vtxBuffer[idxBuffer[pcmd->IdxOffset + i + 0]];
				const ImDrawVert& v1 = vtxBuffer[idxBuffer[pcmd->IdxOffset + i + 1]];
				const ImDrawVert& v2 = vtxBuffer[idxBuffer[pcmd->IdxOffset + i + 2]];
				
				// Bounding box
				int minX = (int)floorf(fminf(fminf(v0.pos.x, v1.pos.x), v2.pos.x));
				int maxX = (int)ceilf(fmaxf(fmaxf(v0.pos.x, v1.pos.x), v2.pos.x));
				int minY = (int)floorf(fminf(fminf(v0.pos.y, v1.pos.y), v2.pos.y));
				int maxY = (int)ceilf(fmaxf(fmaxf(v0.pos.y, v1.pos.y), v2.pos.y));
				
				// Clip to screen and clip rect
				if (minX < clipX1) minX = clipX1;
				if (minY < clipY1) minY = clipY1;
				if (maxX > clipX2) maxX = clipX2;
				if (maxY > clipY2) maxY = clipY2;
				if (minX < 0) minX = 0;
				if (minY < 0) minY = 0;
				if (maxX >= surface->w) maxX = surface->w - 1;
				if (maxY >= surface->h) maxY = surface->h - 1;
				
				if (minX > maxX || minY > maxY) continue;
				
				// Calculate triangle area for barycentric coords
				float area = EdgeFunction(v0.pos.x, v0.pos.y, v1.pos.x, v1.pos.y, v2.pos.x, v2.pos.y);
				if (fabsf(area) < 0.001f) continue; // Degenerate triangle
				float invArea = 1.0f / area;
				
				// Rasterize triangle
				for (int y = minY; y <= maxY; y++) {
					for (int x = minX; x <= maxX; x++) {
						float px = x + 0.5f;
						float py = y + 0.5f;
						
						// Barycentric coordinates using edge functions
						float w0 = EdgeFunction(v1.pos.x, v1.pos.y, v2.pos.x, v2.pos.y, px, py);
						float w1 = EdgeFunction(v2.pos.x, v2.pos.y, v0.pos.x, v0.pos.y, px, py);
						float w2 = EdgeFunction(v0.pos.x, v0.pos.y, v1.pos.x, v1.pos.y, px, py);
						
						// Check if point is inside triangle (all same sign)
						bool inside = (area > 0) ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
						
						if (inside) {
							// Normalize barycentric coordinates
							w0 *= invArea;
							w1 *= invArea;
							w2 *= invArea;
							
							// Interpolate color
							ImU32 col0 = v0.col, col1 = v1.col, col2 = v2.col;
							float r = w0 * ((col0 >> IM_COL32_R_SHIFT) & 0xFF) + w1 * ((col1 >> IM_COL32_R_SHIFT) & 0xFF) + w2 * ((col2 >> IM_COL32_R_SHIFT) & 0xFF);
							float g = w0 * ((col0 >> IM_COL32_G_SHIFT) & 0xFF) + w1 * ((col1 >> IM_COL32_G_SHIFT) & 0xFF) + w2 * ((col2 >> IM_COL32_G_SHIFT) & 0xFF);
							float b = w0 * ((col0 >> IM_COL32_B_SHIFT) & 0xFF) + w1 * ((col1 >> IM_COL32_B_SHIFT) & 0xFF) + w2 * ((col2 >> IM_COL32_B_SHIFT) & 0xFF);
							float a = w0 * ((col0 >> IM_COL32_A_SHIFT) & 0xFF) + w1 * ((col1 >> IM_COL32_A_SHIFT) & 0xFF) + w2 * ((col2 >> IM_COL32_A_SHIFT) & 0xFF);
							
							Uint8 finalR = (Uint8)(r > 255 ? 255 : r);
							Uint8 finalG = (Uint8)(g > 255 ? 255 : g);
							Uint8 finalB = (Uint8)(b > 255 ? 255 : b);
							Uint8 finalA = (Uint8)(a > 255 ? 255 : a);
							
							// Sample texture if textured (font atlas)
							if (isTextured) {
								float u = w0 * v0.uv.x + w1 * v1.uv.x + w2 * v2.uv.x;
								float v = w0 * v0.uv.y + w1 * v1.uv.y + w2 * v2.uv.y;

								// Convert UV to texel coordinates with bilinear filtering for smooth text
								float tx = u * fontWidth - 0.5f;
								float ty = v * fontHeight - 0.5f;

								int tx0 = (int)floorf(tx);
								int ty0 = (int)floorf(ty);
								int tx1 = tx0 + 1;
								int ty1 = ty0 + 1;

								float fx = tx - tx0;
								float fy = ty - ty0;

								// Clamp to texture bounds
								if (tx0 < 0) tx0 = 0;
								if (ty0 < 0) ty0 = 0;
								if (tx1 >= fontWidth) tx1 = fontWidth - 1;
								if (ty1 >= fontHeight) ty1 = fontHeight - 1;
								if (tx0 >= fontWidth) tx0 = fontWidth - 1;
								if (ty0 >= fontHeight) ty0 = fontHeight - 1;

								// Sample 4 texels for bilinear interpolation
								unsigned char* t00 = fontPixels + (ty0 * fontWidth + tx0) * 4;
								unsigned char* t10 = fontPixels + (ty0 * fontWidth + tx1) * 4;
								unsigned char* t01 = fontPixels + (ty1 * fontWidth + tx0) * 4;
								unsigned char* t11 = fontPixels + (ty1 * fontWidth + tx1) * 4;

								// Bilinear interpolation of alpha channel
								float a00 = t00[3], a10 = t10[3], a01 = t01[3], a11 = t11[3];
								float texAlpha = a00 * (1 - fx) * (1 - fy) + a10 * fx * (1 - fy) +
								                 a01 * (1 - fx) * fy + a11 * fx * fy;

								// Multiply alpha by texture alpha
								finalA = (Uint8)((finalA * (int)texAlpha) / 255);
							}
							
							if (finalA > 0) {
								Uint32* pixel = GetPixel(surface, x, y);
								if (pixel) {
									BlendPixel(pixel, finalR, finalG, finalB, finalA, surface->format);
								}
							}
						}
					}
				}
			}
		}
	}
	
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
	
	// Note: Don't call SDL_UpdateRect here - it interferes with YUV overlay video
	// The display is updated by SDL_DisplayYUVOverlay for video, or SDL_Flip elsewhere
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

const char* CImGuiManager::GetDeviceName() const
{
	return m_deviceNameBuffer[0] ? m_deviceNameBuffer : NULL;
}

