#pragma once

#include <string>

namespace DebugLogger {
bool Start(const char* commandLine);
bool Enabled();
const std::string& Path();
void Write(const char* category, const char* format, ...);
void Stop();
}
