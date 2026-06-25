#include "ClassFactory.h"

#include "ThumbnailProviderGuids.h"

#include <new>
#include <shlwapi.h>
#include <string>
#include <windows.h>

#pragma comment(lib, "shlwapi.lib")

HINSTANCE g_instance = nullptr;
long g_moduleRefCount = 0;

namespace {
std::wstring providerClsidKey() {
    return L"Software\\Classes\\CLSID\\" + std::wstring(JarkThumbnailProviderGuids::ProviderClsidString);
}

bool setRegistryValue(HKEY hKey, const std::wstring& subKey, const std::wstring& valueName, const std::wstring& value) {
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

std::wstring modulePath() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD chars = GetModuleFileNameW(g_instance, path.data(), static_cast<DWORD>(path.size()));
    if (chars == 0) {
        return {};
    }

    while (chars == path.size()) {
        path.resize(path.size() * 2);
        chars = GetModuleFileNameW(g_instance, path.data(), static_cast<DWORD>(path.size()));
        if (chars == 0) {
            return {};
        }
    }

    path.resize(chars);
    return path;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = module;
        DisableThreadLibraryCalls(module);
    }

    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
    if (!object) {
        return E_POINTER;
    }

    *object = nullptr;
    if (clsid != JarkThumbnailProviderGuids::CLSID_JarkThumbnailProvider) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    ClassFactory* factory = new (std::nothrow) ClassFactory();
    if (!factory) {
        return E_OUTOFMEMORY;
    }

    HRESULT hr = factory->QueryInterface(riid, object);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return g_moduleRefCount == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    const auto path = modulePath();
    if (path.empty()) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const auto clsidKey = providerClsidKey();
    const auto inprocKey = clsidKey + L"\\InprocServer32";
    if (!setRegistryValue(HKEY_CURRENT_USER, clsidKey, L"", L"JarkViewer Thumbnail Provider") ||
        !setRegistryValue(HKEY_CURRENT_USER, inprocKey, L"", path) ||
        !setRegistryValue(HKEY_CURRENT_USER, inprocKey, L"ThreadingModel", L"Apartment")) {
        return E_FAIL;
    }

    return S_OK;
}

STDAPI DllUnregisterServer() {
    const auto result = SHDeleteKeyW(HKEY_CURRENT_USER, providerClsidKey().c_str());
    return (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) ?
        S_OK : HRESULT_FROM_WIN32(result);
}
