#include "VideoPlayerApp.h"

#include "AudioSpectrumAnalyzer.h"
#include "FormatSniffer.h"
#include "InfoScreen.h"
#include "UiHost.h"
#include "jarkUtils.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

// main.cpp 里的应用名（"JarkViewer"）：标题统一成 `文件名 - JarkViewer`，不新增文案
extern std::wstring_view appName;

namespace {

constexpr int64_t kVolumeOsdDurationMs = 1200;
constexpr float kBarHeightLogical = 50.0f;   // 与看图里动图播放条同高

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 主题色（0xAARRGGBB）→ ImU32，alphaScale 用来做半透明（与看图那边的 imColor 同一个语义）
ImU32 themeColor(uint32_t argb, float alphaScale = 1.0f) {
    const int alpha = static_cast<int>(((argb >> 24) & 0xFF) * alphaScale);
    return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, alpha);
}

// mm:ss（超过一小时 h:mm:ss）
std::string formatTime(int64_t ms) {
    const int64_t totalSeconds = (std::max)(int64_t{ 0 }, ms) / 1000;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds % 3600) / 60;
    const int64_t seconds = totalSeconds % 60;
    if (hours > 0)
        return std::format("{}:{:02d}:{:02d}", hours, minutes, seconds);
    return std::format("{:02d}:{:02d}", minutes, seconds);
}

} // namespace

VideoPlayerApp::VideoPlayerApp() = default;

VideoPlayerApp::~VideoPlayerApp() = default;

HRESULT VideoPlayerApp::Initialize(HINSTANCE hInstance) {
    // 标题要在建窗口之前定下来（窗口一出现就该带着文件名）
    updateWindowCaption();
    return D3D11App::Initialize(hInstance);
}

void VideoPlayerApp::OnWindowCreated() {
    if (!startupPath_.empty())
        startFile(startupPath_);
}

void VideoPlayerApp::startFile(const std::wstring& path) {
    frame_ = cv::Mat();
    displayFrame_ = cv::Mat(); // 上一片的放大副本不必留着占内存
    viewFrameSize_ = {};
    volumeOsdUntilMs_ = 0;
    draggingBar_ = false;
    canvasPressed_ = false;
    playButtonPressed_ = false;
    playButtonHovered_ = false;

    // 换片：关掉旧播放器、位置归零（音量保持不变）
    playback_.open(path);
    placeholder_ = cv::Mat(); // 上一片的静态画面不能留：换新片那一瞬间会露出来
    placeholderStamp_ = 0;    // 指纹里没有文件名，换片一律重画（失败占位 / 纯音频画面）
    infoPanelScroll_ = 0.0f;  // 信息面板换成新文件的内容：滚回顶部（开关状态保留）

    updateWindowCaption();
    updateFitView();
    markPresentRequested();
}

void VideoPlayerApp::dispatchPath(const std::wstring& path) {
    if (path.empty())
        return;

    if (jark::isPlayerFile(path)) {
        startFile(path);
        return;
    }

    // 图片不归播放器：交给 wWinMain 换成看图窗口（按内容换窗口）
    handoffPath_ = path;
    JARK_LOG("播放器: 图片交给看图窗口 {}", jarkUtils::wstringToUtf8(path));
    requestExit();
}

void VideoPlayerApp::requestExit() {
    PostMessageW(m_hWnd, WM_DESTROY, 0, 0);
}

void VideoPlayerApp::updateWindowCaption() {
    const std::wstring& name = playback_.fileName();
    m_wndCaption = name.empty()
        ? std::format(L"{}", appName)
        : std::format(L"{} - {}", name, appName);
    if (m_hWnd)
        SetWindowTextW(m_hWnd, m_wndCaption.c_str());
}

// 现在画的是"纯音频画面"吗（打开了但没有视频轨）：条带是否常驻、要不要叠频谱，都看它
bool VideoPlayerApp::showsAudioScreen() const {
    return playback_.isOpen() && !playback_.hasVideo();
}

