#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

// 共享设置 settings.json 的数据模型与纯逻辑（序列化/解析/校验）。
// 本头文件不依赖 Win32，可在宿主机上直接编译单测（tests/settings_test.cpp）；
// 文件读写在 settings_io.h/.cpp（Win32 原子写 + 修改时间）。
//
// 两个程序同目录共用一份 settings.json：查看器（UseStats.exe）写，
// 记录器（UseStatsRecorder.exe）读并热加载。
//
// 字段与规则：
// - idleSeconds    空闲判定秒数，白名单 {60,120,180,300,600}，默认 180；
//                  非法值回退默认（不部分接受）。
// - backgroundStats 后台统计开关，默认 true。
// - retentionDays  数据保留天数，白名单 {0,90,180,365}，0=永久，默认 0。
// - theme          界面主题，白名单 {classic,soft,dark,red}，默认 classic。
// - autoStart      开机自启的「期望状态」镜像字段，默认 true。注意：自启的
//                  真理在注册表（HKCU Run 值 + HKCU\Software\UseStats 偏好，
//                  见 autostart.h），本字段只是查看器设置页保存的镜像；
//                  记录器热加载不据此改注册表，只有查看器在用户点开关时
//                  才经 autostart 模块同步落注册表。
// - wifiAssist     WiFi 辅助总开关，默认 true：未连接 WLAN 时记录器周期性
//                  调用 WlanScan 刷新可用网络列表（见 wifiassist.h）。
// - wifiAutoConnect 自动连接已保存网络，默认 true；仅在 wifiAssist 开启时
//                  生效，且只连本机已保存过配置文件的网络。
// - mediaNoIdle    看视频/听音频时不判空闲，默认 true：键鼠空闲超过阈值时，
//                  若前台应用同进程树正在出声播放、或系统处于全屏/演示
//                  状态，这一秒仍计入前台（见 mediawatch.h）。解决看
//                  哔哩哔哩/Edge 视频课时人不动被误判空闲的问题。
// - focusGrace     专注应用宽限，默认 true：前台是专注应用（Edge、哔哩哔哩
//                  客户端，名单见 mediawatch.h）时，有效空闲阈值放宽到
//                  15 分钟，覆盖无声阅读/看长文的静止时段。
// - hideSystemApps 排行榜隐藏系统进程，默认 false：纯查看器显示偏好（过滤
//                  名单在页面侧），记录器读到但不据此改变任何采集行为；
//                  数据文件口径不变，关掉开关即恢复显示。
struct UseStatsSettings {
    uint32_t idleSeconds = 180;
    bool backgroundStats = true;
    uint32_t retentionDays = 0;
    std::string theme = "classic";
    bool autoStart = true;
    bool wifiAssist = true;
    bool wifiAutoConnect = true;
    bool mediaNoIdle = true;
    bool focusGrace = true;
    bool hideSystemApps = false;
};

inline bool SettingsValidIdle(uint32_t v) {
    return v == 60 || v == 120 || v == 180 || v == 300 || v == 600;
}
inline bool SettingsValidRetention(uint32_t v) {
    return v == 0 || v == 90 || v == 180 || v == 365;
}
inline bool SettingsValidTheme(const std::string& t) {
    return t == "classic" || t == "soft" || t == "dark" || t == "red";
}

// 把任意来源的设置压回合法域（非法字段回退该项默认值）。
inline UseStatsSettings SettingsSanitize(const UseStatsSettings& in) {
    UseStatsSettings d; // 默认值
    UseStatsSettings out;
    out.idleSeconds = SettingsValidIdle(in.idleSeconds) ? in.idleSeconds : d.idleSeconds;
    out.backgroundStats = in.backgroundStats;
    out.retentionDays = SettingsValidRetention(in.retentionDays) ? in.retentionDays : d.retentionDays;
    out.theme = SettingsValidTheme(in.theme) ? in.theme : d.theme;
    out.autoStart = in.autoStart;
    out.wifiAssist = in.wifiAssist;
    out.wifiAutoConnect = in.wifiAutoConnect;
    out.mediaNoIdle = in.mediaNoIdle;
    out.focusGrace = in.focusGrace;
    out.hideSystemApps = in.hideSystemApps;
    return out;
}

