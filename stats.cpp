// 统计窗口实现：WebView2 宿主 + 数据拼装 + 设置页消息处理（查看器专用）。
//
// 双程序架构下本文件只属于查看器 UseStats.exe：它不采集、不碰采集
// 互斥量，数据全部读记录器落盘的 usage-*.json；设置页的读写经
// WebMessage 通道走 settings_io（settings.json）与 autostart（注册表）。
//
// Loader 的关键约束：WebView2Loader.dll 在首次运行前并不存在于 exe 旁边，
// 若用导入库静态依赖它，exe 在首次启动时就会因缺 DLL 而无法加载。
// 因此这里全程动态加载：DLL 以资源形式内嵌，首次打开统计窗口时释放到
// 程序目录，再 LoadLibrary + GetProcAddress 取两个入口函数。
//
// 页面加载方式（2026-11-05 白屏修复）：不再写 stats_view.html/stats_data.js
// 再用 file:// 导航，而是把数据 JSON 直接内嵌进页面 HTML 字符串，用
// NavigateToString 加载——与程序目录路径（中文/空格）完全无关，也不受
// 目录写权限影响。全链路关键节点写 stats_debug.log：运行时版本探测、
// 环境/控制器创建 HRESULT、数据文件数、注入字节数、导航完成状态、
// 页面回传的渲染结果；另有 8 秒看门狗：导航迟迟不完成就切原生兜底文字，
// 保证窗口永远不会停在空白底色上。
// 若本机没有 WebView2 运行时（Edge 自带），窗口退化为原生中文说明文字，
// 不崩溃、不白屏，采集不受影响。
//
// 第二轮白屏修复（2026-11-05，凭 stats_debug.log 定位）：日志显示
// "env completed hr=0x00000000" 之后再无下文。根因是时序：WM_CREATE
// 在 CreateWindowExW 返回前发出，而 g_hwnd 要等返回后才被 Open()
// 赋值；运行时又恰好在 create 调用内同步回调 env 完成 handler，于是
// handler 的守卫因 g_hwnd==nullptr 命中、CreateCoreWebView2Controller
// 从未被调用，看门狗定时器也因同一判断从未创建，ShowFallback 则在
// 写日志前静默返回——确定性复现、零日志。修复：WM_CREATE 首行先
// g_hwnd = hwnd；各守卫/兜底路径一律先写日志；兜底改由宿主 WM_PAINT
// 用 GDI 绘制，不再依赖子控件。
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM（无边框命中测试用）

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas" // WebView2.h 里的 MSVC 专属 pragma
#endif
#include <WebView2.h>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <shellapi.h>
#include <shlwapi.h>

#include "applog.h"
#include "appinfo.h"
#include "autostart.h"
#include "resource.h"
#include "settings_io.h"
#include "stats.h"
#include "storage.h"
#include "tracker.h"

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#endif

namespace {

constexpr wchar_t kStatsClass[] = L"UseStatsStatsWindow";
constexpr UINT_PTR kWatchdogTimer = 2;       // 导航看门狗（8 秒）
constexpr DWORD kWatchdogMs = 8000;
constexpr size_t kDataAlwaysDays = 35;       // 最近这么多天的数据无条件内嵌
constexpr size_t kDataMaxBytes = 900 * 1024; // 内嵌数据 JSON 的字节上限（NavigateToString 有长度上限）

HINSTANCE g_inst = nullptr;
const Storage* g_storage = nullptr;
const Tracker* g_tracker = nullptr; // 双程序架构后恒为 nullptr（查看器不采集），仅保留接口兼容

// 查看器持有的当前设置（Init 时从 settings.json 读入，设置页改动后同步更新）
UseStatsSettings g_settings;

// 顶部条「今日使用最多」的缓存：查看器没有实时前台信息源，改为读记录器
// 最新落盘的当日文件取 fg 最大应用，5 秒内复用缓存避免每秒读盘。
std::wstring g_nowDate;
ULONGLONG g_nowReadTick = 0;
bool g_nowValid = false;
std::wstring g_nowName;
uint64_t g_nowFg = 0;

HWND g_hwnd = nullptr;
bool g_fallbackActive = false; // 兜底说明是否正在显示（由宿主 WM_PAINT 用 GDI 绘制，不依赖子控件）

void DebugLog(const std::string& msg); // 实现在本文件后部（转调 applog）

// ---------- 无边框窗口（2026-11-09 晚，用户要求 macOS 式精致感） ----------
// 系统标题栏/边框由宿主接管：窗口以 WS_POPUP | WS_THICKFRAME 建窗（保留
// 系统缩放、贴边吸附与阴影的资格），WM_NCCALCSIZE 把客户区撑满整窗、
// WM_NCHITTEST 自报缩放边（仅兜底：真机实证 WebView 子窗口会盖住它，
// 实际缩放走页面边缘手柄 win:resize，见 WebMessageHandler）；圆角与阴影交给 DWM（Win11 的
// DWMWA_WINDOW_CORNER_PREFERENCE=ROUND + 1px 框架延伸），不可用时退回
// 窗口区域圆角。页面顶部带绘制 macOS 红绿灯（红=收起/黄=最小化/绿=最大
// 化），经 WebMessage 通道发 win:close/min/zoom/drag 回宿主执行。
// dwmapi 一律动态加载，不给导入表添新 DLL（与 WebView2Loader 同策略）。
bool g_roundViaRegion = false; // DWM 圆角不可用（Win10）时改用区域圆角
bool g_winStateSent = false;   // 已回传给页面的最大化状态（变化时才再发）
bool g_winStateMax = false;

using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
using DwmExtendFn = HRESULT(WINAPI*)(HWND, const void*);
struct DwmMargins { int left, right, top, bottom; };
DwmSetAttrFn g_dwmSetAttr = nullptr;
DwmExtendFn g_dwmExtend = nullptr;

void LoadDwmApi() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) { DebugLog("frame: dwmapi.dll load failed"); return; }
    g_dwmSetAttr = reinterpret_cast<DwmSetAttrFn>(
        reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    g_dwmExtend = reinterpret_cast<DwmExtendFn>(
        reinterpret_cast<void*>(GetProcAddress(dwm, "DwmExtendFrameIntoClientArea")));
    if (g_dwmSetAttr == nullptr || g_dwmExtend == nullptr)
        DebugLog("frame: dwmapi entry points missing");
}

void UpdateRoundRegion(HWND hwnd) {
    if (!g_roundViaRegion) return;
    if (IsZoomed(hwnd)) { SetWindowRgn(hwnd, nullptr, TRUE); return; }
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return;
    const int w = static_cast<int>(wr.right - wr.left);
    const int h = static_cast<int>(wr.bottom - wr.top);
    if (w <= 0 || h <= 0) return;
    // 圆角直径 20px（半径 10，与页面卡片圆角同量级）
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 20, 20);
    if (rgn != nullptr) SetWindowRgn(hwnd, rgn, TRUE);
}

