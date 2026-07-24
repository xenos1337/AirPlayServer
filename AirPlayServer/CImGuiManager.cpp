#include "CImGuiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "SDL.h"
#include "SDL_syswm.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace {
	const ImVec4 UI_TEXT_PRIMARY = ImVec4(0.91f, 0.92f, 0.93f, 1.00f);
	const ImVec4 UI_TEXT_SECONDARY = ImVec4(0.64f, 0.66f, 0.69f, 1.00f);
	const ImVec4 UI_TEXT_MUTED = ImVec4(0.45f, 0.47f, 0.50f, 1.00f);
	const ImVec4 UI_ACCENT = ImVec4(0.34f, 0.55f, 0.78f, 1.00f);
	const ImVec4 UI_ACCENT_HOVER = ImVec4(0.42f, 0.62f, 0.84f, 1.00f);
	const ImVec4 UI_SUCCESS = ImVec4(0.36f, 0.72f, 0.50f, 1.00f);
	const ImVec4 UI_WARNING = ImVec4(0.82f, 0.61f, 0.31f, 1.00f);
	const ImVec4 UI_ERROR = ImVec4(0.82f, 0.39f, 0.41f, 1.00f);
	const ImVec4 UI_ALLOW_BUTTON = ImVec4(0.12f, 0.40f, 0.24f, 1.00f);
	const ImVec4 UI_ALLOW_BUTTON_HOVER = ImVec4(0.16f, 0.53f, 0.31f, 1.00f);
	const ImVec4 UI_ALLOW_BUTTON_ACTIVE = ImVec4(0.09f, 0.30f, 0.18f, 1.00f);
	const ImVec4 UI_DENY_BUTTON = ImVec4(0.42f, 0.16f, 0.18f, 1.00f);
	const ImVec4 UI_DENY_BUTTON_HOVER = ImVec4(0.55f, 0.21f, 0.24f, 1.00f);
	const ImVec4 UI_DENY_BUTTON_ACTIVE = ImVec4(0.31f, 0.11f, 0.13f, 1.00f);
	const ImVec4 UI_CANVAS = ImVec4(0.045f, 0.049f, 0.055f, 1.00f);
	const ImVec4 UI_SURFACE = ImVec4(0.060f, 0.064f, 0.071f, 0.98f);
	const ImVec4 UI_SURFACE_RAISED = ImVec4(0.088f, 0.093f, 0.102f, 1.00f);

	float Clamp01(float value)
	{
		if (value < 0.0f) return 0.0f;
		if (value > 1.0f) return 1.0f;
		return value;
	}

	float MinFloat(float a, float b)
	{
		return a < b ? a : b;
	}

	float MaxFloat(float a, float b)
	{
		return a > b ? a : b;
	}

	const char* FindFirstFont(const char* const* paths, int count)
	{
		for (int i = 0; i < count; ++i) {
			DWORD attributes = GetFileAttributesA(paths[i]);
			if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
				return paths[i];
			}
		}
		return NULL;
	}

	ImFont* LoadUiFont(ImGuiIO& io, const char* path, float size)
	{
		if (path == NULL) return NULL;
		ImFontConfig config;
		config.Flags = ImFontFlags_NoLoadError;
		config.OversampleH = 3;
		config.OversampleV = 2;
		config.PixelSnapH = false;
		config.PixelSnapV = false;
		config.RasterizerMultiply = 1.0f;
		return io.Fonts->AddFontFromFileTTF(path, size, &config, NULL);
	}

	void SetNextWindowPosConstrained(const char* windowName,
		const ImVec2& defaultPosition, const ImVec2& nextSize, float margin,
		bool useDefaultWhenInactive = false)
	{
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGuiWindow* window = ImGui::FindWindowByName(windowName);
		ImVec2 position = window != NULL && (!useDefaultWhenInactive || window->WasActive)
			? window->Pos : defaultPosition;
		ImVec2 minimum(viewport->WorkPos.x + margin, viewport->WorkPos.y + margin);
		ImVec2 maximum(
			viewport->WorkPos.x + viewport->WorkSize.x - nextSize.x - margin,
			viewport->WorkPos.y + viewport->WorkSize.y - nextSize.y - margin);
		maximum.x = MaxFloat(minimum.x, maximum.x);
		maximum.y = MaxFloat(minimum.y, maximum.y);

		ImVec2 clamped = position;
		if (clamped.x < minimum.x) clamped.x = minimum.x;
		if (clamped.y < minimum.y) clamped.y = minimum.y;
		if (clamped.x > maximum.x) clamped.x = maximum.x;
		if (clamped.y > maximum.y) clamped.y = maximum.y;

		// Keep the pointer-to-window offset in sync at the boundary. Otherwise the
		// panel feels stuck until the pointer travels back across the overshoot.
		if (window != NULL && (clamped.x != position.x || clamped.y != position.y)) {
			ImGuiContext& context = *GImGui;
			if (context.MovingWindow != NULL &&
				context.MovingWindow->RootWindow == window->RootWindow &&
				context.IO.MouseDown[0] && ImGui::IsMousePosValid(&context.IO.MousePos)) {
				context.ActiveIdClickOffset = ImVec2(
					context.IO.MousePos.x - clamped.x,
					context.IO.MousePos.y - clamped.y);
			}
		}
		ImGui::SetNextWindowPos(clamped, ImGuiCond_Always);
	}

	void DrawAirPlayGlyph(ImDrawList* drawList, ImVec2 center, float scale,
		float opacity = 1.0f)
	{
		opacity = Clamp01(opacity);
		if (opacity <= 0.0f) return;

		ImVec4 color = UI_ACCENT;
		color.w *= opacity;
		const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
		const float normalizedScale = MaxFloat(scale, 0.01f);
		const float stroke = MaxFloat(1.0f,
			ImFloor(1.4f * normalizedScale + 0.5f));
		const float phase = ((static_cast<int>(stroke) & 1) != 0) ? 0.5f : 0.0f;
		const auto snapStroke = [phase](float value) {
			return ImFloor(value - phase + 0.5f) + phase;
		};

		const float centerX = snapStroke(center.x);
		const float centerY = snapStroke(center.y);
		const float left = snapStroke(centerX - 16.0f * normalizedScale);
		const float right = snapStroke(centerX + 16.0f * normalizedScale);
		const float top = snapStroke(centerY - 11.5f * normalizedScale);
		const float bottom = snapStroke(centerY + 7.5f * normalizedScale);
		const float radius = 3.0f * normalizedScale;
		const ImU32 canvas = ImGui::ColorConvertFloat4ToU32(UI_CANVAS);

		const ImDrawListFlags savedFlags = drawList->Flags;
		drawList->Flags |= ImDrawListFlags_AntiAliasedLines |
			ImDrawListFlags_AntiAliasedFill;

		// Use ImGui's stable rounded-rectangle tessellation for the screen. A
		// slightly larger canvas-colored triangle removes the crossing stroke and
		// leaves a clean one-pixel separation around the receiver mark.
		drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom),
			packed, radius, 0, stroke);
		drawList->AddTriangleFilled(
			ImVec2(centerX, centerY + 0.25f * normalizedScale),
			ImVec2(centerX + 10.0f * normalizedScale,
				centerY + 16.5f * normalizedScale),
			ImVec2(centerX - 10.0f * normalizedScale,
				centerY + 16.5f * normalizedScale), canvas);
		drawList->AddTriangleFilled(
			ImVec2(centerX, centerY + 1.75f * normalizedScale),
			ImVec2(centerX + 8.5f * normalizedScale,
				centerY + 15.5f * normalizedScale),
			ImVec2(centerX - 8.5f * normalizedScale,
				centerY + 15.5f * normalizedScale), packed);
		drawList->Flags = savedFlags;
	}

	float StatusLineWidth(const char* label, float scale)
	{
		return ImGui::CalcTextSize(label).x + 15.0f * scale;
	}

	void DrawStatusLine(const char* label, ImVec4 color, float scale,
		float opacity = 1.0f)
	{
		opacity = Clamp01(opacity);
		float width = StatusLineWidth(label, scale);
		float height = MaxFloat(ImGui::GetTextLineHeight(), 16.0f * scale);
		ImVec2 position = ImGui::GetCursorScreenPos();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		color.w *= opacity;
		drawList->AddCircleFilled(ImVec2(position.x + 4.0f * scale, position.y + height * 0.5f),
			3.0f * scale, ImGui::ColorConvertFloat4ToU32(color));
		ImVec2 textSize = ImGui::CalcTextSize(label);
		drawList->AddText(ImVec2(position.x + 13.0f * scale,
			position.y + (height - textSize.y) * 0.5f),
			ImGui::ColorConvertFloat4ToU32(color), label);
		ImGui::Dummy(ImVec2(width, height));
	}

	bool DrawCloseButton(const char* id, float size, float scale)
	{
		ImVec2 position = ImGui::GetCursorScreenPos();
		bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size), ImGuiButtonFlags_EnableNav);
		bool hovered = ImGui::IsItemHovered();
		bool active = ImGui::IsItemActive();
		bool focused = ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		if (hovered || active || focused) {
			ImVec4 fill = active ? ImVec4(1.0f, 1.0f, 1.0f, 0.10f)
				: ImVec4(1.0f, 1.0f, 1.0f, 0.055f);
			drawList->AddRectFilled(position, ImVec2(position.x + size, position.y + size),
				ImGui::ColorConvertFloat4ToU32(fill), 3.0f * scale);
			if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		}
		if (focused) {
			drawList->AddRect(position, ImVec2(position.x + size, position.y + size),
				ImGui::ColorConvertFloat4ToU32(UI_ACCENT), 3.0f * scale, 0, 1.0f * scale);
		}
		float arm = 4.0f * scale;
		ImVec2 center(position.x + size * 0.5f, position.y + size * 0.5f);
		ImU32 color = ImGui::ColorConvertFloat4ToU32(hovered ? UI_TEXT_PRIMARY : UI_TEXT_SECONDARY);
		drawList->AddLine(ImVec2(center.x - arm, center.y - arm),
			ImVec2(center.x + arm, center.y + arm), color, 1.35f * scale);
		drawList->AddLine(ImVec2(center.x + arm, center.y - arm),
			ImVec2(center.x - arm, center.y + arm), color, 1.35f * scale);
		return pressed;
	}

	void ShowTooltip(const char* text)
	{
		bool keyboardFocused = ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) || keyboardFocused) {
			ImGui::SetTooltip("%s", text);
		}
	}

	const char* QualityDescription(EQualityPreset preset)
	{
		switch (preset) {
		case QUALITY_GOOD: return "Sharper image at 30 fps";
		case QUALITY_FAST: return "Lowest latency at 60 fps";
		default: return "Smooth 60 fps with balanced filtering";
		}
	}

}

