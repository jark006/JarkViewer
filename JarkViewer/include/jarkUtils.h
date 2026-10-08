#pragma once

#include <iostream>
#include <format>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <semaphore>
#include <string>
#include <vector>
#include <array>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <stdexcept>
#include <ranges>
#include <span>
#include <print>

using std::vector;
using std::string;
using std::wstring;
using std::string_view;
using std::wstring_view;
using std::set;
using std::map;
using std::unordered_set;
using std::unordered_map;

#include "framework.h"
#include "resource.h"

#include "psapi.h"
#include <dxgi.h>
#include <D3D11.h>
#include <wincodec.h>
#include <imm.h>
#include <commdlg.h>
#include <shellapi.h>
#include <winspool.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <mfapi.h>
#include <mfidl.h>
#include <shlwapi.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <shlobj.h>
#include <commctrl.h>
#include <strmif.h>

#define SECURITY_WIN32
#include <sspi.h>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib ,"imm32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "Secur32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Strmiids.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "userenv.lib")

#include <opencv2/core.hpp>
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>

#include "stringRes.h"
#include "Localization.h"

inline const int MIN_VIDEO_BUFF_SIZE = 65536; // 64KiB 视频数据最小尺寸，过小可能是无效数据
inline const int MAX_VIDEO_FRAMES = 120;      // 最大解码帧数，过大可能导致内存占用过高

inline constexpr int64_t ZOOM_BASE = 1 << 16; // 100% 缩放的定点基准（CurImageParameter 与矢量图光栅化共用）

struct ThemeColor {
    uint32_t FG_LIGHT;   // 文字颜色 浅
    uint32_t FG;         // 文字颜色
    uint32_t FG_DEEP;    // 文字颜色 深
    uint32_t BG_LIGHT;   // 背景颜色 浅
    uint32_t BG;         // 背景颜色
    uint32_t BG_DEEP;    // 背景颜色 深

    uint32_t BG_TAG;     // 标签栏背景
    uint32_t BG_BTN;     // 按钮背景
    uint32_t CHECK;      // 选中背景
    uint32_t BLACK_GRID; // 深色棋盘格子
    uint32_t WHITE_GRID; // 浅色棋盘格子

    uint32_t VER;        // 版本文字颜色
};

inline constexpr ThemeColor deepTheme{
    0xFFF6F8FE, 0xFFF1F3F9, 0xFFECEEF4, 0xFF666666, 0xFF1F2024, 0xFF1A1B1F,
    0xFF3E4048, 0xFF666666, 0xFF006AA4, 0xFF282828, 0xFF3C3C3C, 0xFFBCB4EF,
};
inline constexpr ThemeColor lightTheme{
    0xFF666666, 0xFF1F2024, 0xFF1A1B1F, 0xFFF6F8FE, 0xFFF1F3F9, 0xFFECEEF4,
    0xFFCDD7E8, 0xFFCDD7E8, 0xFF4CC2FF, 0xFFDDDDDD, 0xFFFFFFFF, 0xFF3C26BA,
};

// 不要随意更改此结构体的成员顺序或大小，否则会导致设置文件无法兼容
// 设置文件大小固定为4096字节
struct SettingParameter {
    // 常见格式
    static inline std::string_view defaultExtList{ 
        "apng,avif,bmp,gif,heic,heif,ico,jfif,jp2,jpe,jpeg,jpg,jxl,jxr,livp,pbm,pfm,pgm,png,pnm,ppm,qoi,svg,tga,tif,tiff,webp,wp2" };

    uint8_t header[32];
    RECT rect{};                           // 窗口大小位置
    uint32_t showCmd = SW_MAXIMIZE;        // 窗口模式

    uint32_t printerBrightness = 100;      // 亮度调整 (0 ~ 200)
    uint32_t printerContrast = 100;        // 对比度调整 (0 ~ 200)
    uint32_t printercolorMode = 1;         // 颜色打印模式 0=彩色, 1=灰度, 2=黑白文档, 3=黑白抖动
    bool printerInvertColors = false;      // 是否反相
    bool printerBalancedBrightness = false;// 是否均衡亮度 文档优化

