#include "DeviceNameDialog.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDC_EDIT_DEVICENAME 1001
#define IDC_STATIC_LABEL 1002

// Static variable to store result across window procedure calls
static char* g_dialogResult = NULL;

// Window procedure for the device name input dialog
LRESULT CALLBACK DeviceNameWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{

    switch (message)
    {
    case WM_CREATE:
    {
        // Create child controls
        CreateWindowA(
            "STATIC", "Enter the name to display in AirPlay:",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            20, 20, 360, 20,
            hWnd, (HMENU)IDC_STATIC_LABEL, GetModuleHandle(NULL), NULL);

        HWND hEdit = CreateWindowA(
            "EDIT", (const char*)((CREATESTRUCT*)lParam)->lpCreateParams,
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
            20, 45, 360, 25,
            hWnd, (HMENU)IDC_EDIT_DEVICENAME, GetModuleHandle(NULL), NULL);

        CreateWindowA(
            "BUTTON", "OK",
            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_TABSTOP,
            220, 80, 75, 30,
            hWnd, (HMENU)IDOK, GetModuleHandle(NULL), NULL);

        CreateWindowA(
            "BUTTON", "Cancel",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
            305, 80, 75, 30,
            hWnd, (HMENU)IDCANCEL, GetModuleHandle(NULL), NULL);

        SetFocus(hEdit);
        SendMessage(hEdit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            char buffer[256] = { 0 };
            GetWindowTextA(GetDlgItem(hWnd, IDC_EDIT_DEVICENAME), buffer, sizeof(buffer) - 1);
            // Trim whitespace
            size_t len = strlen(buffer);
            while (len > 0 && (buffer[len - 1] == ' ' || buffer[len - 1] == '\t')) {
                buffer[--len] = '\0';
            }
            // Store result
            if (g_dialogResult != NULL) {
                free(g_dialogResult);
            }
            g_dialogResult = _strdup(buffer);
            PostMessage(hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        case IDCANCEL:
            if (g_dialogResult != NULL) {
                free(g_dialogResult);
                g_dialogResult = NULL;
            }
            PostMessage(hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            // Enter key - same as OK
            PostMessage(hWnd, WM_COMMAND, IDOK, 0);
            return 0;
        } else if (wParam == VK_ESCAPE) {
            // Escape key - same as Cancel
            PostMessage(hWnd, WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

// Show the device name dialog and return the entered name
// Returns NULL if cancelled. Caller must free the returned string with free()
char* ShowDeviceNameDialog(const char* defaultName)
{
    // Get default name or PC name
    char hostName[512] = { 0 };
    if (defaultName != NULL && strlen(defaultName) > 0) {
        strcpy_s(hostName, sizeof(hostName), defaultName);
    } else {
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
    }

    HINSTANCE hInst = GetModuleHandle(NULL);
    
    // Register window class with unique name to avoid conflicts
    static const char* className = "AirPlayDeviceNameDialogClass";
    WNDCLASSEXA wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = DeviceNameWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = className;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszMenuName = NULL;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    
    // Unregister if already exists, then register
    UnregisterClassA(className, hInst);
    if (RegisterClassExA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        // Failed to register, try fallback
        return _strdup(hostName);
    }

    // Create dialog window
    HWND hWnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        className,
        "",  // Set title after creation
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        (GetSystemMetrics(SM_CXSCREEN) - 400) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - 150) / 2,
        400, 150,
        NULL, NULL, hInst, (LPVOID)hostName);

    if (hWnd == NULL) {
        // Fallback: just return the default name
        return _strdup(hostName);
    }

    // Explicitly set the window title using SetWindowTextA to ensure proper encoding
    SetWindowTextA(hWnd, "AirPlay Device Name");
    
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    // Message loop - exit when window is destroyed
    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessage(&msg, NULL, 0, 0)) != 0) {
        if (bRet == -1) {
            // Error occurred
            break;
        }
        
        // Check if window still exists
        if (!IsWindow(hWnd)) {
            break;
        }
        
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Get result from static variable
    char* result = g_dialogResult;
    g_dialogResult = NULL;  // Reset for next call
    
    // If no result (cancelled), return NULL
    if (result == NULL) {
        return NULL;
    }

    // If empty result, use default
    if (strlen(result) == 0) {
        free(result);
        return _strdup(hostName);
    }

    return result;
}
