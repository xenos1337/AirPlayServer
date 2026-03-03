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
	void start(CSDLPlayer* pPlayer, const char* serverName = NULL);
	void stop();
	void restart(const char* serverName);
	bool isRunning() const { return m_pServer != NULL; }
	float setVideoScale(float fRatio);

private:
	CAirServerCallback* m_pCallback;
	CSDLPlayer* m_pPlayer;
	void* m_pServer;
};

