#pragma once

#include <windows.h> // 目标版本/精简宏由包含本头的 .cpp 在包含前自行定义

#include <string>

class Storage;
class Tracker;

// 诊断日志：与统计窗口共用程序目录的 stats_debug.log（追加、上限截断）。
// 供托盘等其他模块记录关键节点（如 NIM_ADD 结果）；StatsWindow::Init
// 之前调用会静默跳过（日志路径依赖 Storage）。
void UseStatsDebugLog(const std::string& message);

// 统计窗口（查看器 UseStats.exe 的主窗口）：双击 exe 时打开。
// 窗口内用 WebView2 渲染内嵌页面：数据 JSON 与设置 JSON 直接内嵌进
// HTML 字符串，以 NavigateToString 加载（不写页面文件、不用 file://，
// 与目录路径无关）。全链路关键节点写程序目录的 stats_debug.log；
// 导航 8 秒未完成或失败时切原生兜底文字，窗口不会停在空白上。
// 双程序架构下 tracker 参数恒为 nullptr（查看器不采集，数据读记录器
// 落盘的 usage-*.json）；托盘整合后查看器不注册任何图标——全系统唯一
// 的托盘图标归记录程序：点窗口红 X 只隐藏窗口（进程不退出、WebView2
// 保持存活），记录器单击图标/菜单经注册消息 UseStats.ViewerShow 唤出
// 本窗口，菜单「关闭查看器」经 UseStats.ViewerQuit 让查看器真正结束
// 进程；设置页读写经 WebMessage 通道（见 stats.cpp）。
class StatsWindow {
public:
    static void Init(HINSTANCE hInstance, const Storage* storage, const Tracker* tracker);
    static void Open();     // 已打开则置于前台，不重复创建
    static void Shutdown(); // 窗口还在则销毁
};
