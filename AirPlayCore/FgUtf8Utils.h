#pragma once
#include <string>

class CFgUtf8Utils
{
public:
	CFgUtf8Utils();
	~CFgUtf8Utils();

	static std::wstring UTF8_To_UTF16(const char* utf8Text);
	// AirPlay receiver names must be valid UTF-8 without Chinese characters.
	// An empty name is valid and means "use the host name".
	static bool IsReceiverNameValid(const char* name);
};

