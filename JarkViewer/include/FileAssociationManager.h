#pragma once
#include "FileAssociationNaming.h"
#include "ThumbnailRegistrar.h"
#include "jarkUtils.h"
#include <shlwapi.h>
#include <shlobj.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

struct FileAssociationResult {
    bool associationSucceeded = true;
    bool thumbnailOperationFailed = false;
};

class FileAssociationManager {
private:
    std::wstring m_appPath;
    std::wstring m_legacyProgId;
    ThumbnailRegistrar m_thumbnailRegistrar;

    // 设置注册表值
    bool SetRegistryValue(HKEY hKey, const std::wstring& subKey, const std::wstring& valueName, const std::wstring& value) {
        HKEY hSubKey;
        DWORD disposition;

        LONG result = RegCreateKeyExW(hKey, subKey.c_str(), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_WRITE,
            nullptr, &hSubKey, &disposition);

        if (result != ERROR_SUCCESS) {
            return false;
        }

        result = RegSetValueExW(hSubKey, valueName.c_str(), 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.length() + 1) * sizeof(wchar_t)));

        RegCloseKey(hSubKey);
        return result == ERROR_SUCCESS;
    }

    // 删除注册表键
    bool DeleteRegistryKey(HKEY hKey, const std::wstring& subKey) {
        const auto result = SHDeleteKeyW(hKey, subKey.c_str());
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
    }

    // 删除注册表值
    bool DeleteRegistryValue(HKEY hKey, const std::wstring& subKey, const std::wstring& valueName) {
        HKEY hSubKey;
        LONG result = RegOpenKeyExW(hKey, subKey.c_str(), 0, KEY_WRITE, &hSubKey);

        if (result != ERROR_SUCCESS) {
            return result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
        }

        result = RegDeleteValueW(hSubKey, valueName.c_str());
        RegCloseKey(hSubKey);

        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }

    std::wstring GetExpectedProgId(const std::wstring& extension) const {
        return FileAssociationNaming::BuildProgId(extension);
    }

    std::wstring GetAssociatedProgId(const std::wstring& extension) {
        const std::wstring extKey = FileAssociationNaming::BuildExtensionKey(extension);

        HKEY hKey;
        LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, extKey.c_str(), 0, KEY_READ, &hKey);
        if (result != ERROR_SUCCESS) {
            return {};
        }

        wchar_t progId[256]{};
        DWORD bufferSize = sizeof(progId);
        result = RegQueryValueExW(hKey, L"", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(progId), &bufferSize);

        RegCloseKey(hKey);
        return result == ERROR_SUCCESS ? std::wstring(progId) : std::wstring{};
    }

    bool IsManagedProgId(const std::wstring& progId, const std::wstring& extension) const {
        return progId == GetExpectedProgId(extension) || progId == m_legacyProgId;
    }

    // 检查是否已经关联到当前程序
    bool IsAssociatedWithCurrentApp(const std::wstring& extension) {
        return IsManagedProgId(GetAssociatedProgId(extension), extension);
    }

    // 注册程序ID（不设置图标，保持系统缩略图功能）
    bool RegisterProgId(const std::wstring& extension) {
        const std::wstring progIdKey = FileAssociationNaming::BuildProgIdKey(extension);

        // 设置程序描述
        if (!SetRegistryValue(HKEY_CURRENT_USER, progIdKey, L"", FileAssociationNaming::BuildTypeName(extension))) {
            return false;
        }

        // 设置打开命令
        std::wstring commandKey = progIdKey + L"\\shell\\open\\command";
        std::wstring commandValue = L"\"" + m_appPath + L"\" \"%1\"";
        if (!SetRegistryValue(HKEY_CURRENT_USER, commandKey, L"", commandValue)) {
            return false;
        }

        return true;
    }

    static void MergeThumbnailResult(FileAssociationResult& result, ThumbnailRegistrationResult thumbnailResult) {
        if (thumbnailResult == ThumbnailRegistrationResult::Failed) {
            result.thumbnailOperationFailed = true;
        }
    }

    // 关联文件扩展名
    bool AssociateExtension(const std::wstring& extension, FileAssociationResult& result) {
        const std::wstring progId = GetExpectedProgId(extension);
        const std::wstring extKey = FileAssociationNaming::BuildExtensionKey(extension);

        if (!RegisterProgId(extension)) {
            return false;
        }

        // 设置文件类型关联
        if (!SetRegistryValue(HKEY_CURRENT_USER, extKey, L"", progId)) {
            return false;
        }

        MergeThumbnailResult(result, m_thumbnailRegistrar.AssociateExtension(extension));
        return true;
    }

    // 取消关联文件扩展名
    bool UnassociateExtension(const std::wstring& extension, FileAssociationResult& result) {
        MergeThumbnailResult(result, m_thumbnailRegistrar.UnassociateExtension(extension));
        const std::wstring associatedProgId = GetAssociatedProgId(extension);

        // 检查是否关联到当前程序
        if (!IsManagedProgId(associatedProgId, extension)) {
            return true; // 如果没有关联到当前程序，只清理缩略图挂接
        }

        // 删除类关联
        const std::wstring extKey = FileAssociationNaming::BuildExtensionKey(extension);
        const std::wstring progIdKey = FileAssociationNaming::BuildProgIdKey(extension);
        DeleteRegistryValue(HKEY_CURRENT_USER, extKey, L"");
        DeleteRegistryKey(HKEY_CURRENT_USER, progIdKey);

        return true;
    }

    void CleanupLegacyProgIdIfUnused(const std::vector<std::wstring>& extChecked,
        const std::vector<std::wstring>& extUnchecked) {

        for (const auto& ext : extChecked) {
            if (GetAssociatedProgId(ext) == m_legacyProgId) {
                return;
            }
        }

        for (const auto& ext : extUnchecked) {
            if (GetAssociatedProgId(ext) == m_legacyProgId) {
                return;
            }
        }

        DeleteRegistryKey(HKEY_CURRENT_USER, L"Software\\Classes\\" + m_legacyProgId);
    }

    // 通知系统文件关联已更改
    void NotifySystemChange() {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }

public:
    FileAssociationManager()
        : m_appPath(jarkUtils::getCurrentAppPath()),
        m_legacyProgId(L"JarkViewer.ImageFile"),
        m_thumbnailRegistrar(m_appPath) {
    }

    // 管理文件关联的主函数
    FileAssociationResult ManageFileAssociations(const std::vector<std::wstring>& extChecked,
        const std::vector<std::wstring>& extUnchecked) {

        FileAssociationResult result;

        // 关联需要的扩展名
        for (const auto& ext : extChecked) {
            if (!AssociateExtension(ext, result)) {
                result.associationSucceeded = false;
            }
        }

        // 取消不需要的扩展名关联
        for (const auto& ext : extUnchecked) {
            if (!UnassociateExtension(ext, result)) {
                result.associationSucceeded = false;
            }
        }

        CleanupLegacyProgIdIfUnused(extChecked, extUnchecked);
        MergeThumbnailResult(result, m_thumbnailRegistrar.CleanupProviderClsidIfUnused());

        // 通知系统更改
        NotifySystemChange();

        return result;
    }
};
