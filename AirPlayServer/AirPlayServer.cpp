// AirPlayServer.cpp : AirPlay Receiver - Main Program
// Starts automatically and waits for AirPlay connections.
// Window shows home screen with ImGui UI.
//
#include <windows.h>
#include <signal.h>
#include "Airplay2Head.h"
#include "CAirServerCallback.h"
#include "SDL.h"
#include "CSDLPlayer.h"

// Global player pointer for cleanup handlers
static CSDLPlayer* g_pPlayer = NULL;
static volatile bool g_bShuttingDown = false;

// Cleanup function to stop server gracefully
void CleanupAndShutdown()
{
    if (g_bShuttingDown) {
        return;  // Prevent re-entrancy
    }
    g_bShuttingDown = true;

    if (g_pPlayer != NULL) {
        g_pPlayer->m_server.stop();
        Sleep(150);
    }
}

// atexit handler as a fallback
void AtExitHandler()
{
    CleanupAndShutdown();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

    // Enable per-monitor DPI awareness for sharp rendering on high-DPI displays
    // Use runtime loading since these APIs require Windows 10 1703+
    {
        HMODULE hUser32 = GetModuleHandle(TEXT("user32.dll"));
        if (hUser32) {
            // Try Per-Monitor V2 first (best quality, Windows 10 1703+)
            typedef BOOL(WINAPI* SetDpiAwarenessContextFn)(HANDLE);
            SetDpiAwarenessContextFn fnCtx = (SetDpiAwarenessContextFn)
                GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
            if (fnCtx) {
                fnCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            } else {
                // Fallback: basic DPI awareness (Vista+)
                typedef BOOL(WINAPI* SetProcessDPIAwareFn)();
                SetProcessDPIAwareFn fnDpi = (SetProcessDPIAwareFn)
                    GetProcAddress(hUser32, "SetProcessDPIAware");
                if (fnDpi) fnDpi();
            }
        }
    }

    // Register atexit handler as fallback
    atexit(AtExitHandler);

    // Get default device name (PC name)
    char hostName[512] = { 0 };
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    gethostname(hostName, sizeof(hostName) - 1);
    if (strlen(hostName) == 0) {
        DWORD n = sizeof(hostName) - 1;
        if (::GetComputerNameA(hostName, &n)) {
            if (n > 0 && n < sizeof(hostName)) {
                hostName[n] = '\0';
            }
        }
    }
    if (strlen(hostName) == 0) {
        strcpy_s(hostName, sizeof(hostName), "AirPlay Server");
    }

    // Check if Apple Bonjour Service is installed (required for mDNS discovery)
    {
        SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
        if (hSCM) {
            SC_HANDLE hSvc = OpenServiceA(hSCM, "Bonjour Service", SERVICE_QUERY_STATUS);
            if (!hSvc) {
                CloseServiceHandle(hSCM);
                MessageBoxA(NULL,
                    "Apple Bonjour Service is not installed.\n\n"
                    "AirPlay requires Bonjour for device discovery. "
                    "Please install Bonjour Print Services from:\n"
                    "https://support.apple.com/kb/DL999\n\n"
                    "Alternatively, installing iTunes will also install Bonjour.",
                    "AirPlay Server - Missing Dependency",
                    MB_OK | MB_ICONWARNING);
                WSACleanup();
                return 1;
            }
            CloseServiceHandle(hSvc);
            CloseServiceHandle(hSCM);
        }
    }

    CSDLPlayer player;
    g_pPlayer = &player;  // Set global pointer for cleanup handlers
    player.setServerName(hostName);

    if (!player.init()) {
        g_pPlayer = NULL;
        return 1;
    }

    player.loopEvents();

    // Clear global pointer before player is destroyed
    g_pPlayer = NULL;

    return 0;
}