CImGuiManager::CImGuiManager()
	: m_bInitialized(false)
	, m_rendererDeviceResetPending(false)
	, m_pContext(NULL)
	, m_pWindow(NULL)
	, m_pRenderer(NULL)
	, m_pFontBody(NULL)
	, m_pFontHeading(NULL)
	, m_pFontTitle(NULL)
	, m_pFontPin(NULL)
	, m_pFontMono(NULL)
	, m_bEditingDeviceName(false)
	, m_airPlayPinEnabled(false)
	, m_protectPinFromCapture(true)
	, m_pinApprovalPopupRequested(false)
	, m_overlayState(OVERLAY_LAUNCHER)
	, m_overlayAnchor(0.0f, 0.0f)
	, m_overlayAnchorValid(false)
	, m_overlayExpandedSize(0.0f, 0.0f)
	, m_overlayExpandedSizeValid(false)
	, m_pictureInPictureMode(false)
	, m_qualityPreset(QUALITY_BALANCED)  // Default to balanced (60fps, best available filtering)
	, m_screenCastEnabled(false)
	, m_screenCastHideInterface(true)
	, m_screenCastCropToVideo(true)
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

float CImGuiManager::GetWindowDpiScale() const
{
	float scale = 0.0f;

	// GetDpiForWindow follows PMv2's effective DPI for the monitor currently
	// owning the HWND. Resolve it dynamically so older Windows versions still
	// fall through to SDL's per-display query.
	if (m_pWindow != NULL) {
		SDL_SysWMinfo wmInfo = {};
		SDL_VERSION(&wmInfo.version);
		if (SDL_GetWindowWMInfo(m_pWindow, &wmInfo) &&
			wmInfo.subsystem == SDL_SYSWM_WINDOWS) {
			typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
			HMODULE user32 = GetModuleHandleW(L"user32.dll");
			GetDpiForWindowFn getDpiForWindow = user32 != NULL
				? (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow") : NULL;
			if (getDpiForWindow != NULL) {
				UINT dpi = getDpiForWindow(wmInfo.info.win.window);
				if (dpi > 0) scale = (float)dpi / 96.0f;
			}
		}

		if (scale <= 0.0f) {
			int displayIndex = SDL_GetWindowDisplayIndex(m_pWindow);
			float displayDpi = 0.0f;
			if (displayIndex >= 0 &&
				SDL_GetDisplayDPI(displayIndex, &displayDpi, NULL, NULL) == 0 &&
				displayDpi > 0.0f) {
				scale = displayDpi / 96.0f;
			}
		}
	}

	if (scale <= 0.0f) {
		HDC hdc = GetDC(NULL);
		if (hdc != NULL) {
			scale = (float)GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
			ReleaseDC(NULL, hdc);
		}
	}

	if (scale < 1.0f) scale = 1.0f;
	if (scale > 4.0f) scale = 4.0f;
	return scale;
}

void CImGuiManager::ApplyWindowMinimumSize()
{
	// PiP only needs room for the video and compact exit control. Normal mode
	// retains the full workspace required by settings and session controls.
	if (m_pWindow != NULL) {
		// CSDLPlayer applies a source-aspect-aware minimum while PiP is active.
		// Avoid an independent width/height floor here because that would force
		// portrait devices into a wider window with black side bars.
		float baseWidth = m_pictureInPictureMode ? 1.0f : 560.0f;
		float baseHeight = m_pictureInPictureMode ? 1.0f : 420.0f;
		int minimumWidth = (int)(baseWidth * m_dpiScale + 0.5f);
		int minimumHeight = (int)(baseHeight * m_dpiScale + 0.5f);
		int borderTop = 0;
		int borderLeft = 0;
		int borderBottom = 0;
		int borderRight = 0;
		SDL_GetWindowBordersSize(m_pWindow, &borderTop, &borderLeft,
			&borderBottom, &borderRight);
		int displayIndex = SDL_GetWindowDisplayIndex(m_pWindow);
		SDL_Rect usableBounds = {};
		if (displayIndex >= 0 &&
			SDL_GetDisplayUsableBounds(displayIndex, &usableBounds) == 0) {
			int maximumClientWidth = usableBounds.w - borderLeft - borderRight;
			int maximumClientHeight = usableBounds.h - borderTop - borderBottom;
			if (maximumClientWidth < 1) maximumClientWidth = 1;
			if (maximumClientHeight < 1) maximumClientHeight = 1;
			if (minimumWidth > maximumClientWidth) minimumWidth = maximumClientWidth;
			if (minimumHeight > maximumClientHeight) minimumHeight = maximumClientHeight;
		}
		if (minimumWidth < 1) minimumWidth = 1;
		if (minimumHeight < 1) minimumHeight = 1;
		SDL_SetWindowMinimumSize(m_pWindow, minimumWidth, minimumHeight);
	}
}

void CImGuiManager::SetPictureInPictureMode(bool enabled)
{
	if (m_pictureInPictureMode == enabled) {
		return;
	}
	m_pictureInPictureMode = enabled;
	ApplyWindowMinimumSize();
}

void CImGuiManager::ApplyDpiScale(float dpiScale)
{
	if (dpiScale < 1.0f) dpiScale = 1.0f;
	if (dpiScale > 4.0f) dpiScale = 4.0f;
	m_dpiScale = dpiScale;
	m_overlayExpandedSizeValid = false;
	ApplyWindowMinimumSize();

	// Recreate the style from an unscaled baseline on every transition. Scaling
	// the live style in-place would compound after 100% -> 150% -> 100% moves.
	ImGui::GetStyle() = ImGuiStyle();
	ImGui::StyleColorsDark();
	SetupStyle();
	ImGui::GetStyle().ScaleAllSizes(m_dpiScale);

	// ImGui 1.92's dynamic atlas owns the old/new texture transition. Clear()
	// leaves the old texture in a queueable state and marks the rebuilt atlas for
	// creation; pre-emptively destroying device objects would invalidate that
	// state before ImFontAtlasTextureAdd can retire it.
	RebuildFonts();
}

void CImGuiManager::RebuildFonts()
{
	ImGuiIO& io = ImGui::GetIO();
	io.FontDefault = NULL;
	io.Fonts->Clear();
	io.Fonts->TexGlyphPadding = 1;

	// Explicit type roles create hierarchy and keep live metrics stable.
	const char* bodyPaths[] = {
		"C:\\Windows\\Fonts\\SegUIVar.ttf",
		"C:\\Windows\\Fonts\\segoeui.ttf",
		"C:\\Windows\\Fonts\\arial.ttf"
	};
	const char* emphasisPaths[] = {
		"C:\\Windows\\Fonts\\seguisb.ttf",
		"C:\\Windows\\Fonts\\segoeuib.ttf",
		"C:\\Windows\\Fonts\\segoeui.ttf"
	};
	const char* titlePaths[] = {
		"C:\\Windows\\Fonts\\segoeuib.ttf",
		"C:\\Windows\\Fonts\\seguisb.ttf",
		"C:\\Windows\\Fonts\\segoeui.ttf"
	};
	const char* monoPaths[] = {
		"C:\\Windows\\Fonts\\CascadiaMono.ttf",
		"C:\\Windows\\Fonts\\consola.ttf"
	};

	m_pFontBody = LoadUiFont(io, FindFirstFont(bodyPaths, 3), 15.0f * m_dpiScale);
	if (m_pFontBody == NULL) {
		ImFontConfig defaultConfig;
		defaultConfig.SizePixels = 15.0f * m_dpiScale;
		m_pFontBody = io.Fonts->AddFontDefault(&defaultConfig);
	}
	m_pFontHeading = LoadUiFont(io, FindFirstFont(emphasisPaths, 3), 17.0f * m_dpiScale);
	m_pFontTitle = LoadUiFont(io, FindFirstFont(titlePaths, 3), 25.0f * m_dpiScale);
	m_pFontPin = LoadUiFont(io, FindFirstFont(titlePaths, 3), 52.0f * m_dpiScale);
	m_pFontMono = LoadUiFont(io, FindFirstFont(monoPaths, 2), 13.0f * m_dpiScale);
	if (m_pFontHeading == NULL) m_pFontHeading = m_pFontBody;
	if (m_pFontTitle == NULL) m_pFontTitle = m_pFontHeading;
	if (m_pFontPin == NULL) m_pFontPin = m_pFontTitle;
	if (m_pFontMono == NULL) m_pFontMono = m_pFontBody;
	io.FontDefault = m_pFontBody;
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
	// All application windows are positioned explicitly and real preferences
	// live in airplay_settings.ini. Disable Dear ImGui's transient layout cache
	// so imgui.ini is neither required nor recreated beside the executable.
	io.IniFilename = NULL;

	// Configure font atlas for better quality
	io.Fonts->TexGlyphPadding = 1;  // Padding between glyphs for crisp rendering

	// Keep the owning window so DPI can be sampled again after a monitor move.
	m_pWindow = window;
	m_pRenderer = renderer;
	m_rendererDeviceResetPending = false;
	ApplyDpiScale(GetWindowDpiScale());

	// Initialize SDL2 + SDL2Renderer ImGui backends
	// These handle input, font atlas texture upload, and GPU-accelerated rendering
	ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
	ImGui_ImplSDLRenderer2_Init(renderer);

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
	m_pWindow = NULL;
	m_pRenderer = NULL;
	m_rendererDeviceResetPending = false;
	m_bInitialized = false;
}

void CImGuiManager::NewFrame()
{
	if (!m_bInitialized) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);

	// Sample the HWND at a safe frame boundary. This catches WM_DPICHANGED even
	// if SDL does not emit a display-change event for a particular move.
	if (!m_rendererDeviceResetPending) {
		float currentDpiScale = GetWindowDpiScale();
		float dpiDifference = currentDpiScale - m_dpiScale;
		if (dpiDifference < 0.0f) dpiDifference = -dpiDifference;
		if (dpiDifference > 0.01f) {
			ApplyDpiScale(currentDpiScale);
		}
	}

	// Start new ImGui frame using SDL2 backends
	ImGui_ImplSDLRenderer2_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	m_rendererDeviceResetPending = false;
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

void CImGuiManager::RecreateRendererDeviceObjects()
{
	if (!m_bInitialized || m_pContext == NULL) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);
	ImGui_ImplSDLRenderer2_DestroyDeviceObjects();
	ImGui_ImplSDLRenderer2_CreateDeviceObjects();
	m_rendererDeviceResetPending = true;
}

