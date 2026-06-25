#pragma once

#include "FileAssociationNaming.h"
#include "ThumbnailProviderGuids.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <shlwapi.h>
#include <string>
#include <string_view>
#include <windows.h>

#pragma comment(lib, "shlwapi.lib")

enum class ThumbnailRegistrationResult {
    Succeeded,
    SkippedProviderMissing,
    Failed
};

class ThumbnailRegistrar {
public:
    ThumbnailRegistrar()
        : m_providerDllPath(BuildProviderDllPathFromAppPath({})) {
    }

    explicit ThumbnailRegistrar(std::wstring appPath)
        : m_providerDllPath(BuildProviderDllPathFromAppPath(appPath)) {
    }

    static constexpr std::wstring_view ProviderClsidString() noexcept {
        return JarkThumbnailProviderGuids::ProviderClsidString;
    }

    static constexpr std::wstring_view ThumbnailProviderHandlerString() noexcept {
        return JarkThumbnailProviderGuids::ThumbnailProviderHandlerString;
    }

    static std::wstring BuildProviderDllPathFromAppPath(std::wstring_view appPath) {
        constexpr std::wstring_view dllName = L"JarkThumbnailProvider.dll";
        if (appPath.empty()) {
            return std::wstring(dllName);
        }

        const auto pos = appPath.find_last_of(L"\\/");
        if (pos == std::wstring_view::npos) {
            return std::wstring(dllName);
        }

        std::wstring path(appPath.substr(0, pos + 1));
        path.append(dllName);
        return path;
    }

    static bool IsThumbnailEligibleExtension(std::wstring_view extension) {
        const auto normalized = FileAssociationNaming::NormalizeExtension(extension);
        return std::ranges::any_of(kThumbnailEligibleExtensions, [&](std::wstring_view item) {
            return item == normalized;
        });
    }

    ThumbnailRegistrationResult AssociateExtension(std::wstring_view extension) const {
        try {
            if (!IsThumbnailEligibleExtension(extension)) {
                return ThumbnailRegistrationResult::Succeeded;
            }

            if (!ProviderDllExists()) {
                return DeleteShellExtensionKeyIfOwned(extension) ?
                    ThumbnailRegistrationResult::SkippedProviderMissing :
                    ThumbnailRegistrationResult::Failed;
            }

            if (!EnsureProviderClsidRegistered()) {
                return ThumbnailRegistrationResult::Failed;
            }

            const auto shellExtKey = BuildShellExtensionKey(extension);
            return SetRegistryValue(HKEY_CURRENT_USER, shellExtKey, L"", std::wstring(ProviderClsidString())) ?
                ThumbnailRegistrationResult::Succeeded :
                ThumbnailRegistrationResult::Failed;
        }
        catch (...) {
            return ThumbnailRegistrationResult::Failed;
        }
    }

    ThumbnailRegistrationResult UnassociateExtension(std::wstring_view extension) const {
        try {
            if (!IsThumbnailEligibleExtension(extension)) {
                return ThumbnailRegistrationResult::Succeeded;
            }

            return DeleteShellExtensionKeyIfOwned(extension) ?
                ThumbnailRegistrationResult::Succeeded :
                ThumbnailRegistrationResult::Failed;
        }
        catch (...) {
            return ThumbnailRegistrationResult::Failed;
        }
    }

    ThumbnailRegistrationResult CleanupProviderClsidIfUnused() const {
        try {
            if (HasAnyRegisteredEligibleExtension()) {
                return ThumbnailRegistrationResult::Succeeded;
            }

            return DeleteRegistryKey(HKEY_CURRENT_USER, BuildProviderClsidKey()) ?
                ThumbnailRegistrationResult::Succeeded :
                ThumbnailRegistrationResult::Failed;
        }
        catch (...) {
            return ThumbnailRegistrationResult::Failed;
        }
    }

private:
    static constexpr auto kThumbnailEligibleExtensions = std::to_array<std::wstring_view>({
        L"3fr", L"apng", L"ari", L"arw", L"avif", L"avifs", L"bay", L"blp", L"cap",
        L"cr2", L"cr3", L"crw", L"dcr", L"dcs", L"dds", L"dng", L"drf",
        L"eip", L"erf", L"exr", L"fff", L"gpr", L"hdr", L"heic", L"heif",
        L"iiq", L"jp2", L"jxl", L"jxr", L"k25", L"kdc", L"lep", L"livp",
        L"mdc", L"mef", L"mos", L"mrw", L"nef", L"nrw", L"orf", L"pbm",
        L"pcx", L"pef", L"pfm", L"pgm", L"pic", L"pnm", L"ppm", L"psd",
        L"psdt", L"ptx", L"pxm", L"qoi", L"r3d", L"raf", L"ras", L"raw",
        L"rw2", L"rwl", L"rwz", L"sr", L"sr2", L"srf", L"srw", L"svg",
        L"tga", L"wp2", L"x3f"
    });

