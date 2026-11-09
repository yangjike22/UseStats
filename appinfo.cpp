#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <unordered_map>
#include <vector>

#include "appinfo.h"

#ifdef _MSC_VER
#pragma comment(lib, "version.lib")
#endif

namespace {

// 取路径中的文件名部分
std::wstring BaseName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

// 去掉扩展名：chrome.exe -> chrome
std::wstring StripExt(const std::wstring& fileName) {
    size_t pos = fileName.find_last_of(L'.');
    return (pos == std::wstring::npos || pos == 0) ? fileName : fileName.substr(0, pos);
}

// 拼 StringFileInfo 块名（手写十六进制，避免 swprintf_s 在 MinGW 下的差异）
std::wstring MakeBlockName(WORD lang, WORD cp) {
    static const wchar_t* kHex = L"0123456789abcdef";
    std::wstring s = L"\\StringFileInfo\\";
    for (int shift = 12; shift >= 0; shift -= 4) s += kHex[(lang >> shift) & 0xF];
    for (int shift = 12; shift >= 0; shift -= 4) s += kHex[(cp >> shift) & 0xF];
    s += L"\\FileDescription";
    return s;
}

bool QueryDescription(const BYTE* data, WORD lang, WORD cp, std::wstring& out) {
    std::wstring block = MakeBlockName(lang, cp);
    wchar_t* desc = nullptr;
    UINT len = 0;
    if (VerQueryValueW(const_cast<BYTE*>(data), block.c_str(),
                       reinterpret_cast<LPVOID*>(&desc), &len) &&
        desc != nullptr && len > 0 && desc[0] != L'\0') {
        out.assign(desc);
        return true;
    }
    return false;
}

} // namespace

const std::wstring& AppInfo::FriendlyName(const std::wstring& exePath) {
    static std::unordered_map<std::wstring, std::wstring> cache;
    auto it = cache.find(exePath);
    if (it != cache.end()) return it->second;
    auto inserted = cache.emplace(exePath, Lookup(exePath));
    return inserted.first->second;
}

std::wstring AppInfo::Lookup(const std::wstring& exePath) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
    if (size > 0) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(exePath.c_str(), 0, size, data.data())) {
            std::wstring desc;
            // 1) 优先用文件自带的语言/代码页
            struct LangCp { WORD lang; WORD cp; };
            LangCp* trans = nullptr;
            UINT tlen = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                               reinterpret_cast<LPVOID*>(&trans), &tlen) &&
                trans != nullptr && tlen >= sizeof(LangCp)) {
                if (QueryDescription(data.data(), trans[0].lang, trans[0].cp, desc))
                    return desc;
            }
            // 2) 常见组合兜底：简体中文、英文、语言中立
            static const LangCp kFallback[] = {{0x0804, 1200}, {0x0409, 1200}, {0x0000, 1200}};
            for (const auto& lc : kFallback) {
                if (QueryDescription(data.data(), lc.lang, lc.cp, desc))
                    return desc;
            }
        }
    }
    // 3) 全部失败：文件名去后缀
    return StripExt(BaseName(exePath));
}