void VideoPlayerApp::updatePlaceholder() {
    // 需要"静态画面"的两种情况：打开失败（失败占位）、以及没有视频轨（纯音频）。
    // 播视频时这里什么都不做——画面由帧提供
    PlaceholderKind kind = PlaceholderKind::None;
    if (!playback_.isOpen())
        kind = playback_.error() == jark::VideoPlayback::Error::FileMissing
            ? PlaceholderKind::FileMissing : PlaceholderKind::DecodeFailed;
    else if (showsAudioScreen())
        kind = PlaceholderKind::Audio;

    if (kind == PlaceholderKind::None)
        return;

    const cv::Size size{ (std::max)(1, winWidth), (std::max)(1, winHeight) };
    const uint64_t stamp = jark::infoScreenStamp(size, uiScale(), 0);
    if (stamp == placeholderStamp_)
        return;

    // 指纹里没有文件名（换片要靠 startFile 把 stamp 清掉），这里只管内容与尺寸
    placeholderStamp_ = stamp;
    placeholder_ = jark::renderInfoScreen(kind, playback_.fileName(), size, uiScale(), 0);
}

void VideoPlayerApp::updateFitView() {
    viewFrameSize_ = frame_.size();
    viewWinWidth_ = winWidth;
    viewWinHeight_ = winHeight;
    fitSize_ = {};

    if (frame_.empty() || winWidth <= 0 || winHeight <= 0)
        return;

    // 适应窗口：整幅可见（条带是浮在上面的，不挤压画面）
    const double scale = (std::min)(static_cast<double>(winWidth) / frame_.cols,
        static_cast<double>(winHeight) / frame_.rows);

    view_ = {};
    view_.imageWidth = frame_.cols;
    view_.imageHeight = frame_.rows;
    view_.zoomBase = ZOOM_BASE;
    view_.zoom = (std::max)(int64_t{ 1 }, static_cast<int64_t>(std::llround(scale * ZOOM_BASE)));
    view_.border = false; // 播放器不画图像边框

    // 画面在屏幕上的像素尺寸：与逐帧采样路径算绘制矩形用的是同一处几何
    const jark::CanvasGeometry geometry = jark::imageGeometry(
        view_, cv::Size(winWidth, winHeight), frame_.size());
    fitSize_ = cv::Size(
        (std::max)(1, static_cast<int>(std::llround(geometry.renderedSize.width))),
        (std::max)(1, static_cast<int>(std::llround(geometry.renderedSize.height))));

    // 1:1 时最近邻采到的就是原像素，没有重采样的余地（也不多占一份帧缓冲）
    if (fitSize_ == frame_.size())
        return;

    fitView_ = {};
    fitView_.imageWidth = fitSize_.width;
    fitView_.imageHeight = fitSize_.height;
    fitView_.zoomBase = ZOOM_BASE;
    fitView_.zoom = ZOOM_BASE; // 名义尺寸 = 位图尺寸 ⇒ 采样密度恰好 1:1
    fitView_.border = false;
}

// 逐帧采样路径（drawCanvasImpl）为速度只用最近邻：放大时一个源像素被铺成一整块方块，
// 帧分辨率明显低于画面区域时（小视频、或长边被解码器压到 1920 的 4K 片）颗粒感一眼可见。
// 看图窗口靠"画面静止后平滑重采样"补这一步，播放器没有、也不可能每帧都做一遍。
// 这里改成先把帧重采样到屏幕上的目标尺寸再交给采样路径——密度恰好 1:1，滤波只做一次，
// 采样端退化成逐像素拷贝。
void VideoPlayerApp::drawFitFrame() {
    if (frame_.empty() || mainCanvas.empty())
        return;

    if (fitSize_.empty() || fitSize_ == frame_.size()) {
        jark::drawImageToCanvas(frame_, mainCanvas, view_);
    }
    else {
        // 缩小用面积平均（INTER_AREA **只能**用于缩小，放大会退化成最近邻）；
        // 放大用双线性——每帧都要付一次，本机实测 1920x1080 → 2500x1406：
        // INTER_LINEAR 1.9ms（整帧 10.5ms）、INTER_CUBIC 6.7ms（15.1ms）、Lanczos4 更多，
        // 而 60fps 一帧只有 16.7ms，窗口再大一点就只有双线性还留得住余量。
        // 视频本身有噪声与运动，双线性已经足够消掉颗粒感，多出来的那点锐度不值这个代价。
        const bool shrinking = fitSize_.width <= frame_.cols && fitSize_.height <= frame_.rows;
        cv::resize(frame_, displayFrame_, fitSize_, 0.0, 0.0,
            shrinking ? cv::INTER_AREA : cv::INTER_LINEAR);
        jark::drawImageToCanvas(displayFrame_, mainCanvas, fitView_);
    }

    PresentCanvas(mainCanvas.ptr(), mainCanvas.cols, mainCanvas.rows, (int)mainCanvas.step);
}