    bool isOneToOnePreferred = false;      // 打开图片时优先1:1
    bool hideNavigator = false;            // 原 reserve2，原位复用：旧设置默认显示鸟瞰图

    bool isAllowRotateAnimation = true;
    bool isAllowZoomAnimation = true;
    bool enableColorManagement = true;      // ICC色彩管理
    bool isNoteBeforeDelete = true;         // 删除前提示
    uint32_t switchImageAnimationMode = 0;  // 0: 无动画  1:上下滑动  2:左右滑动

    uint32_t pptOrder = 0;                  // 幻灯片模式  0: 顺序  1:逆序  2:随机
    uint32_t pptTimeout = 5;                // 幻灯片模式  切换间隔 1 ~ 300 秒

    uint32_t UI_Mode = 0;                   // 界面主题 0:跟随系统  1:浅色  2:深色
    uint32_t UI_LANG = 0;                   // 界面语言：0:简体中文 1:繁體中文 2:English 3:日本語 4:한국어（见 Localization.h）

    uint32_t rightClickAction = 0;          // 右键点击行为  0:打开菜单  1:退出程序

    // 最后使用的显示器设备名（MONITORINFOEXW::szDevice，如 \\.\DISPLAY2），空=未知、按主显示器。
    // 从 reserve 里原位划出 64 字节，结构体总布局不变（见下方 offsetof 断言）；
    // 旧设置此段为零，走"没有记录"的回退路径。
    wchar_t monitorDevice[CCHDEVICENAME] = {};

    // 实况照片自动播放时是否出声（默认静音，对齐系统照片应用的习惯；
    // 鼠标悬停「实况」角标是主动操作，会重播并出声，不受此项影响）
    bool livePhotoAutoPlaySound = false;

    wchar_t copyTargetDir[260] = {};        // 复制/移动图片的目标文件夹（空=未设置，首次使用时选择）
    wchar_t externalEditor[260] = {};       // 外部编辑器程序路径（空=未设置，首次使用时选择）

    uint32_t reserve[523]; // 原 800，依次划给 monitorDevice(64) + livePhotoAutoPlaySound(4) + 两个路径(2×520)

    char extCheckedListStr[800];

    SettingParameter() {
        memcpy(extCheckedListStr, defaultExtList.data(), defaultExtList.length() + 1);
        UI_LANG = static_cast<uint32_t>(jark::languageToSetting(jark::languageFromSystem()));
    }

    SettingParameter(const SettingParameter& other) {
        memcpy(extCheckedListStr, defaultExtList.data(), defaultExtList.length() + 1);
        UI_LANG = static_cast<uint32_t>(jark::languageToSetting(jark::languageFromSystem()));

        memcpy(this, &other, sizeof(SettingParameter));
        ValidateParameters();
    }

    SettingParameter& operator=(const SettingParameter& other) {
        if (this != &other) {
            memcpy(this, &other, sizeof(SettingParameter));
            ValidateParameters();
        }
        return *this;
    }

    // 检查参数
    void ValidateParameters() {
        // 窗口位置大小检查：坐标是虚拟屏幕系，允许副屏的负坐标，
        // 但矩形要和某块显示器有交集，完全落在显示器之外（屏幕拔了/换布局）才重置
        if (rect.right <= rect.left) {
            rect.right = rect.left + 800; // 默认宽度
        }
        if (rect.bottom <= rect.top) {
            rect.bottom = rect.top + 600; // 默认高度
        }
        if (!::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
            rect = {};
        }

        // 窗口模式检查 - 仅限 SW_MAXIMIZE SW_NORMAL
        if (showCmd != SW_NORMAL && showCmd != SW_MAXIMIZE) {
            showCmd = SW_MAXIMIZE;
        }

        // 亮度调整范围检查 (0 ~ 200)
        if (printerBrightness > 200) printerBrightness = 100;

        // 对比度调整范围检查 (0 ~ 200)
        if (printerContrast > 200) printerContrast = 100;

        // 颜色模式检查 - 0=彩色, 1=灰度, 2=黑白文档, 3=黑白抖动
        if (printercolorMode > 3) printercolorMode = 1;

        // 动画模式检查 (0~2)
        if (switchImageAnimationMode > 2) switchImageAnimationMode = 0;

        // 幻灯片模式检查 (0~2)
        if (pptOrder > 2) pptOrder = 0;

        // 幻灯片切换间隔检查 (1~300秒)
        if (pptTimeout < 1) pptTimeout = 2;
        else if (pptTimeout > 300) pptTimeout = 5;

        // 界面主题检查 (0~2)
        if (UI_Mode > 2) UI_Mode = 0; // 超出范围则设为跟随系统

        // 语言检查：索引范围为 0 ~ jark::kLanguageCount-1
        if (UI_LANG >= jark::kLanguageCount)
            UI_LANG = static_cast<uint32_t>(jark::kSimplifiedChineseIndex);

        // 右键点击行为检查 (0~1)
        if (rightClickAction > 1) rightClickAction = 0;

        // 确保扩展名列表字符串以空字符结尾
        extCheckedListStr[sizeof(extCheckedListStr) - 1] = 0;
    }
};

