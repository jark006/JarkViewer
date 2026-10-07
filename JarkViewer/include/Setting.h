#pragma once

#include "MatWindow.h"
#include "Localization.h"
#include "TextDrawer.h"
#include "FileAssociationManager.h"
#include "UiFramework.h"

// TODO 检查更新
// 检查是否存在最新版 https://api.github.com/repos/jark006/JarkViewer/releases/latest

extern std::wstring_view appVersion;
extern std::wstring_view jarkLink;
extern std::wstring_view RepositoryLink;
extern std::wstring_view BaiduLink;
extern std::wstring_view LanzouLink;

// 设置窗口。
// 界面用 UiFramework 声明式搭建：控件自己负责绘制与命中，容器按顺序排布，
// 尺寸一律写逻辑像素，由框架与 MatWindow 按系统缩放换算到物理像素。
class Setting : public MatWindow {
private:
    static constexpr int kLogicalWidth = 1000;
    static constexpr int kLogicalHeight = 700; // 固定尺寸，最大 700 以照顾 1366x768 屏幕
    static constexpr int kTabHeight = 50;
    static constexpr int kPagePadding = 20;
    static constexpr int kRowGap = 10;
    static constexpr int kRowHeight = 50;

    static inline const wchar_t* windowsClassName = L"JarkSettingWnd";

    // 关于页的链接热区（逻辑坐标，相对页面左上角）
    static inline const jark::ui::Rect jarkBtnRect{ 440, 50, 520, 120 };
    static inline const jark::ui::Rect reposityBtnRect{ 440, 230, 520, 90 };
    static inline const jark::ui::Rect baiduBtnRect{ 440, 330, 520, 116 };
    static inline const jark::ui::Rect lanzouBtnRect{ 440, 460, 520, 90 };

    TextDrawer textDrawer;
    cv::Mat winCanvas;
    cv::Mat settingRes;
    cv::Mat helpPage, helpPageEN, helpPageDark, helpPageDarkEN;
    cv::Mat aboutPage, aboutPageEN, aboutPageDark, aboutPageDarkEN;

    std::unique_ptr<jark::ui::TabBar> tabBar;
    std::unique_ptr<jark::ui::Panel> pagePanel;
    int builtTab = -1;
    uint32_t chromeLanguage = 0xFFFFFFFF; // 语言变化时重建标签栏与页面文案
    int activeTab = 0;                    // TabBar 绑定的普通变量（curTabIdx 是 volatile）

    // 文件关联页状态
    static inline std::vector<std::string> allSupportExt;
    static inline std::set<std::string> checkedExt;

    void Init(int tabIdx = 0) {
        curTabIdx = tabIdx;

        rcFileInfo rc = jarkUtils::GetResource(IDB_PNG_SETTING_RES, L"PNG");
        settingRes = cv::imdecode(cv::Mat(1, (int)rc.size, CV_8UC1, (uint8_t*)rc.ptr), cv::IMREAD_UNCHANGED);
        if (settingRes.channels() == 3)
            cv::cvtColor(settingRes, settingRes, cv::COLOR_BGR2BGRA);

        helpPage = settingRes({ 0, 0, 1000, 650 });
        helpPageEN = settingRes({ 1000, 0, 1000, 650 });
        helpPageDark = settingRes({ 0, 650, 1000, 650 });
        helpPageDarkEN = settingRes({ 1000, 650, 1000, 650 });

        aboutPage = settingRes({ 0, 1300, 1000, 650 });
        aboutPageEN = settingRes({ 1000, 1300, 1000, 650 });
        aboutPageDark = settingRes({ 0, 1950, 1000, 650 });
        aboutPageDarkEN = settingRes({ 1000, 1950, 1000, 650 });

        // 支持的扩展名与已勾选项
        if (allSupportExt.empty()) {
            std::set<wstring> allSupportExtW;
            allSupportExtW.insert(ImageDatabase::supportExt.begin(), ImageDatabase::supportExt.end());
            allSupportExtW.insert(ImageDatabase::supportRaw.begin(), ImageDatabase::supportRaw.end());
            for (const auto& ext : allSupportExtW)
                allSupportExt.emplace_back(jarkUtils::wstringToUtf8(ext));
        }

        checkedExt.clear();
        for (const auto& ext : jarkUtils::splitString(GlobalVar::settingParameter.extCheckedListStr, ",")) {
            if (!ext.empty())
                checkedExt.insert(ext);
        }
    }

    // —— 各标签页：声明式搭建 ——

