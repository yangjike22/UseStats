// 记录程序 UseStatsRecorder.exe：从单 exe 的 UseStats 拆出的采集职责。
// 托盘常驻、每秒前台采样、每 10 秒后台枚举、每 60 秒落盘并热加载
// settings.json；统计界面由同目录的查看器 UseStats.exe 负责，本程序
// 不碰 WebView2。**全系统唯一的托盘图标归本程序所有**：单击图标（或
// 菜单/第二实例）优先给已有查看器窗口发注册消息 UseStats.ViewerShow
// 把它唤出（包括从红 X 收起的隐藏状态），找不到窗口时才 ShellExecute
// 启动查看器；查看器的真退出也收进本程序的右键菜单「关闭查看器」
//（发注册消息 UseStats.ViewerQuit），查看器自己不再注册任何图标。
// 另带 WiFi 辅助（wifiassist.cpp）：每 15 秒驱动一次状态机，未连 WLAN
// 时刷新可用网络列表并自动连接已保存网络，可在设置页关闭。
//
// 数据（usage-YYYY-MM-DD.json）与旧版同目录同格式；采集互斥量沿用
// Local\UseStats_SingleInstance，保证只有一个采集者在写。
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "applog.h"
#include "appinfo.h"
#include "autostart.h"
#include "resource.h"
#include "settings_io.h"
#include "storage.h"
#include "tracker.h"
#include "traytip.h"
#include "wifiassist.h"

