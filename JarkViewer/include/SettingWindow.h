#pragma once

// 设置窗口（ImGui 版）：常规 / 文件关联 / 帮助 / 关于 四个标签页。
//
// 与旧实现相比：
//   - 界面由 ImGui 绘制（深浅两套主题、DPI 缩放、控件观感统一由 UiHost 管）；
//   - 帮助页与关于页不再贴资源图，改为文字排版与可点击链接；
//   - 不再是独立窗口线程，由主窗口的 DrawUi() 每帧驱动。

#include "FileAssociationManager.h"
#include "ImageDatabase.h"
#include "Localization.h"
#include "UiHost.h"
#include "ThumbnailService.h"
#include "jarkUtils.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <set>
#include <string>
#include <vector>

extern std::wstring_view appVersion;
extern std::wstring_view jarkLink;
extern std::wstring_view RepositoryLink;
extern std::wstring_view BaiduLink;
extern std::wstring_view LanzouLink;

class SettingWindow {
public:
    static SettingWindow& instance() {
        static SettingWindow window;
        return window;
    }

    void open(int tabIndex = 0) {
        visible_ = true;
        activeTab_ = std::clamp(tabIndex, 0, 3);
        focusRequestedTab_ = activeTab_;
        focusRequested_ = true;
        lastColorManagement_ = GlobalVar::settingParameter.enableColorManagement;
        lastUiMode_ = GlobalVar::settingParameter.UI_Mode;
    }

    void close() {
        visible_ = false;
        finishAssociateTab();
    }

    bool visible() const { return visible_; }

    void draw() {
        if (!visible_)
            return;

        const float scale = jark::ui::UiHost::instance().scale();
        const std::string title = jarkUtils::wstringToUtf8(getUIStringW(39).c_str()) + "###settings";

        ImGui::SetNextWindowSize({ 680.0f * scale, 640.0f * scale }, ImGuiCond_FirstUseEver);
        // 常规页包含导航开关与缓存管理，不能缩小到藏住底部控件
        ImGui::SetNextWindowSizeConstraints({ 640.0f * scale, 580.0f * scale }, { FLT_MAX, FLT_MAX });
        if (focusRequested_) {
            const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, { 0.5f, 0.5f });
            focusRequested_ = false;
        }

