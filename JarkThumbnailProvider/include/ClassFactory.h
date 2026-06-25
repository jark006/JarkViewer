#pragma once

#include <unknwn.h>
#include <windows.h>

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() noexcept;
    ~ClassFactory();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) noexcept override;
    IFACEMETHODIMP_(ULONG) AddRef() noexcept override;
    IFACEMETHODIMP_(ULONG) Release() noexcept override;

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) noexcept override;
    IFACEMETHODIMP LockServer(BOOL lock) noexcept override;

private:
    long m_refCount = 1;
};
