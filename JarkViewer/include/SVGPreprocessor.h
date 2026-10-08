#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <tinyxml2.h>

// 因lunaSVG不支持<switch>标签，需预处理SVG；
// 另外 lunaSVG 也不认识 CSS Color 5 的 light-dark() 与自定义属性 var()，
// 颜色值被判为无效时它会把**整个图元丢掉不画**（draw.io / 现代设计工具导出的 SVG 常见），
// 所以在解码前把这两类函数折叠成字面量。
class SVGPreprocessor {
public:
    std::string preprocessSVG(const char* svgContentPtr, size_t nBytes, const std::string& language = "en") {
        cv::tinyxml2::XMLDocument doc;
        if (doc.Parse(svgContentPtr, nBytes) != cv::tinyxml2::XML_SUCCESS) {
            return {};
        }

        processSwitchElements(doc.RootElement(), language);

        std::map<std::string, std::string> customProperties;
        collectCustomProperties(doc.RootElement(), customProperties);
        resolveCssValues(doc.RootElement(), customProperties);

        cv::tinyxml2::XMLPrinter printer;
        doc.Print(&printer);
        return printer.CStr();
    }

private:
    // ---------------- <switch> / systemLanguage ----------------

    void processSwitchElements(cv::tinyxml2::XMLElement* element, const std::string& language) {
        if (!element) return;

        // 先处理所有子元素中的switch（深度优先）
        std::vector<cv::tinyxml2::XMLElement*> children;
        for (auto child = element->FirstChildElement(); child; child = child->NextSiblingElement()) {
            children.push_back(child);
        }

        for (auto child : children) {
            processSwitchElements(child, language);
        }

        // 处理当前元素如果是 switch
        if (std::string(element->Name()) == "switch") {
            processSwitchElement(element, language);
        }
    }

    void processSwitchElement(cv::tinyxml2::XMLElement* switchElement, const std::string& language) {
        cv::tinyxml2::XMLElement* selectedChild = nullptr;

        // 按顺序检查子元素
        for (auto child = switchElement->FirstChildElement(); child; child = child->NextSiblingElement()) {
            if (shouldSelectElement(child, language)) {
                selectedChild = child;
                break;
            }
        }

        if (selectedChild) {
            auto parent = switchElement->Parent();
            auto doc = switchElement->GetDocument();

            // 手动克隆选中的元素
            auto clonedElement = cloneElement(selectedChild, doc);

            // 替换 switch 元素
            parent->InsertAfterChild(switchElement, clonedElement);
            parent->DeleteChild(switchElement);
        }
        else {
            // 如果没有匹配的元素，删除整个switch
            auto parent = switchElement->Parent();
            parent->DeleteChild(switchElement);
        }
    }

    // 手动实现元素克隆
    cv::tinyxml2::XMLElement* cloneElement(cv::tinyxml2::XMLElement* source, cv::tinyxml2::XMLDocument* doc) {
        auto cloned = doc->NewElement(source->Name());

        // 复制属性
        for (auto attr = source->FirstAttribute(); attr; attr = attr->Next()) {
            cloned->SetAttribute(attr->Name(), attr->Value());
        }

        // 复制文本内容
        if (source->GetText()) {
            cloned->SetText(source->GetText());
        }

        // 递归复制子元素
        for (auto child = source->FirstChildElement(); child; child = child->NextSiblingElement()) {
            auto clonedChild = cloneElement(child, doc);
            cloned->InsertEndChild(clonedChild);
        }

        return cloned;
    }