void CImGuiManager::RenderHomeScreen(const char* deviceName, bool isServerRunning)
{
	if (!m_bInitialized) {
		return;
	}
	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();
	float screenW = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 800.0f;
	float screenH = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 600.0f;
	float scale = m_dpiScale;
	bool compact = screenW <= 640.0f * scale || screenH <= 480.0f * scale;
	float margin = (compact ? 28.0f : 48.0f) * scale;
	float contentWidth = MinFloat(440.0f * scale, screenW - margin * 2.0f);
	if (contentWidth < 220.0f * scale) contentWidth = MaxFloat(1.0f, screenW - margin * 2.0f);
	float contentX = ImFloor((screenW - contentWidth) * 0.5f + 0.5f);
	float top = (compact ? 30.0f : 48.0f) * scale;

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

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 windowPos = ImGui::GetWindowPos();
	drawList->AddRectFilled(windowPos, ImVec2(windowPos.x + screenW, windowPos.y + screenH),
		ImGui::ColorConvertFloat4ToU32(UI_CANVAS));

	DrawAirPlayGlyph(drawList,
		ImVec2(windowPos.x + contentX + 18.0f * scale,
			windowPos.y + top + 17.0f * scale), scale);
	float settingsButtonWidth = 104.0f * scale;
	float settingsButtonHeight = 34.0f * scale;
	ImGui::SetCursorPos(ImVec2(screenW - margin - settingsButtonWidth, top));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * scale);
	if (ImGui::Button("Settings##HomeSettings",
		ImVec2(settingsButtonWidth, settingsButtonHeight))) {
		ImGui::OpenPopup("Settings##HomeSettings");
	}
	ImGui::PopStyleVar();
	ShowTooltip("Receiver and security settings");
	const char* statusLabel = isServerRunning ? "Ready" : "Offline";
	ImGui::SetCursorPos(ImVec2(contentX + 50.0f * scale, top + 8.0f * scale));
	DrawStatusLine(statusLabel, isServerRunning ? UI_SUCCESS : UI_ERROR, scale);

	float ruleY = top + 50.0f * scale;
	drawList->AddLine(ImVec2(windowPos.x + contentX, windowPos.y + ruleY),
		ImVec2(windowPos.x + contentX + contentWidth, windowPos.y + ruleY),
		IM_COL32(255, 255, 255, 22), 1.0f * scale);

	if (!m_bEditingDeviceName && strlen(m_deviceNameBuffer) == 0 && deviceName != NULL) {
		strncpy_s(m_deviceNameBuffer, sizeof(m_deviceNameBuffer), deviceName, _TRUNCATE);
	}

	float mainY = ruleY + (compact ? 32.0f : 40.0f) * scale;
	ImGui::SetCursorPos(ImVec2(contentX, mainY));
	if (m_pFontTitle != NULL) ImGui::PushFont(m_pFontTitle);
	ImGui::TextColored(UI_TEXT_PRIMARY, "%s",
		isServerRunning ? "Waiting for a device" : "Receiver unavailable");
	if (m_pFontTitle != NULL) ImGui::PopFont();
	float headingBottom = ImGui::GetItemRectMax().y - windowPos.y;

	char instruction[512];
	if (isServerRunning) {
		strcpy_s(instruction, sizeof(instruction),
			"Open Screen Mirroring on your Apple device and choose the receiver below.");
	} else {
		strcpy_s(instruction, sizeof(instruction),
			"Check that Bonjour is available, then restart the receiver.");
	}
	ImGui::SetCursorPos(ImVec2(contentX, headingBottom + 12.0f * scale));
	ImGui::PushTextWrapPos(contentX + contentWidth);
	ImGui::TextColored(UI_TEXT_SECONDARY, "%s", instruction);
	ImGui::PopTextWrapPos();

	ImGui::PopStyleVar(3);
	RenderSettingsPopup();
	ImGui::End();
}

void CImGuiManager::RenderRequirePinSetting(float contentWidth, float scale)
{
	float contentX = ImGui::GetCursorPosX();
	const float pinToggleHeight = 34.0f * scale;
	const float pinCheckSize = 25.0f * scale;
	ImVec2 pinToggleMin = ImGui::GetCursorScreenPos();
	ImGui::PushID("RequirePinSetting");
	if (ImGui::InvisibleButton("##Toggle", ImVec2(contentWidth, pinToggleHeight))) {
		m_airPlayPinEnabled = !m_airPlayPinEnabled;
	}
	bool pinToggleHovered = ImGui::IsItemHovered();
	bool pinToggleFocused = ImGui::IsItemFocused();
	ImGui::PopID();

	ImDrawList* securityDrawList = ImGui::GetWindowDrawList();
	ImVec2 pinToggleMax(pinToggleMin.x + contentWidth, pinToggleMin.y + pinToggleHeight);
	if (pinToggleHovered) {
		securityDrawList->AddRectFilled(pinToggleMin, pinToggleMax,
			IM_COL32(78, 111, 145, 26), 6.0f * scale);
	}
	if (pinToggleFocused) {
		securityDrawList->AddRect(pinToggleMin, pinToggleMax,
			ImGui::ColorConvertFloat4ToU32(UI_ACCENT_HOVER), 6.0f * scale, 0,
			1.0f * scale);
	}
	ImVec2 pinCheckMin(pinToggleMin.x + 2.0f * scale,
		pinToggleMin.y + (pinToggleHeight - pinCheckSize) * 0.5f);
	ImVec2 pinCheckMax(pinCheckMin.x + pinCheckSize, pinCheckMin.y + pinCheckSize);
	ImVec4 pinCheckFill = m_airPlayPinEnabled
		? UI_ALLOW_BUTTON : UI_SURFACE_RAISED;
	ImVec4 pinCheckBorder = m_airPlayPinEnabled
		? UI_SUCCESS : ImVec4(0.25f, 0.27f, 0.30f, 1.0f);
	securityDrawList->AddRectFilled(pinCheckMin, pinCheckMax,
		ImGui::ColorConvertFloat4ToU32(pinCheckFill), 5.0f * scale);
	securityDrawList->AddRect(pinCheckMin, pinCheckMax,
		ImGui::ColorConvertFloat4ToU32(pinCheckBorder), 5.0f * scale, 0,
		1.0f * scale);
	if (m_airPlayPinEnabled) {
		float checkPad = pinCheckSize / 6.0f;
		ImVec2 pinCheckMarkPos(pinCheckMin.x + checkPad, pinCheckMin.y + checkPad);
		ImGui::RenderCheckMark(securityDrawList, pinCheckMarkPos,
			ImGui::ColorConvertFloat4ToU32(UI_TEXT_PRIMARY), pinCheckSize - checkPad * 2.0f);
	}
	float pinLabelHeight = m_pFontBody != NULL ? m_pFontBody->LegacySize : ImGui::GetTextLineHeight();
	ImVec2 pinLabelPos(pinCheckMax.x + 10.0f * scale,
		pinToggleMin.y + (pinToggleHeight - pinLabelHeight) * 0.5f);
	if (m_pFontBody != NULL) {
		securityDrawList->AddText(m_pFontBody, m_pFontBody->LegacySize, pinLabelPos,
			ImGui::ColorConvertFloat4ToU32(UI_TEXT_PRIMARY), "Require PIN");
	} else {
		securityDrawList->AddText(pinLabelPos, ImGui::ColorConvertFloat4ToU32(UI_TEXT_PRIMARY),
			"Require PIN");
	}
	ShowTooltip("Ask for approval before showing a 4-digit PIN for a new connection");
	float pinToggleBottom = ImGui::GetItemRectMax().y - ImGui::GetWindowPos().y;

	if (m_airPlayPinEnabled) {
		ImGui::SetCursorPos(ImVec2(contentX, pinToggleBottom + 6.0f * scale));
		ImGui::PushTextWrapPos(contentX + contentWidth);
		ImGui::TextColored(UI_TEXT_MUTED,
			"New connections need your approval. After you allow one, this app shows a 4-digit PIN.");
		ImGui::PopTextWrapPos();
		float pinHelpBottom = ImGui::GetItemRectMax().y - ImGui::GetWindowPos().y;
		ImGui::SetCursorPos(ImVec2(contentX, pinHelpBottom + 4.0f * scale));
		ImGui::PushTextWrapPos(contentX + contentWidth);
		ImGui::TextColored(UI_WARNING,
			"Warning: PIN approval is unreliable with MacBooks/macOS.");
		ImGui::PopTextWrapPos();
		ImGui::SetCursorPosX(contentX);
		if (ImGui::Checkbox("Hide PIN from screen capture", &m_protectPinFromCapture)) {
			// Persisted with the other security settings on shutdown.
		}
		ShowTooltip("Exclude the receiver window from recording before showing the PIN locally");
	} else {
		ImGui::SetCursorPos(ImVec2(contentX, pinToggleBottom + 5.0f * scale));
		ImGui::TextColored(UI_TEXT_MUTED, "Off - devices on your network can connect.");
	}
}

