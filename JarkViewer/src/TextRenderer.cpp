#define NOMINMAX 1

#include "TextRenderer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

// 整个工程只有这里定义 STB_TRUETYPE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {

    constexpr size_t kMaxGlyphCache = 4096;

    int clampi(int value, int low, int high) {
        return value < low ? low : (value > high ? high : value);
    }

    // UTF-8 解码：返回码点，i 前进到下一个字符
    int decodeUtf8(const std::string& text, size_t& index) {
        const auto byte = static_cast<uint8_t>(text[index]);
        if (byte < 0x80) {
            ++index;
            return byte;
        }
        if ((byte & 0xE0) == 0xC0 && index + 1 < text.size()) {
            const int codePoint = ((byte & 0x1F) << 6) | (static_cast<uint8_t>(text[index + 1]) & 0x3F);
            index += 2;
            return codePoint;
        }
        if ((byte & 0xF0) == 0xE0 && index + 2 < text.size()) {
            const int codePoint = ((byte & 0x0F) << 12) |
                ((static_cast<uint8_t>(text[index + 1]) & 0x3F) << 6) |
                (static_cast<uint8_t>(text[index + 2]) & 0x3F);
            index += 3;
            return codePoint;
        }
        if ((byte & 0xF8) == 0xF0 && index + 3 < text.size()) {
            const int codePoint = ((byte & 0x07) << 18) |
                ((static_cast<uint8_t>(text[index + 1]) & 0x3F) << 12) |
                ((static_cast<uint8_t>(text[index + 2]) & 0x3F) << 6) |
                (static_cast<uint8_t>(text[index + 3]) & 0x3F);
            index += 4;
            return codePoint;
        }

        ++index;
        return '?';
    }

    std::wstring systemFontDirectory() {
        wchar_t windowsDir[MAX_PATH] = {};
        const UINT length = ::GetWindowsDirectoryW(windowsDir, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return {};
        return std::wstring(windowsDir) + L"\\Fonts\\";
    }

    // 字体文件数据按完整路径进程内共享：TextRenderer 可能有多个实例（EXIF 面板、占位界面），
    // 每个实例只保留自己的 stbtt_fontinfo 与字形缓存，不重复把几十 MB 的 ttf 读进内存。
    // 仅在界面线程使用（TextRenderer 的既有约束）。
    std::shared_ptr<const std::vector<uint8_t>> sharedFontData(const std::wstring& filePath) {
        static std::unordered_map<std::wstring, std::shared_ptr<const std::vector<uint8_t>>> cache;

        const auto cached = cache.find(filePath);
        if (cached != cache.end())
            return cached->second;

        std::shared_ptr<const std::vector<uint8_t>> data;
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            const auto fileSize = static_cast<size_t>(file.tellg());
            if (fileSize > 0 && fileSize <= (64u << 20)) {
                auto bytes = std::make_shared<std::vector<uint8_t>>(fileSize);
                file.seekg(0);
                file.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(fileSize));
                if (file)
                    data = std::move(bytes);
            }
        }
        cache.emplace(filePath, data);
        return data;
    }

} // namespace

// —— FontFace ——

TextRenderer::FontFace::FontFace(FontFace&& other) noexcept
    : data(std::move(other.data)), info(other.info), ready(other.ready) {
    other.info = nullptr;
    other.ready = false;
}

TextRenderer::FontFace& TextRenderer::FontFace::operator=(FontFace&& other) noexcept {
    if (this != &other) {
        delete info;
        data = std::move(other.data);
        info = other.info;
        ready = other.ready;
        other.info = nullptr;
        other.ready = false;
    }
    return *this;
}

TextRenderer::FontFace::~FontFace() {
    delete info;
}

TextRenderer::~TextRenderer() = default;

// —— 加载 ——

bool TextRenderer::loadPrimaryFont() const {
    const std::wstring fontDir = systemFontDirectory();
    if (fontDir.empty())
        return false;

    // 常见中文字体，按优先级尝试（.ttc 取第 0 个字面）
    for (const wchar_t* fileName : { L"msyh.ttc", L"msyhl.ttc", L"Deng.ttf", L"simhei.ttf", L"simsun.ttc", L"segoeui.ttf" }) {
        auto data = sharedFontData(fontDir + fileName);
        if (!data)
            continue;

        FontFace face;
        face.data = std::move(data);

        const int offset = stbtt_GetFontOffsetForIndex(face.data->data(), 0);
        if (offset < 0)
            continue;

        face.info = new stbtt_fontinfo();
        if (!stbtt_InitFont(face.info, face.data->data(), offset)) {
            delete face.info;
            face.info = nullptr;
            continue;
        }

        face.ready = true;
        JARK_LOG("图像文字字体：{}", jarkUtils::wstringToUtf8(fileName));
        faces_.push_back(std::move(face));
        return true;
    }

    JARK_LOG("未找到可用的系统字体，图像文字将无法绘制");
    return false;
}