    std::unique_ptr<jark::ui::Panel> buildGeneralPage() {
        auto& parameter = GlobalVar::settingParameter;
        auto page = std::make_unique<jark::ui::Panel>();

        // 复选项
        struct CheckItem {
            int stringID;
            bool* value;
        };
        const CheckItem checkItems[] = {
            { 12, &parameter.isAllowRotateAnimation },
            { 13, &parameter.isAllowZoomAnimation },
            { 14, &parameter.isNoteBeforeDelete },
            { 15, &parameter.enableColorManagement },
            { 54, &parameter.isOneToOnePreferred },
        };
        for (const auto& item : checkItems) {
            auto text = std::string(getUIString(item.stringID));
            page->add(std::make_unique<jark::ui::CheckBox>(text, item.value), kRowHeight, kRowGap);
        }

        // 切图动画模式
        page->add(std::make_unique<jark::ui::RadioGroup>(
            std::string(getUIString(20)),
            std::vector<std::string>{ getUIString(21), getUIString(22), getUIString(23) },
            &parameter.switchImageAnimationMode), kRowHeight, kRowGap * 3);

        // 主题：切换后立即更新当前配色
        page->add(std::make_unique<jark::ui::RadioGroup>(
            std::string(getUIString(24)),
            std::vector<std::string>{ getUIString(25), getUIString(26), getUIString(27) },
            &parameter.UI_Mode), kRowHeight, kRowGap);

        // 语言（标签 + 各语言母语写法）
        page->add(std::make_unique<jark::ui::RadioGroup>(
            std::string(getUIString(28)),
            std::vector<std::string>{
                std::string(jark::languageDisplayName(jark::Language::SimplifiedChinese)),
                std::string(jark::languageDisplayName(jark::Language::TraditionalChinese)),
                std::string(jark::languageDisplayName(jark::Language::English)),
                std::string(jark::languageDisplayName(jark::Language::Japanese)),
                std::string(jark::languageDisplayName(jark::Language::Korean)),
            },
            &parameter.UI_LANG), kRowHeight, kRowGap);

        // 右键行为
        page->add(std::make_unique<jark::ui::RadioGroup>(
            std::string(getUIString(36)),
            std::vector<std::string>{ getUIString(37), getUIString(38) },
            &parameter.rightClickAction), kRowHeight, kRowGap);

        return page;
    }

    std::unique_ptr<jark::ui::Panel> buildAssociatePage() {
        auto page = std::make_unique<jark::ui::Panel>();

        // 扩展名网格（9 列：给「3fr/jpeg」这类 4 字符的名字留够宽度，避免换行）
        page->add(std::make_unique<jark::ui::CheckGrid>(
            allSupportExt, 9,
            [](const std::string& ext) { return checkedExt.contains(ext); },
            [this](const std::string& ext, bool checked) {
                if (checked)
                    checkedExt.insert(ext);
                else
                    checkedExt.erase(ext);
                isNeedRefreshUI = true;
            }), kLogicalHeight - kTabHeight - 150 - kPagePadding * 2, 0);

        page->add(std::make_unique<jark::ui::Label>(std::string(getUIString(11))), 70, kRowGap);

        // 底部按钮行（水平排布）
        auto buttons = std::make_unique<jark::ui::Row>();
        buttons->add(std::make_unique<jark::ui::Button>(std::string(getUIString(7)), [this]() { restoreDefaultExtensions(); }));
        buttons->add(std::make_unique<jark::ui::Button>(std::string(getUIString(8)), [this]() {
            checkedExt.insert(allSupportExt.begin(), allSupportExt.end());
            isNeedRefreshUI = true;
        }, true));
        buttons->add(std::make_unique<jark::ui::Button>(std::string(getUIString(9)), [this]() {
            checkedExt.clear();
            isNeedRefreshUI = true;
        }));
        buttons->add(std::make_unique<jark::ui::Button>(std::string(getUIString(10)), [this]() { applyFileAssociations(); }));

        page->add(std::move(buttons), 60, kRowGap);
        return page;
    }

    std::unique_ptr<jark::ui::Panel> buildHelpPage() {
        auto page = std::make_unique<jark::ui::Panel>();
        const cv::Mat* image = GlobalVar::isCurrentUIDarkMode ?
            (jark::prefersChineseResources() ? &helpPageDark : &helpPageDarkEN) :
            (jark::prefersChineseResources() ? &helpPage : &helpPageEN);
        page->add(std::make_unique<jark::ui::ImageView>(image), 650, 0);
        return page;
    }