void VideoPlayerApp::DrawScene() {
    if (!windowReady_)
        return;

    bool canvasChanged = false;

    if (playback_.isOpen()) {
        cv::Mat frame;
        if (playback_.takeFrame(frame)) {
            frame_ = std::move(frame);
            canvasChanged = true; // 新帧
        }

        if (!frame_.empty() && (frame_.size() != viewFrameSize_ ||
            winWidth != viewWinWidth_ || winHeight != viewWinHeight_)) {
            updateFitView();
            canvasChanged = true;
        }
    }

    // 音量提示到点后要自己擦掉（之后不会再有事件把它冲掉）
    if (volumeOsdUntilMs_ != 0 && steadyNowMs() >= volumeOsdUntilMs_) {
        volumeOsdUntilMs_ = 0;
        markPresentRequested();
    }

    if (canvasChanged && !frame_.empty()) {
        drawFitFrame();
    }
    else if (frame_.empty()) {
        // 失败占位 / 纯音频画面：这两种情况永远没有帧，画面整块由占位位图提供。
        // 每帧重贴一次与原来的失败占位路径同一个代价（DrawScene 本来就在空闲分支里空转），
        // 换来的是"换尺寸之后上传播入的暂存纹理被重建"这件事自然被覆盖——
        // 少了这一条，音频窗口改大小就会全黑（暂停的视频出过同一个问题，见 OnResize）
        updatePlaceholder();
        if (!placeholder_.empty()) {
            canvasChanged = true;
            placeholder_.copyTo(mainCanvas);
            drawAudioSpectrum(); // 纯音频时往底板上叠加实时频谱（不是音频则什么都不画）
            PresentCanvas(mainCanvas.ptr(), mainCanvas.cols, mainCanvas.rows, (int)mainCanvas.step);
        }
    }

    if (canvasChanged || consumePresentRequest())
        PresentFrame();
}

// 纯音频画面上那列跟着声音跳的条：底板由 InfoScreen 画好（静态缓存），这里每帧把
// 当前播放位置的频谱叠上去。
//
// 电平取的是"播放位置处"的那一帧（MediaPlayer 里带时间戳的环形缓冲），不是解码线程
// 最新算出来的那一帧——解码提前约 2 秒，用最新那帧频谱会比听到的声音早出一两秒。
// 条是底部对齐的（均衡器那套视觉），静音时留一条最小高度当地平线，不然整块底板是空的。
void VideoPlayerApp::drawAudioSpectrum() {
    if (!showsAudioScreen() || mainCanvas.empty())
        return;

    constexpr int kBandCount = jark::AudioSpectrumAnalyzer::kDefaultBandCount;
    float bands[kBandCount] = {};
    playback_.readSpectrum(bands); // 取不到（还没数据）就是全 0，画出来是一条地平线

    const cv::Rect area = jark::audioSpectrumRect({ winWidth, winHeight }, uiScale());
    if (area.width <= 0 || area.height <= 0)
        return;

    const uint32_t accent = GlobalVar::currentTheme.CHECK;
    const cv::Scalar color(accent & 0xFF, (accent >> 8) & 0xFF, (accent >> 16) & 0xFF, (accent >> 24) & 0xFF);
    const int gap = (std::max)(1, static_cast<int>(std::lround(2.0f * uiScale())));
    const int barWidth = (std::max)(1, (area.width - gap * (kBandCount - 1)) / kBandCount);
    const int floorHeight = (std::max)(2, static_cast<int>(std::lround(3.0f * uiScale())));

    for (int band = 0; band < kBandCount; ++band) {
        const double level = std::clamp(static_cast<double>(bands[band]), 0.0, 1.0);
        const int height = (std::max)(floorHeight, static_cast<int>(std::lround(level * area.height)));
        const int x = area.x + band * (barWidth + gap);
        if (x + barWidth > area.x + area.width)
            break; // 窗口特别窄时宁可少画几段，也不要画到外面
        cv::rectangle(mainCanvas,
            { x, area.y + area.height - height, barWidth, height }, color, cv::FILLED);
    }
}

