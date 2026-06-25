#include "ThumbnailProvider.h"

#include "BitmapOut.h"
#include "ThumbDecoders.h"

#include <algorithm>
#include <array>
#include <limits>

extern long g_moduleRefCount;

namespace {
constexpr size_t kMaxStreamBytes = 512ULL * 1024ULL * 1024ULL;
}

ThumbnailProvider::ThumbnailProvider() noexcept {
    InterlockedIncrement(&g_moduleRefCount);
}

ThumbnailProvider::~ThumbnailProvider() {
    InterlockedDecrement(&g_moduleRefCount);
}

IFACEMETHODIMP ThumbnailProvider::QueryInterface(REFIID riid, void** object) noexcept {
    if (!object) {
        return E_POINTER;
    }

    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) {
        *object = static_cast<IInitializeWithStream*>(this);
    }
    else if (riid == IID_IThumbnailProvider) {
        *object = static_cast<IThumbnailProvider*>(this);
    }
    else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ThumbnailProvider::AddRef() noexcept {
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

IFACEMETHODIMP_(ULONG) ThumbnailProvider::Release() noexcept {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
    if (count == 0) {
        delete this;
    }
    return count;
}

IFACEMETHODIMP ThumbnailProvider::Initialize(IStream* stream, DWORD) noexcept {
    if (!stream) {
        return E_POINTER;
    }

    if (m_initialized) {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    }

    try {
        std::array<uint8_t, 64 * 1024> buffer{};
        for (;;) {
            ULONG bytesRead = 0;
            HRESULT hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &bytesRead);
            if (FAILED(hr)) {
                return hr;
            }

            if (bytesRead == 0) {
                break;
            }

            if (m_data.size() > kMaxStreamBytes - bytesRead) {
                m_data.clear();
                return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            }

            m_data.insert(m_data.end(), buffer.begin(), buffer.begin() + bytesRead);
        }
    }
    catch (...) {
        m_data.clear();
        return E_OUTOFMEMORY;
    }

    m_initialized = true;
    return m_data.empty() ? E_FAIL : S_OK;
}

IFACEMETHODIMP ThumbnailProvider::GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alphaType) noexcept {
    if (!bitmap || !alphaType) {
        return E_POINTER;
    }

    *bitmap = nullptr;
    *alphaType = WTSAT_UNKNOWN;

    if (!m_initialized || m_data.empty() || cx == 0) {
        return E_FAIL;
    }

    JarkThumbnail::ThumbBitmap decoded;
    if (!JarkThumbnail::decodeThumbnail(m_data, cx, decoded) || decoded.empty()) {
        return E_FAIL;
    }

    return JarkThumbnail::createThumbnailBitmap(decoded, cx, bitmap, alphaType);
}