        bool open = true;
        if (!ImGui::Begin(title.c_str(), &open,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
            ImGui::End();
            if (!open)
                close();
            return;
        }

        if (ImGui::BeginTabBar("settingsTabs", ImGuiTabBarFlags_None)) {
            const char* tabNames[] = { getUIString(2), getUIString(3), getUIString(4), getUIString(5) };
            for (int tab = 0; tab < 4; ++tab) {
                ImGuiTabItemFlags flags = (focusRequestedTab_ == tab) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(tabNames[tab], nullptr, flags)) {
                    activeTab_ = tab;
                    if (focusRequestedTab_ == tab)
                        focusRequestedTab_ = -1; // 请求已被满足

                    switch (tab) {
                    case 1: drawAssociatePage(); break;
                    case 2: drawHelpPage(); break;
                    case 3: drawAboutPage(); break;
                    default: drawGeneralPage(); break;
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        // ESC 关闭（界面获得键盘焦点时）
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
            close();

        ImGui::End();

        if (!open)
            close();
    }

private:
    SettingWindow() = default;

    // —— 每帧状态检查（主题/语言/ICC 变化后的副作用）——

    void applySideEffects() {
        auto& parameter = GlobalVar::settingParameter;

        if (parameter.enableColorManagement != lastColorManagement_) {
            lastColorManagement_ = parameter.enableColorManagement;
            GlobalVar::isNeedReloadImageCache = true;
        }

        if (parameter.UI_Mode != lastUiMode_) {
            lastUiMode_ = parameter.UI_Mode;
            GlobalVar::isCurrentUIDarkMode = parameter.UI_Mode == 0 ?
                GlobalVar::isSystemDarkMode : (parameter.UI_Mode == 2);
            GlobalVar::currentTheme = GlobalVar::isCurrentUIDarkMode ? deepTheme : lightTheme;
            GlobalVar::isNeedUpdateTheme = true;
        }
    }

    // —— 常规 ——

    void drawGeneralPage() {
        auto& parameter = GlobalVar::settingParameter;
        const float scale = jark::ui::UiHost::instance().scale();

        struct CheckItem {
            uint32_t stringID;
            bool* value;
        };
        const CheckItem checkItems[] = {
            { 12, &parameter.isAllowRotateAnimation },
            { 13, &parameter.isAllowZoomAnimation },
            { 14, &parameter.isNoteBeforeDelete },
            { 15, &parameter.enableColorManagement },
            { 54, &parameter.isOneToOnePreferred },
        };
        for (const auto& item : checkItems)
            ImGui::Checkbox(getUIString(item.stringID), item.value);
        bool showNavigator = !parameter.hideNavigator;
        if (ImGui::Checkbox(getUIString(kStrShowNavigator), &showNavigator))
            parameter.hideNavigator = !showNavigator;

        ImGui::Spacing();
        drawRadioRow(20, { getUIString(21), getUIString(22), getUIString(23) }, &parameter.switchImageAnimationMode, 0);

        ImGui::Spacing();
        drawRadioRow(17, { getUIString(121), getUIString(122), getUIString(123) }, &parameter.pptOrder, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f * scale);
        int timeout = static_cast<int>(parameter.pptTimeout);
        if (ImGui::SliderInt(std::string("##ppt").c_str(), &timeout, 1, 60, "%d s")) {
            parameter.pptTimeout = static_cast<uint32_t>(std::clamp(timeout, 1, 300));
        }

        ImGui::Spacing();
        drawRadioRow(24, { getUIString(25), getUIString(26), getUIString(27) }, &parameter.UI_Mode, 0);

        ImGui::Spacing();
        drawRadioRow(28,
            { std::string(jark::languageDisplayName(jark::Language::SimplifiedChinese)),
              std::string(jark::languageDisplayName(jark::Language::TraditionalChinese)),
              std::string(jark::languageDisplayName(jark::Language::English)),
              std::string(jark::languageDisplayName(jark::Language::Japanese)),
              std::string(jark::languageDisplayName(jark::Language::Korean)),
              std::string(jark::languageDisplayName(jark::Language::Russian)) },
            &parameter.UI_LANG, 0);

        ImGui::Spacing();
        drawRadioRow(36, { getUIString(37), getUIString(38) }, &parameter.rightClickAction, 0);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        auto& thumbnails = jark::ThumbnailService::instance();
        const auto stats = thumbnails.stats();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s: %zu / 1000, %.1f MiB", getUIString(kStrThumbnailCache),
            stats.entries, stats.bytes / (1024.0 * 1024.0));
        ImGui::SameLine();
        ImGui::BeginDisabled(stats.clearState == jark::ClearState::Pending);
        if (ImGui::Button(getUIString(kStrClearCache)))
            thumbnails.clear();
        ImGui::EndDisabled();
        if (stats.clearState == jark::ClearState::Pending)
            ImGui::TextDisabled("%s", getUIString(kStrClearingCache));
        else if (stats.clearState == jark::ClearState::Done)
            ImGui::TextDisabled("%s", getUIString(kStrCacheCleared));
        else if (stats.clearState == jark::ClearState::Failed)
            ImGui::TextWrapped("%s", getUIString(kStrClearCacheFailed));
        else if (!stats.diskAvailable)
            ImGui::TextWrapped("%s", getUIString(kStrMemoryCacheOnly));

        applySideEffects();
    }

    // 一行「标签 + 单选项」
    void drawRadioRow(uint32_t labelId, const std::vector<std::string>& options, uint32_t* value, float indent) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(getUIString(labelId));
        ImGui::SameLine();

        if (indent > 0.0f)
            ImGui::Indent(indent);

        for (size_t index = 0; index < options.size(); ++index) {
            if (index > 0)
                ImGui::SameLine();

            const std::string id = std::string(options[index]) + "##" + std::to_string(labelId) + "_" + std::to_string(index);
            if (ImGui::RadioButton(id.c_str(), *value == index))
                *value = static_cast<uint32_t>(index);
        }
    }

    // —— 文件关联 ——

    void drawAssociatePage() {
        ensureExtensionList();

        const float scale = jark::ui::UiHost::instance().scale();

        // 底部（说明文字按实际折行高度 + 按钮行）精确留白，否则整页会多出一条窗口滚动条
        const float textHeight = ImGui::CalcTextSize(getUIString(11), nullptr, false,
            ImGui::GetContentRegionAvail().x).y;
        const float footerHeight = textHeight + ImGui::GetStyle().ItemSpacing.y +
            ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;

        if (ImGui::BeginChild("extList", { 0, -footerHeight }, ImGuiChildFlags_Borders)) {
            const int columns = 8;
            if (ImGui::BeginTable("extTable", columns, ImGuiTableFlags_SizingStretchSame)) {
                for (size_t index = 0; index < allSupportExt_.size(); ++index) {
                    ImGui::TableNextColumn();
                    bool checked = checkedExt_.contains(allSupportExt_[index]);
                    if (ImGui::Checkbox(allSupportExt_[index].c_str(), &checked)) {
                        if (checked)
                            checkedExt_.insert(allSupportExt_[index]);
                        else
                            checkedExt_.erase(allSupportExt_[index]);
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        ImGui::TextWrapped("%s", getUIString(11));

        const float buttonWidth = 140.0f * scale;
        if (ImGui::Button(getUIString(7), { buttonWidth, 0 }))
            restoreDefaultExtensions();
        ImGui::SameLine();
        if (ImGui::Button(getUIString(8), { buttonWidth, 0 })) {
            checkedExt_.insert(allSupportExt_.begin(), allSupportExt_.end());
        }
        ImGui::SameLine();
        if (ImGui::Button(getUIString(9), { buttonWidth, 0 }))
            checkedExt_.clear();
        ImGui::SameLine();
        if (ImGui::Button(getUIString(10), { buttonWidth, 0 }))
            applyFileAssociations();
    }

    void ensureExtensionList() {
        if (!allSupportExt_.empty())
            return;

        std::set<std::wstring> allSupportExtW;
        allSupportExtW.insert(ImageDatabase::supportExt.begin(), ImageDatabase::supportExt.end());
        allSupportExtW.insert(ImageDatabase::supportRaw.begin(), ImageDatabase::supportRaw.end());
        for (const auto& ext : allSupportExtW)
            allSupportExt_.emplace_back(jarkUtils::wstringToUtf8(ext));

        checkedExt_.clear();
        for (const auto& ext : jarkUtils::splitString(GlobalVar::settingParameter.extCheckedListStr, ",")) {
            if (!ext.empty())
                checkedExt_.insert(ext);
        }
    }

    void restoreDefaultExtensions() {
        memcpy(GlobalVar::settingParameter.extCheckedListStr,
            SettingParameter::defaultExtList.data(),
            SettingParameter::defaultExtList.length() + 1);

        checkedExt_.clear();
        for (const auto& ext : jarkUtils::splitString(GlobalVar::settingParameter.extCheckedListStr, ",")) {
            if (!ext.empty())
                checkedExt_.insert(ext);
        }
    }

    void applyFileAssociations() {
        std::vector<std::wstring> checkedExtW;
        std::vector<std::wstring> unCheckedExtW;
        checkedExtW.reserve(checkedExt_.size());

        for (const auto& ext : checkedExt_)
            checkedExtW.emplace_back(jarkUtils::utf8ToWstring(ext));

        for (const auto& ext : allSupportExt_) {
            if (!checkedExt_.contains(ext))
                unCheckedExtW.emplace_back(jarkUtils::utf8ToWstring(ext));
        }

        FileAssociationManager manager;
        const auto result = manager.ManageFileAssociations(checkedExtW, unCheckedExtW);

        if (!result.associationSucceeded)
            MessageBoxW(nullptr, getUIStringW(3), getUIStringW(1), MB_OK | MB_ICONERROR);
        else if (result.thumbnailOperationFailed)
            MessageBoxW(nullptr, getUIStringW(41), getUIStringW(1), MB_OK | MB_ICONWARNING);
        else
            MessageBoxW(nullptr, getUIStringW(2), getUIStringW(1), MB_OK | MB_ICONINFORMATION);
    }

    void finishAssociateTab() {
        if (allSupportExt_.empty())
            return;

        std::string checkedList;
        for (const auto& ext : checkedExt_) {
            checkedList += ext;
            checkedList += ',';
        }
        if (!checkedList.empty() && checkedList.back() == ',')
            checkedList.pop_back();

        if (checkedList.empty())
            memset(GlobalVar::settingParameter.extCheckedListStr, 0, 4);
        else
            memcpy(GlobalVar::settingParameter.extCheckedListStr, checkedList.data(), checkedList.length() + 1);
    }

    // —— 帮助 ——

    void drawHelpPage() {
        ImGui::TextUnformatted(getUIString(kStrHelpTitle));
        ImGui::Separator();
        ImGui::Spacing();

        // 文案是若干行「按键：说明」，同一行内用两个全角空格分组。
        // 整段 TextUnformatted 出来是一大坨，这里切成表格按列对齐，像一张速查表。
        // 列数固定 2：列再多，像"窗口左右边缘：上一张 / 下一张"这种长条目就会被单元格裁掉，
        // 多出来的分组换到下一行即可。
        const std::string body = getUIString(kStrHelpBody);
        const std::vector<std::string_view> lines = splitViews(body, "\n");

        if (ImGui::BeginTable("helpShortcuts", kHelpColumns,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
            for (const auto& line : lines) {
                if (line.empty())
                    continue;

                for (const auto& cell : splitViews(line, kHelpGroupSeparator)) {
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(cell.data(), cell.data() + cell.size());
                }
                ImGui::TableNextRow();
            }
            ImGui::EndTable();
        }
    }

    // 按分隔符切片（返回视图，不复制字符串）
    static std::vector<std::string_view> splitViews(std::string_view text, std::string_view separator) {
        std::vector<std::string_view> parts;
        size_t start = 0;
        while (true) {
            const size_t position = text.find(separator, start);
            if (position == std::string_view::npos) {
                parts.push_back(text.substr(start));
                break;
            }
            parts.push_back(text.substr(start, position - start));
            start = position + separator.size();
        }
        return parts;
    }

    // —— 关于 ——

    void drawAboutPage() {
        const float scale = jark::ui::UiHost::instance().scale();
        ImGui::Spacing();

        // 软件图标：从老版本关于页的贴图里抠出来的（透明底，深浅主题通用）
        if (const ImTextureID icon = aboutIconTexture()) {
            const float iconSize = 96.0f * scale;
            ImGui::Image(icon, { iconSize, iconSize });

            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Dummy({ 0.0f, 8.0f * scale });

            // 标题字号放大：PushFont 要传“未乘全局缩放”的字号，放大会被算两次
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
            ImGui::TextUnformatted("JarkViewer");
            const float afterTitleY = ImGui::GetCursorPosY();   // 局部坐标（已在下一行）
            ImGui::PopFont();

            // 版本号贴着大标题的底边（同行时 ImGui 按行顶对齐，字号不同会显得悬空）
            ImGui::SameLine(0.0f, 10.0f * scale);
            ImGui::SetCursorPosY(afterTitleY - ImGui::GetStyle().ItemSpacing.y - ImGui::GetFontSize());
            ImGui::TextDisabled("%s", jarkUtils::wstringToUtf8(appVersion).c_str());
            ImGui::TextDisabled("%s", getUIString(19));
            ImGui::TextDisabled("%s", std::string(jarkUtils::COMPILE_DATE_TIME).c_str());
            ImGui::EndGroup();
        }
        else {
            ImGui::TextUnformatted("JarkViewer");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", jarkUtils::wstringToUtf8(appVersion).c_str());
            ImGui::TextDisabled("%s", getUIString(19));
            ImGui::TextDisabled("%s", std::string(jarkUtils::COMPILE_DATE_TIME).c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float buttonWidth = 200.0f * scale;
        if (ImGui::Button("Jark006", { buttonWidth, 0 }))
            jarkUtils::openUrl(jarkLink.data());
        ImGui::SameLine();
        if (ImGui::Button("GitHub", { buttonWidth, 0 }))
            jarkUtils::openUrl(RepositoryLink.data());

        if (ImGui::Button("百度网盘", { buttonWidth, 0 }))
            jarkUtils::openUrl(BaiduLink.data());
        ImGui::SameLine();
        if (ImGui::Button("蓝奏云", { buttonWidth, 0 }))
            jarkUtils::openUrl(LanzouLink.data());
    }

    // 关于页的图标纹理：第一次用到时才解码上传
    ImTextureID aboutIconTexture() {
        if (!aboutIconTried_) {
            aboutIconTried_ = true;
            const auto resource = jarkUtils::GetResource(IDB_PNG_ABOUT_ICON, L"PNG");
            if (resource.size && resource.ptr) {
                const cv::Mat pngData(1, static_cast<int>(resource.size), CV_8UC1,
                    static_cast<uint8_t*>(resource.ptr));
                const cv::Mat image = cv::imdecode(pngData, cv::IMREAD_UNCHANGED);
                if (!image.empty())
                    aboutIcon_ = jark::ui::UiHost::instance().textureFromImage(image, 4); // 槽 4：关于页图标
            }
        }
        return aboutIcon_;
    }

    static constexpr uint32_t kStrShowNavigator = 149;
    static constexpr uint32_t kStrThumbnailCache = 150;
    static constexpr uint32_t kStrClearCache = 151;
    static constexpr uint32_t kStrClearingCache = 152;
    static constexpr uint32_t kStrCacheCleared = 153;
    static constexpr uint32_t kStrClearCacheFailed = 154;
    static constexpr uint32_t kStrMemoryCacheOnly = 155;

    // 帮助页文案（窄表新增条目）
    static constexpr uint32_t kStrHelpTitle = 127;
    static constexpr uint32_t kStrHelpBody = 128;

    // 帮助文案里同一行内的分组分隔符（两个全角空格），以及速查表的列数
    static constexpr std::string_view kHelpGroupSeparator = "　　";
    static constexpr int kHelpColumns = 2;

    bool visible_ = false;
    bool focusRequested_ = false;
    int activeTab_ = 0;
    int focusRequestedTab_ = -1;

    bool lastColorManagement_ = false;
    uint32_t lastUiMode_ = 0;

    std::vector<std::string> allSupportExt_;
    std::set<std::string> checkedExt_;

    ImTextureID aboutIcon_ = 0;
    bool aboutIconTried_ = false;
};
