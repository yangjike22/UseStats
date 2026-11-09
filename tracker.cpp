#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <cwctype>

#include "applog.h"
#include "mediawatch.h"
#include "tracker.h"

namespace {

constexpr uint64_t kBgTickSeconds = 10;        // 后台采样间隔（秒）
constexpr uint32_t kFocusGraceMs = 15 * 60 * 1000; // 专注应用宽限阈值：15 分钟

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring BaseName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

bool EqualsIgnoreCase(const std::wstring& a, const wchar_t* b) {
    size_t n = wcslen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}

// 伪应用：锁屏/登录界面，不计入任何统计（前台命中时归入 idle，后台枚举时跳过）
bool IsSkippedExe(const std::wstring& fileName) {
    return EqualsIgnoreCase(fileName, L"LogonUI.exe") ||
           EqualsIgnoreCase(fileName, L"LockApp.exe");
}

// 取进程 exe 完整路径；失败返回空（常见于受保护的系统进程）
std::wstring ProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return std::wstring();
    wchar_t buf[1024];
    DWORD n = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

uint64_t FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

} // namespace

Tracker::Tracker() : selfPid_(GetCurrentProcessId()) {}

AppUsage& Tracker::GetOrCreate(const std::wstring& key) {
    auto it = apps_.find(key);
    if (it != apps_.end()) return it->second;
    AppUsage u;
    u.path = key;
    auto inserted = apps_.emplace(key, std::move(u));
    return inserted.first->second;
}

void Tracker::ResetDay() {
    apps_.clear();
    prevCpu_.clear();
    idleSeconds_ = 0;
    currentFgPath_.clear();
}

void Tracker::NoteIdleEpisode(const std::wstring* fgPath) {
    if (idleEpisode_) return;
    idleEpisode_ = true;
    // fg 的 exe 名能直接指出「是谁被判了空闲」，是真机排障的关键一行。
    UseStatsDebugLog(std::string("idle: begin fg=") +
                     (fgPath ? WideToUtf8(BaseName(*fgPath)) : std::string("(none)")));
}

void Tracker::OnSecondTick() {
    // 1) 前台窗口所属进程（空闲豁免要先知道前台是谁）
    HWND hwnd = GetForegroundWindow();
    if (hwnd == nullptr) { NoteIdleEpisode(nullptr); ++idleSeconds_; currentFgPath_.clear(); return; }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) { NoteIdleEpisode(nullptr); ++idleSeconds_; currentFgPath_.clear(); return; }

    std::wstring path = ProcessPath(pid);
    if (path.empty() || IsSkippedExe(BaseName(path))) {
        // 取不到路径或锁屏等伪应用：不计入应用，归入 idle
        NoteIdleEpisode(nullptr);
        ++idleSeconds_;
        currentFgPath_.clear();
        return;
    }

    // 2) 空闲判定：键鼠无操作时长达到「有效阈值」才判空闲。
    //    专注应用（Edge/哔哩哔哩）在前台时有效阈值放宽到 15 分钟（focusGrace）。
    DWORD inputIdleMs = 0;
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    if (GetLastInputInfo(&lii)) inputIdleMs = GetTickCount() - lii.dwTime;
    uint32_t effThresholdMs = idleThresholdMs_;
    if (focusGrace_ && MediaWatch::IsFocusExeName(BaseName(path)) &&
        effThresholdMs < kFocusGraceMs)
        effThresholdMs = kFocusGraceMs;
    if (inputIdleMs >= effThresholdMs) {
        // 3) 媒体豁免：前台应用正在出声播放、或系统全屏/演示中——人在看，
        //    这一秒照常计入前台（mediaNoIdle，见 mediawatch.h）。
        const char* reason = "";
        if (mediaNoIdle_ && MediaWatch::ForegroundMediaActive(pid, &reason)) {
            ULONGLONG now = GetTickCount64();
            if (now - lastMediaLogMs_ >= 60000) {
                lastMediaLogMs_ = now;
                UseStatsDebugLog(std::string("media: keep-active reason=") + reason +
                                 " fg=" + WideToUtf8(BaseName(path)));
            }
        } else {
            NoteIdleEpisode(&path);
            ++idleSeconds_;
            currentFgPath_.clear();
            return;
        }
    }

    idleEpisode_ = false;
    currentFgPath_ = path;

    // 4) 计入该应用：前台秒 + 当前小时格
    //    说明：explorer.exe 按正常应用计数（资源管理器是真实使用；
    //    单击桌面空白处时前台同属 explorer，一并计入，见 README 口径）。
    AppUsage& u = GetOrCreate(path);
    ++u.fgSeconds;
    SYSTEMTIME st;
    GetLocalTime(&st);
    ++u.hourlyFg[st.wHour];
}

void Tracker::OnBackgroundTick() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    std::unordered_map<uint32_t, CpuSample> current;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            DWORD pid = pe.th32ProcessID;
            if (pid == 0 || pid == 4) continue;      // Idle / System
            if (pid == selfPid_) continue;          // 不统计自己
            if (IsSkippedExe(pe.szExeFile)) continue; // 伪应用

            // 聚合键：优先完整路径，取不到时用 exe 文件名兜底
            std::wstring path = ProcessPath(pid);
            std::wstring key = path.empty() ? std::wstring(pe.szExeFile) : path;
            AppUsage& u = GetOrCreate(key);
            u.bgSeconds += kBgTickSeconds;

            // CPU 秒增量：与上次采样比较，创建时间不同视为 PID 复用、新进程
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (h != nullptr) {
                FILETIME ct, et, kt, ut;
                if (GetProcessTimes(h, &ct, &et, &kt, &ut)) {
                    uint64_t cpu = FileTimeToU64(kt) + FileTimeToU64(ut);
                    uint64_t created = FileTimeToU64(ct);
                    auto it = prevCpu_.find(pid);
                    if (it != prevCpu_.end() && it->second.createTime == created &&
                        cpu >= it->second.cpuTime) {
                        u.cpuSeconds += static_cast<double>(cpu - it->second.cpuTime) / 1e7;
                    }
                    current[pid] = CpuSample{created, cpu};
                }
                CloseHandle(h);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    prevCpu_.swap(current);
}
