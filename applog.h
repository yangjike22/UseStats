#pragma once
#include <string>

// 诊断日志（原 stats.cpp 内的 DebugLog 抽成共享模块，两个 exe 共用）：
// 写程序目录的 stats_debug.log，追加写入，超过 200KB 截断重写。
// AppLogInit 之前调用静默跳过。UseStatsDebugLog 的声明与 stats.h 保持一致
// （同一函数的重复声明），旧调用点无需改动。
void AppLogInit(const std::wstring& exeDir);
void UseStatsDebugLog(const std::string& message);
