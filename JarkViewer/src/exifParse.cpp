#include "jarkUtils.h"
#include "Localization.h"
#include "exifParse.h"
#include "AiPrompt.h"


namespace {

    bool isUserCommentTag(const std::string& tagName) {
        return tagName.find("UserComment") != std::string::npos || tagName.ends_with("0x9286");
    }

    // UTF-16（可带 BOM）转 UTF-8
    std::string utf16ToUtf8(std::span<const uint8_t> bytes) {
        if (bytes.size() < 2)
            return {};

        bool bigEndian = true;
        bool hadBom = false;
        if (bytes[0] == 0xFF && bytes[1] == 0xFE) {
            bigEndian = false;
            hadBom = true;
            bytes = bytes.subspan(2);
        }
        else if (bytes[0] == 0xFE && bytes[1] == 0xFF) {
            bigEndian = true;
            hadBom = true;
            bytes = bytes.subspan(2);
        }

        const auto decode = [&](bool be) {
            std::wstring text;
            text.reserve(bytes.size() / 2);
            for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
                const uint16_t unit = be
                    ? static_cast<uint16_t>((bytes[i] << 8) | bytes[i + 1])
                    : static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
                text.push_back(static_cast<wchar_t>(unit));
            }
            return text;
        };

        auto text = decode(bigEndian);
        const bool suspicious = std::any_of(text.begin(), text.end(), [](wchar_t c) {
            return c < 0x09 || (c > 0x0D && c < 0x20);
        });
        if (suspicious && !hadBom)
            text = decode(!bigEndian);

        return jarkUtils::wstringToUtf8(text);
    }

    // 解析 Exif UserComment：8 字节字符集前缀 + 正文（ASCII / UNICODE / JIS）
    std::string decodeUserCommentValue(const Exiv2::Value& value, Exiv2::ByteOrder byteOrder) {
        auto clonedValue = value.clone();
        const size_t size = clonedValue->size();
        if (size == 0 || size > INT32_MAX)
            return {};

        std::vector<uint8_t> buffer(size);
        clonedValue->copy(buffer.data(), byteOrder);

        std::span<const uint8_t> body(buffer);
        if (body.size() >= 8 && std::memcmp(body.data(), "UNICODE\0", 8) == 0)
            return utf16ToUtf8(body.subspan(8));

        if (body.size() >= 8 && (std::memcmp(body.data(), "ASCII\0\0\0", 8) == 0 ||
            std::memcmp(body.data(), "JIS\0\0\0\0\0", 8) == 0)) {
            body = body.subspan(8);
        }

        std::string text(reinterpret_cast<const char*>(body.data()), body.size());
        while (!text.empty() && text.back() == '\0')
            text.pop_back();
        return text;
    }

} // namespace

std::string ExifParse::getSimpleInfo(wstring_view path, int width, int height, const uint8_t* buf, size_t fileSize) {
    return (path.ends_with(L".ico") || width == 0 || height == 0) ?
        std::format("{}: {}\n{}: {}\n",
            getUIString(39), jarkUtils::wstringToUtf8(path), getUIString(40), jarkUtils::size2Str(fileSize)) :
        std::format("{}: {}\n{}: {}\n{}: {}x{}",
            getUIString(39), jarkUtils::wstringToUtf8(path), getUIString(40), jarkUtils::size2Str(fileSize), getUIString(41), width, height);
}