void ApplyWindowFrame(HWND hwnd) {
    LoadDwmApi();
    if (g_dwmSetAttr != nullptr) {
        // DWMWA_WINDOW_CORNER_PREFERENCE = 33，DWMWCP_ROUND = 2
        DWORD pref = 2;
        HRESULT hr = g_dwmSetAttr(hwnd, 33, &pref, sizeof(pref));
        char buf[96];
        std::snprintf(buf, sizeof(buf), "frame: dwm round corners hr=0x%08lX",
                      static_cast<unsigned long>(hr));
        DebugLog(buf);
        if (FAILED(hr)) {
            g_roundViaRegion = true;
            UpdateRoundRegion(hwnd);
            DebugLog("frame: corner preference unsupported; region fallback on");
        }
    } else {
        g_roundViaRegion = true;
        UpdateRoundRegion(hwnd);
        DebugLog("frame: no dwm; region fallback on");
    }
    if (g_dwmExtend != nullptr) {
        // 底部延伸 1px 唤起 DWM 投影（无边框窗口的阴影开关）
        DwmMargins m{0, 0, 0, 1};
        HRESULT hr = g_dwmExtend(hwnd, &m);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "frame: dwm extend (shadow) hr=0x%08lX",
                      static_cast<unsigned long>(hr));
        DebugLog(buf);
    }
}

// 页面红绿灯的「拖动」：交给系统标题栏拖拽回路；最大化时先按光标在窗宽
// 中的比例还原再拖，与真实标题栏的手感一致。
void DoCaptionDrag() {
    HWND hwnd = g_hwnd;
    if (hwnd == nullptr) return;
    if (IsZoomed(hwnd)) {
        POINT pt;
        GetCursorPos(&pt);
        RECT wr;
        GetWindowRect(hwnd, &wr);
        double ratio = (wr.right > wr.left)
            ? static_cast<double>(pt.x - wr.left) / (wr.right - wr.left) : 0.5;
        ShowWindow(hwnd, SW_RESTORE);
        RECT nr;
        GetWindowRect(hwnd, &nr);
        int w = static_cast<int>(nr.right - nr.left);
        SetWindowPos(hwnd, nullptr, pt.x - static_cast<int>(w * ratio), pt.y - 18,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);
        DebugLog("frame: drag from maximized; restored under cursor");
    }
    ReleaseCapture();
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

// 托盘整合（2026-11-06 晚）：全系统只有一个托盘图标，归记录程序所有，
// 查看器不再注册自己的图标。两边靠注册消息沟通（记录器 recorder.cpp
// 里同名注册，ID 必然一致）：
//   UseStats.ViewerShow —— 显示/唤出本窗口（走 StatsWindow::Open，含从
//                          红 X 收起的隐藏状态唤出）；
//   UseStats.ViewerQuit —— 真正结束查看器进程（记录器菜单「关闭查看器」
//                          发来，收到后 DestroyWindow 绕过 WM_CLOSE）。
// 点窗口红 X 仍然只是隐藏窗口（不销毁、WebView2 保持存活），把窗口
// 找回来的入口就是记录器那唯一的托盘图标；隐藏时还会顺手探一次记录
// 程序存活（见 WM_CLOSE），保证那个图标一定在场，不会 stranded。
UINT ViewerShowMsg() {
    static UINT msg = RegisterWindowMessageW(L"UseStats.ViewerShow");
    return msg;
}
UINT ViewerQuitMsg() {
    static UINT msg = RegisterWindowMessageW(L"UseStats.ViewerQuit");
    return msg;
}

constexpr wchar_t kRecorderMutexName[] = L"Local\\UseStats_SingleInstance"; // 与 recorder.cpp 一致

// 兜底文案：WebView2 链路任一环节失败/超时时显示在统计窗口里
constexpr wchar_t kFallbackText[] =
    L"无法显示统计页面：WebView2 页面未能加载。\r\n\r\n"
    L"数据仍在正常记录中（程序目录下的 usage-*.json 文件）。\r\n"
    L"若这台电脑没有 Microsoft Edge WebView2 运行时，安装 Microsoft Edge\r\n"
    L"浏览器或 WebView2 运行时之后，重新打开本窗口即可查看统计。\r\n\r\n"
    L"排查时请把程序目录下的 stats_debug.log 发回，并可通过托盘右键菜单\r\n"
    L"「打开数据文件夹」查看原始数据文件。";
ICoreWebView2Controller* g_controller = nullptr;
ICoreWebView2* g_webview = nullptr;
ICoreWebView2Environment* g_env = nullptr; // 供字体资源应答 CreateWebResourceResponse
HMODULE g_loaderDll = nullptr;
bool g_navCompleted = false; // 最新一代导航是否已收到 NavigationCompleted
// 导航代际（2026-11-06 晚竞态修复）：每次 NavigateToString 发出代际 +1；
// NavigationCompleted 按发出顺序到达，第 N 个完成回调对应第 N 代。旧代
// 被新代取代后，其「被取消」完成（webError=9 OperationCanceled）不是
// 加载失败，绝不能杀看门狗/置完成标志/触发兜底——曾有真机事故：用户
// 双击托盘图标连发两次导航、互相取消，取消被误判失败切了兜底，而兜底
// 又是单向门（激活后 Open 不再导航），只能重启进程恢复（见完成回调）。
// 三个计数都与当前 webview 实例绑定：新控制器到达与拆卸时清零。
int g_navGen = 0;           // 已发出的导航代际
int g_navDoneGen = 0;       // 已收到完成回调的代际数
bool g_navInFlight = false; // 最新一代导航尚未完成（Open 防重入用）

typedef HRESULT(STDAPICALLTYPE* PFN_CreateEnvWithOptions)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
typedef HRESULT(STDAPICALLTYPE* PFN_GetBrowserVersion)(PCWSTR, LPWSTR*);

PFN_CreateEnvWithOptions g_createEnv = nullptr;

// ---------- 小工具 ----------

std::string ToUtf8(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

// ---------- 诊断日志：实现已抽到共享模块 applog.cpp，这里只留转调 ----------
// （AppLogInit 在 StatsWindow::Init 里以程序目录调用。）

void DebugLog(const std::string& msg) { UseStatsDebugLog(msg); }

void DebugLogHr(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s hr=0x%08lX", what, static_cast<unsigned long>(hr));
    DebugLog(buf);
}

} // namespace

namespace {

// 与 storage.cpp 相同的转义规则（UTF-8 字节级处理），另把 '<' 转义，
// 保证 JSON 内嵌进 <script> 时不会被 "</script>" 之类字节序列截断
void AppendJsonString(std::string& out, const std::wstring& ws) {
    std::string u8 = ToUtf8(ws);
    out += '"';
    for (char c : u8) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '<') out += "\\u003c";
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

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) &&
              written == size;
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// 取内嵌资源字节；不存在返回 false
bool ResourceBytes(UINT id, const BYTE*& data, DWORD& size) {
    HRSRC res = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (res == nullptr) return false;
    HGLOBAL loaded = LoadResource(g_inst, res);
    if (loaded == nullptr) return false;
    data = static_cast<const BYTE*>(LockResource(loaded));
    size = SizeofResource(g_inst, res);
    return data != nullptr && size > 0;
}

// 把内嵌资源释放到目标路径（已存在且大小一致则跳过）
bool ExtractResource(UINT id, const std::wstring& destPath) {
    const BYTE* data = nullptr;
    DWORD size = 0;
    if (!ResourceBytes(id, data, size)) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(destPath.c_str(), GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow == size && fad.nFileSizeHigh == 0) {
        return true; // 已释放过
    }
    return WriteFileBytes(destPath, data, size);
}

// ---------- COM 事件处理器（手写引用计数，不依赖 WRL） ----------

// WebView2 handler 接口的 IID。MSVC 下 __uuidof(T) 可用，但 MinGW 的
// __uuidof 不认识 SDK 头文件里的这些接口，故 QueryInterface 直接比对 IID
// 常量（数值取自 WebView2.h 中各接口的 MIDL_INTERFACE 声明，两边通用，
// 已与 SDK 1.0.4314.35 的头文件逐一核对一致）。
const GUID kIidEnvCompleted = {0x4e8a3389, 0xc9d8, 0x4bd2,
                               {0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d}};
const GUID kIidCtrlCompleted = {0x6c4819f3, 0xc9b7, 0x4260,
                                {0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c}};
const GUID kIidWebMessage = {0x57213f19, 0x00e6, 0x49fa,
                             {0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2}};
const GUID kIidNavCompleted = {0xd33a35bf, 0x1c49, 0x4f98,
                               {0x93, 0xab, 0x00, 0x6e, 0x05, 0x33, 0xfe, 0x1c}};
const GUID kIidProcessFailed = {0x79e0aea4, 0x990b, 0x42d9,
                                {0xaa, 0x1d, 0x0f, 0xcc, 0x2e, 0x5b, 0xc7, 0xf1}};
const GUID kIidWebResourceReq = {0xab00b74c, 0x15f1, 0x4646,
                                 {0x80, 0xe8, 0xe7, 0x63, 0x41, 0xd2, 0x5d, 0x71}};

template <typename T>
class ComHandler : public T {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

protected:
    virtual ~ComHandler() = default;

private:
    ULONG ref_ = 1;
};

#define USESTATS_IMPLEMENT_QI(IIDCONST)                                     \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override { \
        if (riid == __uuidof(IUnknown) || riid == IIDCONST) {                \
            *ppv = this;                                                    \
            AddRef();                                                       \
            return S_OK;                                                    \
        }                                                                   \
        *ppv = nullptr;                                                     \
        return E_NOINTERFACE;                                               \
    }

