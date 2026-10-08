#pragma once

// 图像内文字渲染（标注文字、EXIF 面板等）：用系统字体在 cv::Mat 上绘制 UTF-8 文本。
//
// 与旧实现（等宽模型 + 逐字固定步进）不同，这里用 stb_truetype 的真实字形度量：
// 每个字形的进退宽度、bearing、字距（kerning）都取自字体本身，并按 (字号, 码位) 缓存位图。
// 界面文字不归它管——那部分交给 ImGui。
//
// 字体来自系统（微软雅黑/等线/黑体/宋体…），工程不再内嵌 ttf。

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <opencv2/opencv.hpp>

#include "jarkUtils.h"

struct stbtt_fontinfo;

class TextRenderer {
public:
    TextRenderer() = default;
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // 字号（像素）。设置后 measureText/putText 都按它计算
    void setSize(int pixelSize);
    int size() const { return size_; }

    // 行距（相对字号的百分比，0.15 = 15%）
    void setLineGap(float percent);

    // 文本尺寸：宽度取最长行，高度为总行高
    cv::Size measure(const char* utf8) const;
    int lineHeight() const;
    int measureText(const char* utf8) const; // 兼容旧接口：最长行宽度
    bool hasGlyph(int codePoint);

    // 在 (x, y) 处绘制（左上角对齐），支持 '\n' 多行；返回绘制区域的尺寸
    cv::Size putText(cv::Mat& image, int x, int y, const char* utf8,
        intUnion color, bool adaptiveContrast = false);

    // 在矩形内绘制：按 rect 宽度自动换行
    void putAlignLeft(cv::Mat& image, cv::Rect rect, const char* utf8,
        intUnion color, bool adaptiveContrast = false);
    void putAlignCenter(cv::Mat& image, cv::Rect rect, const char* utf8,
        intUnion color, bool adaptiveContrast = false);
    void putAlignRight(cv::Mat& image, cv::Rect rect, const char* utf8,
        intUnion color, bool adaptiveContrast = false);

private:
    struct FontFace {
        // 字体文件数据按文件路径进程内共享（多个 TextRenderer 实例不再各读一份 ttf）
        std::shared_ptr<const std::vector<uint8_t>> data;
        stbtt_fontinfo* info = nullptr;
        bool ready = false;

        FontFace() = default;
        FontFace(FontFace&& other) noexcept;
        FontFace& operator=(FontFace&& other) noexcept;
        ~FontFace();
    };

    struct Glyph {
        std::vector<uint8_t> alpha; // width * height 的覆盖率
        int width = 0;
        int height = 0;
        int offsetX = 0;            // 相对笔位置的左上角偏移
        int offsetY = 0;
        float advance = 0.0f;
    };

    bool ensureLoaded() const;
    bool loadPrimaryFont() const;
    bool loadFallbackFonts() const;

    const FontFace& pickFace(int codePoint) const;
    const Glyph& glyphFor(const FontFace& face, int codePoint) const;
    float glyphAdvance(const FontFace& face, int codePoint) const;

    // 按宽度折行；maxWidth <= 0 时只在 '\n' 处断行
    std::vector<std::string> wrapText(const char* utf8, int maxWidth) const;

    void drawLine(cv::Mat& image, int x, int y, const std::string& line,
        const intUnion& color, bool adaptiveContrast) const;

    static void blendGlyph(cv::Mat& image, int x, int y, const Glyph& glyph,
        const intUnion& color, uint8_t coverageLimit);

    mutable std::vector<FontFace> faces_;
    mutable std::unordered_map<uint64_t, Glyph> glyphCache_;
    mutable bool loaded_ = false;
    mutable bool fallbackLoaded_ = false;

    int size_ = 20;
    float lineGapPercent_ = 0.15f;
};
