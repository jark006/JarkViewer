#pragma once

// 重命名弹窗（ImGui）：只编辑文件名主体、扩展名保持原样。校验空名、非法字符、
// 结尾点/空格、保留设备名、过长与同名已存在；通过后把新的完整路径交给主窗口
// （takeConfirmedPath），由主窗口执行改名与列表/缓存同步。

#include "Localization.h"
#include "UiHost.h"
#include "jarkUtils.h"

#include <imgui.h>

#include <array>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <windows.h>

class RenameWindow {
public:
    static RenameWindow& instance() {
        static RenameWindow window;
        return window;
    }

    void open(const std::wstring& path) {
        path_ = path;
        const std::wstring stem = std::filesystem::path(path).stem().wstring();
        const std::string utf8Name = jarkUtils::wstringToUtf8(stem);
        std::snprintf(nameBuffer_.data(), nameBuffer_.size(), "%s", utf8Name.c_str());
        errorText_.clear();
        confirmedPath_.reset();
        visible_ = true;
        focusRequested_ = true;
    }

    void close() {
        visible_ = false;
        focusRequested_ = false;
    }

    bool visible() const { return visible_; }

    // 主窗口每帧取一次：有已确认的新路径则消费（执行改名等后续动作）
    std::optional<std::wstring> takeConfirmedPath() {
        if (!confirmedPath_.has_value())
            return std::nullopt;
        auto result = std::move(confirmedPath_);
        confirmedPath_.reset();
        return result;
    }

    void draw() {
        if (!visible_)
            return;

        const float scale = jark::ui::UiHost::instance().scale();
        const std::string title = std::string(getUIString(177)) + "###rename";

        ImGui::SetNextWindowSize(jark::ui::fitWindowSizeToMainViewport({ 560.0f * scale, 0.0f }), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(jark::ui::fitWindowSizeToMainViewport({ 420.0f * scale, 0.0f }),
            jark::ui::fitWindowSizeToMainViewport({ FLT_MAX, FLT_MAX }));
        if (focusRequested_) {
            const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, { 0.5f, 0.5f });
        }

        bool stayOpen = true;
        if (ImGui::Begin(title.c_str(), &stayOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
            if (focusRequested_) {
                ImGui::SetKeyboardFocusHere();
                focusRequested_ = false;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool submitted = ImGui::InputText("##name", nameBuffer_.data(), nameBuffer_.size(),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

            if (!errorText_.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s", errorText_.c_str());
                ImGui::PopStyleColor();
            }

            const float buttonWidth = 110.0f * scale;
            ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
                ImGui::GetWindowWidth() - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x -
                    ImGui::GetStyle().WindowPadding.x));
            if (ImGui::Button(getUIString(178), ImVec2(buttonWidth, 0)) || submitted)
                apply();
            ImGui::SameLine();
            if (ImGui::Button(getUIString(71), ImVec2(buttonWidth, 0)))
                close();
        }
        ImGui::End();

        if (!stayOpen)
            close();
    }

private:
    void apply() {
        errorText_.clear();
        const std::wstring name = jarkUtils::utf8ToWstring(nameBuffer_.data());
        const std::string validation = validate(name);
        if (!validation.empty()) {
            errorText_ = validation;
            return;
        }

        const std::filesystem::path oldPath(path_);
        const std::wstring newPath = (oldPath.parent_path() /
            (name + oldPath.extension().wstring())).wstring();

        if (_wcsicmp(newPath.c_str(), path_.c_str()) == 0) { // 没改名，直接关
            close();
            return;
        }

        confirmedPath_ = newPath;
        close();
    }

    std::string validate(const std::wstring& name) const {
        constexpr std::wstring_view invalidChars = L"\\/:*?\"<>|";
        if (name.empty() ||
            name.find_first_of(invalidChars) != std::wstring::npos ||
            name.back() == L'.' || name.back() == L' ' ||
            name.size() > 200 ||
            isReservedDeviceName(name))
            return getUIString(179);

        const std::filesystem::path oldPath(path_);
        const std::wstring newPath = (oldPath.parent_path() /
            (name + oldPath.extension().wstring())).wstring();
        std::error_code errorCode;
        if (std::filesystem::exists(newPath, errorCode) && _wcsicmp(newPath.c_str(), path_.c_str()) != 0)
            return getUIString(180);

        return {};
    }

    static bool isReservedDeviceName(const std::wstring& name) {
        std::wstring upper;
        upper.reserve(name.size());
        for (wchar_t c : name)
            upper.push_back(static_cast<wchar_t>(std::towupper(c)));

        static constexpr const wchar_t* kReserved[] = {
            L"CON", L"PRN", L"AUX", L"NUL",
            L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
            L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
        for (const wchar_t* reserved : kReserved) {
            if (upper == reserved)
                return true;
        }
        return false;
    }

    bool visible_ = false;
    bool focusRequested_ = false;
    std::wstring path_;
    std::array<char, 1024> nameBuffer_{};
    std::string errorText_;
    std::optional<std::wstring> confirmedPath_;
};