void ShowFallback();
void NavigateStatsPage();
void ResizeContent();

// 设置通道（实现在数据拼装一节，依赖 EnumerateDates 等后续定义）
std::string BuildSettingsJson();
void PostUtf8Json(ICoreWebView2* sender, const std::string& utf8);
void HandleSettingsSet(const std::string& jsonUtf8, ICoreWebView2* sender);
void RefreshNowCache();

// 把窗口最大化状态回传页面（页面据此隐藏缩放手柄）；只在状态变化时发。
void NotifyWinState() {
    if (g_hwnd == nullptr || g_webview == nullptr) return;
    bool maxed = IsZoomed(g_hwnd) != FALSE;
    if (g_winStateSent && g_winStateMax == maxed) return;
    g_winStateSent = true;
    g_winStateMax = maxed;
    PostUtf8Json(g_webview, maxed ? "{\"type\":\"win-state\",\"max\":true}"
                                  : "{\"type\":\"win-state\",\"max\":false}");
}

class WebMessageHandler : public ComHandler<ICoreWebView2WebMessageReceivedEventHandler> {
public:
    USESTATS_IMPLEMENT_QI(kIidWebMessage)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) override {
        LPWSTR raw = nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&raw)) || raw == nullptr) return S_OK;
        std::wstring msg(raw);
        CoTaskMemFree(raw);

        // 页面诊断回传："log:<文本>" → 写进 stats_debug.log
        if (msg.compare(0, 4, L"log:") == 0) {
            DebugLog("page " + ToUtf8(msg.substr(4)));
            return S_OK;
        }

        // 设置页：拉取当前设置（autoStart 以注册表现读为准）
        if (msg == L"settings:get") {
            PostUtf8Json(sender, BuildSettingsJson());
            return S_OK;
        }
        // 设置页：保存完整设置对象（"settings:set:" + JSON）
        if (msg.compare(0, 13, L"settings:set:") == 0) {
            HandleSettingsSet(ToUtf8(msg.substr(13)), sender);
            return S_OK;
        }
        // 设置页数据管理：打开数据文件夹（数据目录，可能是 record 子文件夹）
        if (msg == L"open-data-dir") {
            if (g_storage != nullptr)
                ShellExecuteW(g_hwnd, L"open", g_storage->DataDir().c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            return S_OK;
        }
        // 设置页数据管理：复制数据目录路径（"copy-text:" + 文本）→ 系统剪贴板，
        // 结果以 {"type":"copy-result"} 回执页面，页面据此显示「已复制」。
        if (msg.compare(0, 10, L"copy-text:") == 0) {
            const std::wstring text = msg.substr(10);
            bool ok = false;
            if (OpenClipboard(g_hwnd)) {
                EmptyClipboard();
                const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (h != nullptr) {
                    void* dst = GlobalLock(h);
                    if (dst != nullptr) {
                        const wchar_t* src = text.c_str();
                        for (size_t i = 0; i <= text.size(); i++)
                            static_cast<wchar_t*>(dst)[i] = src[i];
                        GlobalUnlock(h);
                        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
                        if (!ok) GlobalFree(h); // 成功时所有权转给系统，不再释放
                    } else {
                        GlobalFree(h);
                    }
                }
                CloseClipboard();
            }
            DebugLog(ok ? "clipboard: copy-text ok" : "clipboard: copy-text FAILED");
            PostUtf8Json(sender, ok ? "{\"type\":\"copy-result\",\"ok\":true}"
                                    : "{\"type\":\"copy-result\",\"ok\":false}");
            return S_OK;
        }

        // 无边框窗口的红绿灯与拖动（页面顶部带发出，宿主执行）：
        // 红点与系统红 X 同语义——走 WM_CLOSE 收起逻辑，不真退出。
        if (msg == L"win:close") {
            DebugLog("frame: close via page lights");
            if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            return S_OK;
        }
        if (msg == L"win:min") {
            DebugLog("frame: minimize via page lights");
            if (g_hwnd != nullptr) ShowWindow(g_hwnd, SW_MINIMIZE);
            return S_OK;
        }
        if (msg == L"win:zoom") {
            if (g_hwnd != nullptr) {
                bool zoomed = IsZoomed(g_hwnd) != FALSE;
                ShowWindow(g_hwnd, zoomed ? SW_RESTORE : SW_MAXIMIZE);
                DebugLog(zoomed ? "frame: restore via page lights"
                                : "frame: maximize via page lights");
            }
            return S_OK;
        }
        if (msg == L"win:drag") {
            DoCaptionDrag();
            return S_OK;
        }
        // 页面边缘缩放手柄（win:resize:T/B/L/R/TL/TR/BL/BR）：宿主的
        // WM_NCHITTEST 被 WebView 子窗口整体盖住、真机上根本到不了，缩放
        // 改走页面手柄——按下即以对应 HT 码进入系统缩放回路，后续拖动由
        // 系统接管（与真实边框拖拽同一套机制）。最大化时页面已隐藏手柄，
        // 这里再兜一层。
        if (msg.compare(0, 11, L"win:resize:") == 0 && g_hwnd != nullptr &&
            !IsZoomed(g_hwnd)) {
            const std::wstring dir = msg.substr(11);
            WPARAM ht = 0;
            if (dir == L"L") ht = HTLEFT;
            else if (dir == L"R") ht = HTRIGHT;
            else if (dir == L"T") ht = HTTOP;
            else if (dir == L"B") ht = HTBOTTOM;
            else if (dir == L"TL") ht = HTTOPLEFT;
            else if (dir == L"TR") ht = HTTOPRIGHT;
            else if (dir == L"BL") ht = HTBOTTOMLEFT;
            else if (dir == L"BR") ht = HTBOTTOMRIGHT;
            if (ht != 0) {
                DebugLog("frame: resize via page edge dir=" + ToUtf8(dir));
                POINT pt;
                GetCursorPos(&pt);
                ReleaseCapture();
                SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, ht,
                             MAKELPARAM(pt.x, pt.y));
            }
            return S_OK;
        }

        if (msg != L"now") return S_OK;

        // 顶部条：今日用时最长的应用（数据源为记录器最新落盘的当日文件，
        // ≤60 秒延迟；查看器自身不采集，见 RefreshNowCache）。
        RefreshNowCache();
        std::string j = "{\"type\":\"now\",\"name\":";
        if (g_nowValid) {
            AppendJsonString(j, g_nowName);
            j += ",\"fgToday\":" + std::to_string(g_nowFg) + "}";
        } else {
            j += "null,\"fgToday\":0}";
        }
        PostUtf8Json(sender, j);
        return S_OK;
    }
};

