#include "DebugLogger.h"
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <mutex>
#include <time.h>
#include <string.h>
#include <share.h>

namespace { std::mutex g_mutex; FILE* g_file = NULL; bool g_enabled = false; std::string g_path; }
namespace DebugLogger {
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* p) {
    Write("crash", "unhandled exception code=0x%08lX address=%p", p && p->ExceptionRecord ? p->ExceptionRecord->ExceptionCode : 0, p && p->ExceptionRecord ? p->ExceptionRecord->ExceptionAddress : NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}
bool Start(const char* commandLine) {
    if (!commandLine || (!strstr(commandLine, "--debug") && !strstr(commandLine, "/debug"))) return false;
    char dir[MAX_PATH] = {}; GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir));
    if (!dir[0]) GetTempPathA(sizeof(dir), dir);
    char folder[MAX_PATH] = {}; _snprintf_s(folder, sizeof(folder), _TRUNCATE, "%s\\AirPlayServer\\logs", dir);
    CreateDirectoryA((std::string(dir) + "\\AirPlayServer").c_str(), NULL); CreateDirectoryA(folder, NULL);
    SYSTEMTIME st; GetLocalTime(&st); char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\airplay-%04u%02u%02u-%02u%02u%02u.log", folder, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_file = _fsopen(path, "w", _SH_DENYNO);
    if (!g_file) return false; g_path = path; g_enabled = true;
    SetUnhandledExceptionFilter(CrashFilter);
    Write("startup", "debug logging enabled; command line: %s", commandLine); return true;
}
bool Enabled() { return g_enabled; }
const std::string& Path() { return g_path; }
void Write(const char* category, const char* format, ...) {
    if (!g_enabled || !g_file) return; std::lock_guard<std::mutex> lock(g_mutex); SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_file, "%04u-%02u-%02u %02u:%02u:%02u.%03u [tid=%lu] [%s] ", st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,st.wMilliseconds,GetCurrentThreadId(),category?category:"log");
    va_list ap; va_start(ap, format); vfprintf(g_file, format, ap); va_end(ap); fputc('\n', g_file); fflush(g_file);
}
void Stop() { if (g_file) { Write("shutdown", "server stopped"); fclose(g_file); g_file = NULL; } g_enabled = false; }
}
