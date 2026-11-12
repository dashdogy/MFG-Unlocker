// Exercises the real entry registry/MinHook installer on a provider mapped
// without DllMain/import resolution. No NVIDIA code, Init/Create/Evaluate or GPU
// work runs. All patched pages are private to this short-lived test process.
// --route: the RTX 30/20 translated route's own reader hook owns the
// reader, and dlssg_preset adopts it for the configuration reset.
#include "dlssg_preset.cpp"
#include <iostream>

namespace
{
bool RoutePreset(uint32_t, uint32_t*) noexcept { return false; } // Installed, never called.
}

int wmain(int argc, wchar_t** argv)
{
    const bool routeMode = argc == 3 && _wcsicmp(argv[2], L"--route") == 0;
    if (argc != 2 && !routeMode) { std::cerr << "usage: PresetHookInstallTests provider.dll [--route]\n"; return 2; }
    HMODULE module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module || !dlssg_provider_policy::IsSupportedRetainedProvider(module))
    { std::cerr << "provider mapping/identity failed\n"; return 2; }
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* message) {
        ++checks; failures += !ok;
        std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
    };
    entry_detour::Handle routeHook{};
    if (routeMode)
    {
        void* reader = dlssg_preset::FindPresetReader(module);
        if (!reader) { std::cerr << "provider has no cached preset reader\n"; return 2; }
        entry_detour::InstallOptions options{};
        options.generation = 7;
        options.allowRelocated = true;
        void* original = nullptr;
        check(entry_detour::Install(entry_detour::Kind::eAmperePreset, module, reader,
            reinterpret_cast<void*>(&RoutePreset), original, options, &routeHook) && routeHook,
            "the route's own reader hook is installed");
        check(!dlssg_preset::FindPresetReader(module),
            "the hooked reader no longer matches its prologue: adoption takes the route's target");
        check(!dlssg_preset::AdoptRouteReader(module, reader, 8, routeHook),
            "a generation other than the route's is not adopted");
        check(dlssg_preset::AdoptRouteReader(module, reader, 7, routeHook), "the route reader is adopted");
        check(!entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgCachedPresetRead, module).installed,
            "no second reader hook is installed");
    }
    else
        dlssg_preset::InstallCachedReader(module, 7);
    const auto snapshot = dlssg_preset::ReadSnapshot(module);
    check(snapshot.cachedReaderInstalled, "cached reader detour is current");
    check(snapshot.configurationResetAvailable && snapshot.configurationSlotRva,
        "configuration discovery and both endpoint detours are current");
    check(dlssg_preset::ProviderSupportsLiveSwitch(module), "the verified installed route admits live switching");
    for (const auto kind : {routeMode ? entry_detour::Kind::eAmperePreset : entry_detour::Kind::eDlssgCachedPresetRead,
        entry_detour::Kind::eDlssgEndpointCreate, entry_detour::Kind::eDlssgEndpointEvaluate})
    {
        const auto hook = entry_detour::ReadSnapshot(kind, module);
        std::cout << "kind=" << static_cast<unsigned>(kind) << " rva=0x" << std::hex << hook.targetRva
            << std::dec << " method=" << entry_detour::MethodName(hook.method)
            << " failure=" << entry_detour::FailureName(hook.failure) << '\n';
        check(hook.current && hook.original && hook.generation == 7 && hook.owner == module,
            "entry ownership, generation, trampoline and readback match");
    }
    if (routeMode)
        check(dlssg_preset::AdoptRouteReader(module, entry_detour::ReadSnapshot(routeHook).target, 7, routeHook),
            "repeated adoption preserves the installed route");
    else
        dlssg_preset::InstallCachedReader(module, 7);
    check(dlssg_preset::ReadSnapshot(module).configurationResetAvailable, "repeated discovery preserves the installed route");
    check(!snapshot.observedPresetValid && !snapshot.configurationReads && !snapshot.reportReads,
        "installing hooks alone supplies no configuration evidence");
    std::cout << "PRESET_HOOK_INSTALL" << (routeMode ? " route" : "") << " checks=" << checks
        << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
