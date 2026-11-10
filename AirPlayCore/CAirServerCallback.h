#pragma once
#include <Windows.h>
#include "CSDLPlayer.h"


class CAirServerCallback : public IAirServerCallback
{
public:
	CAirServerCallback();
	virtual ~CAirServerCallback();

public:
	void setPlayer(CSDLPlayer* pPlayer);

public:
	virtual void connected(const char* remoteName, const char* remoteDeviceId);
	virtual void disconnected(const char* remoteName, const char* remoteDeviceId);
	virtual void outputAudio(SFgAudioFrame* data, const char* remoteName, const char* remoteDeviceId);
	virtual void outputH264AccessUnit(SFgH264AccessUnit* data, const char* remoteName, const char* remoteDeviceId);
	virtual void outputVideo(SFgVideoFrame* data, const char* remoteName, const char* remoteDeviceId);
	virtual void videoGeometryChanged(float sourceWidth, float sourceHeight,
		float outputWidth, float outputHeight,
		const char* remoteName, const char* remoteDeviceId);
	virtual void videoSenderPausedChanged(bool paused,
		const char* remoteName, const char* remoteDeviceId);

	virtual void videoPlay(char* url, double volume, double startPos);
	virtual void videoGetPlayInfo(double* duration, double* position, double* rate);

	// Audio volume control (volume in dB: 0.0 = max, -144.0 = mute)
	virtual void setVolume(float volume, const char* remoteName, const char* remoteDeviceId);
	virtual bool requestPinApproval(const char* remoteAddress, const char* pin);
	virtual bool approvePairingRequest(
		const char* remoteName,
		const char* remoteDeviceId,
		const char* remoteModel,
		const char* remoteOsName,
		const char* remoteOsVersion,
		const char* remoteOsBuildVersion,
		const char* remoteSourceVersion,
		const char* pairingFingerprint);

	virtual void log(int level, const char* msg);

protected:
	CSDLPlayer* m_pPlayer;
	char m_chRemoteDeviceId[128];
};
