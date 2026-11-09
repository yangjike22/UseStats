// WiFi 辅助实现：Native WiFi API（wlanapi.dll）动态加载 + 低频状态机。
// 与 WebView2Loader 同风格：DLL 不进导入表，运行时 LoadLibrary +
// GetProcAddress 取入口；wlanapi.dll 在个别精简系统上可能缺失，那就
// 记一行日志后整个功能关停，记录程序照常启动、照常记录。
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wlanapi.h> // 只用类型与常量；函数入口全部动态解析

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "applog.h"
#include "wifiassist.h"

namespace {

// ---- 节奏（Tick 每 15 秒来一次）----
constexpr uint64_t kScanIntervalMs = 45000;    // 未连接时每 ~45 秒强制扫描一次
constexpr uint64_t kDisconnectGraceMs = 20000; // 断开持续 ≥20 秒才尝试自动连接
constexpr uint64_t kConnectRetryMs = 60000;    // 自动连接尝试最小间隔（防抖）

// ---- wlanapi 动态入口 ----
using PFN_WlanOpenHandle = DWORD (WINAPI *)(DWORD, PVOID, PDWORD, PHANDLE);
using PFN_WlanCloseHandle = DWORD (WINAPI *)(HANDLE, PVOID);
using PFN_WlanEnumInterfaces = DWORD (WINAPI *)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
using PFN_WlanFreeMemory = VOID (WINAPI *)(PVOID);
using PFN_WlanQueryInterface = DWORD (WINAPI *)(HANDLE, const GUID*, WLAN_INTF_OPCODE,
                                               PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
using PFN_WlanScan = DWORD (WINAPI *)(HANDLE, const GUID*, PDOT11_SSID, PVOID, PVOID);
using PFN_WlanGetAvailableNetworkList = DWORD (WINAPI *)(HANDLE, const GUID*, DWORD,
                                                         PVOID, PWLAN_AVAILABLE_NETWORK_LIST*);
using PFN_WlanGetProfileList = DWORD (WINAPI *)(HANDLE, const GUID*, PVOID,
                                                PWLAN_PROFILE_INFO_LIST*);
using PFN_WlanConnect = DWORD (WINAPI *)(HANDLE, const GUID*,
                                         const PWLAN_CONNECTION_PARAMETERS, PVOID);

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// SSID 是任意字节串：按 UTF-8 宽松解码（非法字节替换为 ?），仅用于日志
// 与「和配置文件名比对」——比对失败顶多漏连，不会连错。
std::wstring SsidToWide(const DOT11_SSID& ssid) {
    ULONG len = ssid.uSSIDLength;
    if (len > DOT11_SSID_MAX_LENGTH) len = DOT11_SSID_MAX_LENGTH;
    if (len == 0) return std::wstring();
    const char* bytes = reinterpret_cast<const char*>(ssid.ucSSID);
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(len),
                                nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(len), w.data(), n);
    return w;
}

std::string SsidToUtf8(const DOT11_SSID& ssid) { return WideToUtf8(SsidToWide(ssid)); }

void LogErr(const char* what, DWORD err) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "wifi: %s failed err=%lu", what,
                  static_cast<unsigned long>(err));
    UseStatsDebugLog(buf);
}

} // namespace

struct WifiAssist::Impl {
    HMODULE dll = nullptr;
    bool dllTried = false;
    bool dead = false; // DLL 缺失 / 服务不可用等致命问题：本进程内关停
    HANDLE client = nullptr;

    PFN_WlanOpenHandle fnOpen = nullptr;
    PFN_WlanCloseHandle fnClose = nullptr;
    PFN_WlanEnumInterfaces fnEnum = nullptr;
    PFN_WlanFreeMemory fnFree = nullptr;
    PFN_WlanQueryInterface fnQuery = nullptr;
    PFN_WlanScan fnScan = nullptr;
    PFN_WlanGetAvailableNetworkList fnAvail = nullptr;
    PFN_WlanGetProfileList fnProfiles = nullptr;
    PFN_WlanConnect fnConnect = nullptr;

    bool noInterfaceLogged = false;
    bool wasConnected = false;
    bool haveDiscSince = false;
    uint64_t discSinceMs = 0;
    bool haveScan = false;
    uint64_t lastScanMs = 0;
    int lastScanErr = -1; // 同一个扫描错误只记一次
    bool haveConnectTry = false;
    uint64_t lastConnectTryMs = 0;
    bool noCandidateLogged = false;
    std::string lastAutoConnectSsid; // 本模块最近一次 auto-connect 的目标，仅作日志兜底
};