std::string ExifParse::handleMathDiv(std::string_view str) {
    if (str.empty())
        return "";

    // 处理可能的前导负号，并确定数字部分的起始位置
    bool negative = false;
    size_t pos = 0;
    if (str[0] == '-') {
        negative = true;
        pos = 1;
        if (str.size() == 1)   // 单独的“-”无效
            return "";
    }

    // 遍历字符：只允许数字和恰好一个'/'，'/'前后都必须有数字
    size_t slashPos = std::string_view::npos;
    for (size_t i = pos; i < str.size(); ++i) {
        char c = str[i];
        if (c >= '0' && c <= '9') {
            continue;
        }
        else if (c == '/') {
            if (slashPos == std::string_view::npos) {
                slashPos = i;
            }
            else {            // 出现第二个'/'，非法
                return "";
            }
        }
        else {                // 非法字符
            return "";
        }
    }

    // 必须恰好有一个'/'，且分子和分母均非空
    if (slashPos == std::string_view::npos ||
        slashPos == pos || slashPos == str.size() - 1) {
        return "";
    }

    // 解析分子和分母
    try {
        long long numerator = std::stoll(std::string(str.substr(pos, slashPos - pos)));
        long long denominator = std::stoll(std::string(str.substr(slashPos + 1)));

        if (denominator == 0)
            denominator = 1;

        double value = static_cast<double>(numerator) / denominator;
        if (negative)
            value = -value;      // 仅应用一次负号

        std::string result = std::format("{:.2f}", value);
        if (result.size() >= 3 && result.compare(result.size() - 3, 3, ".00") == 0) {
            result.erase(result.size() - 3);
        }
        return result;
    }
    catch (const std::exception&) {
        return "";
    }
}