class NavCompletedHandler : public ComHandler<ICoreWebView2NavigationCompletedEventHandler> {
public:
    USESTATS_IMPLEMENT_QI(kIidNavCompleted)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2NavigationCompletedEventArgs* args) override {
        BOOL success = FALSE;
        COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        if (args != nullptr) {
            args->get_IsSuccess(&success);
            args->get_WebErrorStatus(&status);
        }
        // 完成回调按导航发出顺序到达：本次回调对应第 ++g_navDoneGen 代。
        // 若它后面已经发出了更新一代（doneGen < g_navGen），本次结果已被
        // 取代——尤其「被新导航取消」的 OperationCanceled 不是失败：直接
        // 忽略，不动看门狗（新一代自有计时）、不置完成标志、不触发兜底。
        int doneGen = ++g_navDoneGen;
        if (doneGen < g_navGen) {
            char buf[128];
            if (!success &&
                status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                std::snprintf(buf, sizeof(buf),
                              "nav gen=%d canceled (superseded); ignored", doneGen);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "nav gen=%d completed (superseded); ignored", doneGen);
            }
            DebugLog(buf);
            return S_OK;
        }
        g_navInFlight = false;
        g_navCompleted = true;
        if (g_hwnd != nullptr) KillTimer(g_hwnd, kWatchdogTimer);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "nav completed gen=%d success=%d webError=%d",
                      doneGen, success ? 1 : 0, static_cast<int>(status));
        DebugLog(buf);
        if (!success) {
            ShowFallback();
        } else if (g_fallbackActive) {
            // 导航最终成功：兜底标志清掉（webview 子窗口本已覆盖客户区，
            // 这里只是不留残留状态）
            g_fallbackActive = false;
            if (g_hwnd != nullptr) InvalidateRect(g_hwnd, nullptr, TRUE);
            DebugLog("fallback cleared; navigation completed");
        }
        if (success) {
            // 新页面实例没有窗口状态记忆：强制重发一次最大化状态
            g_winStateSent = false;
            NotifyWinState();
        }
        return S_OK;
    }
};

class ProcessFailedHandler : public ComHandler<ICoreWebView2ProcessFailedEventHandler> {
public:
    USESTATS_IMPLEMENT_QI(kIidProcessFailed)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2ProcessFailedEventArgs* args) override {
        COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
        if (args != nullptr) args->get_ProcessFailedKind(&kind);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "webview process failed kind=%d",
                      static_cast<int>(kind));
        DebugLog(buf);
        if (g_hwnd != nullptr) KillTimer(g_hwnd, kWatchdogTimer);
        ShowFallback();
        return S_OK;
    }
};

// 内嵌字体本地供给：页面 @font-face 向虚拟主机 usestats.local 取
// Noto Sans SC 四个字重的子集 woff2，这里按文件名查表、以 exe 资源字节
// 直接应答——页面与程序全程零网络请求；某个字重没取到时页面按字体栈与
// 字重匹配回退，不影响功能。日志逐个字重打行，便于真机核对供给是否生效。
class FontResourceHandler : public ComHandler<ICoreWebView2WebResourceRequestedEventHandler> {
public:
    USESTATS_IMPLEMENT_QI(kIidWebResourceReq)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2WebResourceRequestedEventArgs* args) override {
        if (args == nullptr || g_env == nullptr) return S_OK;
        ICoreWebView2WebResourceRequest* req = nullptr;
        if (FAILED(args->get_Request(&req)) || req == nullptr) return S_OK;
        LPWSTR uriRaw = nullptr;
        req->get_Uri(&uriRaw);
        std::wstring uri = uriRaw != nullptr ? uriRaw : L"";
        if (uriRaw != nullptr) CoTaskMemFree(uriRaw);
        req->Release();
        const std::wstring kPrefix = L"https://usestats.local/fonts/";
        if (uri.compare(0, kPrefix.size(), kPrefix) != 0) return S_OK;
        const std::wstring file = uri.substr(kPrefix.size());
        struct FontRes { const wchar_t* name; int id; };
        static const FontRes kFonts[] = {
            { L"NotoSansSC-400.woff2", IDR_FONT_NSC_400 },
            { L"NotoSansSC-500.woff2", IDR_FONT_NSC_500 },
            { L"NotoSansSC-600.woff2", IDR_FONT_NSC_600 },
            { L"NotoSansSC-700.woff2", IDR_FONT_NSC_700 },
        };
        int resId = 0;
        for (const FontRes& f : kFonts) if (file == f.name) { resId = f.id; break; }
        if (resId == 0) return S_OK;
        const BYTE* data = nullptr;
        DWORD size = 0;
        if (!ResourceBytes(resId, data, size)) {
            DebugLog("font resource: embedded woff2 missing");
            return S_OK;
        }
        IStream* stream = SHCreateMemStream(data, size);
        if (stream == nullptr) return S_OK;
        ICoreWebView2WebResourceResponse* resp = nullptr;
        HRESULT hr = g_env->CreateWebResourceResponse(
            stream, 200, L"OK",
            L"Content-Type: font/woff2\r\nAccess-Control-Allow-Origin: *\r\n"
            L"Cache-Control: max-age=31536000",
            &resp);
        stream->Release();
        if (SUCCEEDED(hr) && resp != nullptr) {
            args->put_Response(resp);
            resp->Release();
            std::string narrow(file.begin(), file.end());
            DebugLog(("font resource: served embedded " + narrow).c_str());
        } else {
            DebugLogHr("font resource: CreateWebResourceResponse", hr);
        }
        return S_OK;
    }
};

