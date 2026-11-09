// settings.json 纯逻辑单测：序列化/解析往返不丢字段、非法值回退、
// 损坏/半截文件得到完整默认设置。宿主机原生编译运行：
//   g++ -std=c++17 -o /tmp/settings_test tests/settings_test.cpp && /tmp/settings_test
#include <cassert>
#include <iostream>
#include <string>

#include "../settings.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
    if (!cond) {
        ++g_failures;
        std::cout << "FAIL: " << what << "\n";
    }
}

bool Same(const UseStatsSettings& a, const UseStatsSettings& b) {
    return a.idleSeconds == b.idleSeconds && a.backgroundStats == b.backgroundStats &&
           a.retentionDays == b.retentionDays && a.theme == b.theme &&
           a.autoStart == b.autoStart && a.wifiAssist == b.wifiAssist &&
           a.wifiAutoConnect == b.wifiAutoConnect &&
           a.mediaNoIdle == b.mediaNoIdle && a.focusGrace == b.focusGrace &&
           a.hideSystemApps == b.hideSystemApps;
}

} // namespace

int main() {
    // 1) 默认值往返
    {
        UseStatsSettings d;
        Check(Same(ParseSettings(SerializeSettings(d)), d), "default round-trip");
    }
    // 2) 全部非默认字段往返不丢
    {
        UseStatsSettings s;
        s.idleSeconds = 300;
        s.backgroundStats = false;
        s.retentionDays = 90;
        s.theme = "dark";
        s.autoStart = false;
        s.wifiAssist = false;
        s.wifiAutoConnect = false;
        s.mediaNoIdle = false;
        s.focusGrace = false;
        UseStatsSettings back = ParseSettings(SerializeSettings(s));
        Check(Same(back, s), "full round-trip");
    }
    // 3) 每种白名单值都可往返
    for (uint32_t idle : {60u, 120u, 180u, 300u, 600u}) {
        UseStatsSettings s;
        s.idleSeconds = idle;
        Check(ParseSettings(SerializeSettings(s)).idleSeconds == idle, "idle whitelist");
    }
    for (uint32_t r : {0u, 90u, 180u, 365u}) {
        UseStatsSettings s;
        s.retentionDays = r;
        Check(ParseSettings(SerializeSettings(s)).retentionDays == r, "retention whitelist");
    }
    for (const char* t : {"classic", "soft", "dark", "red"}) {
        UseStatsSettings s;
        s.theme = t;
        Check(ParseSettings(SerializeSettings(s)).theme == t, "theme whitelist");
    }
    // 4) 非法值逐项回退默认，其余字段不受影响
    {
        UseStatsSettings back = ParseSettings(
            "{\"idleSeconds\": 42, \"backgroundStats\": false, "
            "\"retentionDays\": 7, \"theme\": \"neon\", \"autoStart\": false}");
        Check(back.idleSeconds == 180, "bad idle -> default");
        Check(back.backgroundStats == false, "bg kept");
        Check(back.retentionDays == 0, "bad retention -> default");
        Check(back.theme == "classic", "bad theme -> default");
        Check(back.autoStart == false, "autoStart kept");
    }
    // 5) 损坏/空/半截文件 → 完整默认设置，不崩
    {
        Check(Same(ParseSettings(""), UseStatsSettings{}), "empty -> defaults");
        Check(Same(ParseSettings("not json at all {{{"), UseStatsSettings{}), "garbage -> defaults");
        UseStatsSettings half = ParseSettings("{\"idleSeconds\": 600, \"backg");
        Check(half.idleSeconds == 600, "half file: valid prefix kept");
        Check(half.backgroundStats == true, "half file: missing field defaulted");
    }
    // 6) bool 也接受 1/0
    {
        UseStatsSettings back = ParseSettings("{\"backgroundStats\": 0, \"autoStart\": 1}");
        Check(back.backgroundStats == false && back.autoStart == true, "bool 1/0");
    }
    // 8) WiFi 两字段：缺失 → 默认 true；显式 false 保留；非法值 → 默认 true
    {
        UseStatsSettings back = ParseSettings("{}");
        Check(back.wifiAssist == true && back.wifiAutoConnect == true, "wifi missing -> default true");
        UseStatsSettings off = ParseSettings("{\"wifiAssist\": false, \"wifiAutoConnect\": 0}");
        Check(off.wifiAssist == false && off.wifiAutoConnect == false, "wifi explicit false kept");
        UseStatsSettings bad = ParseSettings("{\"wifiAssist\": \"yes\", \"wifiAutoConnect\": null}");
        Check(bad.wifiAssist == true && bad.wifiAutoConnect == true, "wifi bad value -> default true");
    }
    // 9) 媒体豁免两字段：缺失 → 默认 true；显式 false 保留；非法值 → 默认 true
    {
        UseStatsSettings back = ParseSettings("{}");
        Check(back.mediaNoIdle == true && back.focusGrace == true, "media missing -> default true");
        UseStatsSettings off = ParseSettings("{\"mediaNoIdle\": false, \"focusGrace\": 0}");
        Check(off.mediaNoIdle == false && off.focusGrace == false, "media explicit false kept");
        UseStatsSettings bad = ParseSettings("{\"mediaNoIdle\": \"yes\", \"focusGrace\": null}");
        Check(bad.mediaNoIdle == true && bad.focusGrace == true, "media bad value -> default true");
    }
    // 10) hideSystemApps：缺失 → 默认 false；显式 true 保留且往返一致；非法值 → 默认 false
    {
        UseStatsSettings back = ParseSettings("{}");
        Check(back.hideSystemApps == false, "hideSys missing -> default false");
        UseStatsSettings on = ParseSettings("{\"hideSystemApps\": true}");
        Check(on.hideSystemApps == true, "hideSys explicit true kept");
        UseStatsSettings bad = ParseSettings("{\"hideSystemApps\": \"yes\"}");
        Check(bad.hideSystemApps == false, "hideSys bad value -> default false");
        UseStatsSettings s;
        s.hideSystemApps = true;
        Check(Same(ParseSettings(SerializeSettings(s)), SettingsSanitize(s)),
              "hideSys serialize/parse round-trip");
    }
    // 7) SerializeSettings 写出前会消毒
    {
        UseStatsSettings s;
        s.idleSeconds = 999;
        std::string j = SerializeSettings(s);
        Check(j.find("\"idleSeconds\": 180") != std::string::npos, "serialize sanitizes");
    }

    if (g_failures) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "OK: settings serialize/parse round-trip, whitelist and fallback all pass\n";
    return 0;
}
