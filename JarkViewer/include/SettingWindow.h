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
        lastSortMode_ = GlobalVar::settingParameter.sortMode;
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

        // 尺寸先收敛到主视口内：装不下的窗口会被多视口模式分离成独立 OS 窗口（角外露黑底、跑到主窗口外）
        ImGui::SetNextWindowSize(
            jark::ui::fitWindowSizeToMainViewport({ 680.0f * scale, 640.0f * scale }), ImGuiCond_FirstUseEver);
        // 常规页包含导航开关与缓存管理，不能缩小到藏住底部控件；上限同样钳到主视口，防拖拽放大后被分离
        ImGui::SetNextWindowSizeConstraints(
            jark::ui::fitWindowSizeToMainViewport({ 640.0f * scale, 580.0f * scale }),
            jark::ui::fitWindowSizeToMainViewport({ FLT_MAX, FLT_MAX }));
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

        if (parameter.sortMode != lastSortMode_) {
            lastSortMode_ = parameter.sortMode;
            GlobalVar::isNeedSortFileList = true; // 主窗口下一帧重排文件列表（当前图片跟着走）
        }

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

        // 勾选项排成 2 列（偶数下标在左列）。ImGui 没有等宽列：第二列起点取第一列
        // （偶数下标项）中最长文本的宽度，再补上勾选框本身（框宽 + 框与文本间距）和一个列间距。
        bool showNavigator = !parameter.hideNavigator;
        bool smoothZoom = !parameter.disableZoomSmoothing;
        bool videoHardwareDecode = !parameter.disableVideoHardwareDecode;
        bool reverseWasdPan = parameter.reverseWasdPan != 0;
        struct CheckItem {
            const char* label;
            bool* value;
        };
        const CheckItem checkItems[] = {
            { getUIString(12), &parameter.isAllowRotateAnimation },
            { getUIString(13), &parameter.isAllowZoomAnimation },
            { getUIString(14), &parameter.isNoteBeforeDelete },
            { getUIString(15), &parameter.enableColorManagement },
            { getUIString(54), &parameter.isOneToOnePreferred },
            { getUIString(kStrShowNavigator), &showNavigator }, // 界面上是"显示"，存储取反
            { getUIString(171), &parameter.livePhotoAutoPlaySound }, // 实况自动播放声音（默认静音）
            { getUIString(kStrStopAtLast), &parameter.stopAtListEnd },          // 到最后一张停住
            { getUIString(kStrBlackFullscreen), &parameter.blackFullscreenBackground }, // 全屏纯黑背景
            { getUIString(kStrSmoothZoom), &smoothZoom }, // 界面上是"平滑"，存储取反
            { getUIString(kStrVideoHardwareDecode), &videoHardwareDecode }, // 视频硬解（默认开，存储取反）
            { getUIString(kStrReverseWasdPan), &reverseWasdPan },
        };
        float secondColumn = 0.0f;
        for (int index = 0; index < IM_ARRAYSIZE(checkItems); index += 2)
            secondColumn = (std::max)(secondColumn, ImGui::CalcTextSize(checkItems[index].label).x);
        secondColumn += ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
            2.0f * ImGui::GetStyle().ItemSpacing.x;

        for (int index = 0; index < IM_ARRAYSIZE(checkItems); ++index) {
            if (index % 2)
                ImGui::SameLine(secondColumn);
            ImGui::Checkbox(checkItems[index].label, checkItems[index].value);
        }
        parameter.hideNavigator = !showNavigator; // 循环后同步回存储（✕ 收起鸟瞰也是写这个字段）
        parameter.disableZoomSmoothing = !smoothZoom;
        parameter.disableVideoHardwareDecode = !videoHardwareDecode;
        parameter.reverseWasdPan = reverseWasdPan ? 1u : 0u;

        ImGui::Spacing();
        drawRadioRow(20, { getUIString(21), getUIString(22), getUIString(23) }, &parameter.switchImageAnimationMode, 0);

        ImGui::Spacing();
        drawRadioRow(kStrSortMode, { getUIString(kStrSortName), getUIString(kStrSortTime),
            getUIString(kStrSortSize) }, &parameter.sortMode, 0);

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
        const float scale = jark::ui::UiHost::instance().scale();
        ImGui::Spacing();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
        ImGui::TextUnformatted(getUIString(kStrHelpTitle));
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", getUIString(kStrHelpIntro));
        ImGui::PopStyleColor();
        ImGui::Spacing();

        const uint32_t categories[] = { kStrHelpBrowse, kStrHelpTools, kStrHelpPlayback };
        const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
        bool categoryChanged = false;
        for (int index = 0; index < IM_ARRAYSIZE(categories); ++index) {
            if (index)
                ImGui::SameLine();
            const bool selected = helpCategory_ == index;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            if (ImGui::Button(getUIString(categories[index]), { width, 36.0f * scale })) {
                categoryChanged = helpCategory_ != index;
                helpCategory_ = index;
            }
            if (selected)
                ImGui::PopStyleColor();
        }
        ImGui::Spacing();

        // 分类导航固定在上方，正文独立滚动；换分类回到页首，长翻译只纵向换行。
        if (ImGui::BeginChild("helpContent", { 0, 0 }, ImGuiChildFlags_None,
            ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            if (categoryChanged)
                ImGui::SetScrollY(0);
            if (helpCategory_ == 0) {
                drawHelpSection("helpMouse", kStrHelpMouse, kStrHelpMouseRows);
                drawHelpSection("helpKeyboard", kStrHelpKeyboard, kStrHelpBrowseRows);
                ImGui::TextWrapped("%s", getUIString(kStrHelpBrowseTip));
            }
            else if (helpCategory_ == 1) {
                drawHelpSection("helpTools", kStrHelpTools, kStrHelpToolRows);
                ImGui::TextWrapped("%s", getUIString(kStrHelpToolsTip));
            }
            else {
                ImGui::TextWrapped("%s", getUIString(kStrHelpPlaybackTip));
                ImGui::Spacing();
                drawHelpSection("helpAnimation", kStrHelpAnimation, kStrHelpAnimationRows);
                drawHelpSection("helpPlayer", kStrHelpPlayer, kStrHelpPlayerRows);
            }
        }
        ImGui::EndChild();
    }

    static void drawHelpKeys(std::string_view keys) {
        const float scale = jark::ui::UiHost::instance().scale();
        const float available = ImGui::GetContentRegionAvail().x;
        float usedWidth = 0.0f;
        bool first = true;
        for (const auto key : splitViews(keys, "|")) {
            const float textWidth = (std::max)(1.0f, available - 16.0f * scale);
            const auto textSize = ImGui::CalcTextSize(key.data(), key.data() + key.size(), false, textWidth);
            const ImVec2 size{ (std::min)(available, textSize.x + 16.0f * scale), textSize.y + 8.0f * scale };
            if (!first && usedWidth + 6.0f * scale + size.x <= available) {
                ImGui::SameLine(0, 6.0f * scale);
                usedWidth += 6.0f * scale;
            }
            else {
                usedWidth = 0.0f;
            }
            const auto pos = ImGui::GetCursorScreenPos();
            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 end{ pos.x + size.x, pos.y + size.y };
            drawList->AddRectFilled(pos, end, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f * scale);
            drawList->AddRect(pos, end, ImGui::GetColorU32(ImGuiCol_Separator), 4.0f * scale);
            drawList->AddText(nullptr, ImGui::GetFontSize(), { pos.x + 8.0f * scale, pos.y + 4.0f * scale },
                ImGui::GetColorU32(ImGuiCol_Text), key.data(), key.data() + key.size(), textWidth);
            ImGui::Dummy(size);
            usedWidth += size.x;
            first = false;
        }
    }

    static void drawHelpSection(const char* id, uint32_t title, uint32_t body) {
        const float scale = jark::ui::UiHost::instance().scale();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 14.0f * scale, 12.0f * scale });
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, { 6.0f * scale, 7.0f * scale });
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_Separator));
        if (ImGui::BeginChild(id, { 0, 0 }, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
            ImGuiChildFlags_AlwaysAutoResize, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::SeparatorText(getUIString(title));
            if (ImGui::BeginTable("shortcuts", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch, 0.46f);
                ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch, 0.54f);
                for (const auto line : splitViews(getUIString(body), "\n")) {
                    const size_t separator = line.find('\t');
                    if (separator == std::string_view::npos)
                        continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextWrapped("%.*s", static_cast<int>(separator), line.data());
                    ImGui::TableNextColumn();
                    drawHelpKeys(line.substr(separator + 1));
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::Spacing();
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
            ImGui::TextDisabled("%s %s UTC+8", getUIString(19), std::string(jarkUtils::COMPILE_DATE_TIME).c_str());
            ImGui::EndGroup();
        }
        else {
            ImGui::TextUnformatted("JarkViewer");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", jarkUtils::wstringToUtf8(appVersion).c_str());
            ImGui::TextDisabled("%s %s UTC+8", getUIString(19), std::string(jarkUtils::COMPILE_DATE_TIME).c_str());
        }

        // 软件简介（正文色，超宽自动换行）
        ImGui::TextWrapped("%s", getUIString(kStrAboutIntro));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float buttonWidth = 200.0f * scale;

        // 相关链接（个人主页与项目仓库）
        ImGui::TextDisabled("%s", getUIString(kStrAboutLinks));
        if (ImGui::Button("Jark006", { buttonWidth, 0 }))
            jarkUtils::openUrl(jarkLink.data());
        ImGui::SameLine();
        if (ImGui::Button("GitHub", { buttonWidth, 0 }))
            jarkUtils::openUrl(RepositoryLink.data());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 网盘分发单独一组（百度网盘与蓝奏云的提取码均为 6666）
        ImGui::TextDisabled("%s", getUIString(kStrNetdiskHint));
        if (ImGui::Button("百度网盘", { buttonWidth, 0 }))
            jarkUtils::openUrl(BaiduLink.data());
        ImGui::SameLine();
        if (ImGui::Button("蓝奏云", { buttonWidth, 0 }))
            jarkUtils::openUrl(LanzouLink.data());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 开源许可与三方组件
        ImGui::TextDisabled("%s: GPL-3.0", getUIString(kStrAboutLicense));
        ImGui::TextDisabled("%s", getUIString(kStrAboutComponents));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", std::string(kAboutComponents).c_str());
        ImGui::PopStyleColor();
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
    static constexpr uint32_t kStrSortMode = 183;
    static constexpr uint32_t kStrSortName = 184;
    static constexpr uint32_t kStrSortTime = 185;
    static constexpr uint32_t kStrSortSize = 186;
    static constexpr uint32_t kStrStopAtLast = 187;
    static constexpr uint32_t kStrBlackFullscreen = 188;
    static constexpr uint32_t kStrSmoothZoom = 189; // 缩放平滑插值（存储字段取反：disableZoomSmoothing）
    static constexpr uint32_t kStrVideoHardwareDecode = 195; // 视频硬件解码（存储字段取反：disableVideoHardwareDecode）
    static constexpr uint32_t kStrReverseWasdPan = 213;
    static constexpr uint32_t kStrNetdiskHint = 166;
    static constexpr uint32_t kStrAboutLinks = 167;
    static constexpr uint32_t kStrAboutIntro = 168;
    static constexpr uint32_t kStrAboutLicense = 169;
    static constexpr uint32_t kStrAboutComponents = 170;

    // 关于页的三方开源组件清单（库名不翻译）
    static constexpr std::string_view kAboutComponents =
        "OpenCV · FFmpeg · libjxl · libheif · libavif · LibRaw · Exiv2 · LunaSVG · "
        "psd_sdk · Dear ImGui · stb · QOI · libwebp2 · DirectXTex · minizip · Lepton";

    // 帮助页文案；每行用 TAB 分隔「操作 / 按键」，| 分隔可替代的输入方式。
    static constexpr uint32_t kStrHelpTitle = 127;
    static constexpr uint32_t kStrHelpIntro = 197;
    static constexpr uint32_t kStrHelpBrowse = 198;
    static constexpr uint32_t kStrHelpTools = 199;
    static constexpr uint32_t kStrHelpPlayback = 200;
    static constexpr uint32_t kStrHelpMouse = 201;
    static constexpr uint32_t kStrHelpKeyboard = 202;
    static constexpr uint32_t kStrHelpAnimation = 203;
    static constexpr uint32_t kStrHelpPlayer = 204;
    static constexpr uint32_t kStrHelpMouseRows = 205;
    static constexpr uint32_t kStrHelpBrowseRows = 206;
    static constexpr uint32_t kStrHelpToolRows = 207;
    static constexpr uint32_t kStrHelpAnimationRows = 208;
    static constexpr uint32_t kStrHelpPlayerRows = 209;
    static constexpr uint32_t kStrHelpBrowseTip = 210;
    static constexpr uint32_t kStrHelpToolsTip = 211;
    static constexpr uint32_t kStrHelpPlaybackTip = 212;

    bool visible_ = false;
    bool focusRequested_ = false;
    int activeTab_ = 0;
    int focusRequestedTab_ = -1;
    int helpCategory_ = 0;

    bool lastColorManagement_ = false;
    uint32_t lastUiMode_ = 0;
    uint32_t lastSortMode_ = 0;

    std::vector<std::string> allSupportExt_;
    std::set<std::string> checkedExt_;

    ImTextureID aboutIcon_ = 0;
    bool aboutIconTried_ = false;
};
