#pragma once

#include "MatWindow.h"
#include "TextDrawer.h"
#include "FileAssociationManager.h"

// TODO 检查更新
// 检查是否存在最新版 https://api.github.com/repos/jark006/JarkViewer/releases/latest

extern std::wstring_view appVersion;
extern std::wstring_view jarkLink;
extern std::wstring_view RepositoryLink;
extern std::wstring_view BaiduLink;
extern std::wstring_view LanzouLink;

struct labelBox {
    cv::Rect rect{};
    string_view text;
};
struct generalTabCheckBox {
    cv::Rect rect{};
    int stringID = 0;
    bool* valuePtr = nullptr;
};

struct generalTabRadio {
    cv::Rect rect{};
    std::vector<int> stringIDs;
    uint32_t* valuePtr = nullptr;
};

class Setting : public MatWindow {
private:
    static const int winWidth = 1000;
    static const int winHeight = 700; // 固定UI尺寸适应任意分辨率和DPI，最大700为了照顾1366*768的屏幕
    static const int tabHeight = 50;
    static const int tabWidth = 150;

    static const int DEBUG_COLOR = 0xFFFF8080;

    static inline const cv::Rect jarkBtnRect{ 440, 100, 520, 120 };
    static inline const cv::Rect reposityBtnRect{ 440, 280, 520, 90 };
    static inline const cv::Rect baiduBtnRect{ 440, 380, 520, 116 };
    static inline const cv::Rect lanzouBtnRect{ 440, 510, 520, 90 };

    static inline const wchar_t* windowsClassName = L"JarkSettingWnd";

    static inline std::vector<string> allSupportExt;
    static inline std::set<string> checkedExt;
    static inline std::vector<generalTabCheckBox> generalTabCheckBoxList;
    static inline std::vector<generalTabRadio> generalTabRadioList;
    static inline std::vector<labelBox> labelList;

    TextDrawer textDrawer;
    cv::Mat winCanvas, settingRes, helpPage, aboutPage, helpPageEN, aboutPageEN, helpPageDark, aboutPageDark, helpPageDarkEN, aboutPageDarkEN;

    void Init(int tabIdx = 0) {
        textDrawer.setSize(24);
        winCanvas = cv::Mat(winHeight, winWidth, CV_8UC4, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG));
        curTabIdx = tabIdx;

        rcFileInfo rc;
        rc = jarkUtils::GetResource(IDB_PNG_SETTING_RES, L"PNG");
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

        // GeneralTab
        if (generalTabCheckBoxList.empty()) {
            generalTabCheckBoxList = {
                { {50, 100, 760, 50}, 12, &GlobalVar::settingParameter.isAllowRotateAnimation },
                { {50, 150, 760, 50}, 13, &GlobalVar::settingParameter.isAllowZoomAnimation },
                { {50, 200, 760, 50}, 14, &GlobalVar::settingParameter.isNoteBeforeDelete },
                { {50, 250, 760, 50}, 15, &GlobalVar::settingParameter.enableColorManagement },
            };
        }
        if (generalTabRadioList.empty()) {
            generalTabRadioList = {
                {{50, 350, 600, 50}, {20, 21, 22, 23}, &GlobalVar::settingParameter.switchImageAnimationMode },
                {{50, 400, 600, 50}, {24, 25, 26, 27}, &GlobalVar::settingParameter.UI_Mode },
                {{50, 450, 450, 50}, {28, 30, 31}, &GlobalVar::settingParameter.UI_LANG },
                {{50, 500, 450, 50}, {36, 37, 38}, &GlobalVar::settingParameter.rightClickAction },
            };
        }

        // AssociateTab
        if (allSupportExt.empty()) {
            std::set<wstring> allSupportExtW;
            allSupportExtW.insert(ImageDatabase::supportExt.begin(), ImageDatabase::supportExt.end());
            allSupportExtW.insert(ImageDatabase::supportRaw.begin(), ImageDatabase::supportRaw.end());
            for (const auto& ext : allSupportExtW)
                allSupportExt.emplace_back(jarkUtils::wstringToUtf8(ext));
        }

        auto checkedExtVec = jarkUtils::splitString(GlobalVar::settingParameter.extCheckedListStr, ",");
        auto filtered = checkedExtVec | std::views::filter([](const std::string& s) { return !s.empty(); });
        checkedExt.clear();
        if (!filtered.empty())
            checkedExt.insert(filtered.begin(), filtered.end());
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

