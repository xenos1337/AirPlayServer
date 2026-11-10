#include "FgUtf8Utils.h"
#include <Windows.h>

CFgUtf8Utils::CFgUtf8Utils()
{
}


CFgUtf8Utils::~CFgUtf8Utils()
{
}

std::wstring CFgUtf8Utils::UTF8_To_UTF16(const char* utf8Text)
{  
	unsigned long len = ::MultiByteToWideChar(CP_UTF8, NULL, utf8Text, -1, NULL, NULL);
	if (len == 0)  
		return std::wstring(L"");  
	wchar_t *buffer = new wchar_t[len];  
	::MultiByteToWideChar(CP_UTF8, NULL, utf8Text, -1, buffer, len);

	std::wstring dest(buffer);
	delete[] buffer;  

	return dest;  
}

static bool IsChineseCodePoint(unsigned int codePoint)
{
	return (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
		(codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
		(codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
		(codePoint >= 0x20000 && codePoint <= 0x2FA1F) ||
		(codePoint >= 0x30000 && codePoint <= 0x323AF);
}

bool CFgUtf8Utils::IsReceiverNameValid(const char* name)
{
	if (name == NULL || name[0] == '\0') {
		return true;
	}

	wchar_t wideName[256];
	int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		name, -1, wideName, 256);
	if (length <= 0) {
		return false;
	}

	for (int i = 0; i < length - 1; ++i) {
		unsigned int codePoint = wideName[i];
		if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
			if (i + 1 >= length - 1 ||
				wideName[i + 1] < 0xDC00 || wideName[i + 1] > 0xDFFF) {
				return false;
			}
			codePoint = 0x10000 + ((codePoint - 0xD800) << 10) +
				(wideName[++i] - 0xDC00);
		} else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
			return false;
		}
		if (IsChineseCodePoint(codePoint)) {
			return false;
		}
	}
	return true;
}
