#pragma once

// 单媒体播放器窗口：一个独立于看图的顶层窗口，视频与音频共用（音频只是没有画面）。
//
// `wWinMain` 按入参在「看图窗口」与它之间**二选一构造**，另一个对象根本不构造——
// 所以播放器里不可能出现"按 P 进幻灯片""按 Q 旋转图片"这类串味：两边不共享任何输入处理。
//
// 它只做三件事：把输入翻译成 VideoPlayback 的调用、把当前帧画到画布（复用 CanvasRenderer）、
// 按状态画底部条带（ImGui 前景绘制列表，只画不用它做输入捕获）。
// 打不开或没有视频轨时退化成 InfoScreen 静态画面（失败占位 / 音频画面），不画条带。
//
// 键位与条带规格见 AGENTS.md「视频播放器」一节。
//
// 窗口几何（位置/大小/最大化/所在显示器）与看图窗口**共用同一份记忆**
// （SettingParameter::rect/showCmd/monitorDevice）：谁退出谁写、谁启动谁读，
// 所以"上次看视频调出来的窗口尺寸"会原样继承给下次看图，反之亦然。

#include "D3D11App.h"

#include <chrono>
#include <string>

#include "CanvasRenderer.h"
#include "VideoPlayback.h"

class VideoPlayerApp : public D3D11App {
public:
    VideoPlayerApp();
    ~VideoPlayerApp() override;

    // 启动要播放的文件（必须在 Initialize 之前设置：窗口标题要带上文件名）
    void setStartupFile(std::wstring path) { startupPath_ = std::move(path); }

    // 退出时若还想打开别的文件（拖入图片 / Ctrl+O 选到图片），由 wWinMain 取走它、
    // 据此决定下一轮开哪个窗口（视频→播放器，图片→看图）
    std::wstring takeHandoffPath() { return std::exchange(handoffPath_, {}); }

    // 没有右键菜单（菜单键 / Shift+F10 也不弹看图那套菜单）
    bool showsContextMenu() const override { return false; }

    HRESULT Initialize(HINSTANCE hInstance) override;
    void OnWindowCreated() override;
    void DrawScene() override;
    void DrawUi() override;
    bool hasVisibleWindows() const override { return false; } // 只用 ImGui 画条带，不做输入捕获

    void OnMouseDown(WPARAM btnState, int x, int y, WPARAM wParam) override;
    void OnMouseUp(WPARAM btnState, int x, int y, WPARAM wParam) override;
    // 失焦/失捕获时收尾：把在拖的进度条按当前位置结束掉（不然把手会卡在拖动状态）
    void OnPointerCancel() override;
    void OnMouseMove(WPARAM btnState, int x, int y) override;
    void OnMouseLeave() override;
    void OnMouseWheel(UINT nFlags, short zDelta, int x, int y) override;
    void OnKeyDown(WPARAM keyValue) override;
    void OnKeyUp(WPARAM keyValue) override { (void)keyValue; }
    void OnDropFiles(WPARAM wParam) override;
    void OnContextMenuCommand(WPARAM wParam) override { (void)wParam; } // 没有右键菜单
    void OnResize(UINT width, UINT height) override;
    void OnDpiChanged() override;
    void OnRequestExitOtherWindows() override {}

private:
    // 打开/换成另一个媒体文件（位置归零、音量保持不变）
    void startFile(const std::wstring& path);
    // 拖入/选中的文件按内容决定去向：视频与音频换片、图片交接给看图窗口
    void dispatchPath(const std::wstring& path);
    void updateWindowCaption();
    // 静态画面：打开失败时的失败占位，或没有视频轨时的音频画面（播视频时什么都不画）
    void updatePlaceholder();
    // 当前是不是"纯音频画面"（打开了但没有视频轨）：条带是否常驻也看它
    bool showsAudioScreen() const;
    void updateFitView();
    // 把当前帧按"适应窗口"的屏幕尺寸重采样后画到画布（1:1 采样，见 .cpp 里的说明）
    void drawFitFrame();
    // 纯音频画面：把当前播放位置的实时频谱画到 InfoScreen 画好的底板上（每帧调用）
    void drawAudioSpectrum();
    void requestExit();

    // —— 底部条带的几何（客户区坐标，全部按 uiScale 缩放）——
    float barHeight() const;
    bool barHit(int y) const;
    bool barVisible() const;
    cv::Rect2f playButtonRect() const;
    cv::Rect2f trackRect() const;
    int64_t trackValueToMs(int x) const;

    std::wstring startupPath_;
    std::wstring handoffPath_;

    jark::VideoPlayback playback_;

    cv::Mat frame_;                  // 当前显示的视频帧（与播放器内部共享像素，浅拷贝即可）
    cv::Mat displayFrame_;           // frame_ 重采样到屏幕尺寸的副本（只在能 1:1 绘制时存在）
    cv::Mat placeholder_;            // 打开失败时的占位画面（Size/DPI 变化才重画）
    uint64_t placeholderStamp_ = 0;
    jark::ViewState view_{};         // 适应窗口：名义尺寸 = 帧尺寸
    jark::ViewState fitView_{};      // 画 displayFrame_ 用的视图：名义尺寸 = fitSize_，采样 1:1
    cv::Size fitSize_{};             // 画面在屏幕上的像素尺寸（1:1 时等于帧尺寸）
    cv::Size viewFrameSize_{};       // 视图是按这个帧尺寸算的（换片/窗口变化要重算）
    int viewWinWidth_ = 0;
    int viewWinHeight_ = 0;

    bool mouseInside_ = false;
    bool draggingBar_ = false;
    bool playButtonPressed_ = false;
    bool canvasPressed_ = false;     // 按下发生在画面（不在条带）上：抬起时算一次"单击画面"
    bool playButtonHovered_ = false;
    bool barShown_ = false;          // 上一帧条带是否可见（用来判断"该重画了"）
    bool windowReady_ = false;
    // 双击画布中央 = 全屏（与看图模式同一套 10~300ms 判定）
    std::chrono::steady_clock::time_point lastClickTimestamp_{};

    int64_t volumeOsdUntilMs_ = 0;   // 音量提示的显示截止时刻（0 = 不显示）
};