bool TextRenderer::loadFallbackFonts() const {
    if (fallbackLoaded_)
        return !faces_.empty();

    fallbackLoaded_ = true;

    const std::wstring fontDir = systemFontDirectory();
    if (fontDir.empty())
        return false;

    // 主字体缺字形时按需追加（韩文/日文特殊字形等）
    for (const wchar_t* fileName : { L"malgun.ttf", L"gulim.ttc", L"meiryo.ttc", L"YuGothM.ttc", L"msjh.ttc" }) {
        auto data = sharedFontData(fontDir + fileName);
        if (!data)
            continue;

        FontFace face;
        face.data = std::move(data);

        const int offset = stbtt_GetFontOffsetForIndex(face.data->data(), 0);
        if (offset < 0)
            continue;

        face.info = new stbtt_fontinfo();
        if (!stbtt_InitFont(face.info, face.data->data(), offset)) {
            delete face.info;
            face.info = nullptr;
            continue;
        }

        face.ready = true;
        JARK_LOG("图像文字回退字体：{}", jarkUtils::wstringToUtf8(fileName));
        faces_.push_back(std::move(face));
        break; // 有一个回退字体就够了，缺字形时再按需扩展
    }

    return !faces_.empty();
}

bool TextRenderer::ensureLoaded() const {
    if (loaded_)
        return !faces_.empty();

    loaded_ = true;
    loadPrimaryFont();
    return !faces_.empty();
}

const TextRenderer::FontFace& TextRenderer::pickFace(int codePoint) const {
    ensureLoaded();

    for (const auto& face : faces_) {
        if (face.ready && stbtt_FindGlyphIndex(face.info, codePoint) != 0)
            return face;
    }

    // 主字体没有这个字形：补一次回退字体再试
    if (!fallbackLoaded_) {
        loadFallbackFonts();
        for (const auto& face : faces_) {
            if (face.ready && stbtt_FindGlyphIndex(face.info, codePoint) != 0)
                return face;
        }
    }

    return faces_.front();
}

// 单个字形的进退宽度（不含字距）
float TextRenderer::glyphAdvance(const FontFace& face, int codePoint) const {
    int advance = 0;
    int leftBearing = 0;
    stbtt_GetCodepointHMetrics(face.info, codePoint, &advance, &leftBearing);
    return advance * stbtt_ScaleForPixelHeight(face.info, static_cast<float>(size_));
}

const TextRenderer::Glyph& TextRenderer::glyphFor(const FontFace& face, int codePoint) const {
    const uint64_t key = (static_cast<uint64_t>(size_) << 32) | static_cast<uint32_t>(codePoint);
    const auto cached = glyphCache_.find(key);
    if (cached != glyphCache_.end())
        return cached->second;

    if (glyphCache_.size() >= kMaxGlyphCache)
        glyphCache_.clear();

    const float scale = stbtt_ScaleForPixelHeight(face.info, static_cast<float>(size_));

    Glyph glyph;

    int advance = 0;
    int leftBearing = 0;
    stbtt_GetCodepointHMetrics(face.info, codePoint, &advance, &leftBearing);
    glyph.advance = advance * scale;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(face.info, codePoint, scale, scale, &x0, &y0, &x1, &y1);

    glyph.width = x1 - x0;
    glyph.height = y1 - y0;
    glyph.offsetX = x0;
    glyph.offsetY = y0;

    if (glyph.width > 0 && glyph.height > 0) {
        glyph.alpha.resize(static_cast<size_t>(glyph.width) * glyph.height);
        stbtt_MakeCodepointBitmap(face.info, glyph.alpha.data(), glyph.width, glyph.height,
            glyph.width, scale, scale, codePoint);
    }

    return glyphCache_.emplace(key, std::move(glyph)).first->second;
}

// —— 度量 ——

void TextRenderer::setSize(int pixelSize) {
    const int clamped = clampi(pixelSize, 6, 512);
    if (clamped == size_)
        return;

    size_ = clamped;
    glyphCache_.clear();
}

void TextRenderer::setLineGap(float percent) {
    lineGapPercent_ = std::clamp(percent, 0.0f, 2.0f);
}

int TextRenderer::lineHeight() const {
    ensureLoaded();
    if (faces_.empty())
        return size_;

    const float scale = stbtt_ScaleForPixelHeight(faces_.front().info, static_cast<float>(size_));
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(faces_.front().info, &ascent, &descent, &lineGap);

    return static_cast<int>(std::ceil((ascent - descent + lineGap) * scale + size_ * lineGapPercent_));
}