void CImGuiManager::RenderSettingsPopup()
{
	if (ImGui::IsPopupOpen("Settings##HomeSettings")) {
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		if (viewport != NULL) {
			ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing,
				ImVec2(0.5f, 0.5f));
		}
		ImGui::SetNextWindowSize(ImVec2(460.0f * m_dpiScale, 0.0f), ImGuiCond_Appearing);
	}
	if (!ImGui::BeginPopupModal("Settings##HomeSettings", NULL,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
		return;
	}

	if (m_pFontHeading != NULL) ImGui::PushFont(m_pFontHeading);
	ImGui::TextColored(UI_TEXT_PRIMARY, "Settings");
	if (m_pFontHeading != NULL) ImGui::PopFont();
	ImGui::Spacing();
	ImGui::TextColored(UI_TEXT_SECONDARY, "Receiver name");
	ImGui::PushItemWidth(-1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		ImVec2(10.0f * m_dpiScale, 11.0f * m_dpiScale));
	if (ImGui::InputText("##ReceiverName", m_deviceNameBuffer, sizeof(m_deviceNameBuffer),
		ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
		m_bEditingDeviceName = false;
	}
	ImGui::PopStyleVar();
	ImGui::PopItemWidth();
	m_bEditingDeviceName = ImGui::IsItemActive();
	ImGui::TextColored(UI_TEXT_MUTED,
		"Shown in Screen Mirroring. Changes apply automatically when disconnected.");

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextColored(UI_TEXT_SECONDARY, "Security");
	RenderRequirePinSetting(ImGui::GetContentRegionAvail().x, m_dpiScale);

	ImGui::Spacing();
	if (ImGui::Button("Close", ImVec2(-1.0f, 0.0f))) {
		m_bEditingDeviceName = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void CImGuiManager::RequestPinApprovalPopup(bool notify)
{
	if (notify && !m_pinApprovalPopupRequested) {
		MessageBeep(MB_ICONASTERISK);
	}
	m_pinApprovalPopupRequested = true;
}

EPinApprovalResult CImGuiManager::RenderPinApprovalPopup(const char* remoteAddress,
	const char* pin, bool awaitingApproval, bool preparingPin, bool showPin,
	bool captureProtectionFailed)
{
	if (!m_bInitialized) {
		return PIN_APPROVAL_NO_ACTION;
	}

	ImGui::SetCurrentContext(m_pContext);
	if (m_pinApprovalPopupRequested) {
		ImGui::OpenPopup("AirPlay connection request##PinApproval");
		m_pinApprovalPopupRequested = false;
	}

	EPinApprovalResult result = PIN_APPROVAL_NO_ACTION;
	if (awaitingApproval || preparingPin || showPin || captureProtectionFailed ||
		m_pinApprovalPopupRequested) {
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		if (viewport != NULL) {
			ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always,
				ImVec2(0.5f, 0.5f));
		}
		// Keep both approval states in one stable modal. AlwaysAutoResize was
		// shrinking the window after Allow because the PIN view has less text.
		ImGui::SetNextWindowSize(ImVec2(390.0f * m_dpiScale,
			260.0f * m_dpiScale), ImGuiCond_Always);
	}
	if (!ImGui::BeginPopupModal("AirPlay connection request##PinApproval", NULL,
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize)) {
		return result;
	}

	if (!awaitingApproval && !preparingPin && !showPin && !captureProtectionFailed) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return result;
	}

	if (awaitingApproval) {
		if (m_pFontHeading != NULL) ImGui::PushFont(m_pFontHeading);
		ImGui::TextColored(UI_TEXT_PRIMARY, "Allow this AirPlay connection?");
		if (m_pFontHeading != NULL) ImGui::PopFont();
		ImGui::Spacing();
		ImGui::TextColored(UI_TEXT_SECONDARY, "A nearby Apple device wants to connect.");
		if (remoteAddress != NULL && remoteAddress[0] != '\0') {
			ImGui::TextColored(UI_TEXT_MUTED, "From %s", remoteAddress);
		}
		ImGui::Spacing();
		ImGui::TextColored(UI_TEXT_MUTED,
			"Allow it to reveal a 4-digit PIN.");

		const ImGuiStyle& style = ImGui::GetStyle();
		float buttonWidth = 132.0f * m_dpiScale;
		float buttonHeight = 40.0f * m_dpiScale;
		float buttonY = ImGui::GetWindowHeight() - style.WindowPadding.y - buttonHeight;
		ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, buttonY));
		ImGui::PushStyleColor(ImGuiCol_Button, UI_DENY_BUTTON);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI_DENY_BUTTON_HOVER);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI_DENY_BUTTON_ACTIVE);
		if (ImGui::Button("Deny", ImVec2(buttonWidth, buttonHeight))) {
			ImGui::CloseCurrentPopup();
			result = PIN_APPROVAL_DENY;
		}
		ImGui::PopStyleColor(3);
		ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - style.WindowPadding.x - buttonWidth,
			buttonY));
		ImGui::PushStyleColor(ImGuiCol_Button, UI_ALLOW_BUTTON);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI_ALLOW_BUTTON_HOVER);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI_ALLOW_BUTTON_ACTIVE);
		if (ImGui::Button("Allow", ImVec2(buttonWidth, buttonHeight))) {
			result = PIN_APPROVAL_ALLOW;
		}
		ImGui::PopStyleColor(3);
	} else if (captureProtectionFailed) {
		if (m_pFontHeading != NULL) ImGui::PushFont(m_pFontHeading);
		ImGui::TextColored(UI_WARNING, "PIN kept private");
		if (m_pFontHeading != NULL) ImGui::PopFont();
		ImGui::Spacing();
		ImGui::PushTextWrapPos();
		ImGui::TextColored(UI_TEXT_SECONDARY,
			"Windows could not exclude this window from screen capture, so the PIN was not shown.");
		ImGui::PopTextWrapPos();

		const ImGuiStyle& style = ImGui::GetStyle();
		float buttonHeight = 40.0f * m_dpiScale;
		float buttonY = ImGui::GetWindowHeight() - style.WindowPadding.y - buttonHeight;
		ImGui::SetCursorPosY(buttonY);
		if (ImGui::Button("Cancel connection", ImVec2(-1.0f, buttonHeight))) {
			ImGui::CloseCurrentPopup();
			result = PIN_APPROVAL_DISMISS;
		}
	} else if (preparingPin) {
		// Capture exclusion is active. Keep the PIN dialog closed for one second
		// so recording software can observe the exclusion before digits exist.
		ImGui::CloseCurrentPopup();
	} else {
		if (m_pFontHeading != NULL) ImGui::PushFont(m_pFontHeading);
		ImGui::TextColored(UI_SUCCESS, "Connection approved");
		if (m_pFontHeading != NULL) ImGui::PopFont();
		ImGui::Spacing();
		ImGui::TextColored(UI_TEXT_SECONDARY,
			"Enter this PIN on your Apple device:");
		ImGui::Spacing();
		const char* displayPin = (pin != NULL && pin[0] != '\0') ? pin : "----";
		if (m_pFontPin != NULL) ImGui::PushFont(m_pFontPin);
		float pinWidth = ImGui::CalcTextSize(displayPin).x;
		ImGui::SetCursorPosX((ImGui::GetWindowWidth() - pinWidth) * 0.5f);
		ImGui::TextColored(UI_TEXT_PRIMARY, "%s", displayPin);
		if (m_pFontPin != NULL) ImGui::PopFont();

		const ImGuiStyle& style = ImGui::GetStyle();
		float buttonHeight = 40.0f * m_dpiScale;
		float buttonY = ImGui::GetWindowHeight() - style.WindowPadding.y - buttonHeight;
		ImGui::SetCursorPosY(buttonY);
		if (ImGui::Button("Cancel", ImVec2(-1.0f, buttonHeight))) {
			ImGui::CloseCurrentPopup();
			result = PIN_APPROVAL_DISMISS;
		}
	}

	ImGui::EndPopup();
	return result;
}

