#pragma once

// 界面语言：字符串表按语言维度存放（见 stringRes.cpp），设置文件里仍以整数保存，
// 保持对旧设置文件的兼容（0=简体中文、1=English 与旧版一致）。

#include <cstdint>
#include <string_view>

namespace jark {

enum class Language : uint8_t {
    SimplifiedChinese = 0,
    TraditionalChinese = 1,
    English = 2,
    Japanese = 3,
    Korean = 4,
    Count = 5,
};

inline constexpr size_t kLanguageCount = static_cast<size_t>(Language::Count);

// 语言在字符串表中的固定下标（越界一律回落到简体中文）
inline constexpr size_t kSimplifiedChineseIndex = 0;
inline constexpr size_t kTraditionalChineseIndex = 1;
inline constexpr size_t kEnglishIndex = 2;

Language currentLanguage() noexcept;

// 设置文件中的整数 <-> 语言
Language languageFromSetting(int value) noexcept;
int languageToSetting(Language language) noexcept;

// 依据系统界面语言选择默认语言
Language languageFromSystem() noexcept;

// 语言选择项显示名（用各语言自己的写法）
std::string_view languageDisplayName(Language language) noexcept;

// 帮助/关于/提示/首页等资源图目前只有中文与英文两套：
// 简体中文与繁體中文用中文图，其它语言用英文图
bool prefersChineseResources() noexcept;

} // namespace jark