    static std::wstring BuildProviderClsidKey() {
        return L"Software\\Classes\\CLSID\\" + std::wstring(ProviderClsidString());
    }

    static std::wstring BuildShellExtensionKey(std::wstring_view extension) {
        return L"Software\\Classes\\SystemFileAssociations\\." +
            FileAssociationNaming::NormalizeExtension(extension) +
            L"\\ShellEx\\" + std::wstring(ThumbnailProviderHandlerString());
    }

    static bool SetRegistryValue(HKEY hKey, const std::wstring& subKey,
        const std::wstring& valueName, const std::wstring& value) {
        HKEY hSubKey = nullptr;
        DWORD disposition = 0;

        LONG result = RegCreateKeyExW(hKey, subKey.c_str(), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hSubKey, &disposition);
        if (result != ERROR_SUCCESS) {
            return false;
        }

        result = RegSetValueExW(hSubKey, valueName.c_str(), 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.length() + 1) * sizeof(wchar_t)));

        RegCloseKey(hSubKey);
        return result == ERROR_SUCCESS;
    }

    static bool DeleteRegistryKey(HKEY hKey, const std::wstring& subKey) {
        const auto result = SHDeleteKeyW(hKey, subKey.c_str());
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
    }

    static std::wstring QueryDefaultValue(HKEY hKey, const std::wstring& subKey) {
        HKEY hSubKey = nullptr;
        LONG result = RegOpenKeyExW(hKey, subKey.c_str(), 0, KEY_READ, &hSubKey);
        if (result != ERROR_SUCCESS) {
            return {};
        }

        wchar_t value[256]{};
        DWORD bufferBytes = sizeof(value);
        result = RegQueryValueExW(hSubKey, L"", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(value), &bufferBytes);

        RegCloseKey(hSubKey);
        return result == ERROR_SUCCESS ? std::wstring(value) : std::wstring{};
    }

    static bool IsProviderClsid(std::wstring_view value) {
        return !value.empty() && _wcsicmp(value.data(), ProviderClsidString().data()) == 0;
    }

    bool EnsureProviderClsidRegistered() const {
        const auto clsidKey = BuildProviderClsidKey();
        const auto inprocKey = clsidKey + L"\\InprocServer32";

        if (!SetRegistryValue(HKEY_CURRENT_USER, clsidKey, L"", L"JarkViewer Thumbnail Provider")) {
            return false;
        }

        if (!SetRegistryValue(HKEY_CURRENT_USER, inprocKey, L"", m_providerDllPath)) {
            return false;
        }

        return SetRegistryValue(HKEY_CURRENT_USER, inprocKey, L"ThreadingModel", L"Apartment");
    }

    bool ProviderDllExists() const {
        const DWORD attributes = GetFileAttributesW(m_providerDllPath.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    static bool DeleteShellExtensionKeyIfOwned(std::wstring_view extension) {
        const auto shellExtKey = BuildShellExtensionKey(extension);
        const auto value = QueryDefaultValue(HKEY_CURRENT_USER, shellExtKey);
        if (!IsProviderClsid(value)) {
            return true;
        }

        return DeleteRegistryKey(HKEY_CURRENT_USER, shellExtKey);
    }

    bool HasAnyRegisteredEligibleExtension() const {
        for (const auto ext : kThumbnailEligibleExtensions) {
            const auto value = QueryDefaultValue(HKEY_CURRENT_USER, BuildShellExtensionKey(ext));
            if (IsProviderClsid(value)) {
                return true;
            }
        }
        return false;
    }

    std::wstring m_providerDllPath;
};
