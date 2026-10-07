#pragma once
#include "jarkUtils.h"

// https://github.com/nothings/stb
// 整个工程只能一个源文件定义 STB_TRUETYPE_IMPLEMENTATION， 其他地方只需include
#include "stb_truetype.h"


class TextDrawer {
public:
    const uint32_t IDR_TTF_DEFAULT = IDR_MSYHMONO_TTF;
    std::vector<std::vector<uint8_t>> asciiCache;

    TextDrawer() {}
    ~TextDrawer() {}

    void setLineGap(float percent);
    void setSize(int newSize);

    // str : UTF-8
    void putText(cv::Mat& img, const int x, const int y, const char* str, intUnion color, bool isAdaptiveFG = false);

    //Rect {x, y, width, height}
    void putAlignCenter(cv::Mat& img, cv::Rect rect, const char* str, intUnion color, bool isAdaptiveFG = false);

    //Rect {x, y, width, height}
    void putAlignLeft(cv::Mat& img, cv::Rect rect, const char* str, intUnion color, bool isAdaptiveFG = false);

    // 某个字符是否可以用当前字体绘制（用于判断是否需要回退字体）
    bool hasGlyph(const int codePoint);

private:
    // 内嵌默认字体 + 按需加载的系统回退字体（内嵌字体没有韩文字形等场景）
    struct FontFace {
        stbtt_fontinfo info{};
        std::vector<uint8_t> data;
        float scale = 0.0f;
        bool ready = false;
    };

    bool hasInit = false;
    float lineGapPercent = 0.1f;

    int fontSize = 16;
    std::vector<FontFace> fonts;
    bool fallbackLoaded = false;

    vector<uint8_t> wordBuff;
    vector<uint8_t> fontFileBuffer;

    rcFileInfo rc;

    bool Init(unsigned int idi, const wchar_t* type);
    bool loadFontFile(FontFace& face, const std::wstring& path);
    void updateScales();
    // 返回能绘制该码点的字体下标（默认返回 0）
    size_t pickFont(int codePoint);
    int putWord(cv::Mat& img, int x, int y, const int codePoint, intUnion color, bool isAdaptiveFG);
};