void VideoPlayerApp::DrawUi() {
    jark::ui::UiHost::instance().setUiVisible(true);

    // 打开失败：只有占位画面，没有条带
    if (!playback_.isOpen())
        return;

    auto* draw = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    const auto pos = [&](float x, float y) { return ImVec2(origin.x + x, origin.y + y); };
    const float scale = uiScale();
    const ImU32 background = ImGui::GetColorU32(ImGuiCol_PopupBg, 0.82f);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    // 进度条就是条带右侧整块、两个色调：已播 = 主题强调色（深），未播 = 它往白色拉的浅色调。
    // 强调色在两套主题里都是"与背景对比的那一档"（深色主题偏深、浅色主题偏浅），
    // 往白色拉只会更浅，所以"左深右浅"两种主题下都成立。
    // **两层都和条带一样半透明**（同一个 α，深浅关系不变）：画面透出来一点，
    // 与条带底（PopupBg × 0.82）是同一套叠色语义，别改成不透明
    constexpr float kTrackAlpha = 0.45f;
    const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(ImGuiCol_CheckMark));
    const ImU32 trackPlayed = ImGui::GetColorU32(ImGuiCol_CheckMark, kTrackAlpha);
    const ImU32 trackUnplayed = ImGui::GetColorU32(ImVec4(
        accent.x + (1.0f - accent.x) * 0.55f,
        accent.y + (1.0f - accent.y) * 0.55f,
        accent.z + (1.0f - accent.z) * 0.55f, kTrackAlpha));

    // —— 媒体信息面板（I / Tab）：先画，条带再压在上面 ——
    // 条带在播放器里是唯一的操作入口，不能被面板盖住
    drawInfoPanel();

    // —— 音量提示：手绘小喇叭 + 百分数字（不新增多语言文案）——
    if (volumeOsdUntilMs_ != 0) {
        const float w = 132.0f * scale;
        const float h = 44.0f * scale;
        const float x = (static_cast<float>(winWidth) - w) / 2.0f;
        const float y = static_cast<float>(winHeight) * 0.10f;
        draw->AddRectFilled(pos(x, y), pos(x + w, y + h), background, 6.0f * scale);

        const float cy = y + h / 2.0f;
        const float bx = x + 18.0f * scale;
        const float boxW = 7.0f * scale;
        const float boxH = 10.0f * scale;
        const float coneH = 18.0f * scale;

        // 喇叭：方块 + 向右张开的三角
        draw->AddRectFilled(pos(bx, cy - boxH / 2.0f), pos(bx + boxW, cy + boxH / 2.0f), text);
        draw->AddTriangleFilled(
            pos(bx + boxW, cy - coneH / 2.0f), pos(bx + boxW, cy + coneH / 2.0f),
            pos(bx + boxW * 2.4f, cy), text);

        if (playback_.volumePercent() > 0) {
            // 两道声波
            for (int i = 1; i <= 2; ++i) {
                const float radius = (5.0f + 4.0f * i) * scale;
                draw->PathArcTo(pos(bx + boxW * 2.4f, cy), radius, -0.72f, 0.72f, 16);
                draw->PathStroke(text, 0, 1.6f * scale);
            }
        }
        else {
            // 静音：一道叉
            const float r = 6.0f * scale;
            const ImVec2 cx = pos(bx + boxW * 3.4f, cy);
            draw->AddLine(ImVec2(cx.x - r, cx.y - r), ImVec2(cx.x + r, cx.y + r), text, 1.6f * scale);
            draw->AddLine(ImVec2(cx.x - r, cx.y + r), ImVec2(cx.x + r, cx.y - r), text, 1.6f * scale);
        }

        const std::string percent = std::format("{}%", playback_.volumePercent());
        const ImVec2 size = ImGui::CalcTextSize(percent.c_str());
        draw->AddText(pos(x + w - 14.0f * scale - size.x, cy - size.y / 2.0f), text, percent.c_str());
    }

    // 条带：纯悬停触发、瞬时显隐（拖动把手期间不隐藏，否则拖到条带外就断了操作）
    if (!barVisible())
        return;

    const float barTop = static_cast<float>(winHeight) - barHeight();
    draw->AddRectFilled(pos(0.0f, barTop),
        pos(static_cast<float>(winWidth), static_cast<float>(winHeight)), background);

    // —— 播放/暂停按钮：正方形，图标手绘（不依赖字体里有没有 ▶ / ⏸）——
    const cv::Rect2f button = playButtonRect();
    if (playButtonPressed_ || playButtonHovered_) {
        draw->AddRectFilled(pos(button.x, button.y), pos(button.x + button.width, button.y + button.height),
            ImGui::GetColorU32(playButtonPressed_ ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered));
    }
    const ImVec2 center = pos(button.x + button.width / 2.0f, button.y + button.height / 2.0f);
    const float iconHalf = button.height * 0.17f;
    if (playback_.isPlaying()) {
        const float barW = iconHalf * 0.55f;
        const float gap = iconHalf * 0.42f;
        draw->AddRectFilled(ImVec2(center.x - gap - barW, center.y - iconHalf),
            ImVec2(center.x - gap, center.y + iconHalf), text);
        draw->AddRectFilled(ImVec2(center.x + gap, center.y - iconHalf),
            ImVec2(center.x + gap + barW, center.y + iconHalf), text);
    }
    else {
        draw->AddTriangleFilled(
            ImVec2(center.x - iconHalf * 0.62f, center.y - iconHalf),
            ImVec2(center.x - iconHalf * 0.62f, center.y + iconHalf),
            ImVec2(center.x + iconHalf * 1.02f, center.y), text);
    }

    // —— 右侧全是进度条：条带右侧整块就是进度条，占满高度、没有内边距与圆角 ——
    const cv::Rect2f track = trackRect();
    const int64_t duration = playback_.durationMs();
    const bool seekable = duration > 0; // 时长未知：只有未播色，不可拖不可点

    const int64_t shown = seekable
        ? (playback_.isScrubbing() ? playback_.scrubTargetMs() : playback_.positionMs())
        : playback_.positionMs();
    float filled = 0.0f;
    if (seekable)
        filled = track.width * static_cast<float>(std::clamp(
            static_cast<double>(shown) / static_cast<double>(duration), 0.0, 1.0));

    // 两段**各铺一层**：已播只铺左边那段、未播只铺右边那段。
    // 别写成"整条先铺浅色、再往上盖深色"——两层半透明叠在已播段里会互相透色，
    // α 一低两块就糊在一起（实测已播/未播亮度只差 15，看着像一种颜色）
    if (filled < track.width)
        draw->AddRectFilled(pos(track.x + filled, track.y),
            pos(track.x + track.width, track.y + track.height), trackUnplayed);
    if (filled > 0.0f)
        draw->AddRectFilled(pos(track.x, track.y),
            pos(track.x + filled, track.y + track.height), trackPlayed);

    // 时间文字水平、竖直居中压在条上（整块都是进度条，只能叠上去），底下垫一层深色
    // 才在"已播深 / 未播浅"两种底色上都看得清
    const std::string timeText = seekable
        ? std::format("{} / {}", formatTime(shown), formatTime(duration))
        : std::format("{} / --:--", formatTime(shown));
    const ImVec2 textSize = ImGui::CalcTextSize(timeText.c_str());
    const ImVec2 textPos = pos(track.x + (track.width - textSize.x) / 2.0f,
        static_cast<float>(winHeight) - barHeight() / 2.0f - textSize.y / 2.0f);
    const float padX = 6.0f * scale;
    const float padY = 2.0f * scale;
    draw->AddRectFilled(ImVec2(textPos.x - padX, textPos.y - padY),
        ImVec2(textPos.x + textSize.x + padX, textPos.y + textSize.y + padY),
        IM_COL32(0, 0, 0, 140), 3.0f * scale);
    draw->AddText(textPos, IM_COL32(255, 255, 255, 235), timeText.c_str());
}