    std::unique_ptr<jark::ui::Panel> buildAboutPage() {
        auto page = std::make_unique<jark::ui::Panel>();
        const cv::Mat* image = GlobalVar::isCurrentUIDarkMode ?
            (jark::prefersChineseResources() ? &aboutPageDark : &aboutPageDarkEN) :
            (jark::prefersChineseResources() ? &aboutPage : &aboutPageEN);
        page->add(std::make_unique<jark::ui::ImageView>(image), 650, 0);

        // 版本信息与链接热区叠在图片上
        const auto textColor = GlobalVar::currentTheme.VER;
        page->overlay(std::make_unique<jark::ui::Label>(
            jarkUtils::wstringToUtf8(appVersion), textColor, jark::ui::Align::Center), { 0, 480, 400, 40 });
        page->overlay(std::make_unique<jark::ui::Label>(
            std::string(getUIString(19)), textColor, jark::ui::Align::Center), { 0, 520, 400, 40 });
        page->overlay(std::make_unique<jark::ui::Label>(
            jarkUtils::COMPILE_DATE_TIME, textColor, jark::ui::Align::Center), { 0, 550, 400, 40 });

        page->overlay(std::make_unique<jark::ui::HotArea>([]() { jarkUtils::openUrl(jarkLink.data()); }), jarkBtnRect);
        page->overlay(std::make_unique<jark::ui::HotArea>([]() { jarkUtils::openUrl(RepositoryLink.data()); }), reposityBtnRect);
        page->overlay(std::make_unique<jark::ui::HotArea>([]() { jarkUtils::openUrl(BaiduLink.data()); }), baiduBtnRect);
        page->overlay(std::make_unique<jark::ui::HotArea>([]() { jarkUtils::openUrl(LanzouLink.data()); }), lanzouBtnRect);

        return page;
    }

