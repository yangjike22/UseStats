#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// 托盘悬停提示（tooltip）文案格式化：纯函数、无 Win32 依赖，
// 供 tests/traytip_test.cpp 在宿主机上直接编译单测。
//
// 格式：今日 {总时长}｜{应用1} {时长} · {应用2} {时长} · {应用3} {时长}
// 例：  今日 5小时23分｜Chrome 1小时52分 · VS Code 1小时10分 · 微信 45分
// 无数据（Top 列表为空）时为「今日暂无记录」。
// 总长不超过 127 个字符（NOTIFYICONDATA::szTip 为 128 含结尾 0）：
// 装不下的名次整体丢弃，不截半句；单个应用名过长时截断加省略号。
namespace TrayTip {

inline constexpr size_t kMaxTipChars = 127;   // szTip 上限（不含结尾 0）
inline constexpr size_t kMaxNameChars = 18;   // 单个应用名的最长显示

// 秒 → 时长：>=1 小时为「X小时Y分」（Y 为 0 时「X小时」），<1 小时为「Y分」。
inline std::wstring FormatDuration(uint64_t seconds) {
    uint64_t h = seconds / 3600;
    uint64_t m = (seconds % 3600) / 60;
    if (h > 0) {
        if (m == 0) return std::to_wstring(h) + L"小时";
        return std::to_wstring(h) + L"小时" + std::to_wstring(m) + L"分";
    }
    return std::to_wstring(m) + L"分";
}

inline std::wstring TruncateName(const std::wstring& name) {
    if (name.size() <= kMaxNameChars) return name;
    return name.substr(0, kMaxNameChars) + L"…";
}

// totalSeconds：今日前台总时长（秒）。
// top：已按前台秒降序排好、且只含 fg>0 的 (友好名, 前台秒)，内部最多取前 3。
inline std::wstring Format(uint64_t totalSeconds,
                           const std::vector<std::pair<std::wstring, uint64_t>>& top) {
    if (top.empty()) return L"今日暂无记录";
    std::wstring out = L"今日 " + FormatDuration(totalSeconds);
    size_t n = top.size() < 3 ? top.size() : 3;
    for (size_t i = 0; i < n; ++i) {
        std::wstring item = (i == 0 ? L"｜" : L" · ") + TruncateName(top[i].first) +
                            L" " + FormatDuration(top[i].second);
        if (out.size() + item.size() > kMaxTipChars) break; // 装不下就少显示一个名次
        out += item;
    }
    return out;
}

} // namespace TrayTip
