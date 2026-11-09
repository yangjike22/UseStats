#pragma once
#include <cstdint>
#include <string>

#include "settings.h"

// settings.json 的文件读写（Win32）：与 usage 数据同目录。
// 写入为原子替换（先写 settings.json.tmp 再改名覆盖），读者不会读到半截；
// 读取失败/文件缺失/内容损坏一律回退默认设置（解析层已保证不抛异常）。
std::wstring SettingsFilePath(const std::wstring& dir);
UseStatsSettings LoadSettingsFile(const std::wstring& dir);
bool SaveSettingsFile(const std::wstring& dir, const UseStatsSettings& s);

// 文件最后修改时间的戳（FILETIME 的 64 位拼值）；文件不存在返回 0。
// 记录器每 60 秒比对一次，变了就热加载。
uint64_t SettingsFileStamp(const std::wstring& dir);