namespace {

// WLAN 服务停掉是致命问题：记一行、关停，直到进程重启或设置重开。
// 其余 API 错误多为瞬时的（如正在切换状态），不在这里处理。
bool IsFatal(DWORD err) {
    return err == ERROR_SERVICE_NOT_ACTIVE || // 1062 WlanSvc 没跑
           err == ERROR_SERVICE_DISABLED ||   // 服务被禁用
           err == ERROR_INVALID_HANDLE;
}

} // namespace

// 设置关掉或功能关停时释放句柄并复位状态机（下次开启从干净状态开始）
void WifiAssist::ResetRuntime(Impl& d) {
    if (d.client != nullptr && d.fnClose != nullptr) d.fnClose(d.client, nullptr);
    d.client = nullptr;
    d.wasConnected = false;
    d.haveDiscSince = false;
    d.haveScan = false;
    d.haveConnectTry = false;
    d.noCandidateLogged = false;
    d.lastScanErr = -1;
    d.lastAutoConnectSsid.clear();
}

void WifiAssist::Disable(Impl& d, const char* why, unsigned long err) {
    char buf[224];
    std::snprintf(buf, sizeof(buf), "wifi: disabled (%s, err=%lu); WLAN service unavailable?",
                  why, err);
    UseStatsDebugLog(buf);
    ResetRuntime(d);
    d.dead = true;
}

// 懒打开 wlanapi.dll + WlanOpenHandle。失败一律走「记一行 + 关停」。
bool WifiAssist::EnsureClient(Impl& d) {
    if (d.client != nullptr) return true;
    if (!d.dllTried) {
        d.dllTried = true;
        d.dll = LoadLibraryW(L"wlanapi.dll");
        if (d.dll != nullptr) {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
            d.fnOpen = reinterpret_cast<PFN_WlanOpenHandle>(GetProcAddress(d.dll, "WlanOpenHandle"));
            d.fnClose = reinterpret_cast<PFN_WlanCloseHandle>(GetProcAddress(d.dll, "WlanCloseHandle"));
            d.fnEnum = reinterpret_cast<PFN_WlanEnumInterfaces>(GetProcAddress(d.dll, "WlanEnumInterfaces"));
            d.fnFree = reinterpret_cast<PFN_WlanFreeMemory>(GetProcAddress(d.dll, "WlanFreeMemory"));
            d.fnQuery = reinterpret_cast<PFN_WlanQueryInterface>(GetProcAddress(d.dll, "WlanQueryInterface"));
            d.fnScan = reinterpret_cast<PFN_WlanScan>(GetProcAddress(d.dll, "WlanScan"));
            d.fnAvail = reinterpret_cast<PFN_WlanGetAvailableNetworkList>(GetProcAddress(d.dll, "WlanGetAvailableNetworkList"));
            d.fnProfiles = reinterpret_cast<PFN_WlanGetProfileList>(GetProcAddress(d.dll, "WlanGetProfileList"));
            d.fnConnect = reinterpret_cast<PFN_WlanConnect>(GetProcAddress(d.dll, "WlanConnect"));
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
            if (d.fnOpen == nullptr || d.fnClose == nullptr || d.fnEnum == nullptr ||
                d.fnFree == nullptr || d.fnQuery == nullptr || d.fnScan == nullptr ||
                d.fnAvail == nullptr || d.fnProfiles == nullptr || d.fnConnect == nullptr) {
                FreeLibrary(d.dll);
                d.dll = nullptr;
            }
        }
    }
    if (d.dll == nullptr) {
        UseStatsDebugLog("wifi: disabled (wlanapi.dll unavailable)");
        d.dead = true;
        return false;
    }
    DWORD negotiated = 0;
    DWORD err = d.fnOpen(2, nullptr, &negotiated, &d.client); // 版本 2 = Vista+
    if (err != ERROR_SUCCESS || d.client == nullptr) {
        Disable(d, "WlanOpenHandle", err);
        return false;
    }
    UseStatsDebugLog("wifi: assist ready");
    return true;
}

