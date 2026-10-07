#pragma once
#include "TextDrawer.h"

#include <fstream>

// https://github.com/nothings/stb
// 整个工程只能一个源文件定义 STB_TRUETYPE_IMPLEMENTATION， 其他地方只需include
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

void TextDrawer::setLineGap(float percent) {
    lineGapPercent = percent;
}

void TextDrawer::setSize(int newSize) {
    fontSize = newSize > 2048 ? 2048 : (newSize < 16 ? 16 : newSize);

    auto newBufferSize = 2ULL * fontSize * fontSize;
    wordBuff.resize(newBufferSize);
    memset(wordBuff.data(), 0, newBufferSize);
    asciiCache.clear();
    asciiCache.resize(256);

    updateScales();
}

// str : UTF-8
void TextDrawer::putText(cv::Mat& img, const int x, const int y, const char* str, intUnion color, bool isAdaptiveFG) {
    if (!hasInit) {
        hasInit = Init(IDR_TTF_DEFAULT, L"TTF");
    }
    if (fonts.empty() || !fonts[0].ready)
        return;

    int codePoint = '?';
    int xOffset = x, yOffset = y;
    const auto len = strlen(str);
    size_t i = 0;
    while (i < len)
    {
        if ((str[i] & 0x80) == 0) {
            codePoint = str[i];
            i++;
        }
        else if ((str[i] & 0xe0) == 0xc0) { // 110x'xxxx 10xx'xxxx
            codePoint = ((str[i] & 0x1f) << 6) | (str[i + 1] & 0x3f);
            i += 2;
        }
        else if ((str[i] & 0xf0) == 0xe0) { // 1110'xxxx 10xx'xxxx 10xx'xxxx
            codePoint = ((str[i] & 0x0f) << 12) | ((str[i + 1] & 0x3f) << 6) | (str[i + 2] & 0x3f);
            i += 3;
        }
        else if ((str[i] & 0xf8) == 0xf0) { // 1111'0xxx 10xx'xxxx 10xx'xxxx 10xx'xxxx
            codePoint = ((str[i] & 0x07) << 18) | ((str[i + 1] & 0x3f) << 12) | ((str[i + 2] & 0x3f) << 6) | (str[i + 3] & 0x3f);
            i += 4;
        }
        else {
            codePoint = '?';
            i++;
        }

        if (codePoint == '\n') {
            yOffset += int(fontSize * (1 + lineGapPercent));
            xOffset = x;
        }
        else {
            xOffset += putWord(img, xOffset, yOffset, codePoint, color, isAdaptiveFG);
        }
    }
}

//Rect {x, y, width, height}
void TextDrawer::putAlignCenter(cv::Mat& img, cv::Rect rect, const char* str, intUnion color, bool isAdaptiveFG) {
    int codePoint = '?';
    int H = 1, W = 0, W_cnt = 0;
    const auto len = strlen(str);
    size_t i = 0;
    while (i < len) {
        if ((str[i] & 0x80) == 0) {
            codePoint = str[i];
            i++;
        }
        else if ((str[i] & 0xe0) == 0xc0) { // 110x'xxxx 10xx'xxxx
            codePoint = ((str[i] & 0x1f) << 6) | (str[i + 1] & 0x3f);
            i += 2;
        }
        else if ((str[i] & 0xf0) == 0xe0) { // 1110'xxxx 10xx'xxxx 10xx'xxxx
            codePoint = ((str[i] & 0x0f) << 12) | ((str[i + 1] & 0x3f) << 6) | (str[i + 2] & 0x3f);
            i += 3;
        }
        else if ((str[i] & 0xf8) == 0xf0) { // 1111'0xxx 10xx'xxxx 10xx'xxxx 10xx'xxxx
            codePoint = ((str[i] & 0x07) << 18) | ((str[i + 1] & 0x3f) << 12) | ((str[i + 2] & 0x3f) << 6) | (str[i + 3] & 0x3f);
            i += 4;
        }
        else {
            codePoint = '?';
            i++;
        }

        if (codePoint == '\n') {
            H++;
            if (W_cnt > W) {
                W = W_cnt;
                W_cnt = 0;
            }
        }
        else {
            W_cnt += (codePoint < 256 ? 1 : 2);
        }
    }

    if (W_cnt > W)
        W = W_cnt;

    const int sizeAndGap = int(fontSize * (1 + lineGapPercent));// Mono Font
    H *= sizeAndGap;
    W = sizeAndGap * W / 2;

    const int x = rect.x + (rect.width - W) / 2;
    const int y = rect.y + (rect.height - H) / 2;

    putText(img, x, y, str, color, isAdaptiveFG);
}