class ControllerHandler : public ComHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    USESTATS_IMPLEMENT_QI(kIidCtrlCompleted)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        DebugLogHr("controller completed", result);
        if (FAILED(result) || controller == nullptr || g_hwnd == nullptr) {
            ShowFallback();
            return S_OK;
        }
        g_controller = controller;
        g_controller->AddRef();
        g_controller->put_IsVisible(TRUE);
        ResizeContent();
        HRESULT hrWeb = g_controller->get_CoreWebView2(&g_webview);
        DebugLogHr("get_CoreWebView2", hrWeb);
        if (FAILED(hrWeb) || g_webview == nullptr) {
            ShowFallback();
            return S_OK;
        }
        // 新 webview 实例：导航代际从零重新计——完成回调的计数顺序只在
        // 同一个实例内成立，旧实例未达的回调不会在这个实例上补发。
        g_navGen = 0;
        g_navDoneGen = 0;
        g_navInFlight = false;
        g_navCompleted = false;
        EventRegistrationToken token{};
        auto* msgHandler = new WebMessageHandler();
        g_webview->add_WebMessageReceived(msgHandler, &token);
        msgHandler->Release();
        auto* navHandler = new NavCompletedHandler();
        g_webview->add_NavigationCompleted(navHandler, &token);
        navHandler->Release();
        auto* failHandler = new ProcessFailedHandler();
        g_webview->add_ProcessFailed(failHandler, &token);
        failHandler->Release();
        // 内嵌字体本地供给（虚拟主机 usestats.local，见 FontResourceHandler）
        auto* fontHandler = new FontResourceHandler();
        g_webview->AddWebResourceRequestedFilter(
            L"https://usestats.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        g_webview->add_WebResourceRequested(fontHandler, &token);
        fontHandler->Release();

        // 若看门狗已先切了兜底（链路曾经卡住又恢复），撤掉兜底再导航（自愈）
        if (g_fallbackActive) {
            g_fallbackActive = false;
            if (g_hwnd != nullptr) InvalidateRect(g_hwnd, nullptr, TRUE);
            DebugLog("fallback cleared; controller arrived late, navigating");
        }
        NavigateStatsPage();
        return S_OK;
    }
};

class EnvHandler : public ComHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    USESTATS_IMPLEMENT_QI(kIidEnvCompleted)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
        DebugLogHr("env completed", result);
        if (FAILED(result) || env == nullptr || g_hwnd == nullptr) {
            // 守卫分支必须留痕：上一轮白屏正是死在这里且全程静默——
            // WM_CREATE 期间 g_hwnd 尚未被 Open() 赋值（该问题已在
            // StatsWndProc 的 WM_CREATE 首行修复），当时日志恰好停在
            // "env completed" 且再无下文，无法定位。
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "env handler: ABORT result=0x%08lX env=%s g_hwnd=%s",
                          static_cast<unsigned long>(result),
                          env != nullptr ? "ok" : "null",
                          g_hwnd != nullptr ? "ok" : "null");
            DebugLog(buf);
            if (g_hwnd != nullptr) KillTimer(g_hwnd, kWatchdogTimer);
            ShowFallback();
            return S_OK;
        }
        g_env = env;
        g_env->AddRef();
        DebugLog("env handler: creating controller");
        auto* handler = new ControllerHandler();
        HRESULT hr = env->CreateCoreWebView2Controller(g_hwnd, handler);
        handler->Release();
        DebugLogHr("CreateCoreWebView2Controller call", hr);
        if (FAILED(hr)) {
            if (g_hwnd != nullptr) KillTimer(g_hwnd, kWatchdogTimer);
            ShowFallback();
        }
        return S_OK;
    }
};

// ---------- 数据拼装 ----------

// 枚举程序目录下所有 usage-YYYY-MM-DD.json 的日期，升序返回
std::vector<std::wstring> EnumerateDates() {
    std::vector<std::wstring> dates;
    std::wstring pattern = g_storage->DataDir() + L"\\usage-*.json";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return dates;
    do {
        std::wstring name(fd.cFileName);
        // usage-YYYY-MM-DD.json：总长 21，去掉前缀 6 与后缀 5
        if (name.size() != 21) continue;
        std::wstring d = name.substr(6, 10);
        bool ok = d[4] == L'-' && d[7] == L'-';
        for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
            ok = ok && d[static_cast<size_t>(i)] >= L'0' && d[static_cast<size_t>(i)] <= L'9';
        if (ok) dates.push_back(d);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(dates.begin(), dates.end());
    return dates;
}

// 单个日文件 → JSON 对象字符串；读不到返回空串
std::string BuildOneDayJson(const std::wstring& date) {
    Tracker tmp; // 借用 Storage::Load 的解析器读单个日文件
    if (!g_storage->Load(tmp, date)) return std::string();
    std::string js = "{\"date\": ";
    AppendJsonString(js, date);
    js += ", \"idleSeconds\": " + std::to_string(tmp.IdleSeconds());
    js += ", \"apps\": [";
    bool firstApp = true;
    for (const auto& kv : tmp.Apps()) {
        const AppUsage& u = kv.second;
        if (!firstApp) js += ",";
        firstApp = false;
        js += "{\"path\": ";
        AppendJsonString(js, u.path);
        js += ", \"name\": ";
        AppendJsonString(js, u.name.empty() ? AppInfo::FriendlyName(u.path) : u.name);
        js += ", \"fgSeconds\": " + std::to_string(u.fgSeconds);
        js += ", \"bgSeconds\": " + std::to_string(u.bgSeconds);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", u.cpuSeconds);
        js += ", \"cpuSeconds\": ";
        js += buf;
        js += ", \"hourlyFg\": [";
        for (int i = 0; i < 24; ++i) {
            if (i) js += ",";
            js += std::to_string(u.hourlyFg[i]);
        }
        js += "]}";
    }
    js += "]}";
    return js;
}

// 读全部日数据，拼成 USAGE_DATA 的 JSON 对象（不含外层赋值）。
// 天数很多时控制内嵌体积：最近 kDataAlwaysDays 天无条件带上，
// 更早的只带到累计 kDataMaxBytes 为止（页面只用最近 30 天与近 3 个月平均，足够）。
std::string BuildDataJson(size_t& outFound, size_t& outIncluded) {
    std::vector<std::wstring> dates = EnumerateDates();
    std::vector<std::string> days; // 升序
    for (const std::wstring& date : dates) {
        std::string one = BuildOneDayJson(date);
        if (!one.empty()) days.push_back(one);
    }
    outFound = days.size();
    // 从最新往回选：先保最近 kDataAlwaysDays 天，其余按字节预算
    size_t startIdx = 0;
    size_t bytes = 0;
    for (size_t i = days.size(); i-- > 0;) {
        size_t nth = days.size() - i; // 这是第几新（1 = 最新）
        if (nth > kDataAlwaysDays && bytes + days[i].size() > kDataMaxBytes) {
            startIdx = i + 1;
            break;
        }
        bytes += days[i].size();
    }
    outIncluded = days.size() - startIdx;
    std::string js = "{\"today\": ";
    AppendJsonString(js, TodayString());
    js += ", \"days\": [";
    for (size_t i = startIdx; i < days.size(); ++i) {
        if (i > startIdx) js += ",";
        js += "\n";
        js += days[i];
    }
    js += "\n]}";
    return js;
}

// ---------- 设置通道与顶部条缓存 ----------

// 把 UTF-8 JSON 字符串经 WebMessage 通道回给页面
void PostUtf8Json(ICoreWebView2* sender, const std::string& utf8) {
    if (sender == nullptr) return;
    std::wstring wj(utf8.size(), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        wj.data(), static_cast<int>(wj.size()));
    sender->PostWebMessageAsJson(wj.c_str());
}

// 当前设置的 JSON（页面注入与 settings:get 回复共用）：
// autoStart 不取镜像字段，而是现读注册表 Run 值——显示的永远是真状态。
std::string BuildSettingsJson() {
    UseStatsSettings s = SettingsSanitize(g_settings);
    std::string j = "{\"type\":\"settings\"";
    j += ", \"idleSeconds\": " + std::to_string(s.idleSeconds);
    j += ", \"backgroundStats\": ";
    j += s.backgroundStats ? "true" : "false";
    j += ", \"retentionDays\": " + std::to_string(s.retentionDays);
    j += ", \"theme\": ";
    AppendJsonString(j, FromUtf8(s.theme));
    j += ", \"autoStart\": ";
    j += AutoStartRunPresent() ? "true" : "false";
    j += ", \"wifiAssist\": ";
    j += s.wifiAssist ? "true" : "false";
    j += ", \"wifiAutoConnect\": ";
    j += s.wifiAutoConnect ? "true" : "false";
    j += ", \"hideSystemApps\": ";
    j += s.hideSystemApps ? "true" : "false";
    j += ", \"dataDir\": ";
    if (g_storage != nullptr) AppendJsonString(j, g_storage->DataDir());
    else j += "null";
    j += ", \"dataDays\": " + std::to_string(EnumerateDates().size());
    j += "}";
    return j;
}

// 设置页保存：页面发来完整对象 → 解析（白名单校验在 ParseSettings 内）→
// 写 settings.json（原子替换，记录器 60 秒内热加载）；autoStart 另经
// autostart 模块即时落注册表（目标=同目录记录器），不依赖记录器在运行。
void HandleSettingsSet(const std::string& jsonUtf8, ICoreWebView2* sender) {
    if (g_storage == nullptr) {
        PostUtf8Json(sender, "{\"type\":\"settings-saved\",\"ok\":false}");
        return;
    }
    g_settings = ParseSettings(jsonUtf8);
    bool fileOk = SaveSettingsFile(g_storage->DataDir(), g_settings);
    // Run 目标只能经 autostart 的唯一入口决定（同目录记录器），这里不许再拼路径
    AutoStartSet(AutoStartRecorderPath(g_storage->ExeDir()), g_settings.autoStart);
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "settings: saved idle=%us bg=%d retention=%u theme=%s autoStart=%d wifi=%d/%d file=%s",
                  g_settings.idleSeconds, g_settings.backgroundStats ? 1 : 0,
                  g_settings.retentionDays, g_settings.theme.c_str(),
                  g_settings.autoStart ? 1 : 0,
                  g_settings.wifiAssist ? 1 : 0, g_settings.wifiAutoConnect ? 1 : 0,
                  fileOk ? "ok" : "FAILED");
    DebugLog(buf);
    PostUtf8Json(sender, fileOk ? "{\"type\":\"settings-saved\",\"ok\":true}"
                                : "{\"type\":\"settings-saved\",\"ok\":false}");
}