std::string ExifParse::exifDataToString(wstring_view path, const Exiv2::ExifData& exifData) {
    if (exifData.empty()) {
        JARK_LOG("No EXIF data {}", jarkUtils::wstringToUtf8(path));
        return "";
    }

    std::ostringstream oss;
    std::ostringstream ossEnd;

    for (const auto& tag : exifData) {
        const std::string& tagName = tag.key();
        bool toEnd = true;
        std::string translatedTagName;

        if (jark::prefersChineseResources()) {
            if (tagName.starts_with("Exif.SubImage")) {
                string tag = "Exif.Image" + tagName.substr(14);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("子图" + tagName.substr(13, 2) + exifTagsMap.at(tag)) :
                    ("子图" + tagName.substr(13));
            }
            else if (tagName.starts_with("Exif.Thumbnail")) {
                string tag = "Exif.Image" + tagName.substr(14);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("缩略图." + exifTagsMap.at(tag)) :
                    ("缩略图" + tagName.substr(14));
            }
            else if (tagName.starts_with("Exif.Nikon")) {
                translatedTagName = "尼康" + tagName.substr(10);
            }
            else if (tagName.starts_with("Exif.CanonCs")) {
                string tag = "Exif.Image" + tagName.substr(12);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(12);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("佳能Cs." + exifTagsMap.at(tag)) :
                    ("佳能Cs." + tagName.substr(13));
            }
            else if (tagName.starts_with("Exif.CanonSi")) {
                string tag = "Exif.Image" + tagName.substr(12);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(12);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("佳能Si." + exifTagsMap.at(tag)) :
                    ("佳能Si." + tagName.substr(13));
            }
            else if (tagName.starts_with("Exif.CanonPi")) {
                string tag = "Exif.Image" + tagName.substr(12);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(12);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("佳能Pi." + exifTagsMap.at(tag)) :
                    ("佳能Pi." + tagName.substr(13));
            }
            else if (tagName.starts_with("Exif.CanonPa")) {
                string tag = "Exif.Image" + tagName.substr(12);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(12);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("佳能Pa." + exifTagsMap.at(tag)) :
                    ("佳能Pa." + tagName.substr(13));
            }
            else if (tagName.starts_with("Exif.Canon")) {
                string tag = "Exif.Image" + tagName.substr(10);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(10);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("佳能." + exifTagsMap.at(tag)) :
                    ("佳能." + tagName.substr(11));
            }
            else if (tagName.starts_with("Exif.Pentax")) {
                string tag = "Exif.Image" + tagName.substr(11);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(11);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("宾得." + exifTagsMap.at(tag)) :
                    ("宾得." + tagName.substr(12));
            }
            else if (tagName.starts_with("Exif.Fujifilm")) {
                string tag = "Exif.Image" + tagName.substr(13);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(13);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("富士." + exifTagsMap.at(tag)) :
                    ("富士." + tagName.substr(14));
            }
            else if (tagName.starts_with("Exif.Olympus")) {
                string tag = "Exif.Image" + tagName.substr(12);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(12);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("奥林巴斯." + exifTagsMap.at(tag)) :
                    ("奥林巴斯." + tagName.substr(13));
            }
            else if (tagName.starts_with("Exif.Panasonic")) {
                string tag = "Exif.Image" + tagName.substr(14);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(14);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("松下." + exifTagsMap.at(tag)) :
                    ("松下." + tagName.substr(15));
            }
            else if (tagName.starts_with("Exif.Sony1")) {
                string tag = "Exif.Image" + tagName.substr(10);
                if (!exifTagsMap.contains(tag))tag = "Exif.Photo" + tagName.substr(10);
                translatedTagName = exifTagsMap.contains(tag) ?
                    ("索尼." + exifTagsMap.at(tag)) :
                    ("索尼." + tagName.substr(11));
            }
            else {
                toEnd = false;
                translatedTagName = exifTagsMap.contains(tagName) ? exifTagsMap.at(tagName) : tagName;
            }
        }
        else {
            translatedTagName = tagName;
        }

        std::string tagValue;
        if (tag.typeId() == Exiv2::TypeId::undefined || tagName == "Exif.Image.XMLPacket") {
            auto tmp = tag.toString();
            std::istringstream iss(tmp);
            std::string result;
            int number;
            while (iss >> number) {
                if (number < ' ' || number > 127)
                    break;
                result += static_cast<char>(number);
            }
            tagValue = result.empty() ? tmp : result + " [" + tmp + "]";
        }
        else {
            tagValue = tag.toString();
        }

        if (tagName == "Exif.GPSInfo.GPSLatitudeRef" || tagName == "Exif.GPSInfo.GPSLongitudeRef") {
            if (tagValue.length() > 0) {
                switch (tagValue[0])
                {
                case 'N':tagValue = getUIString(47); break;
                case 'S':tagValue = getUIString(48); break;
                case 'E':tagValue = getUIString(49); break;
                case 'W':tagValue = getUIString(50); break;
                }
            }
        }

        if (tagName == "Exif.Photo.MakerNote") {
            if (tagValue.length() > 0 && tagValue.starts_with("Apple iOS")) {
                tagValue = "Apple IOS";
            }
        }

        if (tagName == "Exif.GPSInfo.GPSLatitude" || tagName == "Exif.GPSInfo.GPSLongitude") {
            auto firstSpaceIdx = tagValue.find_first_of(' ');
            auto secondSpaceIdx = tagValue.find_last_of(' ');
            if (firstSpaceIdx != string::npos && secondSpaceIdx != string::npos && firstSpaceIdx < secondSpaceIdx) {
                auto n1 = handleMathDiv(tagValue.substr(0, firstSpaceIdx));
                auto n2 = handleMathDiv(tagValue.substr(firstSpaceIdx + 1, secondSpaceIdx - firstSpaceIdx - 1));
                auto n3 = handleMathDiv(tagValue.substr(secondSpaceIdx + 1));
                tagValue = std::format("{}°{}' {}'' ({})", n1, n2, n3, tagValue);
            }
        }
        else if (tagName == "Exif.GPSInfo.GPSTimeStamp") {
            auto firstSpaceIdx = tagValue.find_first_of(' ');
            auto secondSpaceIdx = tagValue.find_last_of(' ');
            if (firstSpaceIdx != string::npos && secondSpaceIdx != string::npos && firstSpaceIdx < secondSpaceIdx) {
                auto n1 = handleMathDiv(tagValue.substr(0, firstSpaceIdx));
                auto n2 = handleMathDiv(tagValue.substr(firstSpaceIdx + 1, secondSpaceIdx - firstSpaceIdx - 1));
                auto n3 = handleMathDiv(tagValue.substr(secondSpaceIdx + 1));
                tagValue = std::format("{}:{}:{} ({})", n1, n2, n3, tagValue);
            }
        }
        else if (isUserCommentTag(tagName)) { // 可能包含 AI 生图提示词
            tagValue = decodeUserCommentValue(tag.value(), Exiv2::ByteOrder::bigEndian);

            if (const auto promptText = jark::formatAiPromptText(tagValue); !promptText.empty())
                tagValue = promptText;
        }
        else if (exifTagsUnicodeStr.contains(tagName)) {
            auto tagValueClone = tag.value().clone();

            if (tagValueClone->size() == 0 || tagValueClone->size() > INT32_MAX) {
                tagValue.clear();
            }
            else {
                vector<WCHAR> buf(tagValueClone->size() / 2 + 1, 0);
                tagValueClone->copy((uint8_t*)buf.data(), Exiv2::ByteOrder::littleEndian);
                wstring_view str(buf.data(), buf.size());
                tagValue = jarkUtils::wstringToUtf8(str);
            }
        }
        else if (2 < tagValue.length() && tagValue.length() < 100) {
            auto res = handleMathDiv(tagValue);
            if (!res.empty())
                tagValue = std::format("{} ({})", res, tagValue);
        }

        string tmp;
        if (isUserCommentTag(tagName))
            tmp = "\n" + translatedTagName + ": " + tagValue; // 可能是一整段提示词，不截断
        else
            tmp = "\n" + translatedTagName + ": " + (tagValue.length() < 100 ? tagValue :
                tagValue.substr(0, 100) + std::format(" ...] length:{}", tagValue.length()));

        if (toEnd)ossEnd << tmp;
        else oss << tmp;
    }

    return oss.str() + ossEnd.str();
}

