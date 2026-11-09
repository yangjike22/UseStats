#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

// 单个应用当天的聚合数据，聚合键为 exe 完整路径。
struct AppUsage {
    std::wstring path;                 // exe 完整路径（取不到路径时为 exe 文件名）
    std::wstring name;                 // 友好名，落盘时填充
    uint64_t fgSeconds = 0;            // 前台秒：该应用处于前台且用户未空闲的秒数
    uint64_t bgSeconds = 0;            // 后台秒：进程存活秒数（含其前台时间，见 README 口径）
    double cpuSeconds = 0.0;           // CPU 秒：内核态+用户态累计增量
    uint64_t hourlyFg[24] = {};        // 按小时分布的前台秒（0~23 时）
};

// 采样器：由主程序的 1 秒定时器驱动。
// OnSecondTick 每秒调用一次；OnBackgroundTick 每 10 秒调用一次。
class Tracker {
public:
    Tracker();

    void OnSecondTick();     // 前台采样 + 空闲判定
    void OnBackgroundTick(); // 进程枚举（存活秒 + CPU 秒增量）

    void ResetDay();         // 跨天时清空当日聚合

    AppUsage& GetOrCreate(const std::wstring& key);
    const std::unordered_map<std::wstring, AppUsage>& Apps() const { return apps_; }

    uint64_t IdleSeconds() const { return idleSeconds_; }
    void SetIdleSeconds(uint64_t v) { idleSeconds_ = v; }

    // 最近一秒采样到的前台应用路径；空闲/锁屏/取不到前台时为空。
    const std::wstring& CurrentFgPath() const { return currentFgPath_; }

    // 空闲判定阈值（毫秒）：由 settings.json 热加载驱动，默认 180 秒。
    void SetIdleThresholdMs(uint32_t ms) { idleThresholdMs_ = ms; }
    uint32_t IdleThresholdMs() const { return idleThresholdMs_; }

    // 空闲豁免（settings.json 热加载驱动，默认开，见 mediawatch.h）：
    // mediaNoIdle = 前台应用正在出声播放/全屏时不判空闲；
    // focusGrace = 前台为专注应用（Edge/哔哩哔哩）时阈值放宽到 15 分钟。
    void SetMediaNoIdle(bool v) { mediaNoIdle_ = v; }
    void SetFocusGrace(bool v) { focusGrace_ = v; }

private:
    void NoteIdleEpisode(const std::wstring* fgPath); // 空闲回合开始时记一行日志（每回合一次）
    struct CpuSample {
        uint64_t createTime = 0; // 进程创建时间（100ns 单位），用于区分 PID 复用
        uint64_t cpuTime = 0;    // 上次采样到的 CPU 累计（100ns 单位）
    };

    std::unordered_map<std::wstring, AppUsage> apps_;
    std::unordered_map<uint32_t, CpuSample> prevCpu_; // key = PID
    uint64_t idleSeconds_ = 0; // 未计入任何应用的秒数（空闲/锁屏/取不到前台）
    uint32_t idleThresholdMs_ = 180 * 1000;
    bool mediaNoIdle_ = true;
    bool focusGrace_ = true;
    bool idleEpisode_ = false;      // 当前是否已在空闲回合中（日志节流）
    uint64_t lastMediaLogMs_ = 0;   // keep-active 日志节流（60 秒一行）
    uint32_t selfPid_ = 0;
    std::wstring currentFgPath_;
};
