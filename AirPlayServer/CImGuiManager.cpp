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
	io.Fonts->AddFontDefault();
	
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
	
	ImGui::SetCurrentContext(m_pContext);
	ImGuiIO& io = ImGui::GetIO();
	
	// Center the window
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.85f); // Semi-transparent background
	
	ImGui::Begin("AirPlay Server", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
	
	ImGui::Text("Device Name:");
	ImGui::SameLine();
	
	// Copy device name to buffer if not editing
	if (!m_bEditingDeviceName && strlen(m_deviceNameBuffer) == 0) {
		if (deviceName) {
			strncpy_s(m_deviceNameBuffer, sizeof(m_deviceNameBuffer), deviceName, _TRUNCATE);
		}
	}
	
	if (ImGui::InputText("##DeviceName", m_deviceNameBuffer, sizeof(m_deviceNameBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
		m_bEditingDeviceName = false;
	}
	m_bEditingDeviceName = ImGui::IsItemActive();
	
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	
	// Connection status
	if (isConnected) {
		ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Status: Connected");
		if (connectedDeviceName) {
			ImGui::Text("Connected from: %s", connectedDeviceName);
		}
	} else {
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Status: Waiting for connection...");
	}
	
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	
	// Instructions
	ImGui::TextWrapped("This device is ready to receive AirPlay connections.");
	ImGui::TextWrapped("Look for \"%s\" in your device's AirPlay menu.", m_deviceNameBuffer[0] ? m_deviceNameBuffer : deviceName);
	
	ImGui::End();
}

void CImGuiManager::RenderOverlay(bool* pShowUI, const char* deviceName, bool isConnected, const char* connectedDeviceName)
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
		// Show small indicator when UI is hidden
		ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.3f);
		ImGui::Begin("##HiddenUI", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
		ImGui::Text("Press H to show UI");
		ImGui::End();
		return;
	}
	
	// Render overlay UI in corner with semi-transparent background
	ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.75f); // Semi-transparent for overlay
	ImGui::Begin("AirPlay Controls", pShowUI, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
	
	ImGui::Text("Device: %s", deviceName ? deviceName : "Unknown");
	
	if (isConnected) {
		ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected");
		if (connectedDeviceName) {
			ImGui::Text("From: %s", connectedDeviceName);
		}
	} else {
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Waiting...");
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
								
								// Convert UV to texel coordinates (floor for nearest sampling)
								int tx = (int)floorf(u * fontWidth);
								int ty = (int)floorf(v * fontHeight);
								
								// Clamp to texture bounds
								if (tx < 0) tx = 0;
								if (ty < 0) ty = 0;
								if (tx >= fontWidth) tx = fontWidth - 1;
								if (ty >= fontHeight) ty = fontHeight - 1;
								
								unsigned char* texel = fontPixels + (ty * fontWidth + tx) * 4;
								// Multiply alpha by texture alpha
								finalA = (Uint8)((finalA * texel[3]) / 255);
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
	
	style.WindowRounding = 5.0f;
	style.FrameRounding = 3.0f;
	style.GrabRounding = 3.0f;
	style.ScrollbarRounding = 3.0f;
	style.WindowPadding = ImVec2(10, 10);
	style.FramePadding = ImVec2(5, 5);
	style.ItemSpacing = ImVec2(8, 6);
	style.ItemInnerSpacing = ImVec2(6, 4);
	style.TouchExtraPadding = ImVec2(0, 0);
	style.IndentSpacing = 21.0f;
	style.ScrollbarSize = 14.0f;
	style.GrabMinSize = 10.0f;
}

const char* CImGuiManager::GetDeviceName() const
{
	return m_deviceNameBuffer[0] ? m_deviceNameBuffer : NULL;
}

