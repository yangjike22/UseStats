#pragma once
#include <string>

class Tracker;

// 当天日期字符串，格式 YYYY-MM-DD（本地时间）。
std::wstring TodayString();

// 数据目录解析（启动时定死，运行中不变）：exe 同目录下若已有名为
// record 的文件夹，数据（usage-*.json 与 settings.json）就存进 record\；
// 否则维持 exe 同目录。用户把数据收进子文件夹只需建好 record 再启动，
// 程序不主动创建它——避免在不想要的地方凭空多出文件夹。
std::wstring ResolveDataDir(const std::wstring& exeDir);

// 数据落盘：DataDir() 下的 usage-YYYY-MM-DD.json。
// Save 为原子写（先写 .tmp 再改名覆盖）；Load 用于启动时续记当天已有数据。
// 注意区分：ExeDir() 只用于定位 exe 本身（自启路径、拉起同目录的另一个
// exe、WebView2Loader.dll、wv2data、诊断日志），数据一律走 DataDir()。
class Storage {
public:
    explicit Storage(std::wstring exeDir)
        : exeDir_(std::move(exeDir)), dataDir_(ResolveDataDir(exeDir_)) {}

    const std::wstring& ExeDir() const { return exeDir_; }
    const std::wstring& DataDir() const { return dataDir_; }
    std::wstring FilePathFor(const std::wstring& dateStr) const;

    bool Save(const Tracker& tracker, const std::wstring& dateStr) const;
    bool Load(Tracker& tracker, const std::wstring& dateStr) const;

private:
    std::wstring exeDir_;
    std::wstring dataDir_;
};
