// 托盘悬停文案格式化单测（宿主机原生编译运行，不依赖 Win32）：
//   g++ -std=c++17 -o /tmp/traytip_test tests/traytip_test.cpp && /tmp/traytip_test
// 注意：本机 wchar_t 为 4 字节、Windows 为 2 字节，但文案按「字符个数」
// 计数（szTip 上限同为字符数），两边语义一致。
#include <cassert>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../traytip.h"

using TrayTip::Format;
using TrayTip::FormatDuration;

static int failures = 0;
static void ExpectEq(const wchar_t* what, const std::wstring& got, const std::wstring& want) {
    if (got != want) {
        ++failures;
        std::fwprintf(stderr, L"FAIL %ls\n  got : %ls\n  want: %ls\n", what, got.c_str(), want.c_str());
    }
}
static void ExpectTrue(const wchar_t* what, bool cond) {
    if (!cond) { ++failures; std::fwprintf(stderr, L"FAIL %ls\n", what); }
}

int main() {
    // 时长格式
    ExpectEq(L"dur 0", FormatDuration(0), L"0分");
    ExpectEq(L"dur 45m", FormatDuration(2700), L"45分");
    ExpectEq(L"dur 1h", FormatDuration(3600), L"1小时");
    ExpectEq(L"dur 1h52m", FormatDuration(6720), L"1小时52分");

    // 任务书示例：今日 5小时23分｜Chrome 1小时52分 · VS Code 1小时10分 · 微信 45分
    std::vector<std::pair<std::wstring, uint64_t>> top = {
        {L"Chrome", 6720}, {L"VS Code", 4200}, {L"微信", 2700}};
    ExpectEq(L"spec example", Format(19380, top),
             L"今日 5小时23分｜Chrome 1小时52分 · VS Code 1小时10分 · 微信 45分");

    // 无数据
    ExpectEq(L"empty", Format(0, {}), L"今日暂无记录");

    // 只取前 3 名
    std::vector<std::pair<std::wstring, uint64_t>> five = {
        {L"A", 3600}, {L"B", 3500}, {L"C", 3400}, {L"D", 3300}, {L"E", 3200}};
    ExpectEq(L"top3 only", Format(17400, five),
             L"今日 4小时50分｜A 1小时 · B 58分 · C 56分");

    // 超长应用名截断加 …
    std::vector<std::pair<std::wstring, uint64_t>> longName = {
        {L"Microsoft Visual Studio Code Insiders", 3600}};
    std::wstring t = Format(3600, longName);
    ExpectTrue(L"long name truncated", t.find(L'…') != std::wstring::npos);
    ExpectTrue(L"long name total <= 127", t.size() <= 127);

    // 整体超限时丢弃后面的名次，结果仍 <= 127 且不截半句
    std::vector<std::pair<std::wstring, uint64_t>> many;
    for (int i = 0; i < 3; ++i)
        many.push_back({L"一个很长很长的应用名称占位符" + std::to_wstring(i), 3600});
    std::wstring t2 = Format(10800, many);
    ExpectTrue(L"overflow <= 127", t2.size() <= 127);
    ExpectTrue(L"overflow keeps first", t2.find(L"今日 3小时") == 0);

    if (failures == 0) std::printf("OK: traytip formatter tests passed\n");
    return failures == 0 ? 0 : 1;
}