void CImGuiManager::RenderOverlay(const char* deviceName, bool isConnected, const char* connectedDeviceName,
	int videoWidth, int videoHeight, float fps, float bitrateMbps,
	unsigned long long totalFrames, unsigned long long droppedFrames,
	float zoomLevel, int rotationAngle,
	bool* pResetView, bool* pRotateView,
	bool capturePrivacyActive, bool* pToggleCapturePrivacy,
	bool captureExclusionAvailable, bool cleanFeedReady,
	bool pictureInPictureActive, bool* pTogglePictureInPicture)
{
	if (!m_bInitialized) {
		return;
	}
	if (pResetView != NULL) *pResetView = false;
	if (pRotateView != NULL) *pRotateView = false;
	if (pToggleCapturePrivacy != NULL) *pToggleCapturePrivacy = false;
	if (pTogglePictureInPicture != NULL) *pTogglePictureInPicture = false;

	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();
	if (m_overlayState == OVERLAY_HIDDEN) {
		return;
	}
	if (m_overlayState == OVERLAY_LAUNCHER) {
		float scale = m_dpiScale;
		float margin = 12.0f * scale;
		ImVec2 launcherSize(160.0f * scale, 40.0f * scale);
		float dismissSize = 40.0f * scale;
		ImVec2 defaultPosition = m_overlayAnchorValid
			? m_overlayAnchor : ImVec2(margin, margin);
		SetNextWindowPosConstrained("##CollapsedControls", defaultPosition,
			launcherSize, margin, true);
		ImGui::SetNextWindowSize(launcherSize, ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.98f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f * scale);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * scale);
		ImGui::Begin("##CollapsedControls", NULL,
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
		m_overlayAnchor = ImGui::GetWindowPos();
		m_overlayAnchorValid = true;

		ImVec2 buttonPos = ImGui::GetCursorScreenPos();
		ImVec2 buttonSize(launcherSize.x - dismissSize, launcherSize.y);
		if (ImGui::InvisibleButton("##OpenOverlay", buttonSize, ImGuiButtonFlags_EnableNav)) {
			m_overlayState = OVERLAY_EXPANDED;
		}
		bool hovered = ImGui::IsItemHovered();
		bool active = ImGui::IsItemActive();
		bool focused = ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		if (hovered || active || focused) {
			ImVec4 fill = active ? ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.18f)
				: ImVec4(1.0f, 1.0f, 1.0f, 0.045f);
			drawList->AddRectFilled(buttonPos,
				ImVec2(buttonPos.x + buttonSize.x, buttonPos.y + buttonSize.y),
				ImGui::ColorConvertFloat4ToU32(fill), 3.0f * scale);
			if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		}
		if (focused) {
			drawList->AddRect(buttonPos,
				ImVec2(buttonPos.x + buttonSize.x, buttonPos.y + buttonSize.y),
				ImGui::ColorConvertFloat4ToU32(UI_ACCENT), 3.0f * scale, 0, 1.0f * scale);
		}
		drawList->AddCircleFilled(ImVec2(buttonPos.x + 12.0f * scale,
			buttonPos.y + buttonSize.y * 0.5f), 3.0f * scale,
			ImGui::ColorConvertFloat4ToU32(UI_SUCCESS));
		ImVec2 labelSize = ImGui::CalcTextSize("Show controls");
		drawList->AddText(ImVec2(buttonPos.x + 24.0f * scale,
			buttonPos.y + (buttonSize.y - labelSize.y) * 0.5f),
			ImGui::ColorConvertFloat4ToU32(UI_TEXT_PRIMARY), "Show controls");
		ShowTooltip("Open session controls (H)");
		ImGui::SameLine(0.0f, 0.0f);
		if (DrawCloseButton("##DismissOverlayLauncher", dismissSize, scale)) {
			m_overlayState = OVERLAY_HIDDEN;
		}
		ShowTooltip("Hide launcher; press H to show controls");
		ImGui::End();
		ImGui::PopStyleVar(3);
		return;
	}

	float scale = m_dpiScale;
	float margin = 12.0f * scale;
	float panelWidth = MinFloat(310.0f * scale,
		MaxFloat(260.0f * scale, io.DisplaySize.x - margin * 2.0f));
	panelWidth = MinFloat(panelWidth, io.DisplaySize.x - margin * 2.0f);
	float maxHeight = MaxFloat(1.0f, io.DisplaySize.y - margin * 2.0f);
	ImGui::SetNextWindowSizeConstraints(ImVec2(panelWidth, 0.0f), ImVec2(panelWidth, maxHeight));
	ImGuiWindow* overlayWindow = ImGui::FindWindowByName("##OverlayControls");
	ImVec2 nextPanelSize(panelWidth, MinFloat(340.0f * scale, maxHeight));
	bool overlayWasActive = overlayWindow != NULL && overlayWindow->WasActive;
	if (overlayWasActive) {
		nextPanelSize = ImGui::CalcWindowNextAutoFitSize(overlayWindow);
		nextPanelSize.x = panelWidth;
		nextPanelSize.y = MinFloat(nextPanelSize.y, maxHeight);
	} else if (m_overlayExpandedSizeValid) {
		nextPanelSize = ImVec2(panelWidth,
			MinFloat(m_overlayExpandedSize.y, maxHeight));
	}
	ImVec2 defaultPosition = m_overlayAnchorValid
		? m_overlayAnchor : ImVec2(margin, margin);
	SetNextWindowPosConstrained("##OverlayControls", defaultPosition,
		nextPanelSize, margin, true);
	if (!overlayWasActive) {
		// AlwaysAutoResize has no previous expanded content to measure on its first
		// frame. Seed a complete panel size so opening never flashes as a 1-line strip.
		ImGui::SetNextWindowSize(nextPanelSize, ImGuiCond_Always);
	}
	ImGui::SetNextWindowBgAlpha(0.985f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 10.0f * scale));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * scale);
	ImGui::Begin("##OverlayControls", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
	m_overlayAnchor = ImGui::GetWindowPos();
	m_overlayAnchorValid = true;
	m_overlayExpandedSize = ImGui::GetWindowSize();
	m_overlayExpandedSizeValid = true;
	ImDrawList* panelDrawList = ImGui::GetWindowDrawList();
	float closeSize = 28.0f * scale;
	float headerY = ImGui::GetCursorPosY();
	const char* rawTitle = connectedDeviceName != NULL && connectedDeviceName[0] != '\0'
		? connectedDeviceName : "AirPlay session";
	ImVec2 titleCursor = ImGui::GetCursorScreenPos();
	float titleHeight = m_pFontHeading != NULL ? m_pFontHeading->LegacySize : ImGui::GetTextLineHeight();
	panelDrawList->AddCircleFilled(
		ImVec2(titleCursor.x + 4.0f * scale, titleCursor.y + titleHeight * 0.5f),
		3.0f * scale, ImGui::ColorConvertFloat4ToU32(isConnected ? UI_SUCCESS : UI_TEXT_MUTED));
	ImVec2 titleMin(titleCursor.x + 14.0f * scale, titleCursor.y);
	float titleRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x -
		closeSize - 8.0f * scale;
	ImVec2 titleMax(titleRight, titleCursor.y + titleHeight);
	if (m_pFontHeading != NULL) ImGui::PushFont(m_pFontHeading);
	ImGui::RenderTextEllipsis(panelDrawList, titleMin, titleMax, titleRight,
		rawTitle, NULL, NULL);
	if (m_pFontHeading != NULL) ImGui::PopFont();
	ImGui::Dummy(ImVec2(titleRight - titleCursor.x, titleHeight));
	if (deviceName != NULL && deviceName[0] != '\0') {
		char sessionTooltip[600];
		snprintf(sessionTooltip, sizeof(sessionTooltip), "Sender: %s\nReceiver: %s", rawTitle, deviceName);
		ShowTooltip(sessionTooltip);
	} else {
		ShowTooltip(rawTitle);
	}
	ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - closeSize, headerY));
	if (DrawCloseButton("##HideOverlay", closeSize, scale)) {
		m_overlayState = OVERLAY_LAUNCHER;
	}
	ShowTooltip("Collapse controls");

	ImGui::SetCursorPosY(headerY + closeSize + 1.0f * scale);

	char resolution[32];
	char fpsText[24];
	char bitrateText[32];
	if (videoWidth > 0 && videoHeight > 0) {
		snprintf(resolution, sizeof(resolution), "%d x %d", videoWidth, videoHeight);
	} else {
		strcpy_s(resolution, sizeof(resolution), "--");
	}
	if (fps > 0.0f) snprintf(fpsText, sizeof(fpsText), "%.1f", fps);
	else strcpy_s(fpsText, sizeof(fpsText), "--");
	if (bitrateMbps > 0.0f) snprintf(bitrateText, sizeof(bitrateText), "%.1f Mbps", bitrateMbps);
	else strcpy_s(bitrateText, sizeof(bitrateText), "--");

	if (m_pFontMono != NULL) ImGui::PushFont(m_pFontMono);
	ImGui::TextColored(UI_TEXT_SECONDARY, "%s   %s fps   %s", resolution, fpsText, bitrateText);
	if (m_pFontMono != NULL) ImGui::PopFont();

	if (droppedFrames > 0) {
		ImGui::TextColored(UI_WARNING, "%llu of %llu frames dropped", droppedFrames, totalFrames);
	}
	ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
	ImGui::Separator();
	ImGui::TextColored(UI_TEXT_SECONDARY, "View");
	char viewText[48];
	snprintf(viewText, sizeof(viewText), "%.1fx / %d deg", zoomLevel, rotationAngle);
	float viewTextWidth = m_pFontMono != NULL
		? m_pFontMono->CalcTextSizeA(m_pFontMono->LegacySize, 1000.0f, 0.0f, viewText).x
		: ImGui::CalcTextSize(viewText).x;
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - viewTextWidth);
	if (m_pFontMono != NULL) ImGui::PushFont(m_pFontMono);
	ImGui::TextColored(UI_TEXT_PRIMARY, "%s", viewText);
	if (m_pFontMono != NULL) ImGui::PopFont();

	float actionGap = ImGui::GetStyle().ItemSpacing.x;
	float actionWidth = (ImGui::GetContentRegionAvail().x - actionGap) * 0.5f;
	if (ImGui::Button("Rotate 90##RotateView", ImVec2(actionWidth, 34.0f * scale))) {
		if (pRotateView != NULL) *pRotateView = true;
	}
	ShowTooltip("Rotate the stream clockwise (R)");
	ImGui::SameLine();
	bool defaultView = zoomLevel <= 1.0001f && rotationAngle == 0;
	if (defaultView) ImGui::BeginDisabled();
	if (ImGui::Button("Reset view##ResetView", ImVec2(actionWidth, 34.0f * scale))) {
		if (pResetView != NULL) *pResetView = true;
	}
	if (defaultView) ImGui::EndDisabled();
	ShowTooltip("Return to fit-to-window at 0 degrees");
	if (ImGui::Button(pictureInPictureActive
		? "Exit picture in picture##PictureInPicture"
		: "Picture in picture##PictureInPicture",
		ImVec2(-1.0f, 34.0f * scale))) {
		if (pTogglePictureInPicture != NULL) *pTogglePictureInPicture = true;
	}
	ShowTooltip(pictureInPictureActive
		? "Restore the normal receiver window (P)"
		: "Keep a compact video window above other apps (P)");
	if (ImGui::Button(capturePrivacyActive ? "Show in captures##CapturePrivacy" : "Hide from captures##CapturePrivacy",
		ImVec2(-1.0f, 34.0f * scale))) {
		if (pToggleCapturePrivacy != NULL) *pToggleCapturePrivacy = true;
	}
	ShowTooltip(capturePrivacyActive
		? "Allow recording software to see the receiver again"
		: "Keep the receiver visible locally, exclude it from capture, and black the clean feed");
	if (!captureExclusionAvailable) {
		ImGui::TextColored(UI_WARNING, "Windows capture exclusion is unavailable.");
	}

	ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
	ImGui::Separator();
	ImGui::TextColored(UI_TEXT_PRIMARY, "Screen Cast");
	if (ImGui::Checkbox("Enable Screen Cast mode", &m_screenCastEnabled)) {
		// CSDLPlayer applies the output transition on this render frame.
	}
	ShowTooltip("Keep controls visible locally while exposing a clean OBS video source");
	if (m_screenCastEnabled) {
		ImGui::Indent(12.0f * scale);
		if (ImGui::Checkbox("Hide interface from captures", &m_screenCastHideInterface)) {
			// Uses Windows capture exclusion when the operating system supports it.
		}
		ShowTooltip("Hides this receiver window from Display Capture; use the clean-feed source in OBS");
		if (ImGui::Checkbox("Crop clean feed to video", &m_screenCastCropToVideo)) {
			// The output geometry updates from the next rendered frame.
		}
		ShowTooltip("Removes letterboxing and pillarboxing from the clean OBS feed");

		ImGui::TextColored(cleanFeedReady ? UI_SUCCESS : UI_WARNING,
			"Share source: AirPlay Receiver - Clean Feed");
		ShowTooltip("Select this source in OBS Window Capture or Discord's Applications picker");
		if (!cleanFeedReady) {
			ImGui::TextColored(UI_WARNING,
				"Clean feed is waiting for its renderer to start");
		}
		if (m_screenCastHideInterface && !captureExclusionAvailable) {
			ImGui::TextColored(UI_WARNING,
				"Display Capture protection is unavailable on this Windows version");
		}
		ImGui::Unindent(12.0f * scale);
	}

	ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
	ImGui::Separator();
	ImGui::TextColored(UI_TEXT_PRIMARY, "Quality");
	const char* qualityLabels[3] = {
		"30 fps / Best", "60 fps / Balanced", "60 fps / Low latency"
	};
	int qualityIndex = (int)m_qualityPreset;
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::Combo("##Quality", &qualityIndex, qualityLabels, 3)) {
		m_qualityPreset = (EQualityPreset)qualityIndex;
	}
	ShowTooltip(QualityDescription(m_qualityPreset));

	ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
	ImGui::Separator();
	int senderPct = (int)(Clamp01(m_deviceVolume) * 100.0f + 0.5f);
	int inputPct = (int)(Clamp01(m_currentAudioLevel) * 100.0f + 0.5f);
	char audioLevels[48];
	snprintf(audioLevels, sizeof(audioLevels), "Sender %d%%  Input %d%%", senderPct, inputPct);
	ImGui::TextColored(UI_TEXT_PRIMARY, "Audio");
	float audioLevelsWidth = ImGui::CalcTextSize(audioLevels).x;
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - audioLevelsWidth);
	ImGui::TextColored(UI_TEXT_MUTED, "%s", audioLevels);

	int localPct = (int)(m_localVolume * 100.0f + 0.5f);
	ImGui::TextColored(UI_TEXT_SECONDARY, "Output volume");
	char volumeText[16];
	snprintf(volumeText, sizeof(volumeText), "%d%%", localPct);
	float volumeTextWidth = m_pFontMono != NULL
		? m_pFontMono->CalcTextSizeA(m_pFontMono->LegacySize, 1000.0f, 0.0f, volumeText).x
		: ImGui::CalcTextSize(volumeText).x;
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - volumeTextWidth);
	if (m_pFontMono != NULL) ImGui::PushFont(m_pFontMono);
	ImGui::TextColored(UI_TEXT_PRIMARY, "%s", volumeText);
	if (m_pFontMono != NULL) ImGui::PopFont();
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::SliderInt("##OutputVolume", &localPct, 0, 100, "", ImGuiSliderFlags_AlwaysClamp)) {
		m_localVolume = localPct / 100.0f;
	}
	ShowTooltip("Playback volume on this PC");

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * scale, 3.0f * scale));
	if (ImGui::Checkbox("Normalize loud audio", &m_bAutoAdjust)) {
		// State is consumed by CSDLPlayer on the same render loop.
	}
	ImGui::PopStyleVar();
	ShowTooltip("Reduces sudden peaks while keeping quieter audio unchanged");

	ImGui::End();
	ImGui::PopStyleVar(3);
}

