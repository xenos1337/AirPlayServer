#pragma once
#include <cstddef>

class CSDLPlayer;
class CAirServerCallback;

class CAirServer
{
public:
	CAirServer();
	~CAirServer();

public:
	void start(CSDLPlayer* pPlayer, const char* serverName = NULL,
		const char* password = NULL, unsigned int displayWidth = 1920,
		unsigned int displayHeight = 1080);
	void stop();
	void restart(const char* serverName, const char* password,
		unsigned int displayWidth, unsigned int displayHeight);
	bool isRunning() const { return m_pServer != NULL; }
	float setVideoScale(float fRatio);

private:
	CAirServerCallback* m_pCallback;
	CSDLPlayer* m_pPlayer;
	void* m_pServer;
};

