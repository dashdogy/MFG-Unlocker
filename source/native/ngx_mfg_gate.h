#pragma once
#include "dlssg_provider_policy.h"
#include "protected_pointer.h"
#include <algorithm>
#include <array>
#include <mutex>

namespace ngx_mfg_gate {
// This is the provider's count/index validator, not the shared NGX runtime's
// feature-support query. Retain all count, index and profile-limit checks.
inline constexpr std::array<uint8_t, 13> kPattern{
    0x84,0xd2,0x0f,0x84,0x03,0x01,0x00,0x00,0xbe,0x05,0x00,0x00,0x00};
inline constexpr std::array<uint8_t, 2> kOriginal{0x0f,0x84};
// Jump over the remaining four bytes of the old near-branch displacement.
// The aligned two-byte CAS avoids the old six-byte executable memcpy.
inline constexpr std::array<uint8_t, 2> kReplacement{0xeb,0x04};
struct Result { bool candidate = false; bool patched = false; uint8_t* match = nullptr; };
struct Site { uint8_t* match = nullptr; uintptr_t functionBegin = 0; };

inline bool Matches(const uint8_t* p) noexcept {
    return !memcmp(p, kPattern.data(), 2)
        && (!memcmp(p+2, kOriginal.data(), 2) || !memcmp(p+2, kReplacement.data(), 2))
        && !memcmp(p+4, kPattern.data()+4, kPattern.size()-4);
}
inline Site Find(HMODULE module) noexcept {
    if (!module) return {};
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->FileHeader.NumberOfSections > 96) return {};
    const size_t imageBytes = nt->OptionalHeader.SizeOfImage;
    auto valid = [imageBytes](size_t rva, size_t size) { return rva < imageBytes && size <= imageBytes-rva; };
    Site found{};
    size_t matches = 0;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i=0; i<nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) || section->VirtualAddress >= imageBytes) continue;
        const size_t size = (std::min)(imageBytes-section->VirtualAddress,
            size_t((std::max)(section->Misc.VirtualSize,section->SizeOfRawData)));
        const auto* begin = base+section->VirtualAddress;
        for (size_t offset=0; offset+kPattern.size()<=size; ++offset) {
            if (!Matches(begin+offset)) continue;
            if (++matches != 1) return {}; // Ambiguity never selects the first match.
            auto* p = const_cast<uint8_t*>(begin+offset);
            const auto rva = size_t(p-base);
            int32_t displacement = 0; memcpy(&displacement,p+4,sizeof(displacement));
            const size_t target = rva+8+displacement;
            constexpr uint8_t countOne[]{0x41,0x83,0xf8,0x01};
            DWORD64 imageBase = 0;
            const auto* function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(p),&imageBase,nullptr);
            if ((reinterpret_cast<uintptr_t>(p+2)&1) || !valid(target,sizeof(countOne))
                || memcmp(base+target,countOne,sizeof(countOne))
                || !function || imageBase != reinterpret_cast<DWORD64>(module)
                || function->BeginAddress > rva || function->EndAddress <= target+sizeof(countOne)
                || !valid(function->BeginAddress,function->EndAddress-function->BeginAddress)
                || !protected_pointer::ProtectionMatches(reinterpret_cast<uintptr_t>(p),PAGE_EXECUTE_READ,kPattern.size())
                || !protected_pointer::ProtectionMatches(reinterpret_cast<uintptr_t>(base+target),PAGE_EXECUTE_READ,sizeof(countOne))) return {};
            found = {p,reinterpret_cast<uintptr_t>(base+function->BeginAddress)};
        }
    }
    return matches == 1 ? found : Site{};
}

struct Publication { HMODULE module=nullptr; uint8_t* match=nullptr; bool ready=false; };
inline std::mutex gMutex;
inline std::array<Publication,64> gPublications{};

inline bool Ready(HMODULE module) noexcept {
    if (!module) return false;
    std::lock_guard lock(gMutex);
    for (const auto& item:gPublications) if (item.module==module)
        return item.ready && Matches(item.match)
            && !memcmp(item.match+2,kReplacement.data(),kReplacement.size())
            && protected_pointer::ProtectionMatches(reinterpret_cast<uintptr_t>(item.match),PAGE_EXECUTE_READ,kPattern.size());
    return false;
}
// GitHub issue 149 (AION 2, DLSS-G 310.6.0.0): a supported provider with no count/index check to restore. 310.1 to
// 310.6 have none (310.7 and later have exactly one), so RTX 40 cannot run them above 2X.
inline bool Missing(HMODULE module) noexcept {
    return module && dlssg_provider_policy::IsSupportedRetainedProvider(module) && !Find(module).match;
}

inline Result Patch(HMODULE module, bool verifiedAdaAdapter) noexcept {
    if (!verifiedAdaAdapter || !dlssg_provider_policy::IsSupportedRetainedProvider(module)) return {};
    // Retention establishes module lifetime before scanning or publishing. No
    // filename, sibling version, cached base address or UI maximum admits it.
    std::lock_guard lock(gMutex);
    const Site site=Find(module);
    if (!site.match) return {};
    for (const auto& item:gPublications) if(item.module==module)
        return {true,item.ready && item.match==site.match
            && !memcmp(site.match+2,kReplacement.data(),2),site.match};
    // A foreign edit must not be adopted as this module's successful patch.
    if (memcmp(site.match+2,kOriginal.data(),2)) return {true,false,site.match};
    auto item=std::find_if(gPublications.begin(),gPublications.end(),[](const auto& p){return !p.module;});
    if(item==gPublications.end()) return {true,false,site.match};
    *item={module,site.match,false}; // Ambiguous publication stays permanently closed.
    const auto result=protected_pointer::ReplaceProtectedBytes(
        reinterpret_cast<uintptr_t>(site.match+2),kOriginal.data(),kReplacement.data(),2,
        PAGE_EXECUTE_READ,&VirtualProtect,&FlushInstructionCache,PAGE_EXECUTE_WRITECOPY);
    item->ready=result.disposition==protected_pointer::PublishDisposition::ePublishedRestored;
    return {true,item->ready,site.match};
}
}