void CImGuiManager::RenderPictureInPictureControls(bool* pExitPictureInPicture)
{
	if (pExitPictureInPicture != NULL) *pExitPictureInPicture = false;
	if (!m_bInitialized) {
		return;
	}

	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();
	bool pointerInside = io.MousePos.x >= 0.0f && io.MousePos.y >= 0.0f &&
		io.MousePos.x < io.DisplaySize.x && io.MousePos.y < io.DisplaySize.y;
	if (!pointerInside) {
		return;
	}
	float scale = m_dpiScale;
	float margin = 7.0f * scale;
	float closeSize = 30.0f * scale;
	ImVec2 controlSize(closeSize, closeSize);
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, margin),
		ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowSize(controlSize, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin("##PictureInPictureControls", NULL,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
	if (DrawCloseButton("##ExitPictureInPicture", closeSize, scale) &&
		pExitPictureInPicture != NULL) {
		*pExitPictureInPicture = true;
	}
	ShowTooltip("Restore the normal receiver window (P)");
	ImGui::End();
	ImGui::PopStyleVar(2);
}

// Helper: draw a quiet sparkline using ImDrawList.
// data is a circular buffer, offset is the write position (oldest data)
static void DrawLineGraph(ImDrawList* drawList, ImVec2 pos, ImVec2 size,
	const float* data, int count, int offset, float minVal, float maxVal,
	ImU32 lineColor, float guideValue, float scale)
{
	ImVec2 end(pos.x + size.x, pos.y + size.y);
	drawList->AddRectFilled(pos, end, IM_COL32(12, 13, 15, 180), 2.0f * scale);
	drawList->AddLine(ImVec2(pos.x, pos.y + size.y * 0.5f),
		ImVec2(end.x, pos.y + size.y * 0.5f), IM_COL32(255, 255, 255, 9), 1.0f * scale);

	float range = maxVal - minVal;
	if (range <= 0.0f) range = 1.0f;
	if (guideValue >= minVal && guideValue <= maxVal) {
		float guideNorm = (guideValue - minVal) / range;
		float guideY = pos.y + size.y - guideNorm * size.y;
		drawList->AddLine(ImVec2(pos.x, guideY), ImVec2(end.x, guideY),
			IM_COL32(255, 255, 255, 52), 1.0f * scale);
	}

	if (data != NULL && count > 1) {
		float stepX = size.x / (float)(count - 1);
		for (int i = 0; i < count - 1; ++i) {
			int idx0 = (offset + i) % count;
			int idx1 = (offset + i + 1) % count;
			float v0 = Clamp01((data[idx0] - minVal) / range);
			float v1 = Clamp01((data[idx1] - minVal) / range);
			float x0 = pos.x + stepX * (float)i;
			float x1 = pos.x + stepX * (float)(i + 1);
			float y0 = pos.y + size.y - v0 * size.y;
			float y1 = pos.y + size.y - v1 * size.y;
			drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), lineColor, 1.4f * scale);
		}
	}
	drawList->AddRect(pos, end, IM_COL32(255, 255, 255, 15), 2.0f * scale, 0, 1.0f * scale);
}

static void DrawPerfChart(const char* label, const char* value, const char* maxLabel,
	const float* history, int historySize, int currentIdx, float minValue, float maxValue,
	ImVec4 color, float guideValue, ImFont* monoFont, float scale)
{
	float startX = ImGui::GetCursorPosX();
	float available = ImGui::GetContentRegionAvail().x;
	ImGui::TextColored(UI_TEXT_SECONDARY, "%s", label);
	if (value != NULL && value[0] != '\0') {
		float valueWidth = monoFont != NULL
			? monoFont->CalcTextSizeA(monoFont->LegacySize, 1000.0f, 0.0f, value).x
			: ImGui::CalcTextSize(value).x;
		ImGui::SameLine();
		ImGui::SetCursorPosX(startX + available - valueWidth);
		if (monoFont != NULL) ImGui::PushFont(monoFont);
		ImGui::TextColored(UI_TEXT_PRIMARY, "%s", value);
		if (monoFont != NULL) ImGui::PopFont();
	}

	ImVec2 graphPos = ImGui::GetCursorScreenPos();
	float graphHeight = 30.0f * scale;
	float graphWidth = ImGui::GetContentRegionAvail().x;
	ImGui::Dummy(ImVec2(graphWidth, graphHeight));
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	DrawLineGraph(drawList, graphPos, ImVec2(graphWidth, graphHeight),
		history, historySize, currentIdx, minValue, maxValue,
		ImGui::ColorConvertFloat4ToU32(color), guideValue, scale);

	ImFont* axisFont = monoFont != NULL ? monoFont : ImGui::GetFont();
	float axisSize = axisFont->LegacySize * 0.72f;
	drawList->AddText(axisFont, axisSize,
		ImVec2(graphPos.x + 5.0f * scale, graphPos.y + 3.0f * scale),
		IM_COL32(190, 197, 208, 125), maxLabel);
}

static void DrawStatRow(const char* label, const char* value, ImFont* monoFont)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextColored(UI_TEXT_MUTED, "%s", label);
	ImGui::TableNextColumn();
	if (monoFont != NULL) ImGui::PushFont(monoFont);
	ImGui::TextColored(UI_TEXT_PRIMARY, "%s", value);
	if (monoFont != NULL) ImGui::PopFont();
}

