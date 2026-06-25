#pragma once

#include <cstdint>
#include <shobjidl.h>
#include <thumbcache.h>
#include <vector>
#include <windows.h>

class ThumbnailProvider final : public IInitializeWithStream, public IThumbnailProvider {
public:
    ThumbnailProvider() noexcept;
    ~ThumbnailProvider();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) noexcept override;
    IFACEMETHODIMP_(ULONG) AddRef() noexcept override;
    IFACEMETHODIMP_(ULONG) Release() noexcept override;

    IFACEMETHODIMP Initialize(IStream* stream, DWORD grfMode) noexcept override;
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alphaType) noexcept override;

private:
    long m_refCount = 1;
    bool m_initialized = false;
    std::vector<uint8_t> m_data;
};