// —— 媒体信息面板 ——
//
// 样式照看图那边那个 EXIF 面板：左侧四分之一宽的圆角面板、文本按宽度折行、滚轮滚动、
// 折行与滚动条那套算法直接复用 `jark::ui::drawWrappedText`。内容由 VideoPlayback::
// infoText()（文件属性 + 容器/流信息）给出，这里只负责排版。
//
// 底部给条带留出高度：条带是播放器唯一的操作入口，压住文字就没法一边看信息一边拖进度
bool VideoPlayerApp::infoPanelVisible() const {
    return showInfo_ && playback_.isOpen() && !playback_.infoText().empty();
}

bool VideoPlayerApp::infoPanelHit(int x, int y) const {
    return infoPanelVisible() && !infoPanelRect_.empty() &&
        static_cast<float>(x) >= infoPanelRect_.x &&
        static_cast<float>(x) < infoPanelRect_.x + infoPanelRect_.width &&
        static_cast<float>(y) >= infoPanelRect_.y &&
        static_cast<float>(y) < infoPanelRect_.y + infoPanelRect_.height;
}

void VideoPlayerApp::drawInfoPanel() {
    if (!infoPanelVisible()) {
        infoPanelRect_ = {}; // 不画时也要清掉矩形，否则滚轮还会落到"看不见的面板"上
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    const auto pos = [&](float x, float y) { return ImVec2(origin.x + x, origin.y + y); };
    const float scale = uiScale();
    const float padding = static_cast<float>(dp(12));
    const float panelWidth = (winWidth - padding * 2.0f) / 4.0f;
    const float panelHeight = winHeight - padding * 2.0f - barHeight();
    infoPanelRect_ = { padding, padding, panelWidth, panelHeight };

    drawList->AddRectFilled(pos(padding, padding), pos(padding + panelWidth, padding + panelHeight),
        themeColor(GlobalVar::currentTheme.BG_DEEP, 0.82f), 8.0f * scale);

    const float contentLeft = padding + dp(10);
    const float contentRight = padding + panelWidth - dp(10);
    const float contentTop = padding + dp(8);
    const float textBottom = padding + panelHeight - dp(8);
    const float textViewHeight = textBottom - contentTop;
    const auto& text = playback_.infoText();

    // 先量总高（draw=false）再按滚动偏移画，屏外的行直接跳过
    const float totalHeight = jark::ui::drawWrappedText(nullptr, origin, contentLeft, 0.0f, contentRight,
        0.0f, 0.0f, text, 0, false);
    infoPanelMaxScroll_ = (std::max)(0.0f, totalHeight - textViewHeight);
    infoPanelScroll_ = (std::clamp)(infoPanelScroll_, 0.0f, infoPanelMaxScroll_);

    drawList->PushClipRect(pos(contentLeft, contentTop), pos(contentRight, textBottom), true);
    jark::ui::drawWrappedText(drawList, origin, contentLeft, contentTop - infoPanelScroll_, contentRight,
        contentTop, textBottom, text, themeColor(GlobalVar::currentTheme.FG), true);
    drawList->PopClipRect();

    // 滚动条：贴在面板右缘（与 EXIF 面板同一套画法）
    if (infoPanelMaxScroll_ > 0.5f) {
        const float thumbHeight = (std::max)(static_cast<float>(dp(24)),
            textViewHeight * textViewHeight / totalHeight);
        const float thumbTop = contentTop + (textViewHeight - thumbHeight) * (infoPanelScroll_ / infoPanelMaxScroll_);
        drawList->AddRectFilled(pos(contentRight + dp(2), contentTop), pos(contentRight + dp(4), textBottom),
            themeColor(GlobalVar::currentTheme.FG, 0.15f));
        drawList->AddRectFilled(pos(contentRight + dp(2), thumbTop), pos(contentRight + dp(4), thumbTop + thumbHeight),
            themeColor(GlobalVar::currentTheme.FG, 0.45f));
    }
}

// —— 条带几何 ——

float VideoPlayerApp::barHeight() const {
    return kBarHeightLogical * uiScale();
}

bool VideoPlayerApp::barHit(int y) const {
    return y >= winHeight - static_cast<int>(std::lround(barHeight()));
}

bool VideoPlayerApp::barVisible() const {
    // 纯音频画面（没有帧可看）下条带**常驻**：整幅画面就剩它一个能看、能操作的东西，
    // 藏起来等于把进度和时间也藏了。
    // 视频仍是"鼠标进入即显示、移开立即隐藏"（不做淡出），拖动把手期间不隐藏
    return showsAudioScreen() || draggingBar_ || mouseInside_;
}

cv::Rect2f VideoPlayerApp::playButtonRect() const {
    const float height = barHeight();
    return { 0.0f, static_cast<float>(winHeight) - height, height, height };
}

cv::Rect2f VideoPlayerApp::trackRect() const {
    // 条带右侧整块就是进度条：贴着播放按钮、一直到窗口右边缘，高度就是整个条带
    const float height = barHeight();
    return { height, static_cast<float>(winHeight) - height,
        (std::max)(1.0f, static_cast<float>(winWidth) - height), height };
}

int64_t VideoPlayerApp::trackValueToMs(int x) const {
    const int64_t duration = playback_.durationMs();
    if (duration <= 0)
        return 0;

    const cv::Rect2f track = trackRect();
    const double ratio = std::clamp((x - track.x) / std::max(1.0, static_cast<double>(track.width)), 0.0, 1.0);
    return static_cast<int64_t>(std::llround(ratio * static_cast<double>(duration)));
}

// —— 输入 ——

void VideoPlayerApp::OnMouseDown(WPARAM btnState, int x, int y, WPARAM wParam) {
    if (btnState != WM_LBUTTONDOWN)
        return;

    if (barHit(y) && playback_.isOpen()) {
        const cv::Rect2f button = playButtonRect();
        if (x >= button.x && x < button.x + button.width && y >= button.y && y < button.y + button.height) {
            playButtonPressed_ = true;
            markPresentRequested();
            return;
        }

        if (playback_.durationMs() > 0) {
            // 按住进度条：拖动期间只出关键帧预览（节流），松手才精确跳转
            draggingBar_ = true;
            SetCapture(m_hWnd);
            playback_.beginScrub(trackValueToMs(x));
            markPresentRequested();
        }
        return;
    }

    // 按下发生在画面上：抬起时算一次"单击画面" = 播放/暂停（见 OnMouseUp）
    canvasPressed_ = true;

    // 画布中央双击 = 全屏（与看图模式同一套判定）。两次单击各切一次播放/暂停，
    // 双击正好抵消，所以双击之后只会多出一次很短的停顿
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClickTimestamp_).count();
    lastClickTimestamp_ = now;
    if (10 < elapsed && elapsed < 300)
        jarkUtils::ToggleFullScreen(m_hWnd);
}