//Rect {x, y, width, height}
void TextDrawer::putAlignLeft(cv::Mat& img, cv::Rect rect, const char* str, intUnion color, bool isAdaptiveFG) {
    if (!hasInit) {
        hasInit = Init(IDR_TTF_DEFAULT, L"TTF");
    }
    if (fonts.empty() || !fonts[0].ready)
        return;

    int codePoint = '?';
    int xOffset = rect.x, yOffset = rect.y;
    int areaWidth = rect.width;
    const auto len = strlen(str);
    size_t i = 0;
    while (i < len) {
        if ((str[i] & 0x80) == 0) {
            codePoint = str[i];
            i++;
        }
        else if ((str[i] & 0xe0) == 0xc0) { // 110x'xxxx 10xx'xxxx
            codePoint = ((str[i] & 0x1f) << 6) | (str[i + 1] & 0x3f);
            i += 2;
        }
        else if ((str[i] & 0xf0) == 0xe0) { // 1110'xxxx 10xx'xxxx 10xx'xxxx
            codePoint = ((str[i] & 0x0f) << 12) | ((str[i + 1] & 0x3f) << 6) | (str[i + 2] & 0x3f);
            i += 3;
        }
        else if ((str[i] & 0xf8) == 0xf0) { // 1111'0xxx 10xx'xxxx 10xx'xxxx 10xx'xxxx
            codePoint = ((str[i] & 0x07) << 18) | ((str[i + 1] & 0x3f) << 12) | ((str[i + 2] & 0x3f) << 6) | (str[i + 3] & 0x3f);
            i += 4;
        }
        else {
            codePoint = '?';
            i++;
        }

        if (codePoint == '\n' || (xOffset + fontSize) > (rect.x+rect.width)) {
            yOffset += int(fontSize * (1 + lineGapPercent));
            if (yOffset + fontSize > (rect.y+rect.height)) {
                rect.x += rect.width;
                yOffset = rect.y;

                if (rect.x >= img.cols) {
                    return;
                }
            }
            xOffset = rect.x;
            if (codePoint == '\n')
                continue;
        }

        xOffset += putWord(img, xOffset, yOffset, codePoint, color, isAdaptiveFG);
    }
}

bool TextDrawer::Init(unsigned int idi, const wchar_t* type) {
    rc = jarkUtils::GetResource(idi, type);

    FontFace font;
    if (!rc.ptr || !stbtt_InitFont(&font.info, rc.ptr, 0)) {
        JARK_LOG("stbtt_InitFont failed (resource {})", idi);
        return false;
    }
    font.ready = true;
    fonts.clear();
    fonts.push_back(std::move(font));

    updateScales();

    auto newBufferSize = 2ULL * fontSize * fontSize;
    wordBuff.resize(newBufferSize);
    memset(wordBuff.data(), 0, newBufferSize);
    asciiCache.clear();
    asciiCache.resize(256);

    return true;
}

// 读取系统字体文件（用于回退字体）
bool TextDrawer::loadFontFile(FontFace& face, const std::wstring& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    const auto fileSize = static_cast<size_t>(file.tellg());
    if (fileSize == 0 || fileSize > (64u << 20))
        return false;

    face.data.resize(fileSize);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(face.data.data()), static_cast<std::streamsize>(fileSize));

    // 支持 ttc 字体集合（取第一个字体）
    const int offset = stbtt_GetFontOffsetForIndex(face.data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&face.info, face.data.data(), offset))
        return false;

    face.ready = true;
    face.scale = stbtt_ScaleForPixelHeight(&face.info, (float)fontSize);
    return true;
}

void TextDrawer::updateScales() {
    for (auto& font : fonts) {
        if (font.ready)
            font.scale = stbtt_ScaleForPixelHeight(&font.info, (float)fontSize);
    }
}

