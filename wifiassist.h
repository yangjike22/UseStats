#pragma once
#include <cstdint>

// WiFi 辅助（记录程序专用，见 wifiassist.cpp）：
// 解决一类真机毛病——电脑未连 WLAN 时，系统的可用网络列表像冻住一样
// 不刷新，明明有网也搜不到，每次都得手动点状态栏去连。本模块在记录
// 程序里挂一个低频状态机（主定时器每 15 秒调一次 Tick）：
// - 未连接时每约 45 秒对无线网卡调一次 WlanScan，强制刷新可用网络
//   列表，用户点开状态栏时列表是新鲜的；
// - 未连接持续 ≥20 秒后，从「可用网络 ∩ 已保存配置文件」里挑信号最
//   强的一个用 WlanConnect 自动连接（尝试间隔 ≥60 秒防抖）。只连本
//   机已保存过配置文件的网络，绝不碰陌生/开放网络。
// 全部经 Native WiFi API（wlanapi.dll 动态加载，不进导入表）；DLL 缺
// 失、WLAN 服务（WlanSvc）不可用或无无线网卡时静默降级，绝不影响记
// 录主业。状态变化与动作都写 stats_debug.log，前缀 wifi:。
class WifiAssist {
public:
    WifiAssist();
    ~WifiAssist();
    WifiAssist(const WifiAssist&) = delete;
    WifiAssist& operator=(const WifiAssist&) = delete;

    // assist=false（设置关）时整体待命并释放 WLAN 句柄；
    // autoConnect 仅在 assist 开启时生效。nowMs 用 GetTickCount64()。
    void Tick(bool assist, bool autoConnect, uint64_t nowMs);

private:
    struct Impl;
    Impl* impl_;

    // 状态机内部步骤（定义在 wifiassist.cpp；interfaceGuid 指向 GUID，
    // 头文件刻意不引入 Win32 类型，与 tracker.h 的风格一致）
    static void ResetRuntime(Impl& d);
    static void Disable(Impl& d, const char* why, unsigned long err);
    static bool EnsureClient(Impl& d);
    static void LogConnected(Impl& d, const void* interfaceGuid);
    static void TryAutoConnect(Impl& d, const void* interfaceGuid, uint64_t nowMs);
};
