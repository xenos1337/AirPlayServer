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

    // Check Bonjour Service (required for mDNS device discovery)
    {
        SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
        if (hSCM)
        {
            // First try with start permission, fall back to query-only
            // (SERVICE_START requires admin — don't let that look like "not installed")
            bool bCanStart = true;
            SC_HANDLE hSvc = OpenServiceA(hSCM, "Bonjour Service",
                SERVICE_QUERY_STATUS | SERVICE_START);
            if (!hSvc)
            {
                bCanStart = false;
                hSvc = OpenServiceA(hSCM, "Bonjour Service", SERVICE_QUERY_STATUS);
            }

            if (!hSvc)
            {
                // Truly not installed
                CloseServiceHandle(hSCM);
                int choice = MessageBoxA(NULL,
                    "Apple Bonjour is not installed.\n\n"
                    "Bonjour is required for AirPlay device discovery.\n\n"
                    "Click OK to open the Bonjour download page, or Cancel to exit.",
                    "AirPlay Server - Bonjour Not Found",
                    MB_OKCANCEL | MB_ICONWARNING);
                if (choice == IDOK)
                    ShellExecuteA(NULL, "open", "https://support.apple.com/kb/DL999",
                        NULL, NULL, SW_SHOWNORMAL);
                WSACleanup();
                return 1;
            }

            // Service exists — check its state
            SERVICE_STATUS ss = {};
            if (QueryServiceStatus(hSvc, &ss))
            {
                if (ss.dwCurrentState == SERVICE_STOPPED ||
                    ss.dwCurrentState == SERVICE_PAUSED)
                {
                    if (bCanStart)
                    {
                        // Try to start it
                        if (!StartServiceA(hSvc, 0, NULL))
                        {
                            DWORD err = GetLastError();
                            if (err != ERROR_SERVICE_ALREADY_RUNNING)
                            {
                                char msg[256];
                                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                                    "Bonjour Service could not be started (error %lu).\n\n"
                                    "Try running as Administrator, or start the service manually via services.msc.",
                                    err);
                                MessageBoxA(NULL, msg, "AirPlay Server - Bonjour Error",
                                    MB_OK | MB_ICONWARNING);
                                CloseServiceHandle(hSvc);
                                CloseServiceHandle(hSCM);
                                WSACleanup();
                                return 1;
                            }
                        }
                        else
                        {
                            // Wait up to 5 seconds for it to reach SERVICE_RUNNING
                            for (int i = 0; i < 50; i++)
                            {
                                Sleep(100);
                                if (QueryServiceStatus(hSvc, &ss) &&
                                    ss.dwCurrentState == SERVICE_RUNNING)
                                    break;
                            }
                        }
                    }
                    else
                    {
                        // No permission to start — ask user to do it manually
                        MessageBoxA(NULL,
                            "Bonjour Service is installed but not running.\n\n"
                            "Try running as Administrator, or start the service manually via services.msc.",
                            "AirPlay Server - Bonjour Stopped",
                            MB_OK | MB_ICONWARNING);
                        CloseServiceHandle(hSvc);
                        CloseServiceHandle(hSCM);
                        WSACleanup();
                        return 1;
                    }
                }
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
