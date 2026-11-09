#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "mediawatch.h"

// IAudioMeterInformation 在本机 MinGW 的 endpointvolume.h 里只有前置声明、
// 查看器的 webview2 compat 头更只有桩，故不包含 endpointvolume.h，
// 在此按 SDK 文档的虚表顺序自声明该接口（方法顺序即虚表顺序，不可改动），
// IID 用文档化的固定值。
struct IAudioMeterInformation : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* pnChannelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 u32ChannelCount,
                                                            float* afPeakValues) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(UINT32* pdwHardwareSupportMask) = 0;
};

namespace {
// 本机 MinGW 的 libuuid 不提供下面这组 GUID 的符号，全部用文档化的固定值自带。
const GUID kCLSIDMMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const GUID kIIDIMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const GUID kIIDAudioSessionManager2 = {
    0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
const GUID kIIDAudioSessionControl2 = {
    0xBFB7FF88, 0x7239, 0x4FC9, {0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D}};
const GUID kIIDAudioMeterInformation = {
    0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

constexpr float kPeakThreshold = 0.02f;          // 出声判定：峰值电平下限
constexpr ULONGLONG kRecheckMs = 2000;           // 检测结果缓存时长
constexpr ULONGLONG kLoudStickyMs = 10000;       // 出声黏性：盖住安静片段

bool g_comTried = false;
ULONGLONG g_lastCheckMs = 0;
uint32_t g_lastPid = 0;
bool g_lastResult = false;
const char* g_lastReason = "";
ULONGLONG g_lastLoudMs = 0;

std::unordered_map<uint32_t, uint32_t> SnapshotParents() {
    std::unordered_map<uint32_t, uint32_t> m;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return m;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            m[static_cast<uint32_t>(pe.th32ProcessID)] =
                static_cast<uint32_t>(pe.th32ParentProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return m;
}

// 默认渲染设备上，是否存在「与 fgPid 同进程树且正在出声」的音频会话。
bool RelatedSessionLoud(DWORD fgPid) {
    if (!g_comTried) {
        g_comTried = true;
        // 失败（如线程已在别的套间）不致命：后续 COM 调用失败会自然降级。
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(kCLSIDMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIIDIMMDeviceEnumerator,
                                reinterpret_cast<void**>(&en))) ||
        en == nullptr)
        return false;
    bool loud = false;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev != nullptr) {
        IAudioSessionManager2* sm = nullptr;
        if (SUCCEEDED(dev->Activate(kIIDAudioSessionManager2, CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&sm))) &&
            sm != nullptr) {
            IAudioSessionEnumerator* se = nullptr;
            if (SUCCEEDED(sm->GetSessionEnumerator(&se)) && se != nullptr) {
                int count = 0;
                se->GetCount(&count);
                struct Cand { DWORD pid; float peak; };
                Cand cands[64];
                int nCand = 0;
                for (int i = 0; i < count && nCand < 64; ++i) {
                    IAudioSessionControl* sc = nullptr;
                    if (FAILED(se->GetSession(i, &sc)) || sc == nullptr) continue;
                    IAudioSessionControl2* sc2 = nullptr;
                    IAudioMeterInformation* meter = nullptr;
                    DWORD pid = 0;
                    float peak = 0.0f;
                    if (SUCCEEDED(sc->QueryInterface(kIIDAudioSessionControl2,
                                                     reinterpret_cast<void**>(&sc2))) &&
                        sc2 != nullptr) {
                        sc2->GetProcessId(&pid);
                        sc2->Release();
                    }
                    if (SUCCEEDED(sc->QueryInterface(kIIDAudioMeterInformation,
                                                     reinterpret_cast<void**>(&meter))) &&
                        meter != nullptr) {
                        meter->GetPeakValue(&peak);
                        meter->Release();
                    }
                    sc->Release();
                    if (pid != 0 && peak >= kPeakThreshold) {
                        cands[nCand].pid = pid;
                        cands[nCand].peak = peak;
                        ++nCand;
                    }
                }
                if (nCand > 0) {
                    std::unordered_map<uint32_t, uint32_t> parents = SnapshotParents();
                    for (int i = 0; i < nCand; ++i) {
                        if (MediaWatch::PidsRelated(parents, cands[i].pid,
                                                     static_cast<uint32_t>(fgPid))) {
                            loud = true;
                            break;
                        }
                    }
                }
                se->Release();
            }
            sm->Release();
        }
        dev->Release();
    }
    en->Release();
    return loud;
}

bool PresentationActive() {
    QUERY_USER_NOTIFICATION_STATE st;
    if (SUCCEEDED(SHQueryUserNotificationState(&st))) {
        return st == QUNS_PRESENTATION_MODE || st == QUNS_RUNNING_D3D_FULL_SCREEN;
    }
    return false;
}

} // namespace

bool MediaWatch::ForegroundMediaActive(uint32_t fgPid, const char** reason) {
    ULONGLONG now = GetTickCount64();
    if (g_lastCheckMs != 0 && g_lastPid == fgPid && now - g_lastCheckMs < kRecheckMs) {
        if (reason != nullptr) *reason = g_lastReason;
        return g_lastResult;
    }
    g_lastCheckMs = now;
    g_lastPid = fgPid;
    bool pres = PresentationActive();
    bool loudNow = RelatedSessionLoud(static_cast<DWORD>(fgPid));
    if (loudNow) g_lastLoudMs = now;
    bool audio = loudNow || (g_lastLoudMs != 0 && now - g_lastLoudMs < kLoudStickyMs);
    g_lastResult = pres || audio;
    g_lastReason = pres ? "presentation" : (audio ? "audio" : "");
    if (reason != nullptr) *reason = g_lastReason;
    return g_lastResult;
}
