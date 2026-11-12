// Frame Generation Preset C on DLSS-G 310.9.16: compiles the production dlssg_preset.cpp and ngx_mfg_gate.h
// against providers mapped with DONT_RESOLVE_DLL_REFERENCES (no DllMain, no imports, no provider code runs).
// entry_detour is stubbed so nothing is installed.
#include "dlssg_preset.cpp"
#include "ngx_mfg_gate.h"
#include "data_image.h"
#include <set>
#include "../ampere_runtime/entry_detour_stub.h"

// rel32 calls (E8) and jumps (E9) in the executable sections that land on target, and the primary
// .pdata function (chained unwind entries followed) holding each call.
struct References
{
    std::set<uint32_t> callers;
    unsigned calls = 0;
    unsigned jumps = 0;
    unsigned unresolved = 0;
};

uint32_t PrimaryFunction(uintptr_t base, uintptr_t address)
{
    DWORD64 owner = 0;
    const auto* function = RtlLookupFunctionEntry(address, &owner, nullptr);
    for (unsigned depth = 0; function && owner == base && depth != 32; ++depth)
    {
        const auto* unwind = reinterpret_cast<const uint8_t*>(base + function->UnwindData);
        if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO)) return function->BeginAddress;
        const size_t codes = (static_cast<size_t>(unwind[2]) + 1) & ~size_t{1};
        function = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + 4 + codes * 2);
    }
    return 0;
}

References DirectReferences(HMODULE module, const void* target)
{
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    References references{};
    for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& section = sections[i];
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const auto* start = reinterpret_cast<const uint8_t*>(base + section.VirtualAddress);
        for (size_t offset = 0; offset + 5 <= section.Misc.VirtualSize; ++offset)
        {
            if (start[offset] != 0xe8 && start[offset] != 0xe9) continue;
            int32_t displacement = 0;
            memcpy(&displacement, start + offset + 1, 4);
            if (reinterpret_cast<uintptr_t>(start + offset + 5) + displacement != reinterpret_cast<uintptr_t>(target))
                continue;
            if (start[offset] == 0xe9)
            {
                ++references.jumps;
                continue;
            }
            ++references.calls;
            const uint32_t caller = PrimaryFunction(base, reinterpret_cast<uintptr_t>(start + offset));
            if (caller) references.callers.insert(caller);
            else ++references.unresolved;
        }
    }
    return references;
}

void RefuseMutation(HMODULE module, void* reader, uint8_t* address, const char* reason)
{
    const auto original = *address;
    DWORD protection = 0, replaced = 0;
    Require(VirtualProtect(address, 1, PAGE_EXECUTE_WRITECOPY, &protection) != 0, "negative fixture page made copy-on-write");
    *address ^= 1;
    FlushInstructionCache(GetCurrentProcess(), address, 1);
    const bool refused = !dlssg_preset::config::Discover(module, reader).slot;
    *address = original;
    FlushInstructionCache(GetCurrentProcess(), address, 1);
    Require(VirtualProtect(address, 1, protection, &replaced) != 0 && *address == original, "negative fixture byte and protection restored");
    Require(refused, reason);
}