    // 画布与字体大小依赖窗口 DPI，必须在窗口创建之后初始化
    void initCanvas() {
        textDrawer.setSize(dp(24));
        winCanvas = cv::Mat(dp(kLogicalHeight), dp(kLogicalWidth), CV_8UC4,
            jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG));
    }

    void rebuildPageIfNeeded() {
        if (builtTab == curTabIdx && pagePanel && chromeLanguage == GlobalVar::settingParameter.UI_LANG)
            return;

        chromeLanguage = GlobalVar::settingParameter.UI_LANG;
        activeTab = curTabIdx;
        tabBar = std::make_unique<jark::ui::TabBar>(
            std::vector<std::string>{ getUIString(2), getUIString(3), getUIString(4), getUIString(5) },
            &activeTab,
            [this]() {
                curTabIdx = activeTab;
                isNeedRefreshUI = true;
            });

        switch (curTabIdx) {
        case 1: pagePanel = buildAssociatePage(); break;
        case 2: pagePanel = buildHelpPage(); break;
        case 3: pagePanel = buildAboutPage(); break;
        default: pagePanel = buildGeneralPage(); break;
        }
        builtTab = curTabIdx;
    }

    // —— 行为 ——

    void restoreDefaultExtensions() {
        memcpy(GlobalVar::settingParameter.extCheckedListStr,
            SettingParameter::defaultExtList.data(),
            SettingParameter::defaultExtList.length() + 1);

        checkedExt.clear();
        for (const auto& ext : jarkUtils::splitString(GlobalVar::settingParameter.extCheckedListStr, ",")) {
            if (!ext.empty())
                checkedExt.insert(ext);
        }
        isNeedRefreshUI = true;
    }

    void applyFileAssociations() {
        std::vector<std::wstring> checkedExtW;
        std::vector<std::wstring> unCheckedExtW;
        checkedExtW.reserve(checkedExt.size());
        unCheckedExtW.reserve(allSupportExt.size() - checkedExt.size());

        for (const auto& ext : checkedExt)
            checkedExtW.emplace_back(jarkUtils::utf8ToWstring(ext));

        for (const auto& ext : allSupportExt) {
            if (!checkedExt.contains(ext))
                unCheckedExtW.emplace_back(jarkUtils::utf8ToWstring(ext));
        }

        FileAssociationManager manager;
        const auto associationResult = manager.ManageFileAssociations(checkedExtW, unCheckedExtW);

        if (!associationResult.associationSucceeded)
            MessageBoxW(nullptr, getUIStringW(3), getUIStringW(1), MB_OK | MB_ICONERROR);
        else if (associationResult.thumbnailOperationFailed)
            MessageBoxW(nullptr, getUIStringW(41), getUIStringW(1), MB_OK | MB_ICONWARNING);
        else
            MessageBoxW(nullptr, getUIStringW(2), getUIStringW(1), MB_OK | MB_ICONINFORMATION);
    }

    // 主题改变后刷新配色与窗口属性
    void applyThemeChange() {
        GlobalVar::isCurrentUIDarkMode = GlobalVar::settingParameter.UI_Mode == 0 ?
            GlobalVar::isSystemDarkMode : (GlobalVar::settingParameter.UI_Mode == 2);
        GlobalVar::currentTheme = GlobalVar::isCurrentUIDarkMode ? deepTheme : lightTheme;
        if (hwnd) {
            BOOL themeMode = GlobalVar::isCurrentUIDarkMode;
            DwmSetWindowAttribute(hwnd, DWMWINDOWATTRIBUTE::DWMWA_USE_IMMERSIVE_DARK_MODE, &themeMode, sizeof(BOOL));
        }
        GlobalVar::isNeedUpdateTheme = true;
    }

    void finishAssociateTab() {
        std::string checkedList;
        for (const auto& ext : checkedExt) {
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

public:
    static inline volatile bool isWorking = false;
    static inline volatile HWND hwnd = nullptr;
    static inline volatile int curTabIdx = 0; // 0:常规  1:文件关联  2:帮助  3:关于

    Setting(int tabIdx = 0) {
        requestExitFlag = false;
        isWorking = true;

        Init(tabIdx);
        windowsMainLoop();

        requestExitFlag = false;
        isWorking = false;
        hwnd = nullptr;
    }

    ~Setting() {}

    static void requestExit() {
        if (hwnd)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

protected:
    void onPaint(HDC hdc) override {
        blitMat(hdc, winCanvas);
    }

    void onLButtonUp() override {
        const bool iccBefore = GlobalVar::settingParameter.enableColorManagement;
        const uint32_t modeBefore = GlobalVar::settingParameter.UI_Mode;

        rebuildPageIfNeeded();

        if (tabBar && tabBar->bounds.contains(m_x, m_y))
            tabBar->onClick(m_x, m_y);
        else if (pagePanel)
            pagePanel->onClick(m_x, m_y);

        // ICC 色彩管理开关变化后需要重新加载图像缓存
        if (GlobalVar::settingParameter.enableColorManagement != iccBefore)
            GlobalVar::isNeedReloadImageCache = true;

        if (GlobalVar::settingParameter.UI_Mode != modeBefore)
            applyThemeChange();

        isNeedRefreshUI = true;
    }

    void onRButtonUp() override {
        if (GlobalVar::settingParameter.rightClickAction == 1)
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }

    void onKeyDown(WPARAM key) override {
        if (key == VK_ESCAPE) {
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        }
        else if (key == VK_TAB) {
            curTabIdx = (curTabIdx + 1) % 4;
            isNeedRefreshUI = true;
        }
    }

    void drawingUI() override {
        rebuildPageIfNeeded();

        jark::ui::UiCanvas canvas(winCanvas, textDrawer, GlobalVar::currentTheme, uiScale());
        canvas.fill({ 0, 0, canvas.width(), canvas.height() }, GlobalVar::currentTheme.BG);

        const int tabHeight = canvas.dp(kTabHeight);
        canvas.fill({ 0, 0, canvas.width(), tabHeight }, GlobalVar::currentTheme.BG_TAG);
        tabBar->bounds = { 0, 0, canvas.width(), tabHeight };
        tabBar->draw(canvas);

        // 当前标签页背景与内容
        const jark::ui::Rect pageBounds{
            canvas.dp(kPagePadding), tabHeight + canvas.dp(kPagePadding),
            canvas.width() - canvas.dp(kPagePadding) * 2,
            canvas.height() - tabHeight - canvas.dp(kPagePadding) * 2 };
        canvas.fill({ 0, tabHeight, canvas.width(), canvas.height() - tabHeight }, GlobalVar::currentTheme.BG);
        pagePanel->bounds = pageBounds;
        pagePanel->draw(canvas);
    }

    void windowsMainLoop() {
        if (!createWindow(kLogicalWidth, kLogicalHeight, windowsClassName, getUIStringW(39)))
            return;

        hwnd = m_hwnd;
        initCanvas();
        isNeedRefreshUI = true;
        runMessageLoop();

        // 退出时保存当前 Tab 状态
        switch (curTabIdx) {
        case 0: break; // 常规页各项直接绑定设置结构，无需额外保存
        case 1: finishAssociateTab(); break;
        }
    }
};