    bool shouldSelectElement(cv::tinyxml2::XMLElement* element, const std::string& language) {
        // 检查 systemLanguage 属性
        const char* systemLang = element->Attribute("systemLanguage");
        if (systemLang) {
            std::string lang(systemLang);
            // 支持语言列表（空格分隔）
            return lang.find(language) != std::string::npos ||
                lang.find(language.substr(0, 2)) != std::string::npos;
        }

        // foreignObject 及其依赖的 SVG 1.1 Extensibility 特性 lunaSVG 都不支持。
        // draw.io 等导出的画布是 <switch><foreignObject …/><text …/></switch>：
        // 前者放 XHTML 文本，后者是等价的 <text> 兜底。必须在这里把前者判为不可用，
        // 否则会选中画不出来的 foreignObject、同时把兜底 <text> 删掉——整张图的文字全丢。
        if (std::strcmp(element->Name(), "foreignObject") == 0)
            return false;

        // 检查 requiredFeatures 属性
        const char* requiredFeatures = element->Attribute("requiredFeatures");
        if (requiredFeatures) {
            // 认得的只有 Extensibility（不支持）；其余特性暂时按支持处理
            return std::string(requiredFeatures).find("Extensibility") == std::string::npos;
        }

        // 检查 requiredExtensions 属性
        const char* requiredExtensions = element->Attribute("requiredExtensions");
        if (requiredExtensions) {
            // lunaSVG通常不支持扩展，返回false
            return false;
        }

        // 没有条件属性的元素总是被选中
        return true;
    }

    // ---------------- CSS 函数折叠（light-dark / var） ----------------

    static std::string trimCopy(const std::string& text) {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
        return text.substr(begin, end - begin);
    }

    // 不区分大小写地在 value 中查找函数名（含左括号）第一次出现的位置
    static size_t findFunction(const std::string& value, const char* name, size_t from) {
        const size_t nameLength = std::strlen(name);
        if (value.size() < nameLength) return std::string::npos;
        for (size_t pos = from; pos + nameLength <= value.size(); ++pos) {
            size_t i = 0;
            for (; i < nameLength; ++i) {
                if (std::tolower(static_cast<unsigned char>(value[pos + i])) !=
                    std::tolower(static_cast<unsigned char>(name[i]))) {
                    break;
                }
            }
            if (i == nameLength) return pos;
        }
        return std::string::npos;
    }

    static bool needsCssResolution(const std::string& value) {
        return findFunction(value, "var(", 0) != std::string::npos ||
            findFunction(value, "light-dark(", 0) != std::string::npos;
    }