void VideoPlayerApp::OnMouseUp(WPARAM btnState, int x, int y, WPARAM wParam) {
    if (btnState != WM_LBUTTONUP)
        return;

    if (playButtonPressed_) {
        playButtonPressed_ = false;
        const cv::Rect2f button = playButtonRect();
        if (x >= button.x && x < button.x + button.width && y >= button.y && y < button.y + button.height)
            playback_.togglePlay();
        markPresentRequested();
        return;
    }

    if (draggingBar_) {
        // 拖到条带外、甚至拖出客户区也要收尾（所以按下时 SetCapture，抬起才 Release）
        draggingBar_ = false;
        ReleaseCapture();
        playback_.endScrub(trackValueToMs(x));
        markPresentRequested();
        return;
    }

    // 画面上单击 = 播放/暂停（按下也在画面上才算，避免"按在条带上、抬在画面上"被当成单击）
    if (canvasPressed_) {
        canvasPressed_ = false;
        const bool inside = x >= 0 && y >= 0 && x < winWidth && y < winHeight && playback_.isOpen();
        if (inside && !barHit(y)) {
            playback_.togglePlay();
            markPresentRequested();
        }
    }
}

void VideoPlayerApp::OnPointerCancel() {
    canvasPressed_ = false; // 失焦/失捕获：这次按下不再算单击

    if (!draggingBar_)
        return;

    draggingBar_ = false;
    ReleaseCapture();
    playback_.endScrub(playback_.scrubTargetMs()); // 按当前目标收尾，不留悬挂的拖动状态
    markPresentRequested();
}

