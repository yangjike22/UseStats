// 查看器 UseStats.exe：双程序架构里的「主程序」。
// 纯查看器：不采集、不占用采集互斥量（Local\UseStats_SingleInstance
// 归记录器），只读同目录的 usage-*.json 渲染界面，并通过设置页
// 写 settings.json / 注册表自启。双击打开窗口；点窗口红 X 只是把
// 窗口隐藏收起（进程不退出、WebView2 保持存活）。托盘整合后查看器
// 不再注册自己的图标：全系统唯一的托盘图标归记录程序，单击它（或
// 重复双击本 exe）经注册消息 UseStats.ViewerShow 唤出本窗口，真正
// 结束进程只有记录器托盘菜单「关闭查看器」发来的 UseStats.ViewerQuit
//（消息处理在 stats.cpp 的 StatsWndProc）。
// 自身的单实例用另一个互斥量：重复双击只把已有窗口唤出
//（含窗口正隐藏收起的情况）。
//
// 启动自愈（2026-11-06）：开机自启若还指着旧路径或查看器本身，开机
// 后不会有人记录。查看器每次启动顺手做两件事——按共享 autostart 的
// 偏好规则把 Run 校准到同目录记录器路径；记录器没在运行就把它拉起。
// 这样用户打开任一程序一次，自启链就恢复正常。
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

#include "applog.h"
#include "autostart.h"
#include "stats.h"
#include "storage.h"

namespace {

constexpr wchar_t kViewerMutexName[] = L"Local\\UseStats_Viewer_SingleInstance";
constexpr wchar_t kRecorderMutexName[] = L"Local\\UseStats_SingleInstance"; // 与 recorder.cpp 一致
constexpr wchar_t kStatsClassName[] = L"UseStatsStatsWindow"; // 与 stats.cpp 保持一致

std::wstring ExeDir() {
    wchar_t buf[1024];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring full(buf, n);
    size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : full.substr(0, pos);
}

std::wstring SelfPath() {
    wchar_t path[1024];
    DWORD n = GetModuleFileNameW(nullptr, path,
                                 static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    return std::wstring(path, n);
}

// 与记录器/stats.cpp 约定的显示消息（同名注册，ID 必然一致）：发给已有
// 查看器窗口后，它走自己的 Open 逻辑唤出（隐藏唤出 + 数据刷新 + 置前）。
UINT ViewerShowMsg() {
    static UINT msg = RegisterWindowMessageW(L"UseStats.ViewerShow");
    return msg;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // WebView2 需要 STA + 消息泵

    // 查看器单实例：已有窗口在则调到前台后退出（对方可能正在启动，短暂重试）
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kViewerMutexName);
    if (mutex == nullptr) {
        CoUninitialize();
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 30; ++i) {
            HWND existing = FindWindowW(kStatsClassName, nullptr);
            if (existing != nullptr) {
                // 不直接摆弄对方窗口：发 ViewerShow 注册消息，让已有
                // 实例走自己的 Open 逻辑（隐藏状态 SW_SHOW 唤出、最小
                // 化恢复、按最新数据重新导航、置前）——与记录器托盘
                // 单击是同一条唤出路径。
                PostMessageW(existing, ViewerShowMsg(), 0, 0);
                break;
            }
            Sleep(100);
        }
        CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }

    Storage storage(ExeDir());
    AppLogInit(storage.ExeDir());
    UseStatsDebugLog("viewer: start");
    UseStatsDebugLog(storage.DataDir() != storage.ExeDir()
                         ? "storage: data dir = record subfolder"
                         : "storage: data dir = exe dir");

    // ---- 启动自愈（问题 2）：自启指向 + 记录程序保活 ----
    // Run 的目标只能是同目录记录器（唯一入口 AutoStartRecorderPath)。
    // 先读出 Run 的现有指向再校准：若它指着查看器自己（旧版本遗留），
    // 点名写进日志，不再靠猜。
    const std::wstring recorderPath = AutoStartRecorderPath(storage.ExeDir());
    std::wstring runBefore;
    bool hadRun = AutoStartReadRunValue(runBefore);
    AutoStartInit(recorderPath); // 偏好规则与记录器侧完全一致（不存在默认开、=0 不启用）
    if (AutoStartRunTargetIs(recorderPath)) {
        std::wstring selfQuoted = L"\"" + SelfPath() + L"\"";
        std::wstring wantQuoted = L"\"" + recorderPath + L"\"";
        if (hadRun && runBefore == selfQuoted)
            UseStatsDebugLog("autostart(viewer): Run pointed at viewer; fixed to recorder");
        else if (hadRun && runBefore == wantQuoted)
            UseStatsDebugLog("autostart(viewer): Run ok -> recorder");
        else
            UseStatsDebugLog("autostart(viewer): Run refreshed -> recorder path");
    } else {
        UseStatsDebugLog("autostart(viewer): Run not at recorder (pref off or write failed)");
    }
    // 记录程序没在运行就顺手拉起：试开它的采集互斥量只探存在性，
    // 拿到立刻关闭，绝不持有（持有会挡住记录器自己的单实例判断）。
    HANDLE probe = OpenMutexW(SYNCHRONIZE, FALSE, kRecorderMutexName);
    if (probe != nullptr) {
        CloseHandle(probe);
        UseStatsDebugLog("viewer: recorder already running");
    } else {
        if (GetFileAttributesW(recorderPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            UseStatsDebugLog("viewer: recorder exe MISSING next to viewer");
        HINSTANCE r = ShellExecuteW(nullptr, L"open", recorderPath.c_str(), nullptr,
                                    storage.ExeDir().c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "viewer: recorder launch FAILED code=%lld",
                          static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
            UseStatsDebugLog(buf);
        } else {
            UseStatsDebugLog("viewer: recorder not running; launched UseStatsRecorder.exe");
        }
    }

    StatsWindow::Init(hInstance, &storage, nullptr);
    StatsWindow::Open(); // 双击即开窗；点红 X 只隐藏收起，真退出走记录器托盘菜单「关闭查看器」

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
