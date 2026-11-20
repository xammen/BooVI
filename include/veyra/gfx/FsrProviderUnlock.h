#pragma once
#include <Windows.h>
#include <d3d12.h>

namespace veyra::gfx {
// AMD's FSR 4 providers answer IsSupported with false on every non-AMD adapter,
// so the SDK hides them. While this object lives, the named provider class of a
// loaded AMD FidelityFX module reports support, as OptiScaler does for its FSR 4
// INT8 option; its destructor puts AMD's check back. Nothing changes on an AMD
// adapter. Create the context while it lives, on one thread.
class FsrProviderUnlock {
public:
    FsrProviderUnlock(ID3D12Device* device, HMODULE module, const char* providerClass);
    ~FsrProviderUnlock();
    FsrProviderUnlock(const FsrProviderUnlock&) = delete;
    FsrProviderUnlock& operator=(const FsrProviderUnlock&) = delete;
    bool active() const { return slot_ != nullptr; }
private:
    void** slot_ = nullptr;
    void* original_ = nullptr;
};
}