std::string ExifParse::xmpDataToString(wstring_view path, const Exiv2::XmpData& xmpData) {
    if (xmpData.empty()) {
        return "";
    }

    string xmpStr = "\n\nXmp Data:";
    for (const auto& tag : xmpData) {
        xmpStr += "\n" + tag.key() + ": " + tag.value().toString();
    }
    return xmpStr;
}

std::string ExifParse::iptcDataToString(wstring_view path, const Exiv2::IptcData& IptcData) {
    if (IptcData.empty()) {
        return "";
    }

    string itpcStr = "\n\nIptc Data:";
    for (const auto& tag : IptcData) {
        itpcStr += "\n" + tag.key() + ": " + tag.value().toString();
    }
    return itpcStr;
}

std::string ExifParse::getExif(wstring_view path, const uint8_t* buf, size_t fileSize) {
    return getExifDetail(path, buf, fileSize).text;
}

ExifParse::Detail ExifParse::getExifDetail(wstring_view path, const uint8_t* buf, size_t fileSize) {
    static std::mutex mtx;

    std::lock_guard<std::mutex> lock(mtx);

    Detail detail;

    try {
        auto image = Exiv2::ImageFactory::open(buf, fileSize);
        image->readMetadata();

        auto exifStr = exifDataToString(path, image->exifData());
        auto xmpStr = xmpDataToString(path, image->xmpData());
        auto iptcStr = iptcDataToString(path, image->iptcData());

        // 方向：直接取结构化字段，不再到文本里搜“方向:”标签再读一个字符
        for (const auto& tag : image->exifData()) {
            if (tag.key() != "Exif.Image.Orientation")
                continue;

            try {
                const int64_t value = tag.toInt64();
                if (value >= 1 && value <= 8)
                    detail.orientation = static_cast<int>(value);
            }
            catch (const std::exception&) {
                detail.orientation = 1; // 非整数值（个别写入器会写字符串），保持默认
            }
            break;
        }

        // AI 生图提示词：主要来自 PNG 文本块（tEXt/zTXt/iTXt）。
        // JPEG/WebP 的提示词存放在 Exif UserComment 里，已在 exifDataToString() 中
        // 就地按提示词格式渲染，无需在这里再补一次（否则会重复显示）。
        string prompt;
        for (const auto& section : jark::extractAiPrompts(std::span<const uint8_t>(buf, fileSize)))
            prompt += section.text;

        if ((exifStr.length() + xmpStr.length() + iptcStr.length() + prompt.length()) > 0)
            detail.text = std::format("\n\n{}\n{}{}{}{}", getUIString(42), exifStr, xmpStr, iptcStr, prompt);
    }
    catch ([[maybe_unused]] const Exiv2::Error& e) {
        JARK_LOG("Caught Exiv2 exception {}\n{}", jarkUtils::wstringToUtf8(path), e.what());
    }
    catch (const std::exception& e) {
        JARK_LOG("EXIF parse failed: {} {}", jarkUtils::wstringToUtf8(path), e.what());
    }

    return detail;
}

