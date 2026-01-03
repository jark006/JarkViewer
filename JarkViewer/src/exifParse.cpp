#include "jarkUtils.h"
#include "exifParse.h"


std::string ExifParse::getSimpleInfo(wstring_view path, int width, int height, const uint8_t* buf, size_t fileSize) {
    return (path.ends_with(L".ico") || width == 0 || height == 0) ?
        std::format("{}: {}\n{}: {}\n",
            getUIString(39), jarkUtils::wstringToUtf8(path), getUIString(40), jarkUtils::size2Str(fileSize)) :
        std::format("{}: {}\n{}: {}\n{}: {}x{}",
            getUIString(39), jarkUtils::wstringToUtf8(path), getUIString(40), jarkUtils::size2Str(fileSize), getUIString(41), width, height);
}

std::string ExifParse::handleMathDiv(string_view str) {
    if (str.empty())return "";

    int divIdx = -1;
    bool isNegative = (str[0] == '-');
    for (int i = isNegative ? 1 : 0; i < str.length(); i++) {
        int c = str[i];
        if ('0' <= c && c <= '9') {
            continue;
        }
        else if (c == '/') {
            if (divIdx == -1) {
                divIdx = i;
                continue;
            }
            else {
                divIdx = -1;
                break;
            }
        }
        else {
            divIdx = -1;
            break;
        }
    }

    if (divIdx > 0) {
        auto a = std::stoll(string(str.substr(0, divIdx)));
        auto b = std::stoll(string(str.substr((size_t)divIdx + 1)));

        if (isNegative)
            a = 0 - a;

        auto resStr = std::format("{:.2f}", (double)a / b);

        if (resStr.ends_with(".00"))
            resStr = resStr.substr(0, resStr.size() - 3);

        return resStr;
    }
    return "";
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

        if (GlobalVar::settingParameter.UI_LANG == 0) {
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
        else if (tagName == "Exif.Photo.UserComment") { // 可能包含AI生图prompt信息
            bool isPrompt = false;
            auto a = tag.value().clone();
            vector<uint8_t> buf(a->size());
            a->copy(buf.data(), Exiv2::ByteOrder::bigEndian);
            if (!memcmp(buf.data(), "UNICODE\0", 8)) {
                wstring_view str((wchar_t*)(buf.data() + 8), (buf.size() - 8) / 2);
                tagValue = jarkUtils::wstringToUtf8(str);
                auto idx = tagValue.find("\nNegative prompt:");
                if (idx != string::npos) {
                    isPrompt = true;
                    tagValue.replace(idx, 17, getUIString(44));
                    tagValue = std::format("{}{}{}", getUIString(46), getUIString(43), tagValue);
                }

                idx = tagValue.find("\nSteps:");
                if (idx != string::npos) {
                    isPrompt = true;
                    tagValue.replace(idx, 7, getUIString(45));
                }
                
                if (tagValue.front() == '{' && tagValue.back() == '}') { // ComfyUI JSON format
                    isPrompt = true;
                    tagValue = getUIString(52) + jarkUtils::convertUnicodeEscapesToUTF8(tagValue);
                }
            }

            if(!isPrompt){
                a->copy(buf.data(), Exiv2::ByteOrder::littleEndian);
                if (!memcmp(buf.data(), "UNICODE\0", 8)) {
                    wstring_view str((wchar_t*)(buf.data() + 8), (buf.size() - 8) / 2);
                    tagValue = jarkUtils::wstringToUtf8(str);
                }
                else {
                    tagValue = string(buf.begin(), buf.end());
                }
            }
        }
        else if (exifTagsUnicodeStr.contains(tagName)) {
            auto a = tag.value().clone();
            vector<WCHAR> buf(a->size()/2+1, 0);
            a->copy((uint8_t*)buf.data(), Exiv2::ByteOrder::littleEndian);
            wstring_view str(buf.data(), buf.size());
            tagValue = jarkUtils::wstringToUtf8(str);            
        }
        else if (2 < tagValue.length() && tagValue.length() < 100) {
            auto res = handleMathDiv(tagValue);
            if (!res.empty())
                tagValue = std::format("{} ({})", res, tagValue);
        }

        string tmp;
        if (tagName == "Exif.Photo.UserComment")
            tmp = "\n" + translatedTagName + ": " + tagValue;
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

std::string ExifParse::parseAiPrompt(wstring_view path, const uint8_t* buf, size_t fileSize) {
    if (fileSize < 1024) // AI生图信息一般不会出现在特别小的图片中
        return "";

    if (!strncmp((const char*)buf + 0x25, "tEXtparameters", 14)) {
        int length = (buf[0x21] << 24) + (buf[0x22] << 16) + (buf[0x23] << 8) + buf[0x24]; // 大端 int
        if ((length + 64ULL) > fileSize) {
            return "";
        }

        string prompt((const char*)buf + 0x29, length); // format: Windows-1252  ISO/IEC 8859-1（Latin-1）

        prompt = jarkUtils::wstringToUtf8(jarkUtils::latin1ToWstring(prompt));

        auto idx = prompt.find("parameters");
        if (idx != string::npos) {
            prompt.replace(idx, 11, getUIString(43));
        }
        else {
            prompt = getUIString(43) + prompt;
        }

        idx = prompt.find("Negative prompt");
        if (idx != string::npos) {
            prompt.replace(idx, 16, getUIString(44));
        }

        idx = prompt.find("\nSteps:");
        if (idx != string::npos) {
            prompt.replace(idx, 7, getUIString(45));
        }

        if (prompt.front() == '{' && prompt.back() == '}') { // ComfyUI JSON format
            prompt = getUIString(52) + jarkUtils::convertUnicodeEscapesToUTF8(prompt);
        }
        else {
            prompt = getUIString(46) + prompt;
        }

        // 有些图片会同时包含 parameters 和 prompt 信息
        if (!strncmp((const char*)buf + 0x31 + length, "tEXtprompt", 10)) {
            int length2 = (buf[0x2d + length] << 24) + (buf[0x2e + length] << 16) + (buf[0x2f + length] << 8) + buf[0x30 + length];
            if ((length + 128ULL + length2) < fileSize) {
                string prompt2((const char*)buf + 0x3c + length, length2-7); // format: Windows-1252  ISO/IEC 8859-1（Latin-1）
                prompt2 = jarkUtils::wstringToUtf8(jarkUtils::latin1ToWstring(prompt2));
                if (prompt2.front() == '{' && prompt2.back() == '}') { // ComfyUI JSON format
                    prompt += getUIString(52) + jarkUtils::convertUnicodeEscapesToUTF8(prompt2);
                }
                else {
                    prompt += getUIString(46) + prompt2;
                }
            }
        }

        return prompt;
    }
    else if (!strncmp((const char*)buf + 0x25, "iTXtparameters", 14)) {
        int length = (buf[0x21] << 24) + (buf[0x22] << 16) + (buf[0x23] << 8) + buf[0x24];
        if ((length + 64ULL) > fileSize) {
            return "";
        }

        string prompt((const char*)buf + 0x38, length - 15); // format: Windows-1252  ISO/IEC 8859-1（Latin-1）

        prompt = jarkUtils::wstringToUtf8(jarkUtils::latin1ToWstring(prompt));

        auto idx = prompt.find("parameters");
        if (idx != string::npos) {
            prompt.replace(idx, 11, getUIString(43));
        }
        else {
            prompt = getUIString(43) + prompt;
        }

        idx = prompt.find("\nNegative prompt");
        if (idx != string::npos) {
            prompt.replace(idx, 16, getUIString(44));
        }

        idx = prompt.find("\nSteps:");
        if (idx != string::npos) {
            prompt.replace(idx, 7, getUIString(45));
        }

        if (prompt.front() == '{' && prompt.back() == '}') { // ComfyUI JSON format
            return getUIString(52) + jarkUtils::convertUnicodeEscapesToUTF8(prompt);
        }
        return getUIString(46) + prompt;
    }
    else if (!strncmp((const char*)buf + 0x25, "tEXtprompt", 10)) {

        int length = (buf[0x21] << 24) + (buf[0x22] << 16) + (buf[0x23] << 8) + buf[0x24];
        if ((length + 64ULL) > fileSize) {
            return "";
        }

        string prompt((const char*)buf + 0x29 + 7, length - 7); // format: Windows-1252  ISO/IEC 8859-1（Latin-1）

        prompt = jarkUtils::wstringToUtf8(jarkUtils::latin1ToWstring(prompt));

        if (prompt.front() == '{' && prompt.back() == '}') { // ComfyUI JSON format
            return getUIString(52) + jarkUtils::convertUnicodeEscapesToUTF8(prompt);
        }
        return getUIString(46) + prompt;
    }

    return "";
}

std::string ExifParse::getExif(wstring_view path, const uint8_t* buf, size_t fileSize) {
    static std::mutex mtx;

    std::lock_guard<std::mutex> lock(mtx);

    try {
        auto image = Exiv2::ImageFactory::open(buf, fileSize);
        image->readMetadata();

        auto exifStr = exifDataToString(path, image->exifData());
        auto xmpStr = xmpDataToString(path, image->xmpData());
        auto iptcStr = iptcDataToString(path, image->iptcData());
        auto prompt = parseAiPrompt(path, buf, fileSize);

        if ((exifStr.length() + xmpStr.length() + iptcStr.length() + prompt.length()) > 0)
            return  std::format("\n\n{}\n{}{}{}{}", getUIString(42), exifStr, xmpStr, iptcStr, prompt);
        else
            return "";
    }
    catch (Exiv2::Error& e) {
        JARK_LOG("Caught Exiv2 exception {}\n{}", jarkUtils::wstringToUtf8(path), e.what());
        return "";
    }
    return "";
}

