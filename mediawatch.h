#pragma once
#include <cstdint>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <unordered_set>

// 媒体豁免：解决「看视频课/读长文时人不动，被空闲判定误杀」的问题。
//
// 背景：空闲判定只看键鼠（GetLastInputInfo）。用户在哔哩哔哩桌面端看课、
// 在 Edge 里工作时常有几分钟到十几分钟不碰键鼠，但人就在电脑前。两条豁免：
//  1) mediaNoIdle（ForegroundMediaActive，实现于 mediawatch.cpp）：
//     键鼠空闲已超阈值时，若满足任一条件，这一秒仍算前台使用——
//     a) 与前台窗口同进程树的某个音频会话正在出声（峰值电平达标，
//        且出声有 10 秒黏性，盖住视频里的安静片段）；
//     b) 系统处于全屏/演示状态（SHQueryUserNotificationState）。
//     进程树匹配是因为浏览器/客户端是多进程的：出声的往往是子进程
//     （渲染/音频服务进程），与前台窗口进程同树即可认定是它在播。
//  2) focusGrace（IsFocusExeName，纯函数）：前台是专注应用时把空闲阈值
//     放宽到 15 分钟（Tracker 侧应用），覆盖无声阅读这种没有音频信号的场景。
//
// 本头文件的纯函数不依赖 Win32，可在宿主机单测（tests/mediawatch_test.cpp）；
// ForegroundMediaActive 只有 Windows 实现，声明也只在 _WIN32 下可见。
namespace MediaWatch {

// a 与 b 是否同属一棵进程树（parentOf: pid -> 父 pid，来自进程快照）。
// 沿父链上溯比对：任一方的祖先链包含另一方或相交，即同树。PID 为 0 视为无效。
inline bool PidsRelated(const std::unordered_map<uint32_t, uint32_t>& parentOf,
                        uint32_t a, uint32_t b) {
    if (a == 0 || b == 0) return false;
    if (a == b) return true;
    std::unordered_set<uint32_t> chainA;
    uint32_t cur = a;
    for (int i = 0; i < 64 && cur != 0; ++i) {
        chainA.insert(cur);
        auto it = parentOf.find(cur);
        if (it == parentOf.end()) break;
        cur = it->second;
    }
    cur = b;
    for (int i = 0; i < 64 && cur != 0; ++i) {
        if (chainA.count(cur)) return true;
        auto it = parentOf.find(cur);
        if (it == parentOf.end()) break;
        cur = it->second;
    }
    return false;
}

// 在已转小写的宽串里找 ASCII 子串（needle 须为小写）。
inline bool ContainsAsciiLower(const std::wstring& lowered, const char* needle) {
    size_t n = 0;
    while (needle[n] != '\0') ++n;
    if (n == 0 || lowered.size() < n) return false;
    for (size_t i = 0; i + n <= lowered.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            if (lowered[i + j] != static_cast<wchar_t>(needle[j])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

// 专注应用名单（按 exe 文件名判断，大小写不敏感）：
// - msedge.exe：Edge 浏览器（看长文/文档/PDF 的静止时段靠宽限覆盖）；
// - 哔哩哔哩客户端：英文名含 "bilibili"，或中文名「哔哩哔哩.exe」
//   （用码点拼，避免源文件字符集影响宽字面量）。
inline bool IsFocusExeName(const std::wstring& baseName) {
    if (baseName.empty()) return false;
    std::wstring low = baseName;
    for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
    if (low == L"msedge.exe") return true;
    if (ContainsAsciiLower(low, "bilibili")) return true;
    static const wchar_t kBilibiliExe[] = {
        static_cast<wchar_t>(0x54D4), static_cast<wchar_t>(0x54E9),
        static_cast<wchar_t>(0x54D4), static_cast<wchar_t>(0x54E9),
        L'.', L'e', L'x', L'e', 0};
    if (low == kBilibiliExe) return true;
    return false;
}

#ifdef _WIN32
// 前台进程此刻是否「媒体使用中」（出声播放或全屏/演示，见文件头）。
// reason 非空时回填命中的原因："audio" / "presentation" / ""。
// 内部 2 秒缓存（按 fgPid 区分），仅在空闲已超阈值时被调用，开销可忽略；
// 任何 COM/音频 API 失败都静默返回 false（退回纯键鼠判定），不影响记录主流程。
bool ForegroundMediaActive(uint32_t fgPid, const char** reason);
#endif

} // namespace MediaWatch