static_assert(sizeof(SettingParameter) == 4096, "sizeof(SettingParameter) != 4096");
static_assert(offsetof(SettingParameter, hideNavigator) == 67);
static_assert(offsetof(SettingParameter, rightClickAction) == 92);
static_assert(offsetof(SettingParameter, monitorDevice) == 96);
static_assert(offsetof(SettingParameter, livePhotoAutoPlaySound) == 160);
static_assert(offsetof(SettingParameter, copyTargetDir) == 162);   // bool 后按 wchar_t 的对齐(2)排
static_assert(offsetof(SettingParameter, externalEditor) == 682);
static_assert(offsetof(SettingParameter, extCheckedListStr) == 3296);

struct rcFileInfo {
    uint8_t* ptr = nullptr;
    size_t size = 0;
};

union intUnion {
    uint32_t u32;
    uint8_t u8[4];
    intUnion() :u32(0) {}
    intUnion(uint32_t n) :u32(n) {}
    intUnion(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
        u8[0] = b0;
        u8[1] = b1;
        u8[2] = b2;
        u8[3] = b3;
    }
    uint8_t& operator[](const int i) {
        return u8[i];
    }
    void operator=(const int i) {
        u32 = i;
    }
    void operator=(const uint32_t i) {
        u32 = i;
    }
    void operator=(const intUnion i) {
        u32 = i.u32;
    }
};

struct Cood {
    int x = 0;
    int y = 0;

    void operator+=(const Cood& other) {
        x += other.x;
        y += other.y;
    }

    void operator+=(const int num) {
        x += num;
        y += num;
    }

    Cood operator+(const Cood& other) const {
        return { x + other.x, y + other.y };
    }

    Cood operator-(const Cood& other) const {
        return { x - other.x, y - other.y };
    }

    Cood operator*(int num) const {
        return { x * num, y * num };
    }

    Cood operator/(int num) const {
        return { x / num, y / num };
    }

    bool operator==(const Cood& other) const {
        return (x == other.x) && (y == other.y);
    }

    bool operator==(const int num) const {
        return (x == num) && (y == num);
    }

    bool operator!=(const int num) const {
        return (x != num) || (y != num);
    }

    void operator=(const int num) {
        x = num;
        y = num;
    }
};

template <>
struct std::formatter<Cood> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }
    auto format(const Cood& cood, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "(x={}, y={})", cood.x, cood.y);
    }
};

namespace jark {
    struct VectorImage;  // 矢量图源，定义见 VectorImage.h
    struct VideoSource;  // 内存中的视频源，定义见 MediaPlayer.h
}

enum class ImageFormat {
    None = 0,       // 解码失败
    Still,          // 静态图: jpg/bmp ...
    Animated,       // 动态图: gif/apng ...
    //LivePhoto       // 实况图: livp/MVIMG ...
};

// 主页/解码失败等占位内容的类型：画面由 jark::renderInfoScreen 按当前语言、主题与 DPI 实时绘制，
// 不再是 home.png / tips.png 那样的固定资源图。
enum class PlaceholderKind : int {
    None = 0,
    Home,               // 打开软件但未指定图片时的主页
    UnsupportedFormat,  // 扩展名与文件头都不是受支持的格式
    DecodeFailed,       // 格式已知但解码失败（损坏、数据异常等）
    FileMissing,        // 文件不存在或无法读取
};

