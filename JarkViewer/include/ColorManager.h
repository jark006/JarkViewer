#pragma once

#include "jarkUtils.h"

class ColorManager {
public:
    void setWindow(HWND hwnd);
    void applyToImageAsset(ImageAsset& imageAsset);

    // 直接作用于 Mat 的入口，返回是否真的执行了变换（恒等会跳过）。
    // 供自检直接调用：--probe --color-test
    static bool applyToMat(cv::Mat& mat, const std::vector<uint8_t>& sourceIcc, const std::vector<uint8_t>& monitorIcc);

    static std::vector<uint8_t> readEmbeddedIccProfile(std::wstring_view path, std::span<const uint8_t> buf);

private:
    HWND hwnd = nullptr;

    std::vector<uint8_t> readMonitorIccProfile() const;
    std::vector<uint8_t>& readMonitorIccProfileCached();
};
