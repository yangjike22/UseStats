#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "applog.h"

namespace {

constexpr size_t kLogMaxBytes = 200 * 1024; // stats_debug.log 上限，超过截断重写
std::wstring g_logDir;                      // 空 = 尚未 AppLogInit，静默跳过

} // namespace

void AppLogInit(const std::wstring& exeDir) { g_logDir = exeDir; }

void UseStatsDebugLog(const std::string& message) {
    if (g_logDir.empty()) return;
    std::wstring path = g_logDir + L"\\stats_debug.log";
    bool truncate = false;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        uint64_t sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        truncate = sz > kLogMaxBytes;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!truncate) SetFilePointer(h, 0, nullptr, FILE_END);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[64];
    std::snprintf(head, sizeof(head), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    std::string line = head + message + "\r\n";
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
}