namespace {

constexpr UINT kTrayCallbackMsg = WM_APP + 1;
constexpr UINT kOpenViewerMsg = WM_APP + 2; // 冷启动自投递与第二实例通知共用
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kCmdOpenDataDir = 1001;
constexpr UINT kCmdExit = 1002;
constexpr UINT kCmdViewer = 1003;
constexpr UINT kCmdAutoStart = 1004;
constexpr UINT kCmdCloseViewer = 1005;
constexpr wchar_t kMutexName[] = L"Local\\UseStats_SingleInstance";
constexpr wchar_t kClassName[] = L"UseStatsHiddenWindow";
constexpr wchar_t kViewerClassName[] = L"UseStatsStatsWindow"; // 与 stats.cpp 保持一致

// 与查看器约定的两条注册消息（查看器 stats.cpp 里同名注册，ID 必然一致）：
// ViewerShow = 显示/唤出统计窗口；ViewerQuit = 真正结束查看器进程。
UINT ViewerShowMsg() {
    static UINT msg = RegisterWindowMessageW(L"UseStats.ViewerShow");
    return msg;
}
UINT ViewerQuitMsg() {
    static UINT msg = RegisterWindowMessageW(L"UseStats.ViewerQuit");
    return msg;
}

Tracker g_tracker;
WifiAssist g_wifi; // WiFi 辅助状态机（每 15 秒驱动一次，见 OnEverySecond）
Storage* g_storage = nullptr;
std::wstring g_today;  // 正在累积的日期（YYYY-MM-DD）
int g_tickCount = 0;   // 自启动起的秒计数
HWND g_hwnd = nullptr;
HINSTANCE g_hInst = nullptr;
NOTIFYICONDATAW g_nid{};

// settings.json 热加载状态：启动时读一次，之后每 60 秒比对修改时间戳
UseStatsSettings g_settings;
uint64_t g_settingsStamp = 0;

// explorer 重启后任务栏会广播这条消息，收到时要重新注册托盘图标
UINT TaskbarCreatedMsg() {
    static UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

std::wstring ExeDir() {
    wchar_t buf[1024];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring full(buf, n);
    size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : full.substr(0, pos);
}

void SaveNow() {
    if (g_storage != nullptr) g_storage->Save(g_tracker, g_today);
}

void OpenDataDir() {
    if (g_storage == nullptr) return;
    ShellExecuteW(g_hwnd, L"open", g_storage->DataDir().c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
}

// 打开统计界面：已有查看器窗口时优先给它发 ViewerShow 注册消息唤出
// （查看器侧走自己的 Open 逻辑：从红 X 收起的隐藏状态 SW_SHOW 唤出、
// 恢复最小化、按最新数据重新导航）；找不到窗口才 ShellExecute 启动
// 同目录的查看器 UseStats.exe（它自己也有单实例逻辑兜底）。
void OpenViewer() {
    if (g_storage == nullptr) return;
    std::wstring viewer = g_storage->ExeDir() + L"\\UseStats.exe";
    HINSTANCE r = ShellExecuteW(g_hwnd, L"open", viewer.c_str(), nullptr,
                                g_storage->ExeDir().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "viewer: ShellExecute FAILED code=%lld",
                      static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
        UseStatsDebugLog(buf);
    }
}

HWND FindViewerWindow() { return FindWindowW(kViewerClassName, nullptr); }

void ShowViewer() {
    // 单击去抖（第一道防线）：600ms 内重复调用直接忽略。用户旧习惯双击
    // 会在 2ms 内连发两次 LBUTTONUP，两次唤出曾让查看器背靠背重导航、
    // 互相取消（webError=9）并误触兜底锁死；查看器侧另有导航代际防线
    // 兜底，这里先把无意义的连发挡在门外。
    static ULONGLONG s_lastShowTick = 0;
    ULONGLONG nowTick = GetTickCount64();
    if (s_lastShowTick != 0 && nowTick - s_lastShowTick < 600) {
        UseStatsDebugLog("tray: click debounced");
        return;
    }
    s_lastShowTick = nowTick;
    HWND vw = FindViewerWindow();
    if (vw != nullptr) {
        PostMessageW(vw, ViewerShowMsg(), 0, 0);
        UseStatsDebugLog("tray: ViewerShow posted to existing viewer window");
        return;
    }
    UseStatsDebugLog("tray: no viewer window; launching UseStats.exe");
    OpenViewer();
}

// 「关闭查看器」：给查看器发 ViewerQuit 注册消息，它收到后真正结束
// 进程（绕过自己 WM_CLOSE 的收起逻辑）。查看器没在运行时菜单项本身
// 是灰的，这里再兜底判一次。
void CloseViewer() {
    HWND vw = FindViewerWindow();
    if (vw == nullptr) {
        UseStatsDebugLog("tray: ViewerQuit skipped (viewer not running)");
        return;
    }
    PostMessageW(vw, ViewerQuitMsg(), 0, 0);
    UseStatsDebugLog("tray: ViewerQuit sent to viewer");
}

// ---------- 数据保留清理 ----------

// 「YYYY-MM-DD」与今天相差的天数（dateStr 晚于今天时为负）；解析失败返回 -1
long DaysBeforeToday(const std::wstring& dateStr, const std::wstring& todayStr) {
    auto toFt = [](const std::wstring& s, FILETIME& ft) {
        if (s.size() != 10) return false;
        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(std::stoi(s.substr(0, 4)));
        st.wMonth = static_cast<WORD>(std::stoi(s.substr(5, 2)));
        st.wDay = static_cast<WORD>(std::stoi(s.substr(8, 2)));
        return SystemTimeToFileTime(&st, &ft) != FALSE;
    };
    FILETIME a, b;
    if (!toFt(dateStr, a) || !toFt(todayStr, b)) return -1;
    auto toU64 = [](const FILETIME& ft) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    int64_t diff = static_cast<int64_t>(toU64(b)) - static_cast<int64_t>(toU64(a));
    return static_cast<long>(diff / (86400LL * 10000000LL));
}

// retentionDays>0 时删掉比保留期更旧的 usage-*.json；0=永久不清理。
void CleanupOldData() {
    uint32_t keep = g_settings.retentionDays;
    if (keep == 0 || g_storage == nullptr) return;
    std::wstring pattern = g_storage->DataDir() + L"\\usage-*.json";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::wstring today = TodayString();
    do {
        std::wstring name(fd.cFileName);
        if (name.size() != 21) continue;
        std::wstring date = name.substr(6, 10);
        long age = DaysBeforeToday(date, today);
        if (age > static_cast<long>(keep)) {
            std::wstring full = g_storage->DataDir() + L"\\" + name;
            if (DeleteFileW(full.c_str())) {
                UseStatsDebugLog("retention: deleted " +
                                 std::string(name.begin(), name.end()));
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------- settings 热加载 ----------

void ApplySettings(const UseStatsSettings& s) {
    g_settings = s;
    g_tracker.SetIdleThresholdMs(s.idleSeconds * 1000);
    g_tracker.SetMediaNoIdle(s.mediaNoIdle);
    g_tracker.SetFocusGrace(s.focusGrace);
}

// settings.json 变了就重读并生效（idle 阈值、后台开关读的是 g_settings 本身、
// 保留天数变化立即清理一次）；文件被删/损坏时 LoadSettingsFile 给默认值。
void CheckSettingsHotReload() {
    if (g_storage == nullptr) return;
    uint64_t stamp = SettingsFileStamp(g_storage->DataDir());
    if (stamp == g_settingsStamp) return;
    g_settingsStamp = stamp;
    UseStatsSettings s = LoadSettingsFile(g_storage->DataDir());
    bool retentionChanged = s.retentionDays != g_settings.retentionDays;
    ApplySettings(s);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "settings: hot-loaded idle=%us bg=%d retention=%u theme=%s wifi=%d/%d media=%d/%d",
                  s.idleSeconds, s.backgroundStats ? 1 : 0, s.retentionDays,
                  s.theme.c_str(), s.wifiAssist ? 1 : 0, s.wifiAutoConnect ? 1 : 0,
                  s.mediaNoIdle ? 1 : 0, s.focusGrace ? 1 : 0);
    UseStatsDebugLog(buf);
    if (retentionChanged) CleanupOldData();
}

// 悬停文案：今日前台总时长 + 前台时长 Top 3（友好名），格式见 traytip.h。
// 数据来自 Tracker 的当日内存聚合（不必等落盘），每 60 秒刷新一次。
std::wstring BuildTrayTipText() {
    std::vector<std::pair<std::wstring, uint64_t>> apps;
    uint64_t total = 0;
    for (const auto& kv : g_tracker.Apps()) {
        total += kv.second.fgSeconds;
        if (kv.second.fgSeconds > 0)
            apps.emplace_back(AppInfo::FriendlyName(kv.second.path), kv.second.fgSeconds);
    }
    std::sort(apps.begin(), apps.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return TrayTip::Format(total, apps);
}

void UpdateTrayTip() {
    if (g_hwnd == nullptr) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    std::wstring tip = BuildTrayTipText();
    lstrcpynW(nid.szTip, tip.c_str(),
              static_cast<int>(sizeof(nid.szTip) / sizeof(nid.szTip[0])));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AddTrayIcon() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kTrayCallbackMsg;
    g_nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    g_nid.uVersion = 0; // NIM_ADD 前必须为 0，注册成功后再 NIM_SETVERSION
    std::wstring tip = BuildTrayTipText();
    lstrcpynW(g_nid.szTip, tip.c_str(),
              static_cast<int>(sizeof(g_nid.szTip) / sizeof(g_nid.szTip[0])));
    // 返回值必须检查并留痕（教训见 git 历史：曾因 WM_CREATE 期间 g_hwnd
    // 尚未赋值，NIM_ADD 拿 nullptr 静默失败、图标从未注册成功）。
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (!ok) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "tray: NIM_ADD FAILED gle=%lu",
                      static_cast<unsigned long>(GetLastError()));
        UseStatsDebugLog(buf);
        return;
    }
    UseStatsDebugLog("tray: NIM_ADD ok");
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    BOOL vok = Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    if (!vok) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "tray: NIM_SETVERSION FAILED gle=%lu",
                      static_cast<unsigned long>(GetLastError()));
        UseStatsDebugLog(buf);
    } else {
        UseStatsDebugLog("tray: NIM_SETVERSION ok (NOTIFYICON_VERSION_4)");
    }
}

void RemoveTrayIcon() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

void ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdViewer, L"打开统计界面");
    // 「关闭查看器」只有查看器真在运行时才可用（点红 X 收起也算在运行）
    AppendMenuW(menu, MF_STRING | (FindViewerWindow() != nullptr ? MF_ENABLED : MF_GRAYED),
                kCmdCloseViewer, L"关闭查看器");
    AppendMenuW(menu, MF_STRING, kCmdOpenDataDir, L"打开数据文件夹");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (AutoStartRunPresent() ? MF_CHECKED : MF_UNCHECKED),
                kCmdAutoStart, L"开机自启");
    // 只退记录程序本身；查看器窗口开着就继续开着，互不影响
    AppendMenuW(menu, MF_STRING, kCmdExit, L"退出记录程序");
    SetMenuDefaultItem(menu, kCmdViewer, FALSE); // 「打开统计界面」为默认项（加粗）
    SetForegroundWindow(g_hwnd); // 保证菜单点击外部时能正常消失
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
}

