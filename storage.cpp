#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "appinfo.h"
#include "storage.h"
#include "tracker.h"

namespace {

std::wstring Pad2(int v) {
    std::wstring s = std::to_wstring(v);
    return (s.size() == 1) ? (L"0" + s) : s;
}

std::string ToUtf8(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

// 先转 UTF-8 再转义：UTF-8 续字节不会与 '"'、'\\' 冲突，按字节处理安全
void AppendJsonString(std::string& out, const std::wstring& ws) {
    std::string u8 = ToUtf8(ws);
    out += '"';
    for (char c : u8) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (uc < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", uc);
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
}

std::string FormatDouble(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

// ---------- 极简 JSON 读取：只解析本程序自己写出的格式 ----------

void SkipWs(const std::wstring& s, size_t& pos) {
    while (pos < s.size() && (s[pos] == L' ' || s[pos] == L'\t' ||
                              s[pos] == L'\n' || s[pos] == L'\r'))
        ++pos;
}

// 找 "key": 并把 pos 定位到值开头，失败返回 false
bool FindValuePos(const std::wstring& s, size_t from, const wchar_t* key, size_t& pos) {
    std::wstring needle = L"\"";
    needle += key;
    needle += L"\"";
    size_t k = s.find(needle, from);
    if (k == std::wstring::npos) return false;
    size_t colon = s.find(L':', k + needle.size());
    if (colon == std::wstring::npos) return false;
    pos = colon + 1;
    SkipWs(s, pos);
    return true;
}

bool ParseJsonString(const std::wstring& s, size_t& pos, std::wstring& out) {
    SkipWs(s, pos);
    if (pos >= s.size() || s[pos] != L'"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        wchar_t c = s[pos++];
        if (c == L'"') return true;
        if (c == L'\\' && pos < s.size()) {
            wchar_t e = s[pos++];
            switch (e) {
                case L'n': out += L'\n'; break;
                case L't': out += L'\t'; break;
                case L'r': out += L'\r'; break;
                case L'u': {
                    if (pos + 4 > s.size()) return false;
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        wchar_t h = s[pos++];
                        cp <<= 4;
                        if (h >= L'0' && h <= L'9') cp |= static_cast<unsigned>(h - L'0');
                        else if (h >= L'a' && h <= L'f') cp |= static_cast<unsigned>(h - L'a' + 10);
                        else if (h >= L'A' && h <= L'F') cp |= static_cast<unsigned>(h - L'A' + 10);
                    }
                    out += static_cast<wchar_t>(cp);
                    break;
                }
                default: out += e; break; // \\、\"、\/ 等
            }
        } else {
            out += c;
        }
    }
    return false;
}

uint64_t ParseUInt(const std::wstring& s, size_t& pos) {
    SkipWs(s, pos);
    uint64_t v = 0;
    while (pos < s.size() && s[pos] >= L'0' && s[pos] <= L'9') {
        v = v * 10 + static_cast<uint64_t>(s[pos] - L'0');
        ++pos;
    }
    return v;
}

double ParseDouble(const std::wstring& s, size_t& pos) {
    SkipWs(s, pos);
    size_t start = pos;
    while (pos < s.size() &&
           ((s[pos] >= L'0' && s[pos] <= L'9') || s[pos] == L'.' ||
            s[pos] == L'-' || s[pos] == L'+' || s[pos] == L'e' || s[pos] == L'E'))
        ++pos;
    return wcstod(s.substr(start, pos - start).c_str(), nullptr);
}

} // namespace

std::wstring TodayString() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return std::to_wstring(st.wYear) + L"-" + Pad2(st.wMonth) + L"-" + Pad2(st.wDay);
}

std::wstring ResolveDataDir(const std::wstring& exeDir) {
    const std::wstring sub = exeDir + L"\\record";
    const DWORD attr = GetFileAttributesW(sub.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return sub;
    return exeDir;
}

std::wstring Storage::FilePathFor(const std::wstring& dateStr) const {
    return dataDir_ + L"\\usage-" + dateStr + L".json";
}

bool Storage::Save(const Tracker& tracker, const std::wstring& dateStr) const {
    // 按前台秒降序排列，文件打开即可读
    std::vector<const AppUsage*> list;
    list.reserve(tracker.Apps().size());
    for (const auto& kv : tracker.Apps()) list.push_back(&kv.second);
    std::sort(list.begin(), list.end(), [](const AppUsage* a, const AppUsage* b) {
        return a->fgSeconds > b->fgSeconds;
    });

    std::string j;
    j += "{\n  \"date\": ";
    AppendJsonString(j, dateStr);
    j += ",\n  \"idleSeconds\": " + std::to_string(tracker.IdleSeconds());
    j += ",\n  \"apps\": [";
    bool first = true;
    for (const AppUsage* u : list) {
        if (!first) j += ",";
        first = false;
        j += "\n    {\"path\": ";
        AppendJsonString(j, u->path);
        j += ", \"name\": ";
        AppendJsonString(j, AppInfo::FriendlyName(u->path));
        j += ", \"fgSeconds\": " + std::to_string(u->fgSeconds);
        j += ", \"bgSeconds\": " + std::to_string(u->bgSeconds);
        j += ", \"cpuSeconds\": " + FormatDouble(u->cpuSeconds);
        j += ", \"hourlyFg\": [";
        for (int h = 0; h < 24; ++h) {
            if (h) j += ", ";
            j += std::to_string(u->hourlyFg[h]);
        }
        j += "]}";
    }
    j += "\n  ]\n}\n";

    // 原子写：先写同目录临时文件，再改名覆盖
    std::wstring finalPath = FilePathFor(dateStr);
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

bool Storage::Load(Tracker& tracker, const std::wstring& dateStr) const {
    HANDLE h = CreateFileW(FilePathFor(dateStr).c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    bool ok = bytes.empty() ||
              (ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
               read == bytes.size());
    CloseHandle(h);
    if (!ok) return false;

    std::wstring s = FromUtf8(bytes);
    size_t pos = 0;
    if (FindValuePos(s, 0, L"idleSeconds", pos))
        tracker.SetIdleSeconds(ParseUInt(s, pos));

    size_t cur = 0;
    size_t vpos = 0;
    while (FindValuePos(s, cur, L"path", vpos)) {
        std::wstring path;
        size_t p = vpos;
        if (!ParseJsonString(s, p, path) || path.empty()) break;
        AppUsage& u = tracker.GetOrCreate(path);
        size_t q = p;
        if (FindValuePos(s, q, L"name", q)) {
            std::wstring nm;
            if (ParseJsonString(s, q, nm)) u.name = nm;
        }
        q = p;
        if (FindValuePos(s, q, L"fgSeconds", q)) u.fgSeconds = ParseUInt(s, q);
        q = p;
        if (FindValuePos(s, q, L"bgSeconds", q)) u.bgSeconds = ParseUInt(s, q);
        q = p;
        if (FindValuePos(s, q, L"cpuSeconds", q)) u.cpuSeconds = ParseDouble(s, q);
        q = p;
        if (FindValuePos(s, q, L"hourlyFg", q)) {
            size_t bracket = s.find(L'[', q);
            if (bracket != std::wstring::npos) {
                size_t hp = bracket + 1;
                for (int i = 0; i < 24; ++i) {
                    while (hp < s.size() && (s[hp] == L',' || s[hp] == L' ')) ++hp;
                    u.hourlyFg[i] = ParseUInt(s, hp);
                }
            }
        }
        cur = p; // 从本对象后续位置继续找下一个 path
    }
    return true;
}