struct ImageAsset {
    ImageFormat format = ImageFormat::None;  // 图像类型：静态/动图/实况
    cv::Mat primaryFrame;                    // 静态图或实况的静态图
    std::vector<cv::Mat> frames;             // 动态图或实况的视频
    std::vector<int> frameDurations;         // 每帧时长
    string exifInfo;                         // 图像EXIF等信息（显示用文本）
    std::vector<uint8_t> iccProfile;         // 图像内嵌ICC配置文件
    std::shared_ptr<jark::VectorImage> vectorSource; // 矢量图源（SVG），按需重新光栅化
    std::shared_ptr<jark::VideoSource> videoSource;  // 视频源（实况照片/视频文件），用于实时播放
    int orientation = 1;                     // EXIF 方向（1~8），解码器已按此旋转像素
    int jpegQuality = 0;                     // JPEG 质量因子（量化表反推的近似值，0=未知/非 JPEG）

    // 占位内容（主页/解码失败）：primaryFrame 为空时表示尚未生成（probe 等无界面场景保持为空，
    // 据此判定解码失败；界面层在尺寸/DPI/语言/主题变化时按 placeholderStamp 重新绘制）
    PlaceholderKind placeholder = PlaceholderKind::None;
    std::wstring placeholderDetail;          // 展示用补充信息（文件路径或扩展名）
    uint64_t placeholderStamp = 0;           // 生成 primaryFrame 时的 jark::infoScreenStamp
};

enum class ActionENUM:int64_t {
    none = 0, slide, preImg, nextImg, firstImg, finalImg, zoomIn, zoomOut, zoomFix, toggleExif, toggleFullScreen, requestExit, refresh,
    rotateLeft, rotateRight, printImage, deleteImg, setting, batchProcess, editImage, slideshow, renameImage,
    jumpToImage, navigateImage
};

enum class CursorPos :int {
    centerArea = 0, leftUp, leftDown, leftEdge, rightEdge, rightDown, rightUp, centerTop,
};

enum class ShowExtraUI :int {
    none = 0, rotateLeftButton, printer, leftArrow, rightArrow, setting, rotateRightButton, animationBar
};

enum class ContextMenu :int {
    openNewImage = 1000, copyImageInfo, copyImagePath, copyImageData, toggleExifDisplay, openContainerFloder, deleteImage,
    openFileProperties, printImage, toggleFullScreen, openSetting, openHelp, aboutSoftware, exitSoftware, batchProcess,
    editImage, slideshow, renameImage, copyToTarget, moveToTarget, chooseTargetDir, openWithEditor, chooseEditor
};

struct Action {
    ActionENUM action = ActionENUM::none;
    union {
        int x;
        int width;
        int value1;
    };
    union {
        int y;
        int height;
        int value2;
    };
    uint64_t generation = 0; // 导航动作所属的图像/目录，换图后不应用旧坐标
};


class OperateQueue {
private:
    std::queue<Action> queue;
    std::mutex mtx;

public:
    ActionENUM lastAction = ActionENUM::none;

    void push(Action action) {
        std::lock_guard<std::mutex> lock(mtx);

        if (!queue.empty() && action.action == ActionENUM::navigateImage &&
            queue.back().action == action.action && queue.back().generation == action.generation) {
            queue.back() = action; // 鸟瞰是绝对位置，不是可以相加的位移
        }
        else if (!queue.empty() && action.action == ActionENUM::slide) {
            Action& back = queue.back();

            if (back.action == ActionENUM::slide) {
                back.x += action.x;
                back.y += action.y;
            }
            else {
                queue.push(action);
            }
        }
        else {
            queue.push(action);
        }
    }

    Action get() {
        std::lock_guard<std::mutex> lock(mtx);

        if (queue.empty())
            return { ActionENUM::none };

        Action res = queue.front();
        queue.pop();
        lastAction = res.action;
        return res;
    }

    bool isNext(const ActionENUM actionEnum) {
        std::lock_guard<std::mutex> lock(mtx);

        if (queue.empty())
            return false;
        return queue.front().action == actionEnum;
    }
};