bool TextDrawer::hasGlyph(const int codePoint) {
    if (!hasInit) {
        hasInit = true;
        Init(IDR_TTF_DEFAULT, L"TTF");
    }
    return pickFont(codePoint) == 0 ?
        stbtt_FindGlyphIndex(&fonts[0].info, codePoint) != 0 : true;
}

// 默认字体缺字形时，按需加载系统回退字体（例如韩文需要 Malgun Gothic）
size_t TextDrawer::pickFont(int codePoint) {
    if (fonts.empty())
        return 0;

    if (stbtt_FindGlyphIndex(&fonts[0].info, codePoint) != 0)
        return 0;

    if (!fallbackLoaded) {
        fallbackLoaded = true;

        wchar_t windowsDir[MAX_PATH] = {};
        const UINT length = ::GetWindowsDirectoryW(windowsDir, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            const std::wstring fontDir = std::wstring(windowsDir) + L"\\Fonts\\";
            // 韩文/其它默认字体缺失的字形：按顺序尝试系统中常见字体
            for (const wchar_t* fileName : { L"malgun.ttf", L"gulim.ttc", L"batang.ttc", L"NanumGothic.ttf", L"msyh.ttc" }) {
                FontFace face;
                if (!loadFontFile(face, fontDir + fileName))
                    continue;

                JARK_LOG("字体回退加载: {}", jarkUtils::wstringToUtf8(fileName));
                fonts.push_back(std::move(face));
                if (stbtt_FindGlyphIndex(&fonts.back().info, codePoint) != 0)
                    break;
            }
        }
    }

    for (size_t i = 1; i < fonts.size(); ++i) {
        if (stbtt_FindGlyphIndex(&fonts[i].info, codePoint) != 0)
            return i;
    }
    return 0;
}

int TextDrawer::putWord(cv::Mat& img, int x, int y, const int codePoint, intUnion color, bool isAdaptiveFG) {
    const size_t fontIndex = pickFont(codePoint);
    const FontFace& font = fonts[fontIndex];
    const float scale = font.scale;

    int c_x0, c_y0, c_x1, c_y1;
    stbtt_GetCodepointBitmapBox(&font.info, codePoint, scale, scale, &c_x0, &c_y0, &c_x1, &c_y1);

    int wordWidth = c_x1 - c_x0;
    int wordHigh = c_y1 - c_y0;

    uint8_t* wordBuffPtr = nullptr;
    if (codePoint < 256 && fontIndex == 0) { // 仅默认字体缓存 ASCII 字形
        if (asciiCache[codePoint].empty()) {
            asciiCache[codePoint].resize(wordBuff.size());
            stbtt_MakeCodepointBitmap(&font.info, asciiCache[codePoint].data(), wordWidth, wordHigh, fontSize, scale, scale, codePoint);
        }
        wordBuffPtr = asciiCache[codePoint].data();
    }
    else {
        stbtt_MakeCodepointBitmap(&font.info, wordBuff.data(), wordWidth, wordHigh, fontSize, scale, scale, codePoint);
        wordBuffPtr = wordBuff.data();
    }

    y += fontSize + c_y0;
    x += c_x0;

    for (int yy = 0; yy < wordHigh; yy++) {
        if (y + yy >= img.rows)
            break;

        auto ptr = (intUnion*)(img.ptr() + img.step1() * (y + yy));

        for (int xx = 0; xx < wordWidth; xx++) {
            if (x + xx >= img.cols)
                break;

            auto& orgColor = ptr[x + xx];

            if (isAdaptiveFG) {
                int gray = (306 * orgColor[0] + 601 * orgColor[1] + 117 * orgColor[2]) / 1024;
                color = gray < 128 ? deepTheme.FG : lightTheme.FG;
            }

            int alpha = wordBuffPtr[yy * fontSize + xx] * color[3] / 255;
            if (alpha)
                orgColor = {
                    (uint8_t)((orgColor[0] * (255 - alpha) + color[0] * alpha + 255) >> 8),
                    (uint8_t)((orgColor[1] * (255 - alpha) + color[1] * alpha + 255) >> 8),
                    (uint8_t)((orgColor[2] * (255 - alpha) + color[2] * alpha + 255) >> 8),
                    255 };
        }
    }

    const int size = int(fontSize * (1 + lineGapPercent));
    return codePoint < 256 ? (size / 2) : size;
}