void OnEverySecond() {
    g_tracker.OnSecondTick();
    ++g_tickCount;
    // 后台统计开关关掉时停掉进程枚举（bgSeconds/cpuSeconds 不再增长）
    if (g_settings.backgroundStats && g_tickCount % 10 == 0)
        g_tracker.OnBackgroundTick();

    // WiFi 辅助：低频状态机，每 15 秒一拍（扫描保鲜/自动连接在模块内节流）
    if (g_tickCount % 15 == 0)
        g_wifi.Tick(g_settings.wifiAssist, g_settings.wifiAutoConnect,
                    GetTickCount64());

    // 跨天：先把旧日数据落盘，再清空重新累积，并按保留期清理一次
    std::wstring today = TodayString();
    if (today != g_today) {
        SaveNow();
        g_tracker.ResetDay();
        g_today = today;
        CleanupOldData();
    }
    if (g_tickCount % 60 == 0) {
        SaveNow();
        UpdateTrayTip(); // 悬停摘要跟随落盘节奏刷新
        CheckSettingsHotReload();
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // explorer 重启（或任务栏重建）后图标会被系统清掉，重新注册一次
    if (msg == TaskbarCreatedMsg()) {
        UseStatsDebugLog("tray: TaskbarCreated received; re-adding icon");
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            // 关键时序：WM_CREATE 在 CreateWindowExW 返回前发出，全局
            // g_hwnd 此时还没被 wWinMain 赋值。先把 g_hwnd 设为 hwnd，
            // AddTrayIcon 里 g_nid.hWnd 才不是 nullptr（旧版正因此
            // NIM_ADD 静默失败、图标从未出现）。
            g_hwnd = hwnd;
            SetTimer(hwnd, kTimerId, 1000, nullptr);
            AddTrayIcon();
            return 0;
        case WM_TIMER:
            if (wParam == kTimerId) OnEverySecond();
            return 0;
        case kTrayCallbackMsg: {
            // VERSION_4 下 lParam 是打包值：事件码在 LOWORD、图标 ID 在
            // HIWORD，绝不能拿整值与 WM_* 比较（曾因此左右键全灭）。
            // 右键发 WM_CONTEXTMENU（旧版发 WM_RBUTTONUP），两个都认；
            // 左键**单击**（WM_LBUTTONUP）直接打开/唤出查看器窗口，
            // 双击的第一下就是 LBUTTONUP 会先触发，DBLCLK/NIN_SELECT 兜底
            const UINT trayEvent = LOWORD(lParam);
            if (trayEvent == WM_RBUTTONUP || trayEvent == WM_CONTEXTMENU) {
                UseStatsDebugLog("tray: menu requested");
                ShowTrayMenu();
            } else if (trayEvent == WM_LBUTTONUP || trayEvent == WM_LBUTTONDBLCLK ||
                       trayEvent == NIN_SELECT) {
                UseStatsDebugLog("tray: click -> show viewer");
                ShowViewer();
            }
            return 0;
        }
        case kOpenViewerMsg:
            ShowViewer();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kCmdOpenDataDir) OpenDataDir();
            else if (LOWORD(wParam) == kCmdViewer) ShowViewer();
            else if (LOWORD(wParam) == kCmdCloseViewer) CloseViewer();
            else if (LOWORD(wParam) == kCmdAutoStart)
                // Run 目标只能经 autostart 的唯一入口决定（同目录记录器）
                AutoStartSet(AutoStartRecorderPath(g_storage->ExeDir()),
                             !AutoStartRunPresent());
            else if (LOWORD(wParam) == kCmdExit) DestroyWindow(hwnd);
            return 0;
        case WM_QUERYENDSESSION:
            SaveNow(); // 关机/注销前落盘
            return TRUE;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            RemoveTrayIcon();
            SaveNow();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    g_hInst = hInstance;

    // 单实例（采集互斥量沿用旧名）：已有记录器在运行时，通知它打开
    // 统计界面，然后本实例退出——保证只有一个采集者在写数据。
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex == nullptr) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 30; ++i) {
            HWND existing = FindWindowW(kClassName, nullptr);
            if (existing != nullptr) {
                PostMessageW(existing, kOpenViewerMsg, 0, 0);
                break;
            }
            Sleep(100);
        }
        CloseHandle(mutex);
        return 0;
    }

    Storage storage(ExeDir());
    g_storage = &storage;
    AppLogInit(storage.ExeDir());
    UseStatsDebugLog("recorder: start");
    UseStatsDebugLog(storage.DataDir() != storage.ExeDir()
                         ? "storage: data dir = record subfolder"
                         : "storage: data dir = exe dir");

    // 设置：启动读一次（之后每 60 秒热加载）
    ApplySettings(LoadSettingsFile(storage.DataDir()));
    g_settingsStamp = SettingsFileStamp(storage.DataDir());

    g_today = TodayString();
    storage.Load(g_tracker, g_today); // 同日重启则续记已有数据（无文件则忽略）
    AutoStartInit(AutoStartRecorderPath(ExeDir())); // 自启默认开/校准 Run 到记录器路径（目标经唯一入口）
    CleanupOldData();          // 按保留期清理一次

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // 隐藏窗口：只承载托盘消息与定时器，从不显示
    g_hwnd = CreateWindowExW(0, kClassName, L"UseStatsRecorder", WS_OVERLAPPEDWINDOW,
                             0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (g_hwnd == nullptr) {
        SaveNow();
        CloseHandle(mutex);
        return 1;
    }

    // 冷启动自动打开一次统计界面（与单击托盘走同一条 ShowViewer 路径）
    PostMessageW(g_hwnd, kOpenViewerMsg, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(mutex);
    return 0;
}
