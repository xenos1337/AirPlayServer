#include "AirPlayCore.h"
#include "CSDLPlayer.h"
#include "DebugLogger.h"
#include "FgUtf8Utils.h"
#include <string.h>

// Every export except the callbacks' targets is called on the WPF UI thread.
static CSDLPlayer* g_player = NULL;

AP_API int __cdecl ap_debug_start(const char* commandLine, char* pathOut, int pathOutSize)
{
	if (pathOut != NULL && pathOutSize > 0) {
		pathOut[0] = '\0';
	}
	if (!DebugLogger::Start(commandLine)) {
		return 0;
	}
	if (pathOut != NULL && pathOutSize > 0) {
		strncpy_s(pathOut, (size_t)pathOutSize, DebugLogger::Path().c_str(), _TRUNCATE);
	}
	return 1;
}

AP_API int __cdecl ap_check_bonjour(unsigned long* error)
{
	if (error != NULL) {
		*error = 0;
	}
	SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (manager == NULL) {
		return AP_BONJOUR_OK;
	}

	// SERVICE_START requires admin rights; fall back to query-only so a
	// permission failure is not reported as "not installed".
	bool canStart = true;
	SC_HANDLE service = OpenServiceW(manager, L"Bonjour Service",
		SERVICE_QUERY_STATUS | SERVICE_START);
	if (service == NULL) {
		canStart = false;
		service = OpenServiceW(manager, L"Bonjour Service", SERVICE_QUERY_STATUS);
	}
	if (service == NULL) {
		CloseServiceHandle(manager);
		return AP_BONJOUR_MISSING;
	}

	int result = AP_BONJOUR_OK;
	SERVICE_STATUS status = {};
	if (QueryServiceStatus(service, &status) &&
		(status.dwCurrentState == SERVICE_STOPPED || status.dwCurrentState == SERVICE_PAUSED)) {
		if (!canStart) {
			result = AP_BONJOUR_STOPPED;
		} else if (!StartServiceW(service, 0, NULL)) {
			DWORD startError = GetLastError();
			if (startError != ERROR_SERVICE_ALREADY_RUNNING) {
				result = AP_BONJOUR_START_FAILED;
				if (error != NULL) {
					*error = startError;
				}
			}
		} else {
			// Wait up to 5 seconds for it to reach SERVICE_RUNNING
			for (int i = 0; i < 50; i++) {
				Sleep(100);
				if (QueryServiceStatus(service, &status) &&
					status.dwCurrentState == SERVICE_RUNNING) {
					break;
				}
			}
		}
	}

	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	return result;
}

AP_API int __cdecl ap_is_receiver_name_valid(const char* name)
{
	return CFgUtf8Utils::IsReceiverNameValid(name) ? 1 : 0;
}

AP_API void __cdecl ap_get_monitor_size(HWND window, int* width, int* height)
{
	unsigned int monitorWidth = 0;
	unsigned int monitorHeight = 0;
	CSDLPlayer::getMonitorSize(window, monitorWidth, monitorHeight);
	*width = (int)monitorWidth;
	*height = (int)monitorHeight;
}

AP_API int __cdecl ap_start(HWND topLevel, const ApCallbacks* callbacks, const ApConfig* config)
{
	if (g_player != NULL || callbacks == NULL || config == NULL) {
		return 0;
	}
	DebugLogger::Write("startup", "AirPlayServer starting; pid=%lu", GetCurrentProcessId());

	g_player = new CSDLPlayer();
	if (!g_player->start(topLevel, *callbacks, *config)) {
		DebugLogger::Write("startup", "player initialization failed");
		delete g_player;
		g_player = NULL;
		return 0;
	}
	return 1;
}

AP_API HWND __cdecl ap_get_view(void)
{
	return g_player != NULL ? g_player->viewWindow() : NULL;
}

AP_API void __cdecl ap_set_config(const ApConfig* config)
{
	if (g_player != NULL && config != NULL) {
		g_player->setConfig(*config);
	}
}

AP_API int __cdecl ap_rotate(void)
{
	return g_player != NULL ? g_player->rotate() : 0;
}

AP_API void __cdecl ap_reset_view(void)
{
	if (g_player != NULL) {
		g_player->resetView();
	}
}

AP_API void __cdecl ap_restart_receiver(void)
{
	if (g_player != NULL) {
		g_player->requestRestart();
	}
}

AP_API void __cdecl ap_set_capture_privacy(int enabled)
{
	if (g_player != NULL) {
		g_player->setCapturePrivacy(enabled != 0);
	}
}

AP_API void __cdecl ap_get_stats(ApStats* stats)
{
	if (stats == NULL) {
		return;
	}
	if (g_player == NULL) {
		memset(stats, 0, sizeof(*stats));
		return;
	}
	g_player->getStats(stats);
}

AP_API void __cdecl ap_pin_answer(int generation, int allow)
{
	if (g_player != NULL) {
		g_player->answerPinApproval((LONG)generation, allow != 0);
	}
}

AP_API void __cdecl ap_pin_dismiss(int generation)
{
	if (g_player != NULL) {
		g_player->dismissPinApproval((LONG)generation);
	}
}

AP_API void __cdecl ap_stop(void)
{
	if (g_player != NULL) {
		g_player->stop();
		// A receiver thread stuck in a hung Bonjour call still references the
		// player; the process is exiting, so leave it allocated.
		if (g_player->canDestroy()) {
			delete g_player;
		}
		g_player = NULL;
	}
	DebugLogger::Stop();
}