void WifiAssist::LogConnected(Impl& d, const void* interfaceGuid) {
    const GUID& guid = *static_cast<const GUID*>(interfaceGuid);
    std::string name;
    DWORD size = 0;
    PVOID data = nullptr;
    if (d.fnQuery(d.client, &guid, wlan_intf_opcode_current_connection, nullptr,
                  &size, &data, nullptr) == ERROR_SUCCESS && data != nullptr) {
        const auto* ca = static_cast<const WLAN_CONNECTION_ATTRIBUTES*>(data);
        if (ca->strProfileName[0] != L'\0')
            name = WideToUtf8(ca->strProfileName);
        else
            name = SsidToUtf8(ca->wlanAssociationAttributes.dot11Ssid);
        d.fnFree(data);
    }
    // 兜底链：strProfileName → 关联 SSID → 本模块最近一次 auto-connect 的目标；
    // 全空时明确写 unavailable，绝不再出现 wifi: connected "" 这种空引号行
    if (name.empty()) name = d.lastAutoConnectSsid;
    if (name.empty())
        UseStatsDebugLog("wifi: connected (ssid unavailable)");
    else
        UseStatsDebugLog("wifi: connected \"" + name + "\"");
}

// 在「可用网络 ∩ 已保存配置文件」中挑信号最强者发起连接。
void WifiAssist::TryAutoConnect(Impl& d, const void* interfaceGuid, uint64_t nowMs) {
    const GUID& guid = *static_cast<const GUID*>(interfaceGuid);
    d.haveConnectTry = true;
    d.lastConnectTryMs = nowMs;

    PWLAN_PROFILE_INFO_LIST profiles = nullptr;
    DWORD err = d.fnProfiles(d.client, &guid, nullptr, &profiles);
    if (err != ERROR_SUCCESS) {
        if (IsFatal(err)) { Disable(d, "WlanGetProfileList", err); return; }
        LogErr("profile list", err);
        return;
    }
    std::vector<std::wstring> saved;
    if (profiles != nullptr) {
        for (DWORD i = 0; i < profiles->dwNumberOfItems; ++i)
            saved.emplace_back(profiles->ProfileInfo[i].strProfileName);
        d.fnFree(profiles);
    }
    if (saved.empty()) return; // 没保存过任何网络：无从连起，保持安静

    PWLAN_AVAILABLE_NETWORK_LIST avail = nullptr;
    err = d.fnAvail(d.client, &guid, 0, nullptr, &avail);
    if (err != ERROR_SUCCESS) {
        if (IsFatal(err)) { Disable(d, "WlanGetAvailableNetworkList", err); return; }
        LogErr("available list", err);
        return;
    }
    const WLAN_AVAILABLE_NETWORK* best = nullptr;
    std::wstring bestProfile;
    if (avail != nullptr) {
        for (DWORD i = 0; i < avail->dwNumberOfItems; ++i) {
            const WLAN_AVAILABLE_NETWORK& n = avail->Network[i];
            if (!n.bNetworkConnectable) continue;
            // 命中条件：SSID 与某个配置文件名一致，或系统已标明该网络
            // 挂着某个配置文件（strProfileName 非空且在保存列表里）
            std::wstring ssid = SsidToWide(n.dot11Ssid);
            const std::wstring* match = nullptr;
            for (const std::wstring& p : saved) {
                if (p == ssid || (n.strProfileName[0] != L'\0' && p == n.strProfileName)) {
                    match = &p;
                    break;
                }
            }
            if (match == nullptr) continue; // 陌生网络：绝不碰
            if (best == nullptr || n.wlanSignalQuality > best->wlanSignalQuality) {
                best = &n;
                bestProfile = *match;
            }
        }
    }
    if (best == nullptr) {
        if (avail != nullptr) d.fnFree(avail);
        if (!d.noCandidateLogged) {
            d.noCandidateLogged = true;
            UseStatsDebugLog("wifi: no saved network in range");
        }
        return;
    }
    DOT11_SSID ssid = best->dot11Ssid;
    DOT11_BSS_TYPE bss = best->dot11BssType;
    std::string ssidUtf8 = SsidToUtf8(ssid);
    if (avail != nullptr) d.fnFree(avail);

    WLAN_CONNECTION_PARAMETERS params{};
    params.wlanConnectionMode = wlan_connection_mode_profile;
    params.strProfile = bestProfile.c_str();
    params.pDot11Ssid = &ssid;
    params.pDesiredBssidList = nullptr;
    params.dot11BssType = bss;
    params.dwFlags = 0;
    err = d.fnConnect(d.client, &guid, &params, nullptr);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "wifi: auto-connect \"%s\" result=%lu",
                  ssidUtf8.c_str(), static_cast<unsigned long>(err));
    UseStatsDebugLog(buf);
    if (err == ERROR_SUCCESS) d.lastAutoConnectSsid = ssidUtf8; // 供 connected 行的日志兜底
    if (IsFatal(err)) Disable(d, "WlanConnect", err);
}