namespace settings_detail {

inline void AppendEscaped(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (uc < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", uc);
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
}

inline void SkipWs(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
        ++pos;
}

// 找 "key": 并把 pos 定位到值开头（settings.json 由本程序自己写出，格式固定；
// 这里只做针对性的极简解析，找不到/格式不对返回 false，调用方回退默认值）。
inline bool FindValuePos(const std::string& s, const char* key, size_t& pos) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    size_t k = s.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = s.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    pos = colon + 1;
    SkipWs(s, pos);
    return true;
}

inline bool ParseBool(const std::string& s, size_t pos, bool& out) {
    if (s.compare(pos, 4, "true") == 0) { out = true; return true; }
    if (s.compare(pos, 5, "false") == 0) { out = false; return true; }
    if (pos < s.size() && (s[pos] == '1' || s[pos] == '0')) {
        out = (s[pos] == '1');
        return true;
    }
    return false;
}

inline bool ParseUInt(const std::string& s, size_t pos, uint32_t& out) {
    SkipWs(s, pos);
    if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return false;
    uint64_t v = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        v = v * 10 + static_cast<uint64_t>(s[pos] - '0');
        if (v > 0xFFFFFFFFull) return false;
        ++pos;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

inline bool ParseString(const std::string& s, size_t pos, std::string& out) {
    SkipWs(s, pos);
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') return true;
        if (c == '\\' && pos < s.size()) {
            char e = s[pos++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    return false;
}

} // namespace settings_detail

// 序列化为固定格式的 JSON（写入前会先过 SettingsSanitize）。
inline std::string SerializeSettings(const UseStatsSettings& in) {
    UseStatsSettings s = SettingsSanitize(in);
    std::string j = "{\n  \"idleSeconds\": ";
    j += std::to_string(s.idleSeconds);
    j += ",\n  \"backgroundStats\": ";
    j += s.backgroundStats ? "true" : "false";
    j += ",\n  \"retentionDays\": ";
    j += std::to_string(s.retentionDays);
    j += ",\n  \"theme\": ";
    settings_detail::AppendEscaped(j, s.theme);
    j += ",\n  \"autoStart\": ";
    j += s.autoStart ? "true" : "false";
    j += ",\n  \"wifiAssist\": ";
    j += s.wifiAssist ? "true" : "false";
    j += ",\n  \"wifiAutoConnect\": ";
    j += s.wifiAutoConnect ? "true" : "false";
    j += ",\n  \"mediaNoIdle\": ";
    j += s.mediaNoIdle ? "true" : "false";
    j += ",\n  \"focusGrace\": ";
    j += s.focusGrace ? "true" : "false";
    j += ",\n  \"hideSystemApps\": ";
    j += s.hideSystemApps ? "true" : "false";
    j += "\n}\n";
    return j;
}

// 解析 settings.json 文本：永不抛异常；缺失/损坏/非法字段回退该项默认值，
// 因此空文件、半截文件、完全损坏的文件都得到一份完整的默认设置。
inline UseStatsSettings ParseSettings(const std::string& json) {
    UseStatsSettings out; // 默认值
    size_t pos = 0;
    uint32_t u = 0;
    bool b = false;
    std::string str;
    if (settings_detail::FindValuePos(json, "idleSeconds", pos) &&
        settings_detail::ParseUInt(json, pos, u) && SettingsValidIdle(u))
        out.idleSeconds = u;
    if (settings_detail::FindValuePos(json, "backgroundStats", pos) &&
        settings_detail::ParseBool(json, pos, b))
        out.backgroundStats = b;
    if (settings_detail::FindValuePos(json, "retentionDays", pos) &&
        settings_detail::ParseUInt(json, pos, u) && SettingsValidRetention(u))
        out.retentionDays = u;
    if (settings_detail::FindValuePos(json, "theme", pos) &&
        settings_detail::ParseString(json, pos, str) && SettingsValidTheme(str))
        out.theme = str;
    if (settings_detail::FindValuePos(json, "autoStart", pos) &&
        settings_detail::ParseBool(json, pos, b))
        out.autoStart = b;
    if (settings_detail::FindValuePos(json, "wifiAssist", pos) &&
        settings_detail::ParseBool(json, pos, b))
        out.wifiAssist = b;
    if (settings_detail::FindValuePos(json, "wifiAutoConnect", pos) &&
        settings_detail::ParseBool(json, pos, b))
        out.wifiAutoConnect = b;
    if (settings_detail::FindValuePos(json, "mediaNoIdle", pos) &&
        settings_detail::ParseBool(json, pos, b))
        out.mediaNoIdle = b;
    if (settings_detail::FindValuePos(json, "focusGrace", pos) &&
        settings_detail::ParseBool(json, pos, b))
        out.focusGrace = b;
    if (settings_detail::FindValuePos(json, "hideSystemApps", pos) &&
        settings_detail::ParseBool(json, pos, b))
        out.hideSystemApps = b;
    return out;
}