cv::Size TextRenderer::measure(const char* utf8) const {
    if (!utf8 || !*utf8 || !ensureLoaded())
        return {};

    const std::string text(utf8);
    const int height = lineHeight();
    int maxWidth = 0;
    int lines = 1;

    size_t index = 0;
    float width = 0.0f;
    int previous = 0;

    while (index < text.size()) {
        const int codePoint = decodeUtf8(text, index);

        if (codePoint == '\n') {
            maxWidth = std::max(maxWidth, static_cast<int>(std::ceil(width)));
            width = 0.0f;
            previous = 0;
            ++lines;
            continue;
        }

        const FontFace& face = pickFace(codePoint);
        const Glyph& glyph = glyphFor(face, codePoint);

        if (previous != 0)
            width += stbtt_GetCodepointKernAdvance(face.info, previous, codePoint) *
            stbtt_ScaleForPixelHeight(face.info, static_cast<float>(size_));

        width += glyph.advance;
        previous = codePoint;
    }

    maxWidth = std::max(maxWidth, static_cast<int>(std::ceil(width)));
    return { maxWidth, height * lines };
}

int TextRenderer::measureText(const char* utf8) const {
    return measure(utf8).width;
}

bool TextRenderer::hasGlyph(int codePoint) {
    ensureLoaded();
    if (faces_.empty())
        return false;

    for (const auto& face : faces_) {
        if (face.ready && stbtt_FindGlyphIndex(face.info, codePoint) != 0)
            return true;
    }
    return false;
}

// —— 绘制 ——

void TextRenderer::blendGlyph(cv::Mat& image, int x, int y, const Glyph& glyph,
    const intUnion& colorRef, uint8_t coverageLimit) {
    if (glyph.width <= 0 || glyph.height <= 0 || image.empty())
        return;

    intUnion color = colorRef; // 拷贝一份：intUnion 的 operator[] 只有非 const 版本

    const int channels = image.channels();
    if (channels != 3 && channels != 4)
        return;

    const uint8_t alphaScale = static_cast<uint8_t>(color[3] * coverageLimit / 255);

    for (int row = 0; row < glyph.height; ++row) {
        const int targetY = y + row;
        if (targetY < 0 || targetY >= image.rows)
            continue;

        uint8_t* targetRow = image.ptr<uint8_t>(targetY);
        const uint8_t* alphaRow = glyph.alpha.data() + static_cast<size_t>(row) * glyph.width;

        for (int column = 0; column < glyph.width; ++column) {
            const int targetX = x + column;
            if (targetX < 0 || targetX >= image.cols)
                continue;

            const int coverage = alphaRow[column] * alphaScale / 255;
            if (coverage <= 0)
                continue;

            uint8_t* pixel = targetRow + static_cast<size_t>(targetX) * channels;
            const int inverse = 255 - coverage;
            pixel[0] = static_cast<uint8_t>((pixel[0] * inverse + color[0] * coverage + 127) / 255);
            pixel[1] = static_cast<uint8_t>((pixel[1] * inverse + color[1] * coverage + 127) / 255);
            pixel[2] = static_cast<uint8_t>((pixel[2] * inverse + color[2] * coverage + 127) / 255);
            if (channels == 4)
                pixel[3] = 255;
        }
    }
}

void TextRenderer::drawLine(cv::Mat& image, int x, int y, const std::string& line,
    const intUnion& color, bool adaptiveContrast) const {
    if (line.empty() || faces_.empty())
        return;

    const FontFace& primary = faces_.front();
    const float primaryScale = stbtt_ScaleForPixelHeight(primary.info, static_cast<float>(size_));

    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(primary.info, &ascent, &descent, &gap);
    const int baseline = y + static_cast<int>(std::lround(ascent * primaryScale));

    // 自适应对比度：先描一层半透明黑影，保证浅色背景下也读得清
    const intUnion shadow(0, 0, 0, 150);

    float penX = 0.0f; // 相对行首的笔位置
    int previous = 0;

    size_t index = 0;
    while (index < line.size()) {
        const int codePoint = decodeUtf8(line, index);
        const FontFace& face = pickFace(codePoint);
        const Glyph& glyph = glyphFor(face, codePoint);

        if (previous != 0) {
            penX += stbtt_GetCodepointKernAdvance(face.info, previous, codePoint) *
                stbtt_ScaleForPixelHeight(face.info, static_cast<float>(size_));
        }

        const int drawX = x + static_cast<int>(std::lround(penX)) + glyph.offsetX;
        const int drawY = baseline + glyph.offsetY;

        if (adaptiveContrast)
            blendGlyph(image, drawX + 1, drawY + 1, glyph, shadow, 255);

        blendGlyph(image, drawX, drawY, glyph, color, 255);

        penX += glyph.advance;
        previous = codePoint;
    }
}