void VideoPlayerApp::OnMouseMove(WPARAM btnState, int x, int y) {
    (void)btnState;

    const bool inside = x >= 0 && y >= 0 && x < winWidth && y < winHeight;
    const bool inBar = barHit(y);
    const bool overButton = inBar && playback_.isOpen() &&
        x >= playButtonRect().x && x < playButtonRect().x + playButtonRect().width &&
        y >= playButtonRect().y && y < playButtonRect().y + playButtonRect().height;
    const bool barWasVisible = barShown_;
    mouseInside_ = inside && inBar;

    if (playButtonHovered_ != overButton) {
        playButtonHovered_ = overButton;
        markPresentRequested();
    }

    if (draggingBar_)
        playback_.updateScrub(trackValueToMs(x)); // 只保留最新目标，丢掉的中间事件不用补偿

    const bool barIsVisible = barVisible();
    if (barIsVisible != barWasVisible)
        markPresentRequested(); // 显隐是瞬时的：进入条带要立刻画出来，移开要立刻擦掉
    barShown_ = barIsVisible;
}

void VideoPlayerApp::OnMouseLeave() {
    mouseInside_ = false;
    playButtonHovered_ = false;
    markPresentRequested();
}

void VideoPlayerApp::OnMouseWheel(UINT nFlags, short zDelta, int x, int y) {
    (void)nFlags;

    // 信息面板内的滚轮只滚面板内容，不穿透成音量（与看图那边 EXIF 面板同一套规则）
    if (infoPanelHit(x, y)) {
        infoPanelScroll_ -= zDelta / static_cast<float>(WHEEL_DELTA) * dp(66);
        infoPanelScroll_ = (std::clamp)(infoPanelScroll_, 0.0f, infoPanelMaxScroll_);
        markPresentRequested();
        return;
    }

    // 滚轮调音量，与 W/S 同一个步进（5%）
    const int notches = zDelta == 0 ? 0 : (zDelta > 0 ? 1 : -1) * (std::max)(1, std::abs(zDelta) / WHEEL_DELTA);
    playback_.adjustVolume(notches * jark::VideoPlayback::kVolumeStepPercent);
    volumeOsdUntilMs_ = steadyNowMs() + kVolumeOsdDurationMs;
    markPresentRequested();
}