// 顶部条缓存刷新：读当日文件，取 fgSeconds 最大的应用（5 秒内复用）
void RefreshNowCache() {
    ULONGLONG now = GetTickCount64();
    std::wstring today = TodayString();
    if (g_nowDate == today && now - g_nowReadTick < 5000) return;
    g_nowReadTick = now;
    g_nowDate = today;
    g_nowValid = false;
    g_nowName.clear();
    g_nowFg = 0;
    if (g_storage == nullptr) return;
    Tracker tmp;
    if (!g_storage->Load(tmp, today)) return;
    const AppUsage* best = nullptr;
    for (const auto& kv : tmp.Apps()) {
        if (best == nullptr || kv.second.fgSeconds > best->fgSeconds) best = &kv.second;
    }
    if (best != nullptr && best->fgSeconds > 0) {
        g_nowValid = true;
        g_nowName = best->name.empty() ? AppInfo::FriendlyName(best->path) : best->name;
        g_nowFg = best->fgSeconds;
    }
}

// 取内嵌页面模板，把数据 JSON 替换占位符，得到完整 HTML（宽字符串，供 NavigateToString）
std::wstring BuildPageHtml(size_t& outFound, size_t& outIncluded, size_t& outBytes) {
    outFound = outIncluded = outBytes = 0;
    const BYTE* raw = nullptr;
    DWORD rawSize = 0;
    if (!ResourceBytes(IDR_STATS_PAGE, raw, rawSize)) {
        DebugLog("page html: resource missing");
        return std::wstring();
    }
    std::string html(reinterpret_cast<const char*>(raw), rawSize);
    if (html.size() >= 3 && html.compare(0, 3, "\xEF\xBB\xBF") == 0) html.erase(0, 3); // 去 UTF-8 BOM
    const std::string token = "/*USESTATS_DATA*/";
    size_t pos = html.find(token);
    if (pos == std::string::npos) {
        DebugLog("page html: data placeholder token not found");
        return std::wstring();
    }
    html.replace(pos, token.size(), BuildDataJson(outFound, outIncluded));
    const std::string settingsToken = "/*USESTATS_SETTINGS*/";
    size_t spos = html.find(settingsToken);
    if (spos != std::string::npos)
        html.replace(spos, settingsToken.size(), BuildSettingsJson());
    outBytes = html.size();
    return FromUtf8(html);
}

// 用最新数据整页导航（NavigateToString 与路径无关）
void NavigateStatsPage() {
    if (g_webview == nullptr || g_hwnd == nullptr) return;
    size_t found = 0, included = 0, bytes = 0;
    std::wstring html = BuildPageHtml(found, included, bytes);
    char buf[192];
    if (html.empty()) {
        DebugLog("navigate aborted: page html empty");
        ShowFallback();
        return;
    }
    std::snprintf(buf, sizeof(buf),
                  "navigate: usageFiles=%zu daysIncluded=%zu htmlBytes=%zu",
                  found, included, bytes);
    DebugLog(buf);
    int gen = ++g_navGen;
    std::snprintf(buf, sizeof(buf), "nav gen=%d start", gen);
    DebugLog(buf);
    g_navCompleted = false;
    g_navInFlight = true;
    SetTimer(g_hwnd, kWatchdogTimer, kWatchdogMs, nullptr);
    HRESULT hr = g_webview->NavigateToString(html.c_str());
    DebugLogHr("NavigateToString call", hr);
    if (FAILED(hr)) {
        // 同步失败：这一代不会再有完成回调，手动对账以免后续完成回调的
        // 代际计数整体错位一代。
        ++g_navDoneGen;
        g_navInFlight = false;
        KillTimer(g_hwnd, kWatchdogTimer);
        ShowFallback();
    }
}

// ---------- 与记录程序的托盘协作 ----------

