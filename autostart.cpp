#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>

#include "applog.h"
#include "autostart.h"

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"UseStats";
constexpr wchar_t kPrefKey[] = L"Software\\UseStats";
constexpr wchar_t kPrefAutoStartName[] = L"AutoStart";

void LogRegResult(const char* what, LONG r) {
    char buf[160];
    if (r == ERROR_SUCCESS)
        std::snprintf(buf, sizeof(buf), "autostart: %s ok", what);
    else
        std::snprintf(buf, sizeof(buf), "autostart: %s FAILED gle=%ld", what, r);
    UseStatsDebugLog(buf);
}

// 宽字符串 → UTF-8（日志文件是 UTF-8；路径里的中文不能靠 %ls 的本地代码页）
std::string ToUtf8(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

// 把 Run 的实际指向写进日志（排障缺口修复：以前只记成败、不记路径，
// Run 指向查看器还是旧目录只能靠猜）。value 是注册表里的原始值（带引号）。
void LogRunTarget(const char* verb, const std::wstring& value) {
    UseStatsDebugLog(std::string("autostart: Run ") + verb + " target=\"" +
                     ToUtf8(value) + "\"");
}

bool ReadRunValue(std::wstring& out) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    wchar_t buf[1024];
    DWORD cb = sizeof(buf) - sizeof(wchar_t); // 留一格给手动补的结尾 \0
    LONG r = RegQueryValueExW(hKey, kRunValueName, nullptr, &type,
                              reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(hKey);
    if (r != ERROR_SUCCESS || type != REG_SZ) return false;
    buf[cb / sizeof(wchar_t)] = L'\0';
    out = buf;
    return true;
}

// 把 Run 值同步到 want 状态（写/删），成败都写日志
bool SyncRunValue(const std::wstring& exePath, bool want) {
    HKEY hKey = nullptr;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hKey);
    if (r != ERROR_SUCCESS) {
        LogRegResult("Run key open", r);
        return false;
    }
    if (want) {
        std::wstring val = L"\"" + exePath + L"\"";
        LogRunTarget("write", val); // 写之前就留痕：目标路径以此行为准
        r = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(val.c_str()),
                           static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        LogRegResult("Run write", r);
    } else {
        r = RegDeleteValueW(hKey, kRunValueName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS; // 本来就没有，视为已同步
        LogRegResult("Run delete", r);
    }
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS;
}

int ReadAutoStartPref() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPrefKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return -1;
    DWORD type = 0, val = 0, cb = sizeof(val);
    LONG r = RegQueryValueExW(hKey, kPrefAutoStartName, nullptr, &type,
                              reinterpret_cast<BYTE*>(&val), &cb);
    RegCloseKey(hKey);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return -1;
    return val == 0 ? 0 : 1;
}

bool WriteAutoStartPref(int pref) {
    HKEY hKey = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, kPrefKey, 0, nullptr, 0,
                             KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (r != ERROR_SUCCESS) {
        LogRegResult("pref key open", r);
        return false;
    }
    DWORD val = static_cast<DWORD>(pref);
    r = RegSetValueExW(hKey, kPrefAutoStartName, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
    RegCloseKey(hKey);
    LogRegResult("pref write", r);
    return r == ERROR_SUCCESS;
}

} // namespace

bool AutoStartRunPresent() {
    std::wstring v;
    return ReadRunValue(v);
}

bool AutoStartRunTargetIs(const std::wstring& exePath) {
    std::wstring cur;
    if (!ReadRunValue(cur)) return false;
    return cur == L"\"" + exePath + L"\"";
}

bool AutoStartReadRunValue(std::wstring& out) { return ReadRunValue(out); }

std::wstring AutoStartRecorderPath(const std::wstring& exeDir) {
    // 【全工程唯一决定 Run 目标路径的地方】永远指向同目录的记录程序。
    return exeDir + L"\\UseStatsRecorder.exe";
}

void AutoStartSet(const std::wstring& exePath, bool enable) {
    SyncRunValue(exePath, enable);
    WriteAutoStartPref(enable ? 1 : 0);
}

void AutoStartInit(const std::wstring& exePath) {
    int pref = ReadAutoStartPref();
    if (pref < 0) {
        AutoStartSet(exePath, true);
        UseStatsDebugLog("autostart: default-on applied");
    } else if (pref == 1) {
        std::wstring cur;
        std::wstring want = L"\"" + exePath + L"\"";
        if (!ReadRunValue(cur) || cur != want) {
            // 刷新前把旧指向落日志：指向查看器、旧目录还是根本不存在，
            // 下次排障看这一行就知道，不用再猜。
            if (!cur.empty()) LogRunTarget("stale", cur);
            UseStatsDebugLog("autostart: pref=1 but Run missing/stale; refreshing");
            SyncRunValue(exePath, true);
        } else {
            LogRunTarget("current", cur);
            UseStatsDebugLog("autostart: pref=1, Run ok");
        }
    } else {
        std::wstring cur;
        if (ReadRunValue(cur)) {
            LogRunTarget("stale", cur);
            UseStatsDebugLog("autostart: pref=0 but Run present; removing");
            SyncRunValue(exePath, false);
        } else {
            UseStatsDebugLog("autostart: pref=0, Run absent");
        }
    }
}
