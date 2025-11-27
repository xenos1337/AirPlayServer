// AirPlayServer.cpp : AirPlay Receiver - Main Program
// Starts automatically and waits for AirPlay connections.
// Window shows home screen with ImGui UI.
//
#include <windows.h>
#include <iostream>
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

    printf("Initiating graceful shutdown...\n");
    if (g_pPlayer != NULL) {
        g_pPlayer->m_server.stop();
        // Give time for server to stop completely
        Sleep(150);
    }
}

// Console control handler for Ctrl+C, Ctrl+Break, console close, etc.
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType) {
        case CTRL_C_EVENT:
            printf("\nCtrl+C received.\n");
            break;
        case CTRL_BREAK_EVENT:
            printf("\nCtrl+Break received.\n");
            break;
        case CTRL_CLOSE_EVENT:
            printf("\nConsole close event received.\n");
            break;
        case CTRL_LOGOFF_EVENT:
            printf("\nUser logoff event received.\n");
            break;
        case CTRL_SHUTDOWN_EVENT:
            printf("\nSystem shutdown event received.\n");
            break;
        default:
            return FALSE;
    }

    CleanupAndShutdown();

    // Post quit event to SDL to exit the event loop
    SDL_Event quitEvent;
    quitEvent.type = SDL_QUIT;
    SDL_PushEvent(&quitEvent);

    // Give some time for cleanup to complete
    Sleep(500);

    return TRUE;
}

// atexit handler as a fallback
void AtExitHandler()
{
    CleanupAndShutdown();
}

int main()
{
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

    // Register console control handler
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        printf("Warning: Failed to set console control handler\n");
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