// 防 stranded：窗口隐藏（红 X 收起）前确认记录程序在跑——把窗口找回
// 来的唯一入口就是它那只托盘图标，它不在场就等于入口丢了。试开它的
// 采集互斥量只探存在性，拿到立刻关闭、绝不持有（持有会挡住记录器
// 自己的单实例判断）；不在就把同目录记录器拉起来。
void EnsureRecorderAliveOnHide() {
    HANDLE probe = OpenMutexW(SYNCHRONIZE, FALSE, kRecorderMutexName);
    if (probe != nullptr) {
        CloseHandle(probe);
        return;
    }
    if (g_storage == nullptr) {
        DebugLog("viewer: recorder probe skipped (no storage)");
        return;
    }
    std::wstring recorder = g_storage->ExeDir() + L"\\UseStatsRecorder.exe";
    HINSTANCE r = ShellExecuteW(g_hwnd, L"open", recorder.c_str(), nullptr,
                                g_storage->ExeDir().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "viewer: recorder launch on hide FAILED code=%lld",
                      static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
        DebugLog(buf);
    } else {
        DebugLog("viewer: recorder not running on hide; launched recorder");
    }
}

// ---------- WebView2 启动 ----------

// 释放并加载 Loader，探测运行时是否存在；每一步都记日志，失败返回 false
bool EnsureWebView2() {
    if (g_createEnv != nullptr) return true;
    std::wstring dllPath = g_storage->ExeDir() + L"\\WebView2Loader.dll";
    bool extracted = ExtractResource(IDR_WV2_LOADER, dllPath);
    DebugLog(std::string("loader: extract ") + (extracted ? "ok" : "FAILED") +
             " path=" + ToUtf8(dllPath));
    if (!extracted) return false;
    g_loaderDll = LoadLibraryW(dllPath.c_str());
    if (g_loaderDll == nullptr) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "loader: LoadLibrary FAILED gle=%lu",
                      static_cast<unsigned long>(GetLastError()));
        DebugLog(buf);
        return false;
    }
    PFN_GetBrowserVersion pfnVer = nullptr;
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
    pfnVer = reinterpret_cast<PFN_GetBrowserVersion>(
        GetProcAddress(g_loaderDll, "GetAvailableCoreWebView2BrowserVersionString"));
    g_createEnv = reinterpret_cast<PFN_CreateEnvWithOptions>(
        GetProcAddress(g_loaderDll, "CreateCoreWebView2EnvironmentWithOptions"));
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
    if (pfnVer == nullptr || g_createEnv == nullptr) {
        DebugLog("loader: GetProcAddress FAILED");
        return false;
    }
    LPWSTR version = nullptr;
    HRESULT hr = pfnVer(nullptr, &version);
    std::string verUtf8 = version != nullptr ? ToUtf8(version) : std::string("(null)");
    char buf[256];
    std::snprintf(buf, sizeof(buf), "runtime probe hr=0x%08lX version=%s",
                  static_cast<unsigned long>(hr), verUtf8.c_str());
    DebugLog(buf);
    if (version != nullptr) CoTaskMemFree(version);
    if (FAILED(hr) || version == nullptr) return false; // 运行时未安装
    return true;
}

void StartWebView() {
    DebugLog("StartWebView");
    // 看门狗从链路一启动就计时：环境/控制器/导航任一环节挂起都会切兜底。
    // 必须留下日志证明定时器真的建上了——上一轮它因 g_hwnd 尚未赋值
    // （WM_CREATE 期间）被 if 跳过，看门狗从未存在过且无任何痕迹。
    if (g_hwnd != nullptr) {
        UINT_PTR t = SetTimer(g_hwnd, kWatchdogTimer, kWatchdogMs, nullptr);
        DebugLog(t != 0 ? "watchdog: timer set (8000ms)" : "watchdog: SetTimer FAILED");
    } else {
        DebugLog("watchdog: NOT set (g_hwnd is null)");
    }
    if (!EnsureWebView2()) {
        if (g_hwnd != nullptr) KillTimer(g_hwnd, kWatchdogTimer);
        ShowFallback();
        return;
    }
    std::wstring userData = g_storage->ExeDir() + L"\\wv2data";
    auto* handler = new EnvHandler();
    HRESULT hr = g_createEnv(nullptr, userData.c_str(), nullptr, handler);
    handler->Release();
    DebugLog("env create call: userDataFolder=" + ToUtf8(userData));
    DebugLogHr("env create call", hr);
    if (FAILED(hr)) {
        if (g_hwnd != nullptr) KillTimer(g_hwnd, kWatchdogTimer);
        ShowFallback();
    }
}

void ShowFallback() {
    // 日志必须在任何 return 之前：上一轮白屏时 ShowFallback 因 g_hwnd 为空
    // 在写日志之前就静默返回，是整个故障「零证据」的直接原因。
    DebugLog(g_fallbackActive ? "fallback: requested (already active)"
                               : "fallback: requested");
    if (g_hwnd == nullptr) {
        DebugLog("fallback: cannot show (g_hwnd is null)");
        return;
    }
    // 兜底会把控制器拆掉：在飞导航随之作废，清掉标志免得挡住兜底恢复
    // 路径里的重试（Open 见兜底激活会重新导航/重建，见 StatsWindow::Open）。
    g_navInFlight = false;
    // 若 WebView 已部分创建，先拆掉，避免残影
    if (g_controller != nullptr) {
        g_controller->Close();
        g_controller->Release();
        g_controller = nullptr;
    }
    if (g_webview != nullptr) {
        g_webview->Release();
        g_webview = nullptr;
    }
    // 兜底内容由宿主窗口的 WM_PAINT 用 GDI 直接绘制（见 StatsWndProc），
    // 不依赖任何子控件的创建与可见性——窗口能画出标题栏，就能画出这段说明。
    g_fallbackActive = true;
    InvalidateRect(g_hwnd, nullptr, TRUE);
    UpdateWindow(g_hwnd); // 立即触发绘制，不等消息队列空转到 WM_PAINT
    DebugLog("fallback: active (painted by host WM_PAINT)");
}

void ResizeContent() {
    if (g_hwnd == nullptr) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (g_controller != nullptr) g_controller->put_Bounds(rc);
    // 兜底文字由 WM_PAINT 按客户区大小绘制，尺寸变化时在 WM_SIZE 里重绘即可
}

void TeardownWebView() {
    if (g_hwnd != nullptr) KillTimer(g_hwnd, kWatchdogTimer);
    if (g_controller != nullptr) {
        g_controller->Close();
        g_controller->Release();
        g_controller = nullptr;
    }
    if (g_webview != nullptr) {
        g_webview->Release();
        g_webview = nullptr;
    }
    g_fallbackActive = false;
    g_navCompleted = false;
    g_navGen = 0;
    g_navDoneGen = 0;
    g_navInFlight = false;
}

