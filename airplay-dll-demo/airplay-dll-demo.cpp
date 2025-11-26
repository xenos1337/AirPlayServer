// airplay-dll-demo.cpp : This file contains the "main" function. Program execution begins and ends here.
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

    printf("Usage: \n [s] to start server\n [q] to stop\n [-] and [=] to scale video size.\n\n");

    CSDLPlayer player;
    player.init();

    player.loopEvents();

    /* This should never happen */
    printf("SDL_WaitEvent error: %s\n", SDL_GetError());

    return 0;
}