struct WinSize {
    int width = 600;
    int height = 400;
    WinSize(){}
    WinSize(int w, int h) :width(w), height(h) {}

    bool operator==(const WinSize size) const {
        return this->width == size.width && this->height == size.height;
    }
    bool operator!=(const WinSize size) const {
        return this->width != size.width || this->height != size.height;
    }
    bool isZero() const {
        return this->width == 0 && this->height == 0;
    }
};

struct MatPack {
    cv::Mat* matPtr = nullptr;
    wstring* titleStrPtr = nullptr;
    void clear() {
        matPtr = nullptr;
        titleStrPtr = nullptr;
    }
};

struct GlobalVar {
    static inline bool isNeedUpdateTheme = false;
    static inline bool isNeedReloadImageCache = false;

    static inline bool isSystemDarkMode = false;    // 系统界面主题：深色/浅色
    static inline bool isCurrentUIDarkMode = false; // 应用实时界面主题：深色/浅色
    static inline ThemeColor currentTheme = deepTheme;

    static inline wstring settingPath;
    static inline string_view settingHeader{ "JarkViewerSetting" };
    static inline SettingParameter settingParameter;
};

class jarkUtils {
public:

    // 日志开关：Debug 构建默认开启（输出到控制台）；Release 构建默认关闭，
    // 可用命令行 `--log` 或环境变量 JARKVIEWER_LOG=1 打开（同时写入日志文件）。
    static bool isLogEnabled() noexcept {
#ifndef NDEBUG
        return true;
#else
        return logEnabled;
#endif
    }

    static void setLogEnabled(bool enabled) noexcept;

    // Release 下开启日志时的输出文件路径（默认 %TEMP%\JarkViewer.log）
    static std::wstring logFilePath();

    static void writeLogLine(std::string_view text);

    template<typename... Args>
    static void log(std::string_view fmt, Args&&... args) {
        if (!jarkUtils::isLogEnabled())
            return;

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::current_zone()->to_local(now);
        auto str = std::format("[{:%H:%M:%S}] {}", time, std::vformat(fmt, std::make_format_args(args...)));
#ifndef NDEBUG
        std::println("{}", str);
#endif
        jarkUtils::writeLogLine(str);
    }

    template<typename... Args>
    static void log(std::wstring_view fmt, Args&&... args) {
        if (!jarkUtils::isLogEnabled())
            return;

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::current_zone()->to_local(now);
        auto wstr = std::format(L"[{:%H:%M:%S}] {}", time, std::vformat(fmt, std::make_wformat_args(args...)));
        auto str = jarkUtils::wstringToUtf8(wstr);
#ifndef NDEBUG
        std::println("{}", str);
#endif
        jarkUtils::writeLogLine(str);
    }

    static string bin2Hex(const void* bytes, const size_t len);

    static cv::Scalar to_cv_scalar(uint32_t color);

    static std::wstring ansiToWstring(string_view str);

    static std::string wstringToAnsi(wstring_view wstr);

    static std::wstring utf8ToWstring(string_view str);

    static std::string wstringToUtf8(wstring_view wstr);

    static std::wstring latin1ToWstring(string_view str);

    static std::string utf8ToAnsi(string_view str);

    static std::string ansiToUtf8(string_view str);

    static std::string convertUnicodeEscapesToUTF8(string_view str);

    static rcFileInfo GetResource(unsigned int idi, const wchar_t* type);

    static string size2Str(const size_t fileSize);

    static string timeStamp2Str(time_t timeStamp);

    static WinSize getWindowSize(HWND hwnd);

    // 设置窗口图标
    static void setWindowIcon(HWND hWnd, WORD wIconId);
    
    // 禁止窗口调整尺寸
    static void disableWindowResize(HWND hwnd);

    static bool copyToClipboard(wstring_view text);

    static bool limitSizeTo16K(cv::Mat& image);

    // Alpha透明通道混合白色背景
    static void flattenRGBAonWhite(cv::Mat& image);

    static void copyImageToClipboard(const cv::Mat& image);

