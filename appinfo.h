#pragma once
#include <string>

// 应用信息解析：exe 路径 -> 友好显示名，内部带缓存，同一路径只查一次。
class AppInfo {
public:
    // 优先读版本信息 FileDescription；失败回退为文件名（去后缀）。
    static const std::wstring& FriendlyName(const std::wstring& exePath);

private:
    static std::wstring Lookup(const std::wstring& exePath);
};