void VideoPlayerApp::OnKeyDown(WPARAM keyValue) {
    if (GetKeyState(VK_CONTROL) & 0x8000) {
        switch (keyValue) {
        case 'O': { // Ctrl+O 换片（选到图片就交给看图窗口）
            const std::wstring picked = jarkUtils::SelectFile(m_hWnd);
            if (!picked.empty())
                dispatchPath(picked);
        } break;

        case 'W':   // Ctrl+W 退出
            requestExit();
            break;

        default:
            // 其余 Ctrl 组合键一律吞掉（播放器没有设置/打印/编辑那套窗口）
            break;
        }
        return;
    }

    switch (keyValue) {
    case VK_SPACE: // 播放/暂停；停在结尾时按它从头重播
        playback_.togglePlay();
        markPresentRequested();
        break;

    // 播放中 ±5 秒；暂停中单帧前进/后退
    case VK_LEFT:
    case 'A':
        if (playback_.isPaused())
            playback_.stepFrame(-1);
        else
            playback_.nudge(-jark::VideoPlayback::kNudgeMs);
        markPresentRequested();
        break;

    case VK_RIGHT:
    case 'D':
        if (playback_.isPaused())
            playback_.stepFrame(1);
        else
            playback_.nudge(jark::VideoPlayback::kNudgeMs);
        markPresentRequested();
        break;

    case VK_TAB:
    case 'I': // 媒体信息面板（与看图窗口的 EXIF 面板同一个键）
        showInfo_ = !showInfo_;
        markPresentRequested();
        break;

    case VK_HOME:
        playback_.seekTo(0);
        markPresentRequested();
        break;

    case VK_END:
        playback_.seekTo(playback_.durationMs());
        markPresentRequested();
        break;

    case VK_UP:
    case 'W':
        playback_.adjustVolume(jark::VideoPlayback::kVolumeStepPercent);
        volumeOsdUntilMs_ = steadyNowMs() + kVolumeOsdDurationMs;
        markPresentRequested();
        break;

    case VK_DOWN:
    case 'S':
        playback_.adjustVolume(-jark::VideoPlayback::kVolumeStepPercent);
        volumeOsdUntilMs_ = steadyNowMs() + kVolumeOsdDurationMs;
        markPresentRequested();
        break;

    case 'F':
    case VK_F11:
        jarkUtils::ToggleFullScreen(m_hWnd);
        markPresentRequested();
        break;

    case VK_ESCAPE: // 单向门：退出程序（要回看图请重新打开图片）
        requestExit();
        break;

    default:
        // 未绑定的键一律吞掉：播放器里不存在"顺势落到看图那套分支"这回事
        JARK_LOG("播放器键位: 0x{:04x}", static_cast<uint64_t>(keyValue));
        break;
    }
}

void VideoPlayerApp::OnDropFiles(WPARAM wParam) {
    HDROP drop = reinterpret_cast<HDROP>(wParam);
    std::wstring path;

    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count > 0) {
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        path.resize(length);
        DragQueryFileW(drop, 0, path.data(), length + 1);
    }
    DragFinish(drop);

    dispatchPath(path); // 视频换片；图片交回 wWinMain 换成看图窗口
}

void VideoPlayerApp::OnResize(UINT width, UINT height) {
    if (width == 0 || height == 0)
        return;

    if (winWidth == static_cast<int>(width) && winHeight == static_cast<int>(height))
        return;

    winWidth = static_cast<int>(width);
    winHeight = static_cast<int>(height);
    windowReady_ = true;

    if (mainCanvas.cols != winWidth || mainCanvas.rows != winHeight) {
        mainCanvas = cv::Mat(winHeight, winWidth, CV_8UC4);
        CreateWindowSizeDependentResources();
    }

    updateFitView();

    // 尺寸一变，上传画布用的暂存纹理就被重建（内容为空），必须立刻重画并重传一次：
    // 播放中下一帧会顺手补上，**暂停时根本没有"下一帧"**，少了这一下就是整窗全黑
    // （DrawScene 那条"尺寸变了就重画"的分支在这里不会触发——updateFitView 已经把
    //   viewWinWidth_/viewWinHeight_ 记成新值了）
    if (!frame_.empty())
        drawFitFrame();
    markPresentRequested();
}

void VideoPlayerApp::OnDpiChanged() {
    updateFitView();
    markPresentRequested();
}