    static void ToggleFullScreen(HWND hwnd);
    static bool IsFullScreen();
    // 只在状态不同时切换（幻灯片播放要求“确保全屏”，不能无脑 toggle）
    static void SetFullScreen(HWND hwnd, bool fullScreen);

    // 选取文件
    static std::wstring SelectFile(HWND hWnd);
    static std::wstring SelectFolder(HWND hWnd); // 选文件夹（复制/移动目标、外部编辑器场景）

    // 启动/加载分段计时：设 JARKVIEWER_STARTUP_TRACE=<文件路径> 时把各阶段"相对首次调用的
    // 毫秒数"追加写入该文件（没设环境变量时零开销）。用于回答"打开一张图为什么慢"。
    static void startupTraceMark(const char* stage);

    // 图像另存为 选取文件路径
    static std::pair<std::wstring, bool> saveImageDialogW(wstring_view title);

    static void openUrl(const wchar_t* url);

    static std::vector<std::string> splitString(std::string_view str, std::string_view delim);

    static void stringReplace(std::string& src, std::string_view oldBlock, std::string_view newBlock);

    static std::vector<std::wstring> splitWstring(std::wstring_view str, std::wstring_view delim);

    static void wstringReplace(std::wstring& src, std::wstring_view oldBlock, std::wstring_view newBlock);

    static void activateWindow(HWND hwnd);

    static std::wstring getCurrentAppPath();

    static void openFileLocation(wstring_view filePath);

    static void openFileProperties(wstring_view filePath);

    static bool getSystemDarkMode();

    static inline const char COMPILE_DATE_TIME[32] = {
        __DATE__[7],
        __DATE__[8],
        __DATE__[9],
        __DATE__[10],// YYYY year
        '-',

        // First month letter, Oct Nov Dec = '1' otherwise '0'
        (__DATE__[0] == 'O' || __DATE__[0] == 'N' || __DATE__[0] == 'D') ? '1' : '0',

        // Second month letter Jan, Jun or Jul
        (__DATE__[0] == 'J') ? ((__DATE__[1] == 'a') ? '1'
        : ((__DATE__[2] == 'n') ? '6' : '7'))
        : (__DATE__[0] == 'F') ? '2'// Feb
        : (__DATE__[0] == 'M') ? (__DATE__[2] == 'r') ? '3' : '5'// Mar or May
        : (__DATE__[0] == 'A') ? (__DATE__[1] == 'p') ? '4' : '8'// Apr or Aug
        : (__DATE__[0] == 'S') ? '9'// Sep
        : (__DATE__[0] == 'O') ? '0'// Oct
        : (__DATE__[0] == 'N') ? '1'// Nov
        : (__DATE__[0] == 'D') ? '2'// Dec
        : 'X',

        '-',
        __DATE__[4] == ' ' ? '0' : __DATE__[4],// First day letter, replace space with digit
        __DATE__[5],// Second day letter
        ' ',
        __TIME__[0],
        __TIME__[1],
        __TIME__[2],
        __TIME__[3],
        __TIME__[4],
        __TIME__[5],
        __TIME__[6],
        __TIME__[7],
        '\0',
    };

private:
    static inline bool logEnabled = false;
};

// 日志宏：先用运行期开关判断，避免 Release 下白算格式化参数
#define JARK_LOG(fmt, ...) do { if (jarkUtils::isLogEnabled()) jarkUtils::log(fmt, ##__VA_ARGS__); } while (false)

class FunctionTimeCount {
public:
#ifndef NDEBUG
    string_view funcName;
    std::chrono::system_clock::time_point start_clock;

    FunctionTimeCount(string_view funcName) : funcName(funcName) {
        reset();
    }

    ~FunctionTimeCount() {
        printTimeCount();
    }

    void reset() {
        start_clock = std::chrono::system_clock::now();
    }

    void printTimeCount() {
        JARK_LOG("{}(): {} ms", funcName, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start_clock).count());
    }

    void printTimeCountAndReset() {
        auto now = std::chrono::system_clock::now();
        auto durations = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_clock).count();
        start_clock = now;
        JARK_LOG("{}(): {} ms", funcName, durations);
    }

#else
    FunctionTimeCount(string_view funcName) {}
#endif
};