cv::Size TextRenderer::putText(cv::Mat& image, int x, int y, const char* utf8,
    intUnion color, bool adaptiveContrast) {
    if (!utf8 || !*utf8 || image.empty() || !ensureLoaded())
        return {};

    const std::string text(utf8);
    const std::vector<std::string> lines = wrapText(utf8, 0);
    const int height = lineHeight();

    int widest = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        drawLine(image, x, y + static_cast<int>(i) * height, lines[i], color, adaptiveContrast);
        widest = std::max(widest, measure(lines[i].c_str()).width);
    }

    return { widest, height * static_cast<int>(lines.size()) };
}

std::vector<std::string> TextRenderer::wrapText(const char* utf8, int maxWidth) const {
    std::vector<std::string> lines;
    if (!utf8 || !*utf8)
        return lines;

    // 宽度按字增量累加，避免每加一个字都重新测量整行
    const std::string text(utf8);
    std::string currentLine;
    float lineWidth = 0.0f;
    int previous = 0;

    size_t index = 0;
    while (index < text.size()) {
        const size_t charStart = index;
        const int codePoint = decodeUtf8(text, index);
        const std::string character = text.substr(charStart, index - charStart);

        if (codePoint == '\n') {
            lines.push_back(std::move(currentLine));
            currentLine.clear();
            lineWidth = 0.0f;
            previous = 0;
            continue;
        }

        const FontFace& face = pickFace(codePoint);
        const float scale = stbtt_ScaleForPixelHeight(face.info, static_cast<float>(size_));
        float glyphWidth = 0.0f;
        if (previous != 0)
            glyphWidth += stbtt_GetCodepointKernAdvance(face.info, previous, codePoint) * scale;
        glyphWidth += glyphAdvance(face, codePoint);

        if (maxWidth > 0 && !currentLine.empty() && lineWidth + glyphWidth > maxWidth) {
            lines.push_back(std::move(currentLine));
            currentLine.clear();
            lineWidth = 0.0f;
        }

        currentLine += character;
        lineWidth += glyphWidth;
        previous = codePoint;
    }

    lines.push_back(std::move(currentLine));
    return lines;
}

void TextRenderer::putAlignLeft(cv::Mat& image, cv::Rect rect, const char* utf8,
    intUnion color, bool adaptiveContrast) {
    if (image.empty() || rect.width <= 0 || !ensureLoaded())
        return;

    const std::vector<std::string> lines = wrapText(utf8, rect.width);
    const int height = lineHeight();

    for (size_t i = 0; i < lines.size(); ++i) {
        const int y = rect.y + static_cast<int>(i) * height;
        if (y + height > rect.y + rect.height)
            break; // 超出矩形就不画了
        drawLine(image, rect.x, y, lines[i], color, adaptiveContrast);
    }
}

void TextRenderer::putAlignCenter(cv::Mat& image, cv::Rect rect, const char* utf8,
    intUnion color, bool adaptiveContrast) {
    if (image.empty() || rect.width <= 0 || !ensureLoaded())
        return;

    const std::vector<std::string> lines = wrapText(utf8, rect.width);
    const int height = lineHeight();
    const int totalHeight = height * static_cast<int>(lines.size());
    const int startY = rect.y + std::max(0, (rect.height - totalHeight) / 2);

    for (size_t i = 0; i < lines.size(); ++i) {
        const int width = measure(lines[i].c_str()).width;
        drawLine(image, rect.x + std::max(0, (rect.width - width) / 2), startY + static_cast<int>(i) * height,
            lines[i], color, adaptiveContrast);
    }
}

void TextRenderer::putAlignRight(cv::Mat& image, cv::Rect rect, const char* utf8,
    intUnion color, bool adaptiveContrast) {
    if (image.empty() || rect.width <= 0 || !ensureLoaded())
        return;

    const std::vector<std::string> lines = wrapText(utf8, rect.width);
    const int height = lineHeight();
    const int totalHeight = height * static_cast<int>(lines.size());
    const int startY = rect.y + std::max(0, (rect.height - totalHeight) / 2);

    for (size_t i = 0; i < lines.size(); ++i) {
        const int width = measure(lines[i].c_str()).width;
        drawLine(image, rect.x + std::max(0, rect.width - width), startY + static_cast<int>(i) * height,
            lines[i], color, adaptiveContrast);
    }
}
