#pragma once
#include <string>

// 开机自启管理（由原 main.cpp 抽出，记录器与查看器共用同一份实现）。
//
// 落点（与单 exe 时代完全一致，保证老用户无感迁移）：
// - 实际开关：HKCU\Software\Microsoft\Windows\CurrentVersion\Run 的
//   UseStats 值（带引号的 exe 完整路径）。
// - 用户偏好：HKCU\Software\UseStats 的 DWORD AutoStart
//   （不存在=从未设置，1=开，0=关）。分开存的原因：首次运行要默认开启，
//   但用户手动关过之后必须记住，不能靠 Run 值在不在反推「从未设置」。
//
// 双程序架构下自启的目标是「记录器」UseStatsRecorder.exe：
// - 记录器启动时调 AutoStartInit(自身路径)：从未设置→默认开启；
//   偏好开→校准 Run（旧版本指向 UseStats.exe、或 exe 被移动过，都刷新
//   为记录器当前路径）；偏好关→确保 Run 不存在。
// - 查看器设置页改开关时调 AutoStartSet(记录器路径, on)：Run 值与偏好
//   同时落盘，即时生效，不依赖记录器正在运行。
// - 查看器每次启动也会调 AutoStartInit(记录器路径) 做一遍同样的校准
//   （自愈）：旧版本遗留的 Run 若指向查看器或旧路径，用户不必非得先
//   打开记录器，打开任一程序一次即可修复（见 viewer.cpp）。
// 所有注册表操作成败都写 stats_debug.log（autostart: 前缀），且每次
// 读写 Run 都把实际路径一并落日志（autostart: Run target="..."），
// 排障时不必再猜 Run 到底指向哪个文件。

// 【唯一入口】Run 的目标路径只能由这个函数决定：永远是 exeDir 同目录
// 的记录程序 UseStatsRecorder.exe。全工程任何调用点（记录器启动校准、
// 记录器托盘开关、查看器设置保存、查看器启动自愈）都必须经它取得目标
// 路径，绝不允许把 UseStats.exe（查看器）或任何其他路径写进 Run。
std::wstring AutoStartRecorderPath(const std::wstring& exeDir);

// Run 值是否存在（查看器设置页显示开关状态的依据）。
bool AutoStartRunPresent();

// 读出 Run 当前的原始值（带引号的完整路径）；不存在返回 false。
// 供查看器启动自愈点名日志（Run 原先到底指向哪）使用。
bool AutoStartReadRunValue(std::wstring& out);

// Run 值是否正好指向 exePath（带引号的完整路径）。查看器启动自愈时
// 用它判断 Run 是否需要刷新（缺失/指向别的程序/旧路径都算不匹配）。
bool AutoStartRunTargetIs(const std::wstring& exePath);

// 唯一的同步点：Run 值与偏好同时落到 enable 状态，目标为 exePath。
void AutoStartSet(const std::wstring& exePath, bool enable);

// 启动校准（记录器专用）：默认开启 + 路径刷新 + 尊重已记住的关闭偏好。
void AutoStartInit(const std::wstring& exePath);
