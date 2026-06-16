#pragma once

#include "jarkUtils.h"

class ColorManager {
public:
    void setWindow(HWND hwnd);
    void applyToImageAsset(ImageAsset& imageAsset);

    static std::vector<uint8_t> readEmbeddedIccProfile(std::wstring_view path, std::span<const uint8_t> buf);

private:
    HWND hwnd = nullptr;

    std::vector<uint8_t> readMonitorIccProfile() const;
    std::vector<uint8_t>& readMonitorIccProfileCached();
    static bool applyToMat(cv::Mat& mat, const std::vector<uint8_t>& sourceIcc, const std::vector<uint8_t>& monitorIcc);
};