    // 从 '(' 开始匹配到同层的 ')'，按顶层逗号切分实参；
    // 返回结束括号之后的位置（括号不配对时返回 npos）
    static size_t splitArguments(const std::string& text, size_t openParen,
        std::vector<std::string>& args, size_t& closeParen) {
        int depth = 0;
        size_t argStart = openParen + 1;
        for (size_t i = openParen; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '(') {
                ++depth;
            }
            else if (c == ')') {
                if (--depth == 0) {
                    args.push_back(trimCopy(text.substr(argStart, i - argStart)));
                    closeParen = i;
                    return i + 1;
                }
            }
            else if (c == ',' && depth == 1) {
                args.push_back(trimCopy(text.substr(argStart, i - argStart)));
                argStart = i + 1;
            }
        }
        return std::string::npos;
    }

    // 把 var(--x[, fallback]) / light-dark(a, b) 折叠成 lunaSVG 认得的字面量。
    // light-dark 取**亮色分支**：查看器画布深浅主题下都保持可读，且结果不随主题变化
    // （位图光栅化结果会进 LRU 缓存，随主题变化的颜色没有意义）。
    static std::string resolveCssValue(const std::string& input,
        const std::map<std::string, std::string>& properties, int depth) {
        if (depth > 8) return input;

        std::string value = input;
        for (int guard = 0; guard < 64; ++guard) {
            const size_t varPos = findFunction(value, "var(", 0);
            const size_t lightDarkPos = findFunction(value, "light-dark(", 0);
            if (varPos == std::string::npos && lightDarkPos == std::string::npos)
                break;

            const bool isVar = lightDarkPos == std::string::npos ||
                (varPos != std::string::npos && varPos <= lightDarkPos);
            const size_t pos = isVar ? varPos : lightDarkPos;
            const size_t openParen = pos + (isVar ? 3 : 10); // "var" / "light-dark" 的长度

            std::vector<std::string> args;
            size_t closeParen = 0;
            if (splitArguments(value, openParen, args, closeParen) == std::string::npos)
                break; // 括号不配对，放弃

            std::string replacement;
            bool resolved = true;
            if (isVar) {
                if (args.empty() || args[0].rfind("--", 0) != 0) {
                    resolved = false;
                }
                else {
                    const auto found = properties.find(args[0]);
                    if (found != properties.end())
                        replacement = resolveCssValue(found->second, properties, depth + 1);
                    else if (args.size() >= 2)
                        replacement = resolveCssValue(args[1], properties, depth + 1);
                    else
                        resolved = false; // 未定义且没有回退值：保持原样
                }
            }
            else {
                if (args.empty()) {
                    resolved = false;
                }
                else {
                    replacement = resolveCssValue(args[0], properties, depth + 1);
                }
            }

            if (!resolved)
                break; // 不认识的写法，避免原地打转

            value.replace(pos, closeParen - pos + 1, replacement);
        }
        return value;
    }

    // 抓取 `--name: value`：既覆盖 <style> 规则表里的 :root{...}，也覆盖内联 style 属性
    static void extractCustomProperties(const std::string& css,
        std::map<std::string, std::string>& properties) {
        size_t pos = 0;
        while ((pos = css.find("--", pos)) != std::string::npos) {
            const size_t nameStart = pos;
            size_t nameEnd = nameStart + 2;
            while (nameEnd < css.size()) {
                const char c = css[nameEnd];
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ++nameEnd;
                else break;
            }
            if (nameEnd == nameStart + 2) {
                pos = nameEnd;
                continue;
            }

            size_t colon = nameEnd;
            while (colon < css.size() && std::isspace(static_cast<unsigned char>(css[colon]))) ++colon;
            if (colon >= css.size() || css[colon] != ':') {
                pos = nameEnd;
                continue;
            }
            ++colon;

            size_t valueEnd = colon;
            while (valueEnd < css.size() && css[valueEnd] != ';' && css[valueEnd] != '}') ++valueEnd;

            const std::string name = css.substr(nameStart, nameEnd - nameStart);
            const std::string propValue = trimCopy(css.substr(colon, valueEnd - colon));
            if (!propValue.empty())
                properties[name] = propValue;
            pos = valueEnd;
        }
    }

    void collectCustomProperties(cv::tinyxml2::XMLElement* element,
        std::map<std::string, std::string>& properties) {
        if (!element) return;

        if (std::strcmp(element->Name(), "style") == 0) {
            if (const char* text = element->GetText())
                extractCustomProperties(text, properties);
        }
        if (const char* style = element->Attribute("style"))
            extractCustomProperties(style, properties);

        for (auto child = element->FirstChildElement(); child; child = child->NextSiblingElement())
            collectCustomProperties(child, properties);
    }

    void resolveCssValues(cv::tinyxml2::XMLElement* element,
        const std::map<std::string, std::string>& properties) {
        if (!element) return;

        // XMLAttribute 只读，改动要经回 XMLElement::SetAttribute（按名字匹配已有属性）；
        // 先把结果收集起来，避免遍历过程中创建属性导致迭代器失效
        std::vector<std::pair<std::string, std::string>> updates;
        for (auto attr = element->FirstAttribute(); attr; attr = attr->Next()) {
            const std::string original = attr->Value() ? attr->Value() : "";
            if (!needsCssResolution(original))
                continue;
            const std::string resolved = resolveCssValue(original, properties, 0);
            if (resolved != original)
                updates.emplace_back(attr->Name() ? attr->Name() : "", resolved);
        }
        for (const auto& [name, value] : updates)
            element->SetAttribute(name.c_str(), value.c_str());

        if (std::strcmp(element->Name(), "style") == 0) {
            if (const char* text = element->GetText()) {
                const std::string original(text);
                const std::string resolved = resolveCssValue(original, properties, 0);
                if (resolved != original)
                    element->SetText(resolved.c_str());
            }
        }

        for (auto child = element->FirstChildElement(); child; child = child->NextSiblingElement())
            resolveCssValues(child, properties);
    }
};