LRESULT CALLBACK StatsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 记录程序托盘发来的两条注册消息：Show = 单击图标/菜单「打开统计
    // 界面」/第二实例唤出，统一走 Open()（含隐藏状态唤出与数据刷新）；
    // Quit = 菜单「关闭查看器」，是查看器唯一的真退出路径——直接
    // DestroyWindow，绕过 WM_CLOSE 的收起逻辑。
    if (msg == ViewerShowMsg()) {
        DebugLog("viewer: show via registered message");
        StatsWindow::Open();
        return 0;
    }
    if (msg == ViewerQuitMsg()) {
        DebugLog("viewer: quit via recorder tray");
        DestroyWindow(hwnd);
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            // 关键时序：WM_CREATE 在 CreateWindowExW 返回之前发出，
            // Open() 里对 g_hwnd 的赋值此时还没发生。必须在这里先把
            // g_hwnd 设为 hwnd，整条 WebView2 链（看门狗定时器、env
            // 完成回调的守卫、ShowFallback）才能看见窗口——上一轮白屏
            // 正是因为 env 完成回调同步触发时 g_hwnd 仍是 nullptr，
            // 回调走了守卫分支且全程静默（见 EnvHandler::Invoke 注释）。
            g_hwnd = hwnd;
            DebugLog("stats window created");
            ApplyWindowFrame(hwnd); // 无边框：DWM 圆角 + 阴影（或区域圆角兜底）
            StartWebView();
            return 0;
        case WM_NCCALCSIZE:
            // 客户区 = 整窗（标题栏与边框的视觉部分全撤），但 WS_THICKFRAME
            // 保留，系统缩放/贴边吸附/最大化语义不变。最大化时按边框厚度
            // 内收，免得内容被屏幕边缘与任务栏吃掉一圈。
            if (wParam == TRUE) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                if (IsZoomed(hwnd)) {
                    int f = GetSystemMetrics(SM_CXSIZEFRAME) +
                            GetSystemMetrics(SM_CXPADDEDBORDER);
                    InflateRect(&params->rgrc[0], -f, -f);
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_NCHITTEST: {
            // 自报缩放边：四边/四角在边框厚度内返回缩放命中，其余全归客户
            // 区（拖动由页面顶部带经 win:drag 走系统标题栏回路）。
            if (IsZoomed(hwnd)) return HTCLIENT;
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT wr;
            if (!GetWindowRect(hwnd, &wr)) return HTCLIENT;
            int f = GetSystemMetrics(SM_CXSIZEFRAME) +
                    GetSystemMetrics(SM_CXPADDEDBORDER);
            if (f < 5) f = 5;
            bool l = pt.x < wr.left + f, r = pt.x >= wr.right - f;
            bool t = pt.y < wr.top + f, b = pt.y >= wr.bottom - f;
            if (t && l) return HTTOPLEFT;
            if (t && r) return HTTOPRIGHT;
            if (b && l) return HTBOTTOMLEFT;
            if (b && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (t) return HTTOP;
            if (b) return HTBOTTOM;
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            // 最小尺寸护住页面的双列图表与摘要带排版
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 760;
            mmi->ptMinTrackSize.y = 560;
            return 0;
        }
        case WM_PAINT: {
            if (!g_fallbackActive) return DefWindowProcW(hwnd, msg, wParam, lParam);
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(246, 242, 234)); // 与页面米白底一致
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(hdc, TRANSPARENT);
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HGDIOBJ oldFont = SelectObject(hdc, font);
            RECT textRc = rc;
            textRc.left += 32; textRc.top += 32;
            textRc.right -= 32; textRc.bottom -= 32;
            DrawTextW(hdc, kFallbackText, -1, &textRc, DT_LEFT | DT_TOP | DT_WORDBREAK);
            SelectObject(hdc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kWatchdogTimer) {
                KillTimer(hwnd, kWatchdogTimer);
                if (!g_navCompleted) {
                    DebugLog("watchdog: navigation did not complete in 8s; showing fallback");
                    ShowFallback();
                }
            }
            return 0;
        case WM_SIZE:
            ResizeContent();
            UpdateRoundRegion(hwnd); // 区域圆角兜底时跟随尺寸（DWM 路径下为空操作）
            NotifyWinState(); // 最大化状态变化时通知页面藏/显缩放手柄
            if (g_fallbackActive) InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_CLOSE:
            // 点红 X（或 Alt+F4）只把窗口收起：隐藏、不销毁，WebView2
            // 与查看器进程都保持存活。把窗口找回来的入口是记录程序那
            // 唯一的托盘图标（单击即唤出）；隐藏前先探一次记录程序
            // 存活，保证那个图标一定在场（防 stranded）。真正退出只有
            // 一条路：记录器托盘菜单「关闭查看器」发来的 ViewerQuit。
            EnsureRecorderAliveOnHide();
            ShowWindow(hwnd, SW_HIDE);
            DebugLog("viewer: hidden to tray");
            return 0;
        case WM_DESTROY:
            TeardownWebView();
            if (g_hwnd == hwnd) g_hwnd = nullptr;
            // 能走到 WM_DESTROY 的只有 ViewerQuit 消息与 Shutdown()：
            // 这才是查看器进程真正的结束点。
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void StatsWindow::Init(HINSTANCE hInstance, const Storage* storage, const Tracker* tracker) {
    g_inst = hInstance;
    g_storage = storage;
    g_tracker = tracker;
    if (storage != nullptr) {
        AppLogInit(storage->ExeDir()); // 幂等：查看器 main 已调过也无妨
        g_settings = LoadSettingsFile(storage->DataDir());
    }
}

void StatsWindow::Open() {
    DebugLog("StatsWindow::Open");
    if (g_hwnd != nullptr) {
        // 已打开：窗口可能正被红 X 收起（隐藏）或最小化，先恢复可见，
        // 再用最新数据重新整页导航，保证数字是最新的
        if (!IsWindowVisible(g_hwnd)) {
            ShowWindow(g_hwnd, SW_SHOW);
            DebugLog("viewer: window unhidden");
        }
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        if (g_fallbackActive) {
            // 兜底不是单向门：清标志、撤残影后当场重试——控制器还活着就
            // 用最新数据重新导航，控制器已在兜底时被拆掉（g_webview 为
            // 空）就重走 StartWebView 创建链；两条路径内部都会重置看门
            // 狗，新控制器到达后由 ControllerHandler 负责导航。render-ok
            // 回来即转正，不必再重启进程（旧版锁死正是坏在这里）。
            g_fallbackActive = false;
            InvalidateRect(g_hwnd, nullptr, TRUE);
            DebugLog("fallback: retrying on open");
            if (g_webview != nullptr) NavigateStatsPage();
            else StartWebView();
        } else if (g_webview != nullptr) {
            if (g_navInFlight) {
                // 已有导航在飞：只显示/置前，不背靠背重发——连发两笔
                // NavigateToString 会互相取消（webError=9），数据刷新
                // 等本轮落地后的下一次 Open 即可。
                DebugLog("open: navigation in flight; show only");
            } else {
                NavigateStatsPage();
            }
        }
        SetForegroundWindow(g_hwnd);
        return;
    }
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = StatsWndProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hbrBackground = CreateSolidBrush(RGB(246, 242, 234)); // 与页面米白底一致
        wc.lpszClassName = kStatsClass;
        RegisterClassExW(&wc);
        classRegistered = true;
    }
    // 无边框：POPUP 出身 + THICKFRAME 保缩放/吸附，MIN/MAX/SYSMENU 保系统
    // 快捷键与任务栏行为，WS_EX_APPWINDOW 保任务栏按钮（红绿灯与拖动见页面
    // 顶部带与本文件 WM_NC* 处理）。
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, kStatsClass, L"电脑使用统计",
                             WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX |
                                 WS_MAXIMIZEBOX | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1040, 800,
                             nullptr, nullptr, g_inst, nullptr);
    if (g_hwnd != nullptr) ShowWindow(g_hwnd, SW_SHOW);
}

void StatsWindow::Shutdown() {
    if (g_hwnd != nullptr) DestroyWindow(g_hwnd);
}