static void DrawMetricPair(const char* label, const char* value, ImFont* monoFont)
{
	ImGui::TableNextColumn();
	ImGui::TextColored(UI_TEXT_MUTED, "%s", label);
	ImGui::TableNextColumn();
	if (monoFont != NULL) ImGui::PushFont(monoFont);
	ImGui::TextColored(UI_TEXT_PRIMARY, "%s", value);
	if (monoFont != NULL) ImGui::PopFont();
}

void CImGuiManager::RenderPerfGraphs(const SPerfData& perf, bool* pOpen)
{
	if (!m_bInitialized || pOpen == NULL || !*pOpen) {
		return;
	}
	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();
	float scale = m_dpiScale;
	float margin = 12.0f * scale;
	float availableWidth = MaxFloat(1.0f, io.DisplaySize.x - margin * 2.0f);
	float availableHeight = MaxFloat(1.0f, io.DisplaySize.y - margin * 2.0f);
	float panelWidth = MinFloat(420.0f * scale, availableWidth);
	float panelHeight = MinFloat(380.0f * scale, availableHeight);
	ImVec2 panelSize(panelWidth, panelHeight);
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImVec2 defaultPosition(
		viewport->WorkPos.x + viewport->WorkSize.x - panelWidth - margin,
		viewport->WorkPos.y + margin);
	SetNextWindowPosConstrained("##PerformancePanel", defaultPosition, panelSize, margin);
	ImGui::SetNextWindowSize(panelSize, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.985f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 10.0f * scale));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * scale);
	ImGui::Begin("##PerformancePanel", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
	float closeSize = 28.0f * scale;
	float headerY = ImGui::GetCursorPosY();
	if (m_pFontHeading != NULL) ImGui::PushFont(m_pFontHeading);
	ImGui::TextColored(UI_TEXT_PRIMARY, "Diagnostics");
	if (m_pFontHeading != NULL) ImGui::PopFont();
	ImGui::SameLine();
	ImGui::TextColored(UI_TEXT_MUTED, "30 s");
	ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - closeSize, headerY));
	if (DrawCloseButton("##ClosePerformance", closeSize, scale)) {
		*pOpen = false;
	}
	ShowTooltip("Close performance panel (F1)");
	ImGui::SetCursorPosY(headerY + closeSize + 2.0f * scale);

	char sourceFps[24];
	char displayFps[32];
	char frameTime[32];
	char latency[32];
	char bitrate[32];
	char audioQueue[32];
	snprintf(sourceFps, sizeof(sourceFps), "%.1f", perf.sourceFps);
	snprintf(displayFps, sizeof(displayFps), "%.1f / %.0f", perf.displayFps, perf.targetFps);
	snprintf(frameTime, sizeof(frameTime), "%.2f ms", perf.frameTimeMs);
	snprintf(latency, sizeof(latency), "%.2f ms", perf.latencyMs);
	snprintf(bitrate, sizeof(bitrate), "%.2f Mbps", perf.bitrateMbps);
	snprintf(audioQueue, sizeof(audioQueue), "%d frames", perf.audioQueueSize);

	if (ImGui::BeginTable("##LiveSummary", 4,
		ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("LabelA", ImGuiTableColumnFlags_WidthFixed, 58.0f * scale);
		ImGui::TableSetupColumn("ValueA", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("LabelB", ImGuiTableColumnFlags_WidthFixed, 58.0f * scale);
		ImGui::TableSetupColumn("ValueB", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();
		DrawMetricPair("Source", sourceFps, m_pFontMono);
		DrawMetricPair("Display", displayFps, m_pFontMono);
		ImGui::TableNextRow();
		DrawMetricPair("Frame", frameTime, m_pFontMono);
		DrawMetricPair("Latency", latency, m_pFontMono);
		ImGui::TableNextRow();
		DrawMetricPair("Bitrate", bitrate, m_pFontMono);
		DrawMetricPair("Buffer", audioQueue, m_pFontMono);
		ImGui::EndTable();
	}

	ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
	ImGui::Separator();
	ImGui::TextColored(UI_TEXT_PRIMARY, "History");
	if (ImGui::BeginTable("##PrimaryCharts", 2,
		ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableNextColumn();
		DrawPerfChart("Frame time", NULL, "33 ms", perf.frameTimeHistory,
			perf.historySize, perf.currentIdx, 0.0f, 33.0f,
			UI_ACCENT, 16.67f, m_pFontMono, scale);
		ImGui::TableNextColumn();
		DrawPerfChart("Bitrate", NULL, "50", perf.bitrateHistory,
			perf.historySize, perf.currentIdx, 0.0f, 50.0f,
			UI_ACCENT, -1.0f, m_pFontMono, scale);
		ImGui::EndTable();
	}
	if (ImGui::CollapsingHeader("More charts")) {
		if (ImGui::BeginTable("##SecondaryCharts", 2,
			ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
			ImGui::TableNextColumn();
			DrawPerfChart("Source FPS", NULL, "80", perf.sourceFpsHistory,
				perf.historySize, perf.currentIdx, 0.0f, 80.0f,
				UI_ACCENT, -1.0f, m_pFontMono, scale);
			ImGui::TableNextColumn();
			DrawPerfChart("Display FPS", NULL, "80", perf.displayFpsHistory,
				perf.historySize, perf.currentIdx, 0.0f, 80.0f,
				UI_ACCENT, perf.targetFps, m_pFontMono, scale);
			ImGui::TableNextColumn();
			DrawPerfChart("Decode latency", NULL, "33 ms", perf.latencyHistory,
				perf.historySize, perf.currentIdx, 0.0f, 33.0f,
				UI_ACCENT, 16.67f, m_pFontMono, scale);
			ImGui::TableNextColumn();
			DrawPerfChart("Audio buffer", NULL, "20", perf.audioQueueHistory,
				perf.historySize, perf.currentIdx, 0.0f, 20.0f,
				UI_ACCENT, -1.0f, m_pFontMono, scale);
			ImGui::EndTable();
		}
	}

	ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
	ImGui::Separator();
	ImGui::TextColored(UI_TEXT_PRIMARY, "Session");

	char videoInfo[64] = "--";
	char dataInfo[48] = "0 MB";
	char frameInfo[64];
	char audioInfo[64];
	char uptime[48] = "--";
	if (perf.videoWidth > 0 && perf.videoHeight > 0) {
		float ar = (float)perf.videoWidth / (float)perf.videoHeight;
		const char* arLabel = "";
		if (ar > 1.76f && ar < 1.78f) arLabel = " | 16:9";
		else if (ar > 1.59f && ar < 1.61f) arLabel = " | 16:10";
		else if (ar > 1.32f && ar < 1.34f) arLabel = " | 4:3";
		else if (ar > 2.32f && ar < 2.35f) arLabel = " | 21:9";
		snprintf(videoInfo, sizeof(videoInfo), "%d x %d%s", perf.videoWidth, perf.videoHeight, arLabel);
	}
	if (perf.totalBytes > 1073741824ULL) {
		snprintf(dataInfo, sizeof(dataInfo), "%.2f GB", (float)perf.totalBytes / 1073741824.0f);
	} else {
		snprintf(dataInfo, sizeof(dataInfo), "%.1f MB", (float)perf.totalBytes / 1048576.0f);
	}
	if (perf.droppedFrames > 0) {
		snprintf(frameInfo, sizeof(frameInfo), "%llu | %llu dropped", perf.totalFrames, perf.droppedFrames);
	} else {
		snprintf(frameInfo, sizeof(frameInfo), "%llu | stable", perf.totalFrames);
	}
	if (perf.audioUnderruns > 0 || perf.audioDropped > 0) {
		snprintf(audioInfo, sizeof(audioInfo), "%d underruns | %d dropped",
			perf.audioUnderruns, perf.audioDropped);
	} else {
		strcpy_s(audioInfo, sizeof(audioInfo), "Stable");
	}
	if (perf.connectionTimeSec > 0.0f) {
		int totalSec = (int)perf.connectionTimeSec;
		int hours = totalSec / 3600;
		int mins = (totalSec % 3600) / 60;
		int secs = totalSec % 60;
		if (hours > 0) snprintf(uptime, sizeof(uptime), "%dh %02dm %02ds", hours, mins, secs);
		else if (mins > 0) snprintf(uptime, sizeof(uptime), "%dm %02ds", mins, secs);
		else snprintf(uptime, sizeof(uptime), "%ds", secs);
	}

	if (ImGui::BeginTable("##SessionDetails", 2,
		ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 90.0f * scale);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
		DrawStatRow("Video", videoInfo, m_pFontMono);
		DrawStatRow("Transferred", dataInfo, m_pFontMono);
		DrawStatRow("Frames", frameInfo, m_pFontMono);
		DrawStatRow("Audio", audioInfo, m_pFontMono);
		DrawStatRow("Uptime", uptime, m_pFontMono);
		ImGui::EndTable();
	}

	ImGui::End();
	ImGui::PopStyleVar(3);
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
	style.WindowRounding = 6.0f;
	style.ChildRounding = 4.0f;
	style.FrameRounding = 4.0f;
	style.PopupRounding = 6.0f;
	style.ScrollbarRounding = 4.0f;
	style.GrabRounding = 4.0f;
	style.TabRounding = 4.0f;
	style.WindowPadding = ImVec2(14.0f, 13.0f);
	style.FramePadding = ImVec2(10.0f, 8.0f);
	style.CellPadding = ImVec2(6.0f, 2.0f);
	style.ItemSpacing = ImVec2(8.0f, 5.0f);
	style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
	style.TouchExtraPadding = ImVec2(1.0f, 1.0f);
	style.DisplayWindowPadding = ImVec2(12.0f, 12.0f);
	style.DisplaySafeAreaPadding = ImVec2(4.0f, 4.0f);
	style.WindowBorderSize = 1.0f;
	style.ChildBorderSize = 1.0f;
	style.PopupBorderSize = 1.0f;
	style.FrameBorderSize = 1.0f;
	style.TabBorderSize = 0.0f;
	style.ScrollbarSize = 12.0f;
	style.GrabMinSize = 12.0f;
	style.WindowMinSize = ImVec2(32.0f, 32.0f);
	style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
	style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
	style.AntiAliasedLines = true;
	style.AntiAliasedFill = true;
	style.DisabledAlpha = 0.46f;

	ImVec4* c = style.Colors;
	ImVec4 border = ImVec4(0.20f, 0.21f, 0.23f, 1.00f);
	c[ImGuiCol_Text] = UI_TEXT_PRIMARY;
	c[ImGuiCol_TextDisabled] = UI_TEXT_MUTED;
	c[ImGuiCol_TextSelectedBg] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.30f);
	c[ImGuiCol_WindowBg] = UI_SURFACE;
	c[ImGuiCol_ChildBg] = UI_CANVAS;
	c[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.079f, 0.086f, 0.995f);
	c[ImGuiCol_Border] = border;
	c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	c[ImGuiCol_FrameBg] = UI_SURFACE_RAISED;
	c[ImGuiCol_FrameBgHovered] = ImVec4(0.125f, 0.132f, 0.145f, 1.00f);
	c[ImGuiCol_FrameBgActive] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.22f);
	c[ImGuiCol_TitleBg] = UI_CANVAS;
	c[ImGuiCol_TitleBgActive] = UI_CANVAS;
	c[ImGuiCol_TitleBgCollapsed] = UI_CANVAS;
	c[ImGuiCol_MenuBarBg] = UI_CANVAS;
	c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.18f);
	c[ImGuiCol_ScrollbarGrab] = ImVec4(0.27f, 0.28f, 0.30f, 1.00f);
	c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.35f, 0.38f, 1.00f);
	c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.41f, 0.42f, 0.45f, 1.00f);
	c[ImGuiCol_CheckMark] = UI_ACCENT_HOVER;
	c[ImGuiCol_SliderGrab] = UI_ACCENT;
	c[ImGuiCol_SliderGrabActive] = UI_ACCENT_HOVER;
	c[ImGuiCol_Button] = UI_SURFACE_RAISED;
	c[ImGuiCol_ButtonHovered] = ImVec4(0.14f, 0.15f, 0.16f, 1.00f);
	c[ImGuiCol_ButtonActive] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.30f);
	c[ImGuiCol_Header] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.14f);
	c[ImGuiCol_HeaderHovered] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.24f);
	c[ImGuiCol_HeaderActive] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.32f);
	c[ImGuiCol_Separator] = border;
	c[ImGuiCol_SeparatorHovered] = UI_ACCENT;
	c[ImGuiCol_SeparatorActive] = UI_ACCENT_HOVER;
	c[ImGuiCol_ResizeGrip] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.10f);
	c[ImGuiCol_ResizeGripHovered] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.30f);
	c[ImGuiCol_ResizeGripActive] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.55f);
	c[ImGuiCol_InputTextCursor] = UI_ACCENT_HOVER;
	c[ImGuiCol_Tab] = UI_SURFACE_RAISED;
	c[ImGuiCol_TabHovered] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.25f);
	c[ImGuiCol_TabSelected] = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.20f);
	c[ImGuiCol_PlotLines] = UI_ACCENT;
	c[ImGuiCol_PlotLinesHovered] = UI_ACCENT_HOVER;
	c[ImGuiCol_PlotHistogram] = UI_WARNING;
	c[ImGuiCol_TableHeaderBg] = UI_SURFACE_RAISED;
	c[ImGuiCol_TableBorderStrong] = border;
	c[ImGuiCol_TableBorderLight] = ImVec4(border.x, border.y, border.z, 0.55f);
	c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	c[ImGuiCol_TableRowBgAlt] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	c[ImGuiCol_NavCursor] = UI_ACCENT_HOVER;
	c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.62f);
	c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.25f);
	c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.58f);
}

