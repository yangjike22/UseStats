// mediawatch 纯逻辑单测：进程树关系判定 + 专注应用名单。
// 宿主机原生编译运行：
//   g++ -std=c++17 -o /tmp/mediawatch_test tests/mediawatch_test.cpp && /tmp/mediawatch_test
#include <cstdint>
#include <iostream>
#include <unordered_map>

#include "../mediawatch.h"

namespace {
int g_failures = 0;
void Check(bool cond, const char* what) {
    if (!cond) {
        ++g_failures;
        std::cout << "FAIL: " << what << "\n";
    }
}
} // namespace

int main() {
    // 进程树：1(根) -> 2 -> 3 -> 4；1 -> 5；9 独立
    std::unordered_map<uint32_t, uint32_t> p = {
        {1, 0}, {2, 1}, {3, 2}, {4, 3}, {5, 1}, {9, 0}};
    using MediaWatch::PidsRelated;
    Check(PidsRelated(p, 4, 1), "descendant vs root");
    Check(PidsRelated(p, 1, 4), "root vs descendant");
    Check(PidsRelated(p, 4, 5), "cousins share root");
    Check(PidsRelated(p, 3, 3), "same pid");
    Check(!PidsRelated(p, 4, 9), "unrelated trees");
    Check(!PidsRelated(p, 0, 1), "pid 0 invalid");
    Check(!PidsRelated(p, 1, 0), "pid 0 invalid (b)");
    Check(!PidsRelated(p, 42, 43), "unknown pids");
    Check(PidsRelated(p, 42, 42), "unknown but equal");
    // 环形父链（损坏快照）不能死循环
    std::unordered_map<uint32_t, uint32_t> cyc = {{7, 8}, {8, 7}};
    Check(PidsRelated(cyc, 7, 8), "cycle handled");

    using MediaWatch::IsFocusExeName;
    Check(IsFocusExeName(L"msedge.exe"), "edge exact");
    Check(IsFocusExeName(L"MSEDGE.EXE"), "edge upper");
    Check(IsFocusExeName(L"bilibili.exe"), "bilibili en");
    Check(IsFocusExeName(L"Bilibili.exe"), "bilibili en case");
    {
        std::wstring bili;
        bili += static_cast<wchar_t>(0x54D4);
        bili += static_cast<wchar_t>(0x54E9);
        bili += static_cast<wchar_t>(0x54D4);
        bili += static_cast<wchar_t>(0x54E9);
        bili += L".exe";
        Check(IsFocusExeName(bili), "bilibili zh name");
    }
    Check(!IsFocusExeName(L"chrome.exe"), "chrome not focus");
    Check(!IsFocusExeName(L"notmsedge.exe"), "substring is not edge");
    Check(!IsFocusExeName(L""), "empty");

    if (g_failures) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "OK: mediawatch tree relation + focus list all pass\n";
    return 0;
}
