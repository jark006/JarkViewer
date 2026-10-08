#include "Localization.h"

#include "jarkUtils.h"

namespace jark {

Language currentLanguage() noexcept {
    // 设置文件里的 UI_LANG 是唯一数据源：设置页直接绑定该字段，
    // 这里每次读取即可，无需额外同步状态。
    return languageFromSetting(static_cast<int>(GlobalVar::settingParameter.UI_LANG));
}

Language languageFromSetting(int value) noexcept {
    if (value < 0 || value >= static_cast<int>(Language::Count))
        return Language::SimplifiedChinese;

    return static_cast<Language>(value);
}

int languageToSetting(Language language) noexcept {
    return static_cast<int>(language);
}

Language languageFromSystem() noexcept {
    const LANGID languageId = ::GetUserDefaultUILanguage();

    switch (PRIMARYLANGID(languageId)) {
    case LANG_CHINESE:
        switch (SUBLANGID(languageId)) {
        case SUBLANG_CHINESE_TRADITIONAL:
        case SUBLANG_CHINESE_HONGKONG:
        case SUBLANG_CHINESE_MACAU:
            return Language::TraditionalChinese;
        default:
            return Language::SimplifiedChinese; // 含新加坡等使用简体的地区
        }
    case LANG_JAPANESE:
        return Language::Japanese;
    case LANG_KOREAN:
        return Language::Korean;
    case LANG_RUSSIAN:
        return Language::Russian;
    default:
        return Language::English;
    }
}

std::string_view languageDisplayName(Language language) noexcept {
    switch (language) {
    case Language::SimplifiedChinese:  return "简体中文";
    case Language::TraditionalChinese: return "繁體中文";
    case Language::English:            return "English";
    case Language::Japanese:           return "日本語";
    case Language::Korean:             return "한국어";
    case Language::Russian:            return "Русский";
    default:                           return "简体中文";
    }
}

bool prefersChineseResources() noexcept {
    const Language language = currentLanguage();
    return language == Language::SimplifiedChinese || language == Language::TraditionalChinese;
}

} // namespace jark
