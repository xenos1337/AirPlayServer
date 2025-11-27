// AirPlayServer.cpp : AirPlay Receiver - Main Program
// Starts automatically and waits for AirPlay connections.
// Window shows home screen with ImGui UI.
//
#include <windows.h>
#include <iostream>
#include "Airplay2Head.h"
#include "CAirServerCallback.h"
#include "SDL.h"
#include "CSDLPlayer.h"

int main()
{
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

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
    player.setServerName(hostName);

    if (!player.init()) {
        return 1;
    }

    player.loopEvents();

    return 0;
}
