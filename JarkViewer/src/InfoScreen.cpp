#define NOMINMAX 1 // 同一 TU 内混用 windows.h/OpenCV/exiv2 时避免 min/max 宏破坏模板解析

#include "InfoScreen.h"

#include "ImageDatabase.h"
#include "Localization.h"
#include "Resource.h"
#include "TextRenderer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <unordered_set>
#include <vector>

extern std::wstring_view appVersion;

namespace jark {
namespace {

// 窄表新增文案（追加在表尾，见 stringRes.cpp）
constexpr uint32_t kStrHomeHint = 156;
constexpr uint32_t kStrZoneRotateLeft = 157;
constexpr uint32_t kStrZoneRotateRight = 158;
constexpr uint32_t kStrZonePrev = 159;
constexpr uint32_t kStrZoneNext = 160;
constexpr uint32_t kStrZonePrint = 161;
constexpr uint32_t kStrZoneSetting = 162;
constexpr uint32_t kStrHomeCaption = 163;
constexpr uint32_t kStrErrorUnsupported = 164;
constexpr uint32_t kStrErrorDecode = 165;
constexpr uint32_t kStrErrorMissing = 166;
constexpr uint32_t kStrReasonUnsupported = 167;
constexpr uint32_t kStrReasonDecode = 168;
constexpr uint32_t kStrReasonMissing = 169;
constexpr uint32_t kStrFormatsTitle = 170;
constexpr uint32_t kStrFormatsCommon = 171;
constexpr uint32_t kStrFormatsVideo = 172;
constexpr uint32_t kStrImageArea = 173;

// 警示色：只用于失败页的徽章与标题点缀（主题里没有红色）
constexpr uint32_t kErrorAccentDeep = 0xFFE06E5E;
constexpr uint32_t kErrorAccentLight = 0xFFC42B1C; // 与窗口关闭按钮的悬停红一致

uint32_t mixColor(uint32_t from, uint32_t to, float t) {
    const auto channel = [&](int shift) {
        const auto a = static_cast<int>((from >> shift) & 0xFF);
        const auto b = static_cast<int>((to >> shift) & 0xFF);
        return static_cast<uint32_t>(std::lround(a * (1.0f - t) + b * t)) & 0xFF;
    };
    return 0xFF000000u | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

cv::Scalar bgra(uint32_t argb) {
    return { static_cast<double>(argb & 0xFF), static_cast<double>((argb >> 8) & 0xFF),
        static_cast<double>((argb >> 16) & 0xFF), static_cast<double>((argb >> 24) & 0xFF) };
}

// 注意：形状颜色一律经 bgra() 转成 4 通道 Scalar——直接把 0xAARRGGBB 当整数传给
// cv::rectangle/circle 只会填到第一个通道（alpha 变 0，画出来是透明的）。
void fillRoundedRect(cv::Mat& image, cv::Rect rect, int radius, uint32_t color) {
    const cv::Scalar scalar = bgra(color);
    const int r = std::clamp(radius, 0, (std::min)(rect.width, rect.height) / 2);
    if (r <= 0) {
        cv::rectangle(image, rect, scalar, cv::FILLED);
        return;
    }
    cv::rectangle(image, { rect.x + r, rect.y }, { rect.x + rect.width - r, rect.y + rect.height }, scalar, cv::FILLED);
    cv::rectangle(image, { rect.x, rect.y + r }, { rect.x + rect.width, rect.y + rect.height - r }, scalar, cv::FILLED);
    for (const auto corner : { cv::Point{ rect.x + r, rect.y + r }, cv::Point{ rect.x + rect.width - 1 - r, rect.y + r },
             cv::Point{ rect.x + r, rect.y + rect.height - 1 - r }, cv::Point{ rect.x + rect.width - 1 - r, rect.y + rect.height - 1 - r } })
        cv::circle(image, corner, r, scalar, cv::FILLED, cv::LINE_AA);
}

void strokeRoundedRect(cv::Mat& image, cv::Rect rect, int radius, uint32_t color, int thickness) {
    const cv::Scalar scalar = bgra(color);
    const int r = std::clamp(radius, 0, (std::min)(rect.width, rect.height) / 2);
    if (r <= 0) {
        cv::rectangle(image, rect, scalar, thickness, cv::LINE_AA);
        return;
    }
    const int x0 = rect.x, y0 = rect.y, x1 = rect.x + rect.width, y1 = rect.y + rect.height;
    cv::line(image, { x0 + r, y0 }, { x1 - r, y0 }, scalar, thickness, cv::LINE_AA);
    cv::line(image, { x0 + r, y1 }, { x1 - r, y1 }, scalar, thickness, cv::LINE_AA);
    cv::line(image, { x0, y0 + r }, { x0, y1 - r }, scalar, thickness, cv::LINE_AA);
    cv::line(image, { x1, y0 + r }, { x1, y1 - r }, scalar, thickness, cv::LINE_AA);
    cv::ellipse(image, { x0 + r, y0 + r }, { r, r }, 180, 0, 90, scalar, thickness, cv::LINE_AA);
    cv::ellipse(image, { x1 - r, y0 + r }, { r, r }, 90, 0, 90, scalar, thickness, cv::LINE_AA);
    cv::ellipse(image, { x1 - r, y1 - r }, { r, r }, 0, 0, 90, scalar, thickness, cv::LINE_AA);
    cv::ellipse(image, { x0 + r, y1 - r }, { r, r }, 270, 0, 90, scalar, thickness, cv::LINE_AA);
}

// 把带透明通道的小图（应用图标）合成到画布上
void blendImage(cv::Mat& target, const cv::Mat& source, int x, int y) {
    if (source.empty() || source.type() != CV_8UC4)
        return;
    for (int row = 0; row < source.rows; ++row) {
        const int targetY = y + row;
        if (targetY < 0 || targetY >= target.rows)
            continue;
        const auto* sourceRow = source.ptr<uint8_t>(row);
        auto* targetRow = target.ptr<uint8_t>(targetY);
        for (int column = 0; column < source.cols; ++column) {
            const int targetX = x + column;
            if (targetX < 0 || targetX >= target.cols)
                continue;
            const int alpha = sourceRow[column * 4 + 3];
            if (alpha == 0)
                continue;
            auto* pixel = targetRow + static_cast<size_t>(targetX) * 4;
            if (alpha == 255)
                std::copy_n(sourceRow + column * 4, 4, pixel);
            else
                for (int c = 0; c < 3; ++c)
                    pixel[c] = static_cast<uint8_t>((sourceRow[column * 4 + c] * alpha + pixel[c] * (255 - alpha) + 127) / 255);
        }
    }
}

cv::Mat loadAppIcon(int pixelSize) {
    const auto resource = jarkUtils::GetResource(IDB_PNG_ABOUT_ICON, L"PNG");
    if (!resource.size || !resource.ptr)
        return {};
    const cv::Mat encoded(1, static_cast<int>(resource.size), CV_8UC1, const_cast<uint8_t*>(resource.ptr));
    cv::Mat icon = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
    if (icon.empty())
        return {};
    if (icon.type() != CV_8UC4)
        cv::cvtColor(icon, icon, icon.channels() == 3 ? cv::COLOR_BGR2BGRA : cv::COLOR_GRAY2BGRA);
    if (icon.cols != pixelSize)
        cv::resize(icon, icon, { pixelSize, pixelSize }, 0, 0, cv::INTER_AREA);
    return icon;
}

// 尾部/首部省略（UTF-8 安全）
std::string elideRight(TextRenderer& text, std::string value, int maxWidth) {
    if (maxWidth <= 0 || text.measure(value.c_str()).width <= maxWidth)
        return value;
    while (!value.empty() && text.measure((value + "…").c_str()).width > maxWidth) {
        value.pop_back();
        while (!value.empty() && (static_cast<uint8_t>(value.back()) & 0xC0) == 0x80)
            value.pop_back();
    }
    return value + "…";
}
std::string elideLeft(TextRenderer& text, std::string value, int maxWidth) {
    if (maxWidth <= 0 || text.measure(value.c_str()).width <= maxWidth)
        return value;
    while (!value.empty() && text.measure(("…" + value).c_str()).width > maxWidth) {
        value.erase(0, 1);
        while (!value.empty() && (static_cast<uint8_t>(value.front()) & 0xC0) == 0x80)
            value.erase(0, 1);
    }
    return "…" + value;
}

// 占位界面专用的文字绘制实例（字体数据与其它实例共享，见 TextRenderer）
TextRenderer& screenText() {
    static TextRenderer renderer;
    renderer.setLineGap(0.22f);
    return renderer;
}

// 按当前主题取色，并以逻辑像素为单位推进布局
struct ScreenContext {
    cv::Mat canvas;
    TextRenderer* text = nullptr;
    float scale = 1.0f;
    uint32_t bg = 0, fg = 0, muted = 0, tag = 0, accent = 0;
    int centerX = 0;
    int y = 0;

    int px(float logical) const { return static_cast<int>(std::lround(logical * scale)); }
    int width() const { return canvas.cols; }
    int height() const { return canvas.rows; }

    void gap(float logical) { y += px(logical); }

    // 居中一行（超出宽度先省略）；返回实际占用高度
    int centerLine(const std::string& value, float fontLogical, uint32_t color, float maxWidthLogical) {
        if (value.empty())
            return 0;
        text->setSize(px(fontLogical));
        const int maxWidth = px(maxWidthLogical);
        const std::string shown = elideRight(*text, value, maxWidth);
        const cv::Size measured = text->measure(shown.c_str());
        text->putAlignCenter(canvas, { 0, y, width(), measured.height }, shown.c_str(), color);
        y += measured.height;
        return measured.height;
    }
};

ScreenContext makeContext(PlaceholderKind kind, cv::Size size, float scale) {
    const bool dark = GlobalVar::isCurrentUIDarkMode;
    const auto& theme = GlobalVar::currentTheme;

    ScreenContext context;
    context.scale = scale;
    context.bg = theme.BG;
    context.fg = theme.FG;
    context.muted = mixColor(theme.FG, theme.BG, 0.45f);
    context.tag = theme.BG_TAG;
    context.accent = dark ? kErrorAccentDeep : kErrorAccentLight;
    context.canvas = cv::Mat(size, CV_8UC4, bgra(theme.BG));
    context.text = &screenText();
    context.centerX = size.width / 2;
    return context;
}

// 主页：图标 + 名称/版本 + 打开提示 + 悬停分区示意图 + 常用操作提示
cv::Mat renderHome(cv::Size size, float scale) {
    auto context = makeContext(PlaceholderKind::Home, size, scale);
    const std::wstring_view version = appVersion;

    const int iconSize = context.px(84);
    const float widthLogical = static_cast<float>(size.width) / scale;
    const float diagramWidth = std::clamp(widthLogical - 140.0f, 200.0f, 600.0f);
    const float diagramHeight = diagramWidth * 0.52f;

    // 内容总高（含各行行高），整体垂直居中
    const auto lineHeightOf = [&](float fontLogical) {
        context.text->setSize(context.px(fontLogical));
        return context.text->lineHeight();
    };
    int total = iconSize + context.px(14) + lineHeightOf(38) + context.px(2) + lineHeightOf(16) +
        context.px(26) + lineHeightOf(19) + context.px(30) + context.px(diagramHeight) + context.px(26) + lineHeightOf(15);
    context.y = (std::max)(context.px(20), (context.height() - total) / 2);

    if (const cv::Mat icon = loadAppIcon(iconSize); !icon.empty()) {
        blendImage(context.canvas, icon, context.centerX - icon.cols / 2, context.y);
        context.y += iconSize;
    }

    context.gap(14);
    context.centerLine("JarkViewer", 38, context.fg, widthLogical - 120.0f);
    context.gap(2);
    context.centerLine(jarkUtils::wstringToUtf8(version), 16, context.muted, widthLogical - 120.0f);
    context.gap(26);
    context.centerLine(getUIString(kStrHomeHint), 19, context.fg, widthLogical - 120.0f);
    context.gap(30);

    // 悬停分区示意图：窗口轮廓 + 六个热区 + 四周名称（与真实热区一致）
    const int diagramWidthPx = context.px(diagramWidth);
    const int diagramHeightPx = context.px(diagramHeight);
    const int windowX = context.centerX - diagramWidthPx / 2;
    const int windowY = context.y;
    const cv::Rect windowRect{ windowX, windowY, diagramWidthPx, diagramHeightPx };
    strokeRoundedRect(context.canvas, windowRect, context.px(10),
        mixColor(context.fg, context.bg, 0.72f), (std::max)(1, context.px(1.0f)));

    const int inset = context.px(12);
    const int zoneWidth = context.px(96);
    const int zoneHeight = context.px(64);
    const int zoneRadius = context.px(8);
    const cv::Rect zones[6] = {
        { windowRect.x + inset, windowRect.y + inset, zoneWidth, zoneHeight },                          // 左上 左转
        { windowRect.x + windowRect.width - inset - zoneWidth, windowRect.y + inset, zoneWidth, zoneHeight }, // 右上 右转
        { windowRect.x + inset, windowRect.y + (windowRect.height - zoneHeight) / 2, zoneWidth, zoneHeight }, // 左中 上一张
        { windowRect.x + windowRect.width - inset - zoneWidth, windowRect.y + (windowRect.height - zoneHeight) / 2, zoneWidth, zoneHeight }, // 右中 下一张
        { windowRect.x + inset, windowRect.y + windowRect.height - inset - zoneHeight, zoneWidth, zoneHeight }, // 左下 打印
        { windowRect.x + windowRect.width - inset - zoneWidth, windowRect.y + windowRect.height - inset - zoneHeight, zoneWidth, zoneHeight }, // 右下 设置
    };
    const uint32_t zoneLabels[6] = { kStrZoneRotateLeft, kStrZoneRotateRight, kStrZonePrev,
        kStrZoneNext, kStrZonePrint, kStrZoneSetting };
    for (int i = 0; i < 6; ++i) {
        fillRoundedRect(context.canvas, zones[i], zoneRadius, context.tag);
        context.text->setSize(context.px(15));
        const cv::Rect labelRect = (i % 2 == 0)
            ? cv::Rect{ windowRect.x - context.px(104), zones[i].y, context.px(90), zones[i].height }
            : cv::Rect{ windowRect.x + windowRect.width + context.px(14), zones[i].y, context.px(90), zones[i].height };
        if (i % 2 == 0)
            context.text->putAlignRight(context.canvas, labelRect, getUIString(zoneLabels[i]), context.muted);
        else
            context.text->putAlignLeft(context.canvas, labelRect, getUIString(zoneLabels[i]), context.muted);
    }
    // 窗口中央标注，说明这块空框是"图像显示区"
    context.text->setSize(context.px(15));
    context.text->putAlignCenter(context.canvas, windowRect, getUIString(kStrImageArea), context.muted);
    context.y += diagramHeightPx;

    context.gap(26);
    context.centerLine(getUIString(kStrHomeCaption), 15, context.muted, widthLogical - 100.0f);
    return context.canvas;
}

// 排除失败页：警示徽章 + 标题/原因 + 文件名与路径 + 当前支持的格式清单
cv::Mat renderError(PlaceholderKind kind, const std::wstring& detail, cv::Size size, float scale) {
    auto context = makeContext(kind, size, scale);
    const float widthLogical = static_cast<float>(size.width) / scale;
    const bool dark = GlobalVar::isCurrentUIDarkMode;

    const std::string path = jarkUtils::wstringToUtf8(detail);
    const std::string fileName = path.empty() ? std::string()
        : jarkUtils::wstringToUtf8(std::filesystem::path(detail).filename().wstring());

    std::string title = getUIString(kind == PlaceholderKind::UnsupportedFormat ? kStrErrorUnsupported :
        (kind == PlaceholderKind::FileMissing ? kStrErrorMissing : kStrErrorDecode));
    std::string reason = getUIString(kind == PlaceholderKind::UnsupportedFormat ? kStrReasonUnsupported :
        (kind == PlaceholderKind::FileMissing ? kStrReasonMissing : kStrReasonDecode));
    if (kind == PlaceholderKind::UnsupportedFormat && !detail.empty()) {
        auto extension = jarkUtils::wstringToUtf8(std::filesystem::path(detail).extension().wstring());
        if (!extension.empty() && extension.size() <= 16)
            reason += std::format(" (*{})", extension);
    }

    std::string common, video, raw;
    {
        const auto join = [](const std::unordered_set<std::wstring_view>& set) {
            std::vector<std::wstring> sorted(set.begin(), set.end());
            std::sort(sorted.begin(), sorted.end());
            std::string joined;
            for (const auto& item : sorted) {
                joined += jarkUtils::wstringToUtf8(item);
                joined += ' ';
            }
            if (!joined.empty())
                joined.pop_back();
            return joined;
        };
        common = std::format("{}：{}", getUIString(kStrFormatsCommon), join(ImageDatabase::supportExt));
        raw = std::format("RAW：{}", join(ImageDatabase::supportRaw));
        video = std::format("{}：{}", getUIString(kStrFormatsVideo), join(ImageDatabase::videoExt));
    }

    // 格式清单按可用宽度折行后的实际高度（putAlignLeft 内部按 rect 宽度折行）
    context.text->setSize(context.px(15));
    const int blockWidth = context.px((std::min)(860.0f, widthLogical - 140.0f));
    const int blockLineHeight = context.text->lineHeight();
    struct FormatBlock {
        const std::string* text;
        int lines = 1;
        int height = 0;
    };
    std::vector<FormatBlock> blocks;
    for (const std::string* block : { &common, &raw, &video }) {
        if (block->empty())
            continue;
        const int textWidth = context.text->measure(block->c_str()).width;
        FormatBlock item{ block, (std::max)(1, (textWidth + blockWidth - 1) / blockWidth) };
        item.height = item.lines * blockLineHeight;
        blocks.push_back(item);
    }

    const auto lineHeightOf = [&](float fontLogical) {
        context.text->setSize(context.px(fontLogical));
        return context.text->lineHeight();
    };
    int total = context.px(86) + context.px(16) + lineHeightOf(30) + context.px(12) + lineHeightOf(19) +
        context.px(10) + lineHeightOf(22) + context.px(4) + lineHeightOf(14) + context.px(24) + context.px(1) +
        context.px(20) + lineHeightOf(15) + context.px(10) + context.px(8);
    for (const auto& block : blocks)
        total += block.height + context.px(8);
    total += context.px(14) + lineHeightOf(17);
    context.y = (std::max)(context.px(20), (context.height() - total) / 2);

    // 警示徽章：圆形描边 + 感叹号
    {
        const int radius = context.px(43);
        const int thickness = (std::max)(2, context.px(3.0f));
        const cv::Point center{ context.centerX, context.y + radius };
        cv::circle(context.canvas, center, radius, bgra(context.accent), thickness, cv::LINE_AA);
        const int barWidth = context.px(9);
        const int barHeight = context.px(30);
        fillRoundedRect(context.canvas, { center.x - barWidth / 2, center.y - context.px(17), barWidth, barHeight },
            barWidth / 2, context.accent);
        cv::circle(context.canvas, { center.x, center.y + context.px(23) }, context.px(5), bgra(context.accent), cv::FILLED, cv::LINE_AA);
        context.y += radius * 2;
    }
    context.gap(16);
    context.centerLine(title, 30, context.fg, widthLogical - 160.0f);
    context.gap(12);
    context.centerLine(reason, 19, context.muted, widthLogical - 160.0f);
    context.gap(10);
    if (!fileName.empty())
        context.centerLine(fileName, 22, context.fg, widthLogical - 160.0f);
    if (!path.empty()) {
        context.text->setSize(context.px(14));
        const std::string shown = elideLeft(*context.text, path, context.px(widthLogical - 160.0f));
        const cv::Size measured = context.text->measure(shown.c_str());
        context.text->putAlignCenter(context.canvas, { 0, context.y, context.width(), measured.height }, shown.c_str(), context.muted);
        context.y += measured.height;
    }
    context.gap(24);

    // 分隔线
    {
        const int thickness = (std::max)(1, context.px(1.0f));
        cv::line(context.canvas, { context.centerX - context.px(280), context.y },
            { context.centerX + context.px(280), context.y }, bgra(context.tag), thickness, cv::LINE_AA);
        context.y += thickness;
    }
    context.gap(20);
    context.centerLine(getUIString(kStrFormatsTitle), 15, context.muted, widthLogical - 100.0f);
    context.gap(10);

    // 支持的格式清单：直接读扩展名集合，永远与解码能力一致
    {
        context.text->setSize(context.px(15));
        const int blockX = context.centerX - blockWidth / 2;
        for (const auto& block : blocks) {
            context.text->putAlignLeft(context.canvas,
                { blockX, context.y, blockWidth, block.height }, block.text->c_str(), context.muted);
            context.y += block.height;
            context.gap(8);
        }
    }
    context.gap(14);
    context.centerLine(getUIString(kStrHomeHint), 17, context.muted, widthLogical - 120.0f);
    return context.canvas;
}

} // namespace

uint64_t infoScreenStamp(cv::Size size, float scale) {
    uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
    mix(static_cast<uint32_t>(size.width));
    mix(static_cast<uint32_t>(size.height));
    mix(static_cast<uint32_t>(std::lround(scale * 100.0f)));
    mix(GlobalVar::settingParameter.UI_LANG);
    mix(GlobalVar::isCurrentUIDarkMode ? 1u : 2u);
    return hash;
}

cv::Mat renderInfoScreen(PlaceholderKind kind, const std::wstring& detail, cv::Size size, float scale) {
    if (kind == PlaceholderKind::None)
        return {};

    size.width = (std::max)(size.width, 32);
    size.height = (std::max)(size.height, 32);
    scale = std::clamp(scale, 0.5f, 4.0f);

    if (kind == PlaceholderKind::Home)
        return renderHome(size, scale);
    return renderError(kind, detail, size, scale);
}

const char* placeholderName(PlaceholderKind kind) {
    switch (kind) {
    case PlaceholderKind::Home: return "home";
    case PlaceholderKind::UnsupportedFormat: return "unsupported";
    case PlaceholderKind::DecodeFailed: return "decode-failed";
    case PlaceholderKind::FileMissing: return "missing";
    default: return "";
    }
}

} // namespace jark
