#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

#include "settings_io.h"

std::wstring SettingsFilePath(const std::wstring& dir) {
    return dir + L"\\settings.json";
}

UseStatsSettings LoadSettingsFile(const std::wstring& dir) {
    HANDLE h = CreateFileW(SettingsFilePath(dir).c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return UseStatsSettings{};
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return UseStatsSettings{};
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    bool ok = bytes.empty() ||
              (ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
               read == bytes.size());
    CloseHandle(h);
    if (!ok) return UseStatsSettings{};
    return ParseSettings(bytes);
}

bool SaveSettingsFile(const std::wstring& dir, const UseStatsSettings& s) {
    std::string j = SerializeSettings(s);
    std::wstring finalPath = SettingsFilePath(dir);
    std::wstring tmpPath = finalPath + L".tmp";
    HANDLE h = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, j.data(), static_cast<DWORD>(j.size()), &written, nullptr) &&
              written == j.size();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    if (!MoveFileExW(tmpPath.c_str(), finalPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

uint64_t SettingsFileStamp(const std::wstring& dir) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(SettingsFilePath(dir).c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
           fad.ftLastWriteTime.dwLowDateTime;
}
