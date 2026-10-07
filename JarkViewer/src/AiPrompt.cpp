#include "AiPrompt.h"

#include "jarkUtils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <unordered_set>

#include <zlib.h>

namespace jark {
namespace {

    constexpr std::array<uint8_t, 8> PNG_SIGNATURE{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

    // AI 生图参数块的体积上限，避免异常文件撑爆内存
    constexpr size_t MAX_TEXT_CHUNK_BYTES = 8u << 20;
    constexpr size_t MAX_INFLATE_BYTES = 16u << 20;

    uint32_t readBe32(std::span<const uint8_t> data, size_t offset) {
        return (static_cast<uint32_t>(data[offset]) << 24) |
            (static_cast<uint32_t>(data[offset + 1]) << 16) |
            (static_cast<uint32_t>(data[offset + 2]) << 8) |
            static_cast<uint32_t>(data[offset + 3]);
    }

    std::string latin1ToUtf8(std::string_view text) {
        return jarkUtils::wstringToUtf8(jarkUtils::latin1ToWstring(text));
    }

    // zlib 解压（zTXt / iTXt 压缩文本），输出上限 MAX_INFLATE_BYTES
    std::optional<std::string> inflateZlib(std::span<const uint8_t> input) {
        if (input.empty())
            return std::nullopt;

        z_stream stream{};
        stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
        stream.avail_in = static_cast<uInt>(input.size());

        if (inflateInit(&stream) != Z_OK)
            return std::nullopt;

        std::string output;
        std::array<char, 16 * 1024> buffer{};
        int status = Z_OK;

        do {
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());

            status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
                inflateEnd(&stream);
                return std::nullopt;
            }

            const size_t produced = buffer.size() - stream.avail_out;
            if (produced > 0)
                output.append(buffer.data(), produced);

            if (output.size() > MAX_INFLATE_BYTES || status == Z_BUF_ERROR) {
                inflateEnd(&stream);
                return std::nullopt;
            }
        } while (status != Z_STREAM_END);

        inflateEnd(&stream);
        return output;
    }

    struct PngTextChunk {
        std::string keyword;
        std::string text;
    };

    // 按 PNG 规范遍历数据块，取出所有文本块（tEXt / zTXt / iTXt）
    std::vector<PngTextChunk> readPngTextChunks(std::span<const uint8_t> data) {
        std::vector<PngTextChunk> chunks;

        if (data.size() < PNG_SIGNATURE.size() ||
            std::memcmp(data.data(), PNG_SIGNATURE.data(), PNG_SIGNATURE.size()) != 0) {
            return chunks;
        }

        size_t position = PNG_SIGNATURE.size();
        while (position + 12 <= data.size()) {
            const uint32_t length = readBe32(data, position);
            const char* type = reinterpret_cast<const char*>(data.data() + position + 4);

            if (length > MAX_TEXT_CHUNK_BYTES || length > data.size() - position - 12)
                break; // 长度异常或越界，停止解析

            const std::span<const uint8_t> payload = data.subspan(position + 8, length);

            if (std::memcmp(type, "IEND", 4) == 0)
                break;

            const bool isText = std::memcmp(type, "tEXt", 4) == 0 ||
                std::memcmp(type, "zTXt", 4) == 0 ||
                std::memcmp(type, "iTXt", 4) == 0;

            if (isText && !payload.empty()) {
                // keyword 以 '\0' 结尾
                const auto terminator = std::find(payload.begin(), payload.end(), uint8_t{ 0 });
                if (terminator != payload.end()) {
                    const size_t keywordLength = static_cast<size_t>(terminator - payload.begin());
                    if (keywordLength > 79) {
                        position += 12 + length;
                        continue; // PNG 规范：keyword 最长 79 字节
                    }

                    PngTextChunk chunk;
                    chunk.keyword = std::string(
                        reinterpret_cast<const char*>(payload.data()), keywordLength);

                    const std::span<const uint8_t> rest = payload.subspan(keywordLength + 1);

                    if (std::memcmp(type, "tEXt", 4) == 0) {
                        chunk.text = latin1ToUtf8(std::string_view(
                            reinterpret_cast<const char*>(rest.data()), rest.size()));
                    }
                    else if (std::memcmp(type, "zTXt", 4) == 0 && rest.size() >= 1) {
                        if (auto inflated = inflateZlib(rest.subspan(1)))
                            chunk.text = latin1ToUtf8(*inflated);
                    }
                    else if (std::memcmp(type, "iTXt", 4) == 0 && rest.size() >= 3) {
                        const bool compressed = rest[0] == 1;
                        std::span<const uint8_t> body = rest.subspan(2); // 跳过压缩标记与方法

                        // 语言标签与翻译后的关键字，各自以 '\0' 结尾
                        for (int field = 0; field < 2; ++field) {
                            const auto end = std::find(body.begin(), body.end(), uint8_t{ 0 });
                            if (end == body.end()) {
                                body = {};
                                break;
                            }
                            body = body.subspan(static_cast<size_t>(end - body.begin()) + 1);
                        }

                        if (!body.empty()) {
                            if (compressed) {
                                if (auto inflated = inflateZlib(body))
                                    chunk.text = *inflated; // iTXt 为 UTF-8
                            }
                            else {
                                chunk.text = std::string(
                                    reinterpret_cast<const char*>(body.data()), body.size());
                            }
                        }
                    }

                    if (!chunk.text.empty())
                        chunks.push_back(std::move(chunk));
                }
            }

            position += 12 + length;
        }

        return chunks;
    }