int wmain(int argc, wchar_t** argv)
{
    using namespace dlssg_preset;
    namespace policy = dlssg_provider_policy;
    try
    {
        if (argc != 3) throw std::runtime_error("usage: audit 310.9.0.dll 310.9.16.dll");
        Require(IsValidSelection(kPresetC) && IsOverride(kPresetC) && !IsValidSelection(4), "Preset C is a valid selection");
        Setting setting{};
        setting.version = kSettingVersion;
        setting.id = kSettingId;
        Require(OverrideRead(kSettingId, kSettingNotFound, setting, kPresetC) && setting.current.dword == 3,
            "Preset C override supplies driver value 3");

        const HMODULE oldProvider = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        const HMODULE newProvider = LoadLibraryExW(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        Require(oldProvider && newProvider, "providers mapped without running their code");
        void* const oldReader = FindCachedReader(oldProvider);
        void* const newReader = FindCachedReader(newProvider);
        std::cout << std::hex << "reader_rva_310_9_0=0x" << reinterpret_cast<uintptr_t>(oldReader) - reinterpret_cast<uintptr_t>(oldProvider)
            << " reader_rva_310_9_16=0x" << reinterpret_cast<uintptr_t>(newReader) - reinterpret_cast<uintptr_t>(newProvider)
            << std::dec << '\n';
        Require(oldReader && reinterpret_cast<uintptr_t>(oldReader) - reinterpret_cast<uintptr_t>(oldProvider) == 0x13F00,
            "310.9.0 cached preset reader (one call site)");
        Require(newReader && reinterpret_cast<uintptr_t>(newReader) - reinterpret_cast<uintptr_t>(newProvider) == 0x14000,
            "310.9.16 cached preset reader (two call sites, one reader)");

        Require(policy::IsSupportedRetainedProvider(newProvider) && policy::IsSupportedRetainedProvider(oldProvider),
            "both providers eligible on Ada");
        Require(policy::IsSupportedRetainedAmpereProvider(newProvider) && policy::IsSupportedRetainedAmpereProvider(oldProvider),
            "RTX 30 admits 310.9.16 and 310.9.0");
        Require(policy::IsSupportedAmpereProvider(newProvider, argv[2]) && policy::IsSupportedAmpereProvider(oldProvider, argv[1]),
            "RTX 30 path check matches");
        Require(EffectiveSelection(kPresetC, newProvider) == kPresetC && EffectiveSelection(kPresetC, oldProvider) == kGameOrDriver,
            "the RTX 30 preset hook applies the same Preset C rule");
        Require(PresetCSupported(newProvider) && !PresetCSupported(oldProvider), "Preset C support follows the provider version");
        Require(EffectivePreset(kPresetC, newProvider) == kPresetC && EffectivePreset(kPresetC, oldProvider) == kGameOrDriver,
            "Preset C reaches 310.9.16 only; elsewhere the game/driver value stays");
        Require(EffectivePreset(kPresetA, oldProvider) == kPresetA && EffectivePreset(kPresetB, newProvider) == kPresetB
            && EffectivePreset(kGameOrDriver, newProvider) == kGameOrDriver, "A, B and game/driver unchanged");
        Require(ReadSnapshot(newProvider).presetCAvailable && !ReadSnapshot(oldProvider).presetCAvailable,
            "status reports Preset C availability per provider");

        // Live preset switching. Both builds are inspected per-Create readers: every call of their cached
        // reader (it serves several DRS keys) sits in the two EndpointCore::Create helpers (310.9.0 0x34100/0x37B20,
        // 310.9.16 0x344F0/0x38020), and no jump reaches it.
        const auto oldConfig = config::Discover(oldProvider, oldReader);
        const auto newConfig = config::Discover(newProvider, newReader);
        std::cout << std::hex << "config_slots=" << reinterpret_cast<uintptr_t>(oldConfig.slot) - reinterpret_cast<uintptr_t>(oldProvider)
            << ',' << reinterpret_cast<uintptr_t>(newConfig.slot) - reinterpret_cast<uintptr_t>(newProvider) << std::dec << '\n';
        Require(oldConfig.slot && reinterpret_cast<uintptr_t>(oldConfig.slot) - reinterpret_cast<uintptr_t>(oldProvider) == 0x6689e0,
            "310.9.0 configuration discovered from code and RTTI");
        Require(newConfig.slot && reinterpret_cast<uintptr_t>(newConfig.slot) - reinterpret_cast<uintptr_t>(newProvider) == 0x66aa20,
            "310.9.16 configuration discovered from code and RTTI");
        Require(oldConfig.configurationReturn && newConfig.configurationReturn
            && oldConfig.apply.begin != oldConfig.report.begin && newConfig.apply.begin != newConfig.report.begin,
            "configuration reads are distinguished from report reads");
        Require(!config::Discover(nullptr, nullptr).slot && !config::Discover(newProvider, oldReader).slot,
            "absent or mismatched reader fails closed");
        RefuseMutation(newProvider, newReader, reinterpret_cast<uint8_t*>(newConfig.configurationReturn - 9),
            "missing configuration preset-key call refuses live switching");
        uintptr_t locator = 0;
        provider_image::SafeRead(newConfig.controlVtable - 8, locator);
        uint32_t typeRva = 0;
        provider_image::SafeRead(locator + 12, typeRva);
        RefuseMutation(newProvider, newReader, reinterpret_cast<uint8_t*>(newProvider) + typeRva + 16 + 25,
            "wrong EndpointConfiguration RTTI refuses live switching");
        Require(config::Discover(newProvider, newReader).slot == newConfig.slot,
            "restored provider remains structurally discoverable");
        const auto oldReferences = DirectReferences(oldProvider, oldReader);
        const auto newReferences = DirectReferences(newProvider, newReader);
        for (const auto* references : {&oldReferences, &newReferences})
        {
            std::cout << std::hex << "reader_calls=" << std::dec << references->calls << " callers=";
            for (const uint32_t caller : references->callers) std::cout << std::hex << "0x" << caller << ' ';
            std::cout << std::dec << "jumps=" << references->jumps << " unresolved=" << references->unresolved << '\n';
        }
        Require(oldReferences.callers == std::set<uint32_t>{0x34100, 0x37B20} && oldReferences.calls >= 2
            && !oldReferences.jumps && !oldReferences.unresolved,
            "310.9.0 reader calls come only from the two inspected EndpointCore::Create helpers");
        Require(newReferences.callers == std::set<uint32_t>{0x344F0, 0x38020} && newReferences.calls >= 2
            && !newReferences.jumps && !newReferences.unresolved,
            "310.9.16 reader calls come only from the two inspected EndpointCore::Create helpers");
        // No cached reader hook is installed here (entry_detour is stubbed) and nothing read the preset yet.
        Require(!ProviderSupportsLiveSwitch(newProvider) && !ReadProvidersSupportLiveSwitch(),
            "live switching needs a hooked per-Create reader that has been read");

        const auto oldGate = ngx_mfg_gate::Find(oldProvider);
        const auto newGate = ngx_mfg_gate::Find(newProvider);
        std::cout << std::hex << "mfg_gate_rva_310_9_0=0x" << reinterpret_cast<uintptr_t>(oldGate.match) - reinterpret_cast<uintptr_t>(oldProvider)
            << " mfg_gate_rva_310_9_16=0x" << reinterpret_cast<uintptr_t>(newGate.match) - reinterpret_cast<uintptr_t>(newProvider)
            << std::dec << '\n';
        Require(oldGate.match && newGate.match, "RTX 40 MFG count gate found once in both providers");
        std::cout << "PRESET_C_310916_AUDIT_PASSED\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "AUDIT_FAILED: " << error.what() << '\n';
        return 1;
    }
}
