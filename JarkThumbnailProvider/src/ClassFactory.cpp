#include "ClassFactory.h"

#include "ThumbnailProvider.h"

#include <new>

extern long g_moduleRefCount;

ClassFactory::ClassFactory() noexcept {
    InterlockedIncrement(&g_moduleRefCount);
}

ClassFactory::~ClassFactory() {
    InterlockedDecrement(&g_moduleRefCount);
}

IFACEMETHODIMP ClassFactory::QueryInterface(REFIID riid, void** object) noexcept {
    if (!object) {
        return E_POINTER;
    }

    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
        *object = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) ClassFactory::AddRef() noexcept {
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

IFACEMETHODIMP_(ULONG) ClassFactory::Release() noexcept {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
    if (count == 0) {
        delete this;
    }
    return count;
}

IFACEMETHODIMP ClassFactory::CreateInstance(IUnknown* outer, REFIID riid, void** object) noexcept {
    if (!object) {
        return E_POINTER;
    }

    *object = nullptr;
    if (outer) {
        return CLASS_E_NOAGGREGATION;
    }

    ThumbnailProvider* provider = new (std::nothrow) ThumbnailProvider();
    if (!provider) {
        return E_OUTOFMEMORY;
    }

    HRESULT hr = provider->QueryInterface(riid, object);
    provider->Release();
    return hr;
}

IFACEMETHODIMP ClassFactory::LockServer(BOOL lock) noexcept {
    if (lock) {
        InterlockedIncrement(&g_moduleRefCount);
    }
    else {
        InterlockedDecrement(&g_moduleRefCount);
    }

    return S_OK;
}
