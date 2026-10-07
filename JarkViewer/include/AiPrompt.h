#pragma once

// AI 生图提示词提取（Stable Diffusion WebUI / ComfyUI / NovelAI 等）。
//
// 旧实现直接按固定偏移读取 PNG 文本块（buf + 0x25、buf + 0x31…），只在“参数块恰好
// 位于那个位置”的 PNG 上有效，换个写入器或多几个块就静默失效。这里改为按 PNG 规范
// 遍历数据块，并支持 tEXt/iTXt/zTXt（含 zlib 压缩）与 EXIF/XMP 里的同款文本。

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jark {

struct AiPromptSection {
    std::string text; // 可直接显示的文本（自带本地化小标题）
};

// 从整份文件内容中提取 AI 生图提示词；无相关内容返回空
std::vector<AiPromptSection> extractAiPrompts(std::span<const uint8_t> data);

// 一段文本是否像 Stable Diffusion WebUI 的参数块（正/负向提示词 + 采样参数）
bool looksLikeWebUiParameters(std::string_view text);

// 把 WebUI 参数块整理成显示文本（拆出正向/负向提示词与采样参数）；不像则返回空
std::string formatWebUiParameters(std::string_view text);

// 把 ComfyUI/RandomX 工作流 JSON 整理成显示文本；不是 JSON 则返回空
std::string formatComfyWorkflow(std::string_view json);

// 是否是一段 JSON 对象（ComfyUI 工作流的特征）
bool looksLikeJsonObject(std::string_view text);

// 统一入口：把一段原始文本整理成可显示的提示词文本；既不是 WebUI 参数也不是工作流则返回空
std::string formatAiPromptText(std::string_view rawText);

} // namespace jark
