#include "CAirServerCallback.h"
#include <stdio.h>
#include "FgUtf8Utils.h"
#include <locale.h>
#include "DebugLogger.h"

#define min(a,b)            (((a) < (b)) ? (a) : (b))

CAirServerCallback::CAirServerCallback()
	: m_pPlayer(NULL)
{
	memset(m_chRemoteDeviceId, 0, 128);
}

CAirServerCallback::~CAirServerCallback()
{
}

void CAirServerCallback::setPlayer(CSDLPlayer* pPlayer)
{
	m_pPlayer = pPlayer;
}

void CAirServerCallback::connected(const char* remoteName, const char* remoteDeviceId) {
	DebugLogger::Write("connection", "connected name=%s device=%s", remoteName ? remoteName : "(null)", remoteDeviceId ? remoteDeviceId : "(null)");
	if (remoteDeviceId != NULL) {
		strncpy(m_chRemoteDeviceId, remoteDeviceId, 128);
	}

	setlocale(LC_CTYPE, "");
	std::wstring name = CFgUtf8Utils::UTF8_To_UTF16(remoteName);
	wprintf(L"Client connected: %s\n", name.c_str());

	// Update connection state and show the window when a client connects
	if (m_pPlayer) {
		// Convert wide string to UTF-8 for ImGui
		char deviceNameUtf8[256] = { 0 };
		if (remoteName) {
			// Simple conversion - in production, use proper UTF-8 conversion
			WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, deviceNameUtf8, sizeof(deviceNameUtf8), NULL, NULL);
		}
		m_pPlayer->setConnected(true, deviceNameUtf8);
		m_pPlayer->requestShowWindow();
	}
}

void CAirServerCallback::disconnected(const char* remoteName, const char* remoteDeviceId) {
	DebugLogger::Write("connection", "disconnected name=%s device=%s", remoteName ? remoteName : "(null)", remoteDeviceId ? remoteDeviceId : "(null)");
	memset(m_chRemoteDeviceId, 0, 128);
	printf("Client disconnected\n");

	// Update connection state - keep window visible to show home screen
	if (m_pPlayer) {
		m_pPlayer->setConnected(false, NULL);
		// Don't hide window - show home screen instead
		// GPU render loop clears to black each frame, so no explicit clear needed
	}
}

void CAirServerCallback::outputAudio(SFgAudioFrame* data, const char* remoteName, const char* remoteDeviceId)
{
	if (m_pPlayer)
	{
		if (m_chRemoteDeviceId[0] == '\0' && remoteDeviceId != NULL)
		{
			strncpy(m_chRemoteDeviceId, remoteDeviceId, 128);
		}
		if (0 != strcmp(m_chRemoteDeviceId, remoteDeviceId))
		{
			return;
		}
		m_pPlayer->outputAudio(data);
	}
}

void CAirServerCallback::outputH264AccessUnit(SFgH264AccessUnit* data, const char* remoteName, const char* remoteDeviceId)
{
	(void)data;
	(void)remoteName;
	(void)remoteDeviceId;
}

void CAirServerCallback::outputVideo(SFgVideoFrame* data, const char* remoteName, const char* remoteDeviceId)
{
	if (m_pPlayer)
	{
		if (m_chRemoteDeviceId[0] == '\0' && remoteDeviceId != NULL)
		{
			strncpy(m_chRemoteDeviceId, remoteDeviceId, 128);
		}
		if (0 != strcmp(m_chRemoteDeviceId, remoteDeviceId))
		{
			return;
		}
		m_pPlayer->outputVideo(data);
	}
}

void CAirServerCallback::videoGeometryChanged(float sourceWidth, float sourceHeight,
	float outputWidth, float outputHeight,
	const char* remoteName, const char* remoteDeviceId)
{
	UNREFERENCED_PARAMETER(sourceWidth);
	UNREFERENCED_PARAMETER(sourceHeight);
	UNREFERENCED_PARAMETER(outputWidth);
	UNREFERENCED_PARAMETER(outputHeight);
	UNREFERENCED_PARAMETER(remoteName);
	UNREFERENCED_PARAMETER(remoteDeviceId);
}

void CAirServerCallback::videoSenderPausedChanged(bool paused,
	const char* remoteName, const char* remoteDeviceId)
{
	UNREFERENCED_PARAMETER(paused);
	UNREFERENCED_PARAMETER(remoteName);
	UNREFERENCED_PARAMETER(remoteDeviceId);
}

void CAirServerCallback::videoPlay(char* url, double volume, double startPos)
{
	DebugLogger::Write("video", "play url=%s volume=%.3f start=%.3f", url ? url : "(null)", volume, startPos);
	printf("Play: %s", url);
}

void CAirServerCallback::setVolume(float volume, const char* remoteName, const char* remoteDeviceId)
{
	DebugLogger::Write("audio", "volume=%.3f name=%s device=%s", volume, remoteName ? remoteName : "(null)", remoteDeviceId ? remoteDeviceId : "(null)");
	if (m_pPlayer)
	{
		m_pPlayer->setVolume(volume);
	}
}

bool CAirServerCallback::requestPinApproval(const char* remoteAddress, const char* pin)
{
	return m_pPlayer != NULL && m_pPlayer->requestPinApproval(remoteAddress, pin);
}

bool CAirServerCallback::approvePairingRequest(
	const char* remoteName,
	const char* remoteDeviceId,
	const char* remoteModel,
	const char* remoteOsName,
	const char* remoteOsVersion,
	const char* remoteOsBuildVersion,
	const char* remoteSourceVersion,
	const char* pairingFingerprint)
{
	UNREFERENCED_PARAMETER(remoteDeviceId);
	UNREFERENCED_PARAMETER(remoteModel);
	UNREFERENCED_PARAMETER(remoteOsName);
	UNREFERENCED_PARAMETER(remoteOsVersion);
	UNREFERENCED_PARAMETER(remoteOsBuildVersion);
	UNREFERENCED_PARAMETER(remoteSourceVersion);
	UNREFERENCED_PARAMETER(pairingFingerprint);

	printf("Pairing request received from %s\n", remoteName != NULL ? remoteName : "Unknown device");
	return true;
}

double dbDuration = 10000;
double dbPosition = 0;
void CAirServerCallback::videoGetPlayInfo(double* duration, double* position, double* rate)
{
	*duration = 1000;
	*position = dbPosition;
	*rate = 1.0;
	dbPosition += 1;
}

void CAirServerCallback::log(int level, const char* msg)
{
	DebugLogger::Write("airplay", "level=%d %s", level, msg ? msg : "(null)");
#ifdef _DEBUG
	OutputDebugStringA(msg);
	OutputDebugStringA("\n");
#else
	printf("%s\n", msg);
#endif
}
