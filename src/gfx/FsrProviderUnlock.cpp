#include "veyra/gfx/FsrProviderUnlock.h"

#include "veyra/Log.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::gfx {
namespace {
// x64 MSVC RTTI: the type descriptor holds the mangled class name, the complete
// object locator points at it by RVA, and the vtable slot before the first
// virtual function points at the locator.
struct TypeDescriptor { void* vtable; void* spare; char name[1]; };
struct CompleteObjectLocator { uint32_t signature, offset, constructorOffset, typeDescriptor, hierarchy, self; };
// ffxProvider vtable of FidelityFX SDK 2.x: [0] destructor, [1] version id, [2] IsSupported(device).
constexpr size_t kIsSupportedSlot = 2;

std::vector<std::span<const uint8_t>> dataSections(HMODULE module, bool writable) {
    std::vector<std::span<const uint8_t>> out;
    const auto base = reinterpret_cast<const uint8_t*>(module);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        if (bool(section->Characteristics & IMAGE_SCN_MEM_WRITE) != writable) continue;
        out.emplace_back(base + section->VirtualAddress, section->Misc.VirtualSize);
    }
    return out;
}

void** findVtable(HMODULE module, std::string_view className) {
    const auto mangled = std::format(".?AV{}@@", className);
    const std::string_view needle(mangled.c_str(), mangled.size() + 1);
    const TypeDescriptor* type = nullptr;
    for (auto section : dataSections(module, true)) {
        const std::string_view data(reinterpret_cast<const char*>(section.data()), section.size());
        if (const auto at = data.find(needle); at != data.npos) {
            type = reinterpret_cast<const TypeDescriptor*>(data.data() + at - offsetof(TypeDescriptor, name));
            break;
        }
    }
    if (!type) return nullptr;
    const auto base = reinterpret_cast<const uint8_t*>(module);
    const auto typeRva = uint32_t(reinterpret_cast<const uint8_t*>(type) - base);
    const CompleteObjectLocator* locator = nullptr;
    for (auto section : dataSections(module, false)) {
        for (size_t at = 0; !locator && at + sizeof(CompleteObjectLocator) <= section.size(); at += 4) {
            const auto candidate = reinterpret_cast<const CompleteObjectLocator*>(section.data() + at);
            if (candidate->signature == 1 && candidate->offset == 0 && candidate->typeDescriptor == typeRva &&
                candidate->self == uint32_t(section.data() + at - base)) locator = candidate;
        }
        if (locator) break;
    }
    if (!locator) return nullptr;
    for (auto section : dataSections(module, false)) {
        const auto entries = reinterpret_cast<const uintptr_t*>(section.data());
        for (size_t i = 0; i + 1 < section.size() / sizeof(uintptr_t); ++i)
            if (entries[i] == reinterpret_cast<uintptr_t>(locator)) return reinterpret_cast<void**>(const_cast<uintptr_t*>(&entries[i + 1]));
    }
    return nullptr;
}

bool reportSupported(void*, void*) { return true; }

uint32_t adapterVendor(ID3D12Device* device) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
        FAILED(adapter->GetDesc1(&desc))) return 0;
    return desc.VendorId;
}

bool writeSlot(void** slot, void* value) {
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(*slot), protection, &protection);
    return true;
}
}

FsrProviderUnlock::FsrProviderUnlock(ID3D12Device* device, HMODULE module, const char* providerClass) {
    if (!device || !module) return;
    const auto vendor = adapterVendor(device);
    if (vendor == 0x1002 || vendor == 0) return;
    void** vtable = findVtable(module, providerClass);
    if (!vtable) {
        log::warn("fsr-unlock", std::format("{} not found in this FidelityFX module", providerClass));
        return;
    }
    auto* slot = &vtable[kIsSupportedSlot];
    const auto original = *slot;
    if (!writeSlot(slot, reinterpret_cast<void*>(&reportSupported))) return;
    slot_ = slot; original_ = original;
    log::info("fsr-unlock", std::format("{} reports support on vendor=0x{:04X}", providerClass, vendor));
}

FsrProviderUnlock::~FsrProviderUnlock() {
    if (slot_) writeSlot(slot_, original_);
}
}