WifiAssist::WifiAssist() : impl_(new Impl()) {}
WifiAssist::~WifiAssist() {
    ResetRuntime(*impl_);
    if (impl_->dll != nullptr) FreeLibrary(impl_->dll);
    delete impl_;
}

void WifiAssist::Tick(bool assist, bool autoConnect, uint64_t nowMs) {
    Impl& d = *impl_;
    if (!assist) {
        ResetRuntime(d);
        d.dead = false; // 允许之后在设置里重新打开
        return;
    }
    if (d.dead) return;
    if (!EnsureClient(d)) return;

    // 枚举无线网卡（15 秒一拍足够跟上插拔）；没有无线网卡就静默待命
    PWLAN_INTERFACE_INFO_LIST ifaces = nullptr;
    DWORD err = d.fnEnum(d.client, nullptr, &ifaces);
    if (err != ERROR_SUCCESS) {
        if (IsFatal(err)) { Disable(d, "WlanEnumInterfaces", err); return; }
        LogErr("enum interfaces", err);
        return;
    }
    if (ifaces == nullptr || ifaces->dwNumberOfItems == 0) {
        if (ifaces != nullptr) d.fnFree(ifaces);
        if (!d.noInterfaceLogged) {
            d.noInterfaceLogged = true;
            UseStatsDebugLog("wifi: no wireless interface; standby");
        }
        d.wasConnected = false;
        d.haveDiscSince = false;
        return;
    }
    d.noInterfaceLogged = false;
    GUID guid = ifaces->InterfaceInfo[0].InterfaceGuid;
    d.fnFree(ifaces);

    // 当前接口状态
    DWORD size = 0;
    PVOID data = nullptr;
    err = d.fnQuery(d.client, &guid, wlan_intf_opcode_interface_state, nullptr,
                    &size, &data, nullptr);
    if (err != ERROR_SUCCESS || data == nullptr) {
        if (IsFatal(err)) { Disable(d, "WlanQueryInterface", err); return; }
        if (err != ERROR_SUCCESS) LogErr("query interface state", err);
        return;
    }
    WLAN_INTERFACE_STATE state = *static_cast<const WLAN_INTERFACE_STATE*>(data);
    d.fnFree(data);

    if (state == wlan_interface_state_connected) {
        if (!d.wasConnected) {
            d.wasConnected = true;
            LogConnected(d, &guid);
        }
        d.haveDiscSince = false;
        d.noCandidateLogged = false;
        return;
    }
    d.wasConnected = false;
    if (!d.haveDiscSince) {
        d.haveDiscSince = true;
        d.discSinceMs = nowMs;
    }
    // 过渡态（关联/发现/认证/断开中）不插手，等它落定
    if (state != wlan_interface_state_disconnected &&
        state != wlan_interface_state_not_ready)
        return;

    // 扫描保鲜：未连接期间每 ~45 秒强制刷新一次可用网络列表
    if (!d.haveScan || nowMs - d.lastScanMs >= kScanIntervalMs) {
        d.haveScan = true;
        d.lastScanMs = nowMs;
        DWORD serr = d.fnScan(d.client, &guid, nullptr, nullptr, nullptr);
        if (serr == ERROR_SUCCESS) {
            UseStatsDebugLog("wifi: disconnected, scan requested");
            d.lastScanErr = -1;
        } else {
            if (d.lastScanErr != static_cast<int>(serr)) {
                d.lastScanErr = static_cast<int>(serr);
                LogErr("scan", serr);
            }
            if (IsFatal(serr)) { Disable(d, "WlanScan", serr); return; }
        }
    }

    // 自动连接已保存网络：断开持续 ≥20 秒且距上次尝试 ≥60 秒
    if (!autoConnect) return;
    if (nowMs - d.discSinceMs < kDisconnectGraceMs) return;
    if (d.haveConnectTry && nowMs - d.lastConnectTryMs < kConnectRetryMs) return;
    TryAutoConnect(d, &guid, nowMs);
}