    std::string_view trimAscii(std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\n' ||
            text.front() == '\r' || text.front() == '\t')) {
            text.remove_prefix(1);
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\n' ||
            text.back() == '\r' || text.back() == '\t')) {
            text.remove_suffix(1);
        }
        return text;
    }

    bool looksLikeJson(std::string_view text) {
        const auto trimmed = trimAscii(text);
        return trimmed.size() >= 2 && trimmed.front() == '{' && trimmed.back() == '}';
    }

} // namespace

bool looksLikeWebUiParameters(std::string_view text) {
    // WebUI 参数块的稳定特征：采样参数行的组合
    if (text.find("Negative prompt:") != std::string_view::npos &&
        text.find("Steps:") != std::string_view::npos) {
        return true;
    }

    return text.find("Steps:") != std::string_view::npos &&
        (text.find("Sampler:") != std::string_view::npos ||
            text.find("CFG scale:") != std::string_view::npos ||
            text.find("Model hash:") != std::string_view::npos);
}

std::string formatWebUiParameters(std::string_view text) {
    std::string prompt(text);

    if (const auto index = prompt.find("parameters"); index != std::string::npos)
        prompt.replace(index, 11, getUIString(43)); // 正向提示词
    else
        prompt.insert(0, getUIString(43));

    if (const auto index = prompt.find("Negative prompt:"); index != std::string::npos)
        prompt.replace(index, 16, getUIString(44)); // 负向提示词

    if (const auto index = prompt.find("\nSteps:"); index != std::string::npos)
        prompt.replace(index, 7, getUIString(45)); // 采样参数

    return prompt;
}

std::string formatComfyWorkflow(std::string_view json) {
    return std::string(getUIString(52)) + jarkUtils::convertUnicodeEscapesToUTF8(std::string(json));
}

bool looksLikeJsonObject(std::string_view text) {
    return looksLikeJson(text);
}

std::string formatAiPromptText(std::string_view rawText) {
    if (looksLikeJson(rawText))
        return formatComfyWorkflow(rawText);
    if (looksLikeWebUiParameters(rawText))
        return formatWebUiParameters(rawText);
    return {};
}

std::vector<AiPromptSection> extractAiPrompts(std::span<const uint8_t> data) {
    std::vector<AiPromptSection> sections;

    std::unordered_set<std::string> seen;
    auto addSection = [&](std::string text) {
        if (text.empty() || seen.contains(text))
            return;
        seen.insert(text);
        sections.push_back(AiPromptSection{ std::move(text) });
    };

    // 关键词与 AI 生成相关（parameters/prompt/workflow 为 WebUI、ComfyUI 的约定）
    static const std::unordered_set<std::string> aiKeywords{
        "parameters", "prompt", "workflow", "Prompt", "Workflow", "Comment", "Description"
    };

    for (auto& chunk : readPngTextChunks(data)) {
        const bool isAiKeyword = aiKeywords.contains(chunk.keyword);
        const bool isPossibleWebUi = looksLikeWebUiParameters(chunk.text);

        if (!isAiKeyword && !isPossibleWebUi)
            continue;

        if (looksLikeJson(chunk.text)) {
            addSection(formatComfyWorkflow(chunk.text));
        }
        else if (isPossibleWebUi) {
            addSection(formatWebUiParameters(chunk.text));
        }
        else if (chunk.keyword == "Comment" || chunk.keyword == "Description") {
            // 其它写入器留下的自由文本，按提示词原样展示
            addSection(std::string(getUIString(46)) + std::string(trimAscii(chunk.text)));
        }
    }

    return sections;
}

} // namespace jark
