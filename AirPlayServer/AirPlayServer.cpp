// AirPlayServer.cpp : AirPlay Receiver - Main Program
// Starts automatically and waits for AirPlay connections.
// Window appears when a device connects.
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

    printf("===========================================\n");
    printf("       AirPlay Receiver for Windows\n");
    printf("===========================================\n\n");
    printf("The server will start automatically.\n");
    printf("Display window is hidden until a device connects.\n\n");
    printf("Controls (when window is visible):\n");
    printf("  [q] - Stop server\n");
    printf("  [s] - Restart server\n");
    printf("  [-] and [=] - Scale video size\n\n");

    CSDLPlayer player;
    if (!player.init()) {
        printf("Failed to initialize player!\n");
        return 1;
    }

    player.loopEvents();

    printf("AirPlay Receiver shutting down.\n");

    return 0;
}
