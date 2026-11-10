#pragma once
#include <Windows.h>

#ifdef AIRPLAYCORE_EXPORTS
#define AP_API extern "C" __declspec(dllexport)
#else
#define AP_API extern "C" __declspec(dllimport)
#endif

#define AP_HISTORY_SIZE 30

#define AP_MOD_CTRL 1
#define AP_MOD_SHIFT 2
#define AP_MOD_ALT 4

#define AP_SERVER_OFFLINE 0
#define AP_SERVER_RUNNING 1
#define AP_SERVER_STARTING 2

#define AP_BONJOUR_OK 0
#define AP_BONJOUR_MISSING 1
#define AP_BONJOUR_STOPPED 2
#define AP_BONJOUR_START_FAILED 3

// Mirrored by Native.cs. Keep the field order and types identical.
typedef struct ApConfig
{
	char deviceName[256];
	int pinEnabled;
	int matchMonitor;
	int customWidth;
	int customHeight;
	int quality;
	int screenCast;
	int screenCastCrop;
	int localVolume;
} ApConfig;

// Called on native threads. Strings are UTF-8 and only valid during the call.
typedef struct ApCallbacks
{
	void(__cdecl* connectionChanged)(int connected, const char* deviceName);
	// The first frame of a session, or of a new video size, is on screen.
	void(__cdecl* videoSizeChanged)(int width, int height);
	void(__cdecl* pinRequest)(int generation, const char* remoteAddress, const char* pin);
	void(__cdecl* pinClosed)(int generation);
	void(__cdecl* keyUp)(int virtualKey, int modifiers);
	void(__cdecl* doubleClick)(void);
} ApCallbacks;

// Packed to 4 so the layout does not depend on 64-bit field alignment rules,
// which differ between compilers on x86. Native.cs uses Pack = 4 as well.
#pragma pack(push, 4)
typedef struct ApStats
{
	unsigned long long totalFrames;
	unsigned long long droppedFrames;
	unsigned long long totalBytes;
	float sourceFpsHistory[AP_HISTORY_SIZE];
	float displayFpsHistory[AP_HISTORY_SIZE];
	float frameTimeHistory[AP_HISTORY_SIZE];
	float latencyHistory[AP_HISTORY_SIZE];
	float bitrateHistory[AP_HISTORY_SIZE];
	float audioQueueHistory[AP_HISTORY_SIZE];
	int historyIndex;
	float sourceFps;
	float displayFps;
	float frameTimeMs;
	float latencyMs;
	float bitrateMbps;
	float targetFps;
	int videoWidth;
	int videoHeight;
	int audioUnderruns;
	int audioDropped;
	int audioQueueSize;
	float connectionSeconds;
	float zoom;
	int rotation;
	int cleanFeedReady;
	int serverState;        // AP_SERVER_*
	float senderVolume;
	float inputLevel;
	int audioStreamRate;    // Sender sample rate, 0 without audio
	int audioDeviceRate;    // Output device sample rate, 0 without audio
	char renderer[32];      // SDL render driver, e.g. "direct3d11"
} ApStats;
#pragma pack(pop)

AP_API int __cdecl ap_debug_start(const char* commandLine, char* pathOut, int pathOutSize);
AP_API int __cdecl ap_check_bonjour(unsigned long* error);
AP_API int __cdecl ap_is_receiver_name_valid(const char* name);
AP_API void __cdecl ap_get_monitor_size(HWND window, int* width, int* height);

AP_API int __cdecl ap_start(HWND topLevel, const ApCallbacks* callbacks, const ApConfig* config);
AP_API HWND __cdecl ap_get_view(void);
AP_API void __cdecl ap_set_config(const ApConfig* config);
AP_API int __cdecl ap_rotate(void);
AP_API void __cdecl ap_reset_view(void);
AP_API void __cdecl ap_restart_receiver(void);
AP_API void __cdecl ap_set_capture_privacy(int enabled);
AP_API void __cdecl ap_get_stats(ApStats* stats);
AP_API void __cdecl ap_pin_answer(int generation, int allow);
AP_API void __cdecl ap_pin_dismiss(int generation);
AP_API void __cdecl ap_stop(void);