void CImGuiManager::RenderDisconnectMessage(const char* deviceName, float visibility)
{
	if (!m_bInitialized) {
		return;
	}
	visibility = Clamp01(visibility);
	if (visibility <= 0.0f) return;

	ImGui::SetCurrentContext(m_pContext);
	float scale = m_dpiScale;
	ImGuiIO& io = ImGui::GetIO();
	float screenW = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 800.0f;
	float screenH = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 600.0f;
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(screenW, screenH), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, visibility);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin("##Disconnect", NULL,
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs);

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec4 canvas = UI_CANVAS;
	canvas.w = visibility;
	drawList->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(screenW, screenH),
		ImGui::ColorConvertFloat4ToU32(canvas));

	char heading[384];
	if (deviceName != NULL && deviceName[0] != '\0') {
		snprintf(heading, sizeof(heading), "%s disconnected", deviceName);
	} else {
		strcpy_s(heading, sizeof(heading), "Stream disconnected");
	}
	if (m_pFontHeading != NULL) ImGui::PushFont(m_pFontHeading);
	ImVec2 headingSize = ImGui::CalcTextSize(heading);
	float headingWidth = MinFloat(headingSize.x, screenW - 48.0f * scale);
	if (headingWidth < 1.0f) headingWidth = 1.0f;
	ImVec2 headingMin((screenW - headingWidth) * 0.5f,
		screenH * 0.5f - 23.0f * scale);
	ImVec2 headingMax(headingMin.x + headingWidth, headingMin.y + headingSize.y);
	ImGui::RenderTextEllipsis(drawList, headingMin, headingMax, headingMax.x,
		heading, NULL, &headingSize);
	if (m_pFontHeading != NULL) ImGui::PopFont();

	const char* detail = "Waiting for another device";
	ImVec2 detailSize = ImGui::CalcTextSize(detail);
	ImGui::SetCursorPos(ImVec2((screenW - detailSize.x) * 0.5f,
		screenH * 0.5f + 7.0f * scale));
	ImGui::TextColored(UI_TEXT_SECONDARY, "%s", detail);

	ImGui::End();
	ImGui::PopStyleVar(4);
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

	// Screen-cast defaults preserve the existing receiver behavior until enabled.
	m_screenCastEnabled = GetPrivateProfileIntA("ScreenCast", "Enabled", 0, iniPath) != 0;
	m_screenCastHideInterface = GetPrivateProfileIntA("ScreenCast", "HideInterface", 1, iniPath) != 0;
	m_screenCastCropToVideo = GetPrivateProfileIntA("ScreenCast", "CropToVideo", 1, iniPath) != 0;

	// PIN protection is opt-in. The temporary four-digit PIN is generated in
	// memory by the receiver and is never persisted.
	m_airPlayPinEnabled = GetPrivateProfileIntA("Security", "RequirePin", 0, iniPath) != 0;
	m_protectPinFromCapture = GetPrivateProfileIntA("Security", "HidePin", 1, iniPath) != 0;

	// Load the three-state overlay setting. Preserve an explicit legacy hidden
	// choice; only a missing legacy key uses the discoverable launcher default.
	char legacyOverlay[16] = { 0 };
	GetPrivateProfileStringA("General", "ShowOverlay", "",
		legacyOverlay, sizeof(legacyOverlay), iniPath);
	int overlayState = GetPrivateProfileIntA("General", "OverlayState", -1, iniPath);
	if (overlayState >= OVERLAY_EXPANDED && overlayState <= OVERLAY_HIDDEN) {
		m_overlayState = (EOverlayState)overlayState;
		// If an older build ran later, it could only update ShowOverlay. A mismatch
		// is therefore a newer legacy choice and takes precedence on re-upgrade.
		if (legacyOverlay[0] != '\0') {
			bool legacyExpanded = atoi(legacyOverlay) != 0;
			bool stateExpanded = m_overlayState == OVERLAY_EXPANDED;
			if (legacyExpanded != stateExpanded) {
				m_overlayState = legacyExpanded ? OVERLAY_EXPANDED : OVERLAY_HIDDEN;
			}
		}
	} else {
		if (legacyOverlay[0] == '\0') {
			m_overlayState = OVERLAY_LAUNCHER;
		} else {
			m_overlayState = atoi(legacyOverlay) != 0
				? OVERLAY_EXPANDED : OVERLAY_HIDDEN;
		}
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

	WritePrivateProfileStringA("Security", "RequirePin",
		m_airPlayPinEnabled ? "1" : "0", iniPath);
	WritePrivateProfileStringA("Security", "HidePin",
		m_protectPinFromCapture ? "1" : "0", iniPath);
	// Remove the old persistent custom-PIN value when a new build saves settings.
	WritePrivateProfileStringA("Security", "PinProtected", NULL, iniPath);

	// Save quality preset
	char buf[16];
	sprintf_s(buf, sizeof(buf), "%d", (int)m_qualityPreset);
	WritePrivateProfileStringA("General", "QualityPreset", buf, iniPath);

	WritePrivateProfileStringA("ScreenCast", "Enabled",
		m_screenCastEnabled ? "1" : "0", iniPath);
	WritePrivateProfileStringA("ScreenCast", "HideInterface",
		m_screenCastHideInterface ? "1" : "0", iniPath);
	WritePrivateProfileStringA("ScreenCast", "CropToVideo",
		m_screenCastCropToVideo ? "1" : "0", iniPath);

	// Save the explicit overlay state. Keep the legacy boolean for older builds.
	sprintf_s(buf, sizeof(buf), "%d", (int)m_overlayState);
	WritePrivateProfileStringA("General", "OverlayState", buf, iniPath);
	WritePrivateProfileStringA("General", "ShowOverlay",
		m_overlayState == OVERLAY_EXPANDED ? "1" : "0", iniPath);

	// Save auto-adjust
	WritePrivateProfileStringA("Audio", "AutoAdjust", m_bAutoAdjust ? "1" : "0", iniPath);

	// Save local volume (stored as 0-100 integer)
	char volBuf[16];
	sprintf_s(volBuf, sizeof(volBuf), "%d", (int)(m_localVolume * 100.0f));
	WritePrivateProfileStringA("Audio", "LocalVolume", volBuf, iniPath);
}
