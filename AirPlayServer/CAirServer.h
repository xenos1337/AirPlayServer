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
	bool isRunning() const { return m_pServer != NULL; }
	float setVideoScale(float fRatio);

private:
	CAirServerCallback* m_pCallback;
	void* m_pServer;
};