    void refreshGeneralTab() {
        cv::rectangle(winCanvas, { 0, tabHeight, winWidth, winHeight - tabHeight }, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG), -1);

        for (auto& cbox : generalTabCheckBoxList) {
#ifndef NDEBUG
            cv::rectangle(winCanvas, cbox.rect, jarkUtils::to_cv_scalar(DEBUG_COLOR), 1);
#endif
            cv::Rect rect({ cbox.rect.x + 8, cbox.rect.y + 8, cbox.rect.height - 16, cbox.rect.height - 16 }); //方形
            cv::rectangle(winCanvas, rect, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.FG_DEEP), 4);
            if (*cbox.valuePtr) {
                rect = { cbox.rect.x + 14, cbox.rect.y + 14, cbox.rect.height - 28, cbox.rect.height - 28 }; //小方形
                cv::rectangle(winCanvas, rect, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.CHECK), -1);
            }

            rect = { cbox.rect.x + cbox.rect.height, cbox.rect.y+8, cbox.rect.width - cbox.rect.height, cbox.rect.height };
            textDrawer.putAlignLeft(winCanvas, rect, getUIString(cbox.stringID), GlobalVar::currentTheme.FG);
        }

        for (auto& radio : generalTabRadioList) {
            int idx = *radio.valuePtr;
            if (idx >= radio.stringIDs.size())
                idx = 0;
#ifndef NDEBUG
            cv::rectangle(winCanvas, radio.rect, jarkUtils::to_cv_scalar(DEBUG_COLOR), 1);
#endif
            int itemWidth = radio.rect.width / (int)radio.stringIDs.size();
            cv::Rect rect1 = { radio.rect.x + itemWidth * (1 + idx) , radio.rect.y + 4, itemWidth, radio.rect.height - 6 }; // 当前项背景框
            cv::rectangle(winCanvas, rect1, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.CHECK), -1);

            cv::Rect rect2 = { radio.rect.x + itemWidth , radio.rect.y + 4, radio.rect.width - itemWidth, radio.rect.height - 6 }; //大框
            cv::rectangle(winCanvas, rect2, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.FG_DEEP), 2);

            for (int i = 0; i < radio.stringIDs.size(); i++) {
                cv::Rect rect3 = { radio.rect.x + itemWidth * (i), radio.rect.y , itemWidth, radio.rect.height };
                textDrawer.putAlignCenter(winCanvas, rect3, getUIString(radio.stringIDs[i]), GlobalVar::currentTheme.FG);
            }
        }

        for (auto& labelBox : labelList) {
            cv::Rect rect = {
                labelBox.rect.x + labelBox.rect.height,
                labelBox.rect.y,
                labelBox.rect.width - labelBox.rect.height,
                labelBox.rect.height };
            textDrawer.putAlignCenter(winCanvas, rect, labelBox.text.data(), GlobalVar::currentTheme.FG);
        }
    }

    void refreshAssociateTab() {
        cv::rectangle(winCanvas, { 0, tabHeight, winWidth, winHeight - tabHeight }, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG), -1);

        // 需和 handleAssociateTab 参数保持一致
        const int xOffset = 20, yOffset = 70;
        const int gridWidth = 80, gridHeight = 40;
        const int gridNumPerLine = 12;
        const int btnWidth = 200;
        const int btnHeight = 60;
        const cv::Rect btnRectList[4] = {
            { winWidth - btnWidth * 4, winHeight - btnHeight, btnWidth, btnHeight }, // defaultCheck
            { winWidth - btnWidth * 3, winHeight - btnHeight, btnWidth, btnHeight }, // allCheckBtnRect
            { winWidth - btnWidth * 2, winHeight - btnHeight, btnWidth, btnHeight }, // allClearBtnRect
            { winWidth - btnWidth, winHeight - btnHeight, btnWidth, btnHeight }, // confirmBtnRect
        };

        const int btnTextList[4] = { 7, 8, 9, 10 };

        int idx = 0;
        for (const auto& ext : allSupportExt) {
            int y = idx / gridNumPerLine;
            int x = idx % gridNumPerLine;
            cv::Rect rect({ xOffset + gridWidth * x, yOffset + gridHeight * y, gridWidth, gridHeight });
            if (checkedExt.contains(ext)) {
                cv::Rect rect2{ rect.x + 2, rect.y + 4, rect.width - 4, rect.height - 4 };
                cv::rectangle(winCanvas, rect2, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.CHECK), -1);
            }
            textDrawer.putAlignCenter(winCanvas, rect, ext.c_str(), GlobalVar::currentTheme.FG);
            idx++;
        }

        for (int i = 0; i < 4; i++) {
            const cv::Rect& rect = btnRectList[i];
            cv::Rect rect2{ rect.x + 4, rect.y + 8, rect.width - 8, rect.height - 12 };
            cv::rectangle(winCanvas, rect2, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG_BTN), -1);
            textDrawer.putAlignCenter(winCanvas, rect, getUIString(btnTextList[i]), GlobalVar::currentTheme.FG);
        }

        textDrawer.putAlignLeft(winCanvas, { 20, winHeight - 200, winWidth - 20, winHeight }, getUIString(11), GlobalVar::currentTheme.FG);
    }

    void refreshHelpTab() {
        if (GlobalVar::isCurrentUIDarkMode)
            jarkUtils::overlayImg(winCanvas, GlobalVar::settingParameter.UI_LANG == 0 ? helpPageDark : helpPageDarkEN, 0, 50);
        else
            jarkUtils::overlayImg(winCanvas, GlobalVar::settingParameter.UI_LANG == 0 ? helpPage : helpPageEN, 0, 50);
    }

    void refreshAboutTab() {
        if (GlobalVar::isCurrentUIDarkMode)
            jarkUtils::overlayImg(winCanvas, GlobalVar::settingParameter.UI_LANG == 0 ? aboutPageDark : aboutPageDarkEN, 0, 50);
        else
            jarkUtils::overlayImg(winCanvas, GlobalVar::settingParameter.UI_LANG == 0 ? aboutPage : aboutPageEN, 0, 50);

        auto textColor = GlobalVar::currentTheme.VER;
        textDrawer.putAlignCenter(winCanvas, { 0, 530, 400, 40 }, jarkUtils::wstringToUtf8(appVersion).c_str(), textColor);
        textDrawer.putAlignCenter(winCanvas, { 0, 570, 400, 40 }, getUIString(19), textColor);
        textDrawer.putAlignCenter(winCanvas, { 0, 600, 400, 40 }, jarkUtils::COMPILE_DATE_TIME, textColor);

#ifndef NDEBUG
        cv::rectangle(winCanvas, jarkBtnRect, jarkUtils::to_cv_scalar(DEBUG_COLOR), 1);
        cv::rectangle(winCanvas, reposityBtnRect, jarkUtils::to_cv_scalar(DEBUG_COLOR), 1);
        cv::rectangle(winCanvas, baiduBtnRect, jarkUtils::to_cv_scalar(DEBUG_COLOR), 1);
        cv::rectangle(winCanvas, lanzouBtnRect, jarkUtils::to_cv_scalar(DEBUG_COLOR), 1);
#endif
    }

    void onPaint(HDC hdc) override {
        if (!winCanvas.empty())
            blitMat(hdc, winCanvas);
    }

    void onLButtonUp() override {
        // 标签栏点击
        if (m_y < 50) {
            int newTabIdx = m_x / tabWidth;
            if (newTabIdx <= 3 && newTabIdx != curTabIdx) {
                switch (curTabIdx) {
                case 0: finishGeneralTab(); break;
                case 1: finishAssociateTab(); break;
                }
                curTabIdx = newTabIdx;
                isNeedRefreshUI = true;
            }
            return;
        }

        // 各 Tab 点击处理
        switch (curTabIdx) {
        case 0: handleGeneralTab(cv::EVENT_LBUTTONUP, m_x, m_y, 0); break;
        case 1: handleAssociateTab(cv::EVENT_LBUTTONUP, m_x, m_y, 0); break;
        case 3: handleAboutTab(cv::EVENT_LBUTTONUP, m_x, m_y, 0); break;
        }
    }

    void onRButtonUp() override {
        if (GlobalVar::settingParameter.rightClickAction == 1) {
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        }
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
        // 绘制标签栏
        cv::rectangle(winCanvas, { 0, 0, winWidth, tabHeight }, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG_TAG), -1);
        cv::rectangle(winCanvas, { curTabIdx * tabWidth, 0, tabWidth, tabHeight }, jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG), -1);
        textDrawer.putAlignCenter(winCanvas, { 0, 0,150, 50 }, getUIString(2), GlobalVar::currentTheme.FG);
        textDrawer.putAlignCenter(winCanvas, { 150, 0,150, 50 }, getUIString(3), GlobalVar::currentTheme.FG);
        textDrawer.putAlignCenter(winCanvas, { 300, 0,150, 50 }, getUIString(4), GlobalVar::currentTheme.FG);
        textDrawer.putAlignCenter(winCanvas, { 450, 0,150, 50 }, getUIString(5), GlobalVar::currentTheme.FG);

        switch (curTabIdx) {
        case 0:refreshGeneralTab(); break;
        case 1:refreshAssociateTab(); break;
        case 2:refreshHelpTab(); break;
        default:refreshAboutTab(); break;
        }
    }

    void handleGeneralTab(int event, int x, int y, int flags) {
        if (event == cv::EVENT_LBUTTONUP) {
            for (auto& cbox : generalTabCheckBoxList) {
                if (isInside(x, y, cbox.rect)) {
                    *cbox.valuePtr = !(*cbox.valuePtr);
                    if (cbox.valuePtr == &GlobalVar::settingParameter.enableColorManagement)
                        GlobalVar::isNeedReloadImageCache = true;
                    isNeedRefreshUI = true;
                }
            }

            for (auto& radio : generalTabRadioList) {
                if (isInside(x, y, radio.rect)) {
                    int itemWidth = radio.rect.width / (int)radio.stringIDs.size();
                    int clickIdx = (x - radio.rect.x) / itemWidth - 1;
                    if (0 <= clickIdx && clickIdx < radio.stringIDs.size() - 1) {
                        *radio.valuePtr = clickIdx;

                        if (radio.stringIDs.front() == 24) { // UI_Mode 颜色主题改变
                            GlobalVar::isCurrentUIDarkMode = GlobalVar::settingParameter.UI_Mode == 0 ?
                                GlobalVar::isSystemDarkMode : (GlobalVar::settingParameter.UI_Mode == 2);
                            GlobalVar::currentTheme = GlobalVar::isCurrentUIDarkMode ? deepTheme : lightTheme;
                            updateWindowAttribute();
                            GlobalVar::isNeedUpdateTheme = true;
                        }
                        isNeedRefreshUI = true;
                    }
                }
            }
        }
    }

    void finishGeneralTab() {

    }

    void updateWindowAttribute() {
        if (hwnd) {
            BOOL themeMode = GlobalVar::isCurrentUIDarkMode;
            DwmSetWindowAttribute(hwnd, DWMWINDOWATTRIBUTE::DWMWA_USE_IMMERSIVE_DARK_MODE, &themeMode, sizeof(BOOL));
        }
    }

    int getGridIndex(int x, int y, int xOffset = 50, int yOffset = 200,
        int gridW = 20, int gridH = 10, int colsPerRow = 10, int idxMax = -1) {  // 默认不检查 idxMax
        int relativeX = x - xOffset;
        int relativeY = y - yOffset;
        if (relativeX < 0 || relativeY < 0) {
            return -1; // 无效坐标
        }
        int col = relativeX / gridW;
        int row = relativeY / gridH;
        if (col >= colsPerRow) {
            return -1; // 超出列范围
        }
        int index = row * colsPerRow + col;
        if (idxMax >= 0 && index >= idxMax) {  // 如果 idxMax 有效，并且 index 超出
            return -1;
        }
        return index;
    }

    template<typename T>
    void toggle(std::set<T>& s, const T& value) {
        if (!s.insert(value).second) {
            s.erase(value);
        }
    }

    bool SetupFileAssociations(const std::vector<std::wstring>& extChecked,
        const std::vector<std::wstring>& extUnchecked) {

        FileAssociationManager manager;
        return manager.ManageFileAssociations(extChecked, extUnchecked);
    }

    void handleAssociateTab(int event, int x, int y, int flags) {

        // 需和 refreshAssociateTab 参数保持一致
        const int xOffset = 20, yOffset = 70;
        const int gridWidth = 80, gridHeight = 40;
        const int gridNumPerLine = 12;
        const int btnWidth = 200;
        const int btnHeight = 60;
        const cv::Rect btnRectList[4] = {
            { winWidth - btnWidth * 4, winHeight - btnHeight, btnWidth, btnHeight }, // defaultCheck
            { winWidth - btnWidth * 3, winHeight - btnHeight, btnWidth, btnHeight }, // allCheckBtnRect
            { winWidth - btnWidth * 2, winHeight - btnHeight, btnWidth, btnHeight }, // allClearBtnRect
            { winWidth - btnWidth, winHeight - btnHeight, btnWidth, btnHeight }, // confirmBtnRect
        };

        if (event == cv::EVENT_LBUTTONUP) {
            int gridIdx = getGridIndex(x, y, xOffset, yOffset, gridWidth, gridHeight, gridNumPerLine, (int)allSupportExt.size());
            if (gridIdx >= 0) {
                const auto& targetExt = (allSupportExt)[gridIdx];
                toggle(checkedExt, targetExt);
                isNeedRefreshUI = true;
            }
            else if (isInside(x, y, btnRectList[0])) { // 恢复默认勾选
                memcpy(GlobalVar::settingParameter.extCheckedListStr,
                    SettingParameter::defaultExtList.data(),
                    SettingParameter::defaultExtList.length() + 1);

                auto checkedExtVec = jarkUtils::splitString(GlobalVar::settingParameter.extCheckedListStr, ",");
                auto filtered = checkedExtVec | std::views::filter([](const std::string& s) { return !s.empty(); });
                checkedExt.clear();
                if (!filtered.empty())
                    checkedExt.insert(filtered.begin(), filtered.end());

                isNeedRefreshUI = true;
            }
            else if (isInside(x, y, btnRectList[1])) { // 全选
                checkedExt.insert(allSupportExt.begin(), allSupportExt.end());
                isNeedRefreshUI = true;
            }
            else if (isInside(x, y, btnRectList[2])) { // 全不选
                checkedExt.clear();
                isNeedRefreshUI = true;
            }
            else if (isInside(x, y, btnRectList[3])) { // 立即关联
                std::vector<std::wstring> checkedExtW, unCheckedExtW;

                checkedExtW.reserve(checkedExt.size());
                unCheckedExtW.reserve(allSupportExt.size() - checkedExt.size());

                for (const auto& ext : checkedExt) {
                    checkedExtW.emplace_back(jarkUtils::utf8ToWstring(ext));
                }
                for (const auto& ext : allSupportExt) {
                    if (!checkedExt.contains(ext))
                        unCheckedExtW.emplace_back(jarkUtils::utf8ToWstring(ext));
                }

                if (SetupFileAssociations(checkedExtW, unCheckedExtW)) {
                    MessageBoxW(nullptr, getUIStringW(2), getUIStringW(1), MB_OK | MB_ICONINFORMATION);
                }
                else {
                    MessageBoxW(nullptr, getUIStringW(3), getUIStringW(1), MB_OK | MB_ICONERROR);
                }
            }
        }

    }

    void finishAssociateTab() {
        std::string checkedList;
        for (const auto& ext : checkedExt) {
            checkedList += ext;
            checkedList += ',';
        }
        if (checkedList.back() == ',')
            checkedList.pop_back();
        if (checkedList.empty())
            memset(GlobalVar::settingParameter.extCheckedListStr, 0, 4);
        else
            memcpy(GlobalVar::settingParameter.extCheckedListStr, checkedList.data(), checkedList.length() + 1);
    }

    bool isInside(int x, int y, const cv::Rect& rect) {
        return rect.x < x && x < (rect.x + rect.width) && rect.y < y && y < (rect.y + rect.height);
    }

    void handleAboutTab(int event, int x, int y, int flags) {
        if (event == cv::EVENT_LBUTTONUP) {
            if (isInside(x, y, jarkBtnRect)) {
                jarkUtils::openUrl(jarkLink.data());
            }
            if (isInside(x, y, reposityBtnRect)) {
                jarkUtils::openUrl(RepositoryLink.data());
            }
            if (isInside(x, y, baiduBtnRect)) {
                jarkUtils::openUrl(BaiduLink.data());
            }
            if (isInside(x, y, lanzouBtnRect)) {
                jarkUtils::openUrl(LanzouLink.data());
            }
        }
    }

    void windowsMainLoop() {
        if (!createWindow(winWidth, winHeight, windowsClassName, getUIStringW(39)))
            return;

        hwnd = m_hwnd;
        runMessageLoop();

        // 退出时保存当前 Tab 状态
        switch (curTabIdx) {
        case 0: finishGeneralTab(); break;
        case 1: finishAssociateTab(); break;
        }
    }
};
