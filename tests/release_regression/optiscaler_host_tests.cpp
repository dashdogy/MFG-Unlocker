// OptiScaler mode against the exact release DLL. The OptiScaler stand-in
// (OptiScalerHostFixture.dll copied to OptiScaler.dll) carries OptiScaler's
// version-resource identity and NGX-facade export shape. Modes:
//   standalone  RTXMFG alone: no OptiScaler mode, menu factories still wrapped.
//   proxy       OptiScaler mapped first (sibling proxy), then RTXMFG.
//   plugin      OptiScaler loads plugins\MFGUnlocker.asi from its DllMain and calls
//               InitializeASI under the loader lock.
//   late        RTXMFG first, OptiScaler mapped afterwards.
//   optin       proxy layout with "optiScalerUi":"rtxmfg" in the saved control.
//   hidden      proxy layout with "optiScalerUi":"optiscaler": no RTXMFG menu.
//   shared      proxy layout with "optiScalerUi":"shared": RTXMFG's menu offers the
//               DLSS presets and DLSS 5. The default without the key while
//               OptiScaler hands no plugin API, so proxy, plugin, late and the
//               Streamline modes run shared (their stand-in has no API).
//   forza       proxy layout without the key, run as forza_steamworks_release_final.exe
//               (run.py): a ForzaTech executable defaults to hidden.
//   forza-shared  the same with "optiScalerUi":"shared": the opt-in still applies.
//   impostor    a non-OptiScaler caller invokes InitializeASI.
//   fakenvapi   a game-folder nvapi64.dll that answers fakenvapi's interface.
//   streamline  proxy layout; OptiScaler's private Streamline
//               (OptiScaler\streamline\sl.interposer.dll) loads before the
//               game's interposer.
//   second-streamline  RTXMFG alone; a second interposer loads after the game's.
//   streamline-device  proxy layout, no game interposer: OptiScaler's private
//               interposer loads, its sl.dlss_g (a clamp stand-in) loads, and its
//               slSetD3DDevice gets a D3D12 device on the default adapter. On a
//               verified Ada adapter the wrapper clamp is patched before that
//               call reaches the instance (where slOnPluginStartup reads it).
//               Only an armed RTX 30 reaches the RTX 30 startup decision there.
//   streamline-warp  the same with a WARP device: nothing is selected or patched.
//   xefg-output, xefg-output-optin, xefg-output-hidden, xefg-output-factory
//               (with the XeSS pair): the menu on OptiScaler's XeFG
//               output when no factory route saw the game's swapchain
//               (RunXefgOutput).
//   menutab, menutab-plugin, menutab-shared, menutab-default, menutab-fork2: the stand-in also hands OptiScaler's
//               plugin API to the plugin it loads (OptiScalerMenuTabFixture.dll), as OptiScaler's loader does right
//               after InitializeASI. menutab-plugin (hidden): RTXMFG accepts it, registers its tab, the saved DLSS
//               presets and DLSS 5 apply, still no factory proxy, and the tab draws through the stand-in's widget
//               functions. menutab-shared (plugin layout): accepted, no tab, RTXMFG's own menu. menutab (proxy
//               layout): OptiScaler never calls OptiPluginInit, so hidden behaves as without the API. menutab-default
//               (plugin layout, no key): the API makes hidden the default. menutab-fork2 runs as menutab-plugin with
//               our fork's earlier version 2 table instead of upstream's merged one, which RTXMFG adapts.
//               Every case: OptiPluginInit is exported; impostor: it refuses an API table from outside OptiScaler.
// The Streamline modes check that RTXMFG routes and detours only the game's
// interposer (and only slSetD3DDevice of OptiScaler's private one) and that
// calls never cross instances.
// Every case writes a saved control with fixed/preset/V-Sync/Reflex/XeSS
// overrides and checks the effective values RTXMFG reports and that the saved
// file stays byte-identical.
#include "single_module_status.h"
#include "optiscaler_plugin.h"
#include "ui_status_json.h"
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

namespace
{
int gFailures = 0;
void Check(bool passed, const char* text)
{
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    if (!passed) ++gFailures;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

bool Has(const std::string& json, const char* key, const char* value)
{
    return json.find(std::string("\"") + key + "\":" + value) != std::string::npos;
}

// Status JSON is republished by the worker; wait until a predicate holds.
template<class Predicate>
std::string WaitStatus(const std::filesystem::path& path, Predicate ready, unsigned timeoutMs = 8000)
{
    std::string text;
    for (unsigned waited = 0; waited <= timeoutMs; waited += 25)
    {
        text = ReadText(path);
        if (ui_status_json::CompleteObject(text) && ready(text)) return text;
        Sleep(25);
    }
    return text;
}

MfgSingleModuleStatus Query(HMODULE module)
{
    MfgSingleModuleStatus status{};
    status.size = sizeof(status);
    using QueryFn = BOOL(WINAPI*)(MfgSingleModuleStatus*);
    if (auto query = reinterpret_cast<QueryFn>(GetProcAddress(module, "MfgUnlockSingleModuleQuery")))
        query(&status);
    return status;
}

// Same identity as run.py's runtime_log(): FNV-1a 64 of the lower-cased
// executable path in UTF-16.
std::filesystem::path RuntimeLog()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    uint64_t identity = 14695981039346656037ull;
    for (const wchar_t* p = path; *p; ++p)
    {
        wchar_t c = *p == L'/' ? L'\\' : *p;
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
        identity = (identity ^ static_cast<uint16_t>(c)) * 1099511628211ull;
    }
    std::wstring stem = std::filesystem::path(path).stem().wstring().substr(0, 40);
    for (auto& c : stem)
    {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
        if (!(c < 128 && (iswalnum(c) || c == L'-' || c == L'_'))) c = L'_';
    }
    wchar_t name[128]{};
    swprintf_s(name, L"RTXMFG-%s-%016llX.log", stem.c_str(), static_cast<unsigned long long>(identity));
    return std::filesystem::path(path).parent_path() / name;
}

// A factory requested through this executable's GetProcAddress reaches
// RTXMFG's menu gateway only when RTXMFG owns the UI.
uint64_t FactoryWrappersAfterCreate(HMODULE candidate)
{
    using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = dxgi ? reinterpret_cast<CreateFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (create) create(IID_PPV_ARGS(&factory));
    return Query(candidate).factoryWrappersCreated;
}

bool Inside(HMODULE module, const void* address)
{
    HMODULE owner = nullptr;
    return address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &owner) && owner == module;
}

// Export RVA from a mapped image's export table (not through GetProcAddress).
DWORD ExportRva(const unsigned char* base, const char* name)
{
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i)
        if (!std::strcmp(reinterpret_cast<const char*>(base + names[i]), name)) return functions[ordinals[i]];
    return 0;
}

// The loaded entry's first bytes equal the file's bytes at the same RVA.
bool EntryUntouched(HMODULE module, const std::filesystem::path& file, const char* name)
{
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const DWORD rva = ExportRva(base, name);
    std::ifstream stream(file, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(stream), {}};
    if (!rva || bytes.size() < 0x400) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(bytes.data() + reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data())->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        if (rva < section->VirtualAddress || rva >= section->VirtualAddress + section->SizeOfRawData) continue;
        const size_t offset = section->PointerToRawData + (rva - section->VirtualAddress);
        return offset + 16 <= bytes.size() && std::memcmp(base + rva, bytes.data() + offset, 16) == 0;
    }
    return false;
}

// The game's interposer is sl.interposer.dll beside the executable; the other
// one is OptiScaler's private Streamline or any interposer loaded after it.
void CheckStreamlineInstances(const std::filesystem::path& here, const std::filesystem::path& other,
    bool otherFirst, bool optiScalerPrivate)
{
    using SetDeviceFn = int (*)(void*);
    using CountFn = unsigned(WINAPI*)();
    HMODULE second = otherFirst ? LoadLibraryW(other.c_str()) : nullptr;
    HMODULE game = LoadLibraryW((here / L"sl.interposer.dll").c_str());
    if (!otherFirst) second = LoadLibraryW(other.c_str());
    Check(game && second && game != second, "game and second Streamline interposers load as separate instances");
    if (!game || !second) return;
    auto gameSet = reinterpret_cast<SetDeviceFn>(GetProcAddress(game, "slSetD3DDevice"));
    Check(gameSet && !Inside(game, reinterpret_cast<void*>(gameSet)), "the game's slSetD3DDevice is routed through RTXMFG");
    for (const char* name : {"slInit", "slSetD3DDevice", "slUpgradeInterface", "slGetFeatureFunction"})
    {
        const std::string text = std::string("second instance resolves ") + name + " to its own export";
        Check(Inside(second, reinterpret_cast<void*>(GetProcAddress(second, name))), text.c_str());
    }
    Sleep(1500); // background module inspection
    for (const char* name : {"slInit", "slUpgradeInterface"})
    {
        const std::string text = std::string("second instance ") + name + " entry bytes untouched";
        Check(EntryUntouched(second, other, name), text.c_str());
    }
    // OptiScaler's private instance gets its own slSetD3DDevice entry (Ada
    // maximum before its plug-in startup); any other instance stays untouched.
    Check(EntryUntouched(second, other, "slSetD3DDevice") != optiScalerPrivate, optiScalerPrivate
        ? "OptiScaler's private slSetD3DDevice entry is hooked"
        : "second instance slSetD3DDevice entry bytes untouched");
    auto secondSet = reinterpret_cast<SetDeviceFn>(reinterpret_cast<const unsigned char*>(second)
        + ExportRva(reinterpret_cast<const unsigned char*>(second), "slSetD3DDevice"));
    auto gameCount = reinterpret_cast<CountFn>(GetProcAddress(game, "FixtureDeviceSets"));
    auto secondCount = reinterpret_cast<CountFn>(GetProcAddress(second, "FixtureDeviceSets"));
    if (!gameSet || !gameCount || !secondCount) return;
    gameSet(nullptr);
    Check(gameCount() == 1 && secondCount() == 0, "the game's slSetD3DDevice reaches only the game's instance");
    secondSet(nullptr);
    Check(gameCount() == 1 && secondCount() == 1, "the second instance's slSetD3DDevice stays in that instance");
    gameSet(nullptr);
    Check(gameCount() == 2 && secondCount() == 1, "the game's calls still reach the game's instance afterwards");
}

std::string WaitLog(const char* text, unsigned timeoutMs = 3000)
{
    std::string log;
    for (unsigned waited = 0; waited <= timeoutMs; waited += 25)
    {
        log = ReadText(RuntimeLog());
        if (log.find(text) != std::string::npos) break;
        Sleep(25);
    }
    return log;
}

// OptiScaler's DLSS-G output with no game Streamline: OptiScaler loads its
// private interposer, slInit loads sl.dlss_g, and slSetD3DDevice follows at
// once on the same thread, before discovery runs.
void CheckPrivateStartupOrder(const std::filesystem::path& here, const std::filesystem::path& status, bool warp)
{
    const auto folder = here / L"OptiScaler" / L"streamline";
    std::filesystem::create_directories(folder);
    const auto interposerPath = folder / L"sl.interposer.dll";
    if (!CopyFileW((here / L"hotpatch" / L"sl.interposer.dll").c_str(), interposerPath.c_str(), FALSE)
        || !CopyFileW((here / L"MaximumClampFixture.dll").c_str(), (folder / L"sl.dlss_g.dll").c_str(), FALSE))
    {
        Check(false, "private Streamline fixtures copied");
        return;
    }
    HMODULE interposer = LoadLibraryW(interposerPath.c_str());
    Check(interposer != nullptr, "private interposer loads");
    if (!interposer) return;
    Check(!EntryUntouched(interposer, interposerPath, "slSetD3DDevice"),
        "private slSetD3DDevice is hooked when its LoadLibrary returns");
    HMODULE wrapper = LoadLibraryW((folder / L"sl.dlss_g.dll").c_str());
    using ReadFn = int(WINAPI*)();
    auto clampPatched = wrapper ? reinterpret_cast<ReadFn>(GetProcAddress(wrapper, "FixtureClampPatched")) : nullptr;
    Check(clampPatched && clampPatched() == 0, "wrapper clamp native before a device is seen");

    using DeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto createDevice = d3d12 ? reinterpret_cast<DeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    auto createFactory = dxgi ? reinterpret_cast<FactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (warp && createFactory && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))))
        factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (createDevice && (!warp || adapter))
        createDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    Check(device != nullptr, warp ? "WARP D3D12 device created" : "default-adapter D3D12 device created");
    if (!device || !clampPatched) return;

    using ProbeFn = void(WINAPI*)(HMODULE);
    using CountFn = unsigned(WINAPI*)();
    using SetDeviceFn = int (*)(void*);
    auto probe = reinterpret_cast<ProbeFn>(GetProcAddress(interposer, "FixtureProbeWrapper"));
    auto atSet = reinterpret_cast<ReadFn>(GetProcAddress(interposer, "FixtureWrapperAtDeviceSet"));
    auto count = reinterpret_cast<CountFn>(GetProcAddress(interposer, "FixtureDeviceSets"));
    const auto* base = reinterpret_cast<const unsigned char*>(interposer);
    auto set = reinterpret_cast<SetDeviceFn>(base + ExportRva(base, "slSetD3DDevice"));
    Check(probe && atSet && count, "interposer fixture probe exports");
    if (!probe || !atSet || !count) return;
    probe(wrapper);
    // OptiScaler resolves its private exports with the loader's raw lookup.
    set(device.Get());
    Check(count() == 1, "the private slSetD3DDevice reaches the private instance once");

    const std::string log = WaitLog("OptiScaler private Streamline slSetD3DDevice: call=1 ");
    const bool verifiedAda = log.find("adapter verification: luid=") != std::string::npos
        && log.find("verified=1") != std::string::npos;
    // Only an armed RTX 30 reaches the private RTX 30 startup decision.
    if (log.find("OptiScaler private Streamline slSetD3DDevice: call=1 ada=0 ampere=1") != std::string::npos)
        Check(log.find("OptiScaler private Streamline RTX 30 startup: call=1 decision=") != std::string::npos,
            "RTX 30: the private hook decides the RTX 30 startup");
    else
        Check(log.find("OptiScaler private Streamline RTX 30 startup") == std::string::npos,
            "not RTX 30: the private hook takes no RTX 30 startup");
    if (warp)
    {
        Check(log.find("OptiScaler private Streamline slSetD3DDevice: call=1 ada=0") != std::string::npos,
            "WARP: the private hook selects nothing");
        Check(atSet() == 0 && clampPatched() == 0, "WARP: the wrapper clamp stays native");
        Check(Has(ReadText(status), "gpuFamily", "0"), "WARP: no GPU family selected");
    }
    else if (verifiedAda)
    {
        Check(log.find("OptiScaler private Streamline slSetD3DDevice: call=1 ada=1") != std::string::npos,
            "Ada: the private hook selects the verified adapter");
        Check(atSet() == 1, "Ada: the wrapper clamp is patched before the private instance's slSetD3DDevice runs");
        Check(Has(WaitStatus(status, [](const std::string& text) { return Has(text, "gpuFamily", "1"); }),
            "gpuFamily", "1"), "Ada: status reports the Ada family");
    }
    else
    {
        Check(log.find("OptiScaler private Streamline slSetD3DDevice: call=1 ") != std::string::npos,
            "the private hook ran");
        std::printf("SKIP default adapter is not a verified Ada GPU; patch-before-startup not applicable\n");
    }
    Sleep(1500); // background module inspection
    Check(EntryUntouched(interposer, interposerPath, "slInit")
        && EntryUntouched(interposer, interposerPath, "slUpgradeInterface"),
        "private slInit and slUpgradeInterface entries stay untouched");
    Check(count() == 1, "no other route reached the private instance");
}

// The Trails in the Sky 2nd Chapter shape (DX11, OptiScaler XeFG output): OptiScaler loads its XeSS pair
// and resolves xefgSwapChainD3D12CreateContext in its DllMain, before plug-ins, and creates its context
// about 390 ms after start. Detouring that entry after the rest of the worker's startup (about 380 ms
// after RTXMFG's own load) leaves the context stock 2x. Both layouts: RTXMFG under a proxy name after
// OptiScaler's initialization, and as the plug-in OptiScaler loads right after the pair.
constexpr double kXefgRouteBoundMs = 150.0;

int RunXefgEarly(const std::filesystem::path& here, const std::filesystem::path& candidate,
    const std::filesystem::path& host, const std::filesystem::path& plugin, bool pluginLayout)
{
    SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_XEFG", (here / L"OptiScaler").c_str());
    if (pluginLayout) SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_PLUGIN", plugin.c_str());
    LARGE_INTEGER frequency{}, loaded{}, now{};
    QueryPerformanceFrequency(&frequency);
    HMODULE optiscaler = LoadLibraryW(host.c_str());
    HMODULE rtxmfg = pluginLayout ? GetModuleHandleW(plugin.c_str()) : LoadLibraryW(candidate.c_str());
    QueryPerformanceCounter(&loaded);
    SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_XEFG", nullptr);
    SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_PLUGIN", nullptr);

    using StateFn = void*(__cdecl*)(unsigned char*);
    using CallFn = int(__cdecl*)(void*, void**);
    auto state = optiscaler ? reinterpret_cast<StateFn>(GetProcAddress(optiscaler, "OptiFixtureXefgCreate")) : nullptr;
    auto callCreate = optiscaler ? reinterpret_cast<CallFn>(GetProcAddress(optiscaler, "OptiFixtureCallXefgCreate")) : nullptr;
    unsigned char resolved[16]{};
    auto* create = state ? static_cast<const unsigned char*>(state(resolved)) : nullptr;
    Check(create != nullptr && callCreate != nullptr,
        "OptiScaler stand-in loaded OptiScaler\\libxell.dll + libxess_fg.dll and resolved the creation in DllMain");
    Check(rtxmfg != nullptr, pluginLayout ? "stand-in loaded plugins\\MFGUnlocker.asi after the XeSS pair"
        : "candidate loaded after OptiScaler's initialization");
    if (!create || !callCreate || !rtxmfg) return 1;

    double routeMs = -1;
    for (;;)
    {
        QueryPerformanceCounter(&now);
        const double elapsed = double(now.QuadPart - loaded.QuadPart) * 1000.0 / double(frequency.QuadPart);
        if (std::memcmp(resolved, create, sizeof(resolved)) != 0) { routeMs = elapsed; break; }
        if (elapsed > 5000) break;
        Sleep(1);
    }
    std::printf("INFO XeFG creation entry detoured %.1f ms after RTXMFG's load (bound %.0f ms)\n", routeMs, kXefgRouteBoundMs);
    Check(routeMs >= 0, "RTXMFG detours the creation entry OptiScaler already resolved");
    Check(routeMs >= 0 && routeMs < kXefgRouteBoundMs, "the XeFG route is live well before OptiScaler's first creation");

    using DeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto createDevice = d3d12 ? reinterpret_cast<DeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    auto createFactory = dxgi ? reinterpret_cast<FactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (createFactory && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))))
        factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (createDevice && adapter)
        createDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    Check(device != nullptr, "WARP D3D12 device created");
    if (!device) return 1;
    void* context = nullptr;
    const int result = callCreate(device.Get(), &context);
    std::printf("INFO host XeFG creation on WARP returned %d\n", result);

    std::string log, line;
    for (unsigned waited = 0; waited <= 5000 && line.empty(); waited += 25)
    {
        log = ReadText(RuntimeLog());
        const size_t at = log.find("\"stage\":\"host_context_create\"");
        if (at != std::string::npos) line = log.substr(at, log.find('\n', at) - at);
        else Sleep(25);
    }
    Check(!line.empty(), "OptiScaler's creation reaches RTXMFG's host route (host_context_create)");
    Check(line.find("\"unlocked\":true") != std::string::npos && line.find("\"intel\":false") != std::string::npos,
        "the host unlock is applied before XeFG creates the context");
    Check(log.find("\"stage\":\"host_unlock\"") != std::string::npos && log.find("host_unlock_rejected") == std::string::npos,
        "host_unlock logged, not rejected");
    const std::string firstCall =
        "\"stage\":\"sdk_first_call\",\"api\":\"xefgSwapChainD3D12CreateContext\",\"caller\":\"OptiScaler.dll\"";
    const size_t first = log.find("\"stage\":\"sdk_first_call\"");
    Check(first != std::string::npos && log.compare(first, firstCall.size(), firstCall) == 0,
        "the first SDK call RTXMFG sees is OptiScaler's creation");
    std::printf("COMPLETE optiscaler xefg-%s failures=%d\n", pluginLayout ? "plugin" : "proxy", gFailures);
    return gFailures ? 1 : 0;
}

#if defined(OPTISCALER_FIXTURE_XEFG)
size_t Count(const std::string& text, const char* needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

std::string StageLine(const std::string& log, const char* stage)
{
    const std::string key = std::string("\"stage\":\"") + stage + "\"";
    const size_t at = log.rfind(key);
    return at == std::string::npos ? std::string() : log.substr(at, log.find('\n', at) - at);
}

LRESULT CALLBACK OutputWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(window, message, wParam, lParam);
}

void Pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}

// The Honkai Star Rail shape (Unity 2019.4, OptiScaler as dbghelp.dll with XeFG output): the game creates its
// swapchain through imports RTXMFG has not armed yet, so the factory route never sees it.
// Here OptiScaler's plug-in route builds its XeFG output before this executable makes any factory call: RTXMFG's menu
// attaches on XeFG's native output chain during OptiScaler's InitFromSwapChainDesc and draws on XeFG's output frames.
//   xefg-output          "shared" (the default): registered there; nothing drawn while no DLSS feature runs
//                        (the shared menu's rule).
//   xefg-output-optin    "rtxmfg": registered, and the menu's startup hint is drawn there.
//   xefg-output-hidden   "optiscaler": nothing attaches; XeFG presents unchanged.
//   xefg-output-factory  shared; OptiScaler's factory hook answers this executable's own creation, made through
//                        RTXMFG's factory proxy (the Subnautica 2 order): the factory route's one renderer only.
int RunXefgOutput(const std::filesystem::path& here, const std::filesystem::path& host,
    const std::filesystem::path& plugin, const std::filesystem::path& status, const std::wstring& variant)
{
    const bool optin = variant == L"xefg-output-optin", hidden = variant == L"xefg-output-hidden";
    const bool factoryRoute = variant == L"xefg-output-factory";
    SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_XEFG", (here / L"OptiScaler").c_str());
    SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_PLUGIN", plugin.c_str());
    HMODULE optiscaler = LoadLibraryW(host.c_str());
    HMODULE rtxmfg = GetModuleHandleW(plugin.c_str());
    SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_XEFG", nullptr);
    SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_PLUGIN", nullptr);

    using StateFn = void*(__cdecl*)(unsigned char*);
    using SwapchainFn = int(__cdecl*)(ID3D12CommandQueue*, HWND, UINT, UINT, void**, IDXGISwapChain1**);
    using DestroyFn = int(__cdecl*)(void*);
    using IntFn = int(__cdecl*)();
    using HookedFn = void*(__cdecl*)(int*, int*);
    auto state = optiscaler ? reinterpret_cast<StateFn>(GetProcAddress(optiscaler, "OptiFixtureXefgCreate")) : nullptr;
    auto makeOutput = optiscaler ? reinterpret_cast<SwapchainFn>(GetProcAddress(optiscaler, "OptiFixtureXefgSwapchain")) : nullptr;
    auto destroyOutput = optiscaler ? reinterpret_cast<DestroyFn>(GetProcAddress(optiscaler, "OptiFixtureXefgDestroy")) : nullptr;
    auto realFactory = optiscaler ? reinterpret_cast<IntFn>(GetProcAddress(optiscaler, "OptiFixtureRealFactory")) : nullptr;
    auto hookFactory = optiscaler ? reinterpret_cast<IntFn>(GetProcAddress(optiscaler, "OptiFixtureHookFactory")) : nullptr;
    auto hooked = optiscaler ? reinterpret_cast<HookedFn>(GetProcAddress(optiscaler, "OptiFixtureHookedXefg")) : nullptr;
    unsigned char resolved[16]{};
    auto* create = state ? static_cast<const unsigned char*>(state(resolved)) : nullptr;
    Check(create && makeOutput && destroyOutput && realFactory && hookFactory && hooked,
        "OptiScaler stand-in loaded its XeSS pair and builds OptiScaler's XeFG output");
    Check(rtxmfg != nullptr, "stand-in loaded plugins\\MFGUnlocker.asi and called InitializeASI");
    if (!create || !makeOutput || !destroyOutput || !realFactory || !hookFactory || !hooked || !rtxmfg) return 1;
    bool routed = false;
    for (unsigned waited = 0; waited <= 5000 && !routed; waited += 5)
    {
        routed = std::memcmp(resolved, create, sizeof(resolved)) != 0;
        if (!routed) Sleep(5);
    }
    Check(routed, "RTXMFG's XeFG route is live before OptiScaler's first creation");
    const char* owner = optin ? "\"rtxmfg\"" : hidden ? "\"optiscaler\"" : "\"shared\"";
    const std::string json = WaitStatus(status, [&](const std::string& text) {
        return Has(text, "optiScalerRoute", "\"plugin\"") && Has(text, "optiScalerUiOwner", owner); });
    Check(Has(json, "optiScalerUiOwner", owner), "the saved UI mode is in force");

    // The game's renderer: a D3D12 device on the default adapter, its direct queue and window.
    using DeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto createDevice = d3d12 ? reinterpret_cast<DeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (createDevice) createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (device) device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = OutputWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"RTXMFGXefgOutputCase";
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"XeFG output", WS_OVERLAPPEDWINDOW, 40, 40, 1296, 759,
        nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window) ShowWindow(window, SW_SHOWNA);
    Pump();
    Check(device && queue && window, "D3D12 device (default adapter), direct queue and window created");
    if (!device || !queue || !window) return 1;
    Check(realFactory() == 1, "the factory OptiScaler passes XeFG is DXGI's own object");
    const auto before = Query(rtxmfg);
    Check(before.swapchainWrappersCreated == 0, "no swapchain wrapped before OptiScaler's XeFG output");
    std::printf("INFO before: factoryGatewayCalls=%llu factoryWrappers=%llu\n",
        static_cast<unsigned long long>(before.factoryGatewayCalls), static_cast<unsigned long long>(before.factoryWrappersCreated));

    void* context = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
    if (factoryRoute)
    {
        Check(hookFactory() == 0, "OptiScaler's factory hook installed");
        // This executable's own factory, requested through its GetProcAddress: RTXMFG's gateway wraps it.
        using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);
        HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto createFactory = dxgi ? reinterpret_cast<FactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        if (createFactory) createFactory(IID_PPV_ARGS(&factory));
        Check(factory && Query(rtxmfg).factoryWrappersCreated > before.factoryWrappersCreated,
            "the game's factory is RTXMFG's factory proxy");
        if (!factory) return 1;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 1280; desc.Height = 720; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 3; desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const HRESULT created = factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain);
        int calls = 0, result = 0;
        context = hooked(&calls, &result);
        std::printf("INFO factory creation 0x%08X hook calls=%d result=%d\n", static_cast<unsigned>(created), calls, result);
        Check(SUCCEEDED(created) && chain && calls == 1 && result == 0 && context,
            "OptiScaler's hook answered the game's creation with its XeFG output");
    }
    else
    {
        const int result = makeOutput(queue.Get(), window, 1280, 720, &context, chain.GetAddressOf());
        std::printf("INFO OptiScaler XeFG output result %d\n", result);
        Check(result == 0 && context && chain, "OptiScaler's XeFG output created (CreateContext, InitFromSwapChainDesc, GetSwapChainPtr)");
    }
    if (!context || !chain) return 1;

    WaitLog("\"stage\":\"host_ui_attachment\"");
    std::string log = WaitLog("\"stage\":\"host_pacing_owner\"");
    // The factory route registers its renderer after that line is queued; the 100 ms log drain can land between them.
    if (factoryRoute) log = WaitLog("MFG_PROXY_UI renderer registered");
    const std::string attachment = StageLine(log, "host_ui_attachment");
    std::printf("INFO %s\n", attachment.c_str());
    Check(log.find("\"stage\":\"host_context_create\"") != std::string::npos
        && log.find("\"stage\":\"host_pacing_owner\"") != std::string::npos,
        "OptiScaler's XeFG creation and initialization reached RTXMFG's host route");
    const auto attached = Query(rtxmfg);
    const size_t renderers = Count(log, "MFG_PROXY_UI renderer registered");
    if (hidden)
    {
        Check(attachment.find("\"attached\":false") != std::string::npos
            && attachment.find("\"reason\":\"ui_mode\"") != std::string::npos, "hidden: no menu attaches to XeFG's output");
        Check(renderers == 0 && attached.swapchainWrappersCreated == 0, "hidden: no renderer and no swapchain proxy");
    }
    else if (factoryRoute)
    {
        Check(attachment.find("\"attached\":false") != std::string::npos
            && attachment.find("\"reason\":\"factory_route\"") != std::string::npos,
            "XeFG's output chain is left to the factory route in flight");
        Check(renderers == 1 && log.find("MFG_PROXY_UI renderer registered layer=creation-return base=OptiScaler swapchain") != std::string::npos,
            "one renderer for the window: the factory route's, on OptiScaler's XeFG swapchain");
        Check(attached.swapchainWrappersCreated == 1 && attached.liveSwapchainWrappers == 1, "one swapchain proxy");
    }
    else
    {
        Check(attachment.find("\"attached\":true") != std::string::npos,
            "the menu attached on XeFG's output chain inside OptiScaler's InitFromSwapChainDesc");
        Check(log.find("MFG_PROXY_UI renderer registered layer=xefg-output") != std::string::npos
            && log.find("MFG_PROXY_UI XeFG output swapchain attached") != std::string::npos,
            "renderer registered on XeFG's output chain (device, queue and LUID matched)");
        Check(renderers == 1 && attached.swapchainWrappersCreated == 1 && attached.liveSwapchainWrappers == 1,
            "one renderer and one swapchain proxy for the window");
    }

    // The game's frames, presented through the XeFG swapchain (generation off): XeFG presents each on its output chain.
    const auto presentBefore = Query(rtxmfg);
    unsigned presented = 0;
    HRESULT last = S_OK;
    for (int frame = 0; frame < 120; ++frame)
    {
        Pump();
        last = chain->Present(0, 0);
        if (SUCCEEDED(last)) ++presented;
        Sleep(4);
    }
    Sleep(200);
    Pump();
    const auto presentAfter = Query(rtxmfg);
    std::printf("INFO presented=%u last=0x%08X dxgiFrames %llu->%llu renderedFrames %llu->%llu\n", presented,
        static_cast<unsigned>(last), static_cast<unsigned long long>(presentBefore.dxgiFrames),
        static_cast<unsigned long long>(presentAfter.dxgiFrames), static_cast<unsigned long long>(presentBefore.renderedFrames),
        static_cast<unsigned long long>(presentAfter.renderedFrames));
    Check(presented == 120, "the game's 120 Presents through XeFG's swapchain succeed");
    const uint64_t sessionFrames = presentAfter.dxgiFrames - presentBefore.dxgiFrames;
    const uint64_t drawn = presentAfter.renderedFrames - presentBefore.renderedFrames;
    if (hidden)
        Check(sessionFrames == 0 && drawn == 0, "hidden: no menu session presents");
    else
        Check(sessionFrames > 0, factoryRoute ? "the factory route's session sees the game's Presents"
            : "the menu session sees XeFG's output Presents");
    if (optin)
        Check(drawn > 0, "the menu draws on XeFG's output chain (startup hint)");
    else if (!hidden)
        Check(drawn == 0, "shared UI mode: nothing drawn while no DLSS feature runs");

    // Teardown: the game's last reference to the XeFG swapchain goes, then OptiScaler destroys the XeFG context
    // (XeFG 1.3.1 refuses its destruction with POINTER_STILL_IN_USE while that pointer is held).
    chain.Reset();
    const int destroyed = destroyOutput(context);
    std::printf("INFO XeFG context destruction result %d\n", destroyed);
    Check(destroyed == 0, "OptiScaler's XeFG context destroyed");
    log = WaitLog("\"stage\":\"host_context_destroy\"");
    if (!hidden && !factoryRoute)
    {
        log = WaitLog("\"stage\":\"host_ui_detach\"");
        const std::string detach = StageLine(log, "host_ui_detach");
        std::printf("INFO %s\n", detach.c_str());
        Check(detach.find("\"retired\":true") != std::string::npos, "the menu retired with the XeFG context");
    }
    else Check(log.find("\"stage\":\"host_ui_detach\"") == std::string::npos, "no menu to retire with the XeFG context");
    Check(Query(rtxmfg).liveSwapchainWrappers == 0, "no swapchain proxy left");
    DestroyWindow(window);
    Pump();
    std::printf("COMPLETE optiscaler %ls failures=%d\n", variant.c_str(), gFailures);
    return gFailures ? 1 : 0;
}
#endif
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring mode = argv[1];
    const std::filesystem::path candidate = argv[2];
    const auto here = std::filesystem::current_path();
    const auto config = here / L"optiscaler.json";
    const auto status = here / L"optiscaler.status.json";
    SetEnvironmentVariableW(L"RTX_MFG_CONFIG_PATH", config.c_str());
    SetEnvironmentVariableW(L"RTX_MFG_STATUS_PATH", status.c_str());
    const bool known = mode == L"standalone" || mode == L"proxy" || mode == L"plugin"
        || mode == L"late" || mode == L"optin" || mode == L"hidden" || mode == L"shared" || mode == L"impostor"
        || mode == L"fakenvapi"
        || mode == L"streamline" || mode == L"second-streamline" || mode == L"streamline-device"
        || mode == L"streamline-warp" || mode == L"xefg-proxy" || mode == L"xefg-plugin"
        || mode == L"xefg-output" || mode == L"xefg-output-optin" || mode == L"xefg-output-hidden"
        || mode == L"xefg-output-factory" || mode == L"forza" || mode == L"forza-shared"
        || mode == L"menutab" || mode == L"menutab-plugin" || mode == L"menutab-shared" || mode == L"menutab-default"
        || mode == L"menutab-fork2";
    const bool forza = mode == L"forza" || mode == L"forza-shared";
    const bool privateStartup = mode == L"streamline-device" || mode == L"streamline-warp";
    const bool menuTab = mode.starts_with(L"menutab");
    const bool pluginRoute = mode == L"plugin" || mode == L"menutab-plugin" || mode == L"menutab-shared"
        || mode == L"menutab-default" || mode == L"menutab-fork2";
    // pluginabi.1: the stand-in offers our fork's version 2 table instead of upstream's.
    const bool fork2Api = mode == L"menutab-fork2";
    if (!known || std::filesystem::exists(config)) return 2;
    if (forza)
    {
        // The gate's file-name rule: this harness must run under a Forza executable name.
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (_wcsnicmp(std::filesystem::path(self).filename().c_str(), L"forza", 5) != 0) return 2;
    }

    std::string saved = R"({"followGame":false,"mode":"fixed","multiplier":4,"dlssgPreset":1,)"
        R"("vsyncMode":1,"reflexFrameLimitFps":90,"xessMultiplier":4,"xessForceEnable":true,)"
        R"("xessLowLatencyMode":2,"xessFrameLimitFps":120,"generatedOnlyDebug":true,"dlssSrPreset":11,"dlss5":true)";
    saved += mode == L"optin" || mode == L"xefg-output-optin" ? R"(,"optiScalerUi":"rtxmfg"})"
        // menutab and menutab-plugin set "optiscaler": hidden is not the default outside a ForzaTech executable, and the
        // tab is offered only in the hidden mode. menutab-default has no key: OptiScaler's plugin API makes hidden the default.
        : mode == L"hidden" || mode == L"xefg-output-hidden" || mode == L"menutab" || mode == L"menutab-plugin"
            || mode == L"menutab-fork2"
        ? R"(,"optiScalerUi":"optiscaler"})"
        : mode == L"shared" || mode == L"forza-shared" || mode == L"xefg-output" || mode == L"xefg-output-factory"
            || mode == L"menutab-shared"
        ? R"(,"optiScalerUi":"shared"})" : "}";
    std::ofstream(config, std::ios::binary) << saved;

    const auto host = here / L"OptiScaler.dll";
    if (!CopyFileW((here / (menuTab ? L"OptiScalerMenuTabFixture.dll" : L"OptiScalerHostFixture.dll")).c_str(),
            host.c_str(), FALSE)) return 3;
    const auto pluginDirectory = here / L"plugins";
    const auto plugin = pluginDirectory / L"MFGUnlocker.asi";
    std::filesystem::create_directories(pluginDirectory);
    if (!CopyFileW(candidate.c_str(), plugin.c_str(), FALSE)) return 3;
    if (mode == L"xefg-proxy" || mode == L"xefg-plugin")
        return RunXefgEarly(here, candidate, host, plugin, mode == L"xefg-plugin");
    if (mode.starts_with(L"xefg-output"))
    {
#if defined(OPTISCALER_FIXTURE_XEFG)
        return RunXefgOutput(here, host, plugin, status, mode);
#else
        Check(false, "harness built with the XeSS SDK headers (XESS_SDK_ROOT)");
        return 1;
#endif
    }

    HMODULE rtxmfg = nullptr;
    HMODULE optiscaler = nullptr;
    if (mode == L"standalone" || mode == L"second-streamline")
    {
        rtxmfg = LoadLibraryW(candidate.c_str());
    }
    else if (mode == L"proxy" || mode == L"optin" || mode == L"hidden" || mode == L"shared" || mode == L"streamline"
        || privateStartup || forza || mode == L"menutab")
    {
        optiscaler = LoadLibraryW(host.c_str());
        rtxmfg = LoadLibraryW(candidate.c_str());
    }
    else if (pluginRoute)
    {
        SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_PLUGIN", plugin.c_str());
        if (fork2Api) SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_API", L"fork2");
        optiscaler = LoadLibraryW(host.c_str());
        SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_PLUGIN", nullptr);
        SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_API", nullptr);
        rtxmfg = GetModuleHandleW(plugin.c_str());
        int loaded = 0, initialized = 0, error = 0;
        using ReportFn = void(__cdecl*)(int*, int*, int*);
        if (auto report = optiscaler ? reinterpret_cast<ReportFn>(GetProcAddress(optiscaler, "OptiFixtureReport")) : nullptr)
            report(&loaded, &initialized, &error);
        Check(loaded && initialized, "OptiScaler stand-in loaded plugins\\MFGUnlocker.asi and called InitializeASI in DllMain");
        if (!loaded) std::printf("plugin load error %d\n", error);
    }
    else if (mode == L"late")
    {
        rtxmfg = LoadLibraryW(candidate.c_str());
        WaitStatus(status, [](const std::string&) { return true; });
        optiscaler = LoadLibraryW(host.c_str());
    }
    else if (mode == L"impostor")
    {
        rtxmfg = LoadLibraryW(plugin.c_str());
        using InitializeFn = void(*)();
        if (auto initialize = rtxmfg ? reinterpret_cast<InitializeFn>(GetProcAddress(rtxmfg, "InitializeASI")) : nullptr)
            initialize();
        else
            Check(false, "candidate exports InitializeASI");
        // menutab.2: a plugin API table from outside OptiScaler is refused.
        static OptiPluginApi fakeApi{};
        fakeApi.size = sizeof(fakeApi);
        fakeApi.version = OPTI_PLUGIN_API_VERSION;
        fakeApi.RegisterTab = [](OptiPlugin*, const char*, const char*, OptiDrawTabFn, void*) { return true; };
        if (auto init = rtxmfg ? reinterpret_cast<PFN_OptiPluginInit>(GetProcAddress(rtxmfg, OPTI_PLUGIN_INIT_NAME)) : nullptr)
            Check(!init(&fakeApi, reinterpret_cast<OptiPlugin*>(&fakeApi)), "OptiPluginInit refuses an API table from outside OptiScaler");
    }
    else if (mode == L"fakenvapi")
    {
        const auto fake = here / L"nvapi64.dll";
        if (!CopyFileW((here / L"FakeNvapiFixture.dll").c_str(), fake.c_str(), FALSE)) return 3;
        if (!LoadLibraryW(fake.c_str())) return 3;
        rtxmfg = LoadLibraryW(candidate.c_str());
    }
    Check(rtxmfg != nullptr, "candidate loaded");
    if (!rtxmfg) return 1;
    Check(GetProcAddress(rtxmfg, "InitializeASI") != nullptr, "candidate exports InitializeASI");
    Check(GetProcAddress(rtxmfg, OPTI_PLUGIN_INIT_NAME) != nullptr, "candidate exports OptiPluginInit");
    Check(Query(rtxmfg).backendOwner == 1, "candidate owns the backend");

    const bool expectHost = mode == L"proxy" || mode == L"plugin" || mode == L"late" || mode == L"optin"
        || mode == L"hidden" || mode == L"shared" || mode == L"streamline" || privateStartup || forza || menuTab;
    // Frame generation is OptiScaler's unless "rtxmfg"; the menu is gone when "optiscaler" and, without the key, for a
    // ForzaTech executable or once OptiScaler handed its plugin API (shared everywhere else).
    const bool delegated = expectHost && mode != L"optin";
    const bool hidden = mode == L"hidden" || mode == L"forza" || mode == L"menutab" || mode == L"menutab-plugin"
        || mode == L"menutab-default" || mode == L"menutab-fork2";
    // menutab.2: OptiScaler hands its plugin API only to a plugin it loads; while hidden, RTXMFG's tab offers the
    // DLSS presets and DLSS 5.
    const bool apiExpected = menuTab && pluginRoute;
    const bool tabExpected = apiExpected && hidden;
    const char* route = pluginRoute ? "\"plugin\"" : mode == L"late" ? "\"late\""
        : expectHost ? "\"proxy\"" : "\"none\"";
    const std::string json = WaitStatus(status, [&](const std::string& text) {
        return Has(text, "optiScalerRoute", route)
            && (mode != L"fakenvapi" || Has(text, "fakeNvapiObserved", "true"))
            && (mode != L"menutab-default" || Has(text, "optiScalerUiOwner", "\"optiscaler\""));
    });
    Check(ui_status_json::CompleteObject(json), "status published");
    Check(Has(json, "integrationMode", expectHost ? "\"optiscaler\"" : "\"standalone\""), "integration mode");
    Check(Has(json, "optiScalerDetected", expectHost ? "true" : "false"), "OptiScaler detection by version resource");
    Check(Has(json, "optiScalerRoute", route), "OptiScaler route");
    Check(Has(json, "optiScalerUiOwner", hidden ? "\"optiscaler\"" : delegated ? "\"shared\"" : expectHost ? "\"rtxmfg\"" : "\"none\""),
        "UI owner");
    Check(Has(json, "optiScalerMenuSuppressed", hidden ? "true" : "false"), "menu suppression");
    if (expectHost)
        Check(Has(json, "optiScalerProductVersion", "\"10.0.0-fixture\""), "OptiScaler version read from the mapped image");
    Check(Has(json, "optiScalerPluginCallerAccepted", pluginRoute ? "true" : "false"), "plugin caller acceptance");
    Check(Has(json, "optiScalerPluginCallerRejected", mode == L"impostor" ? "true" : "false"),
        "a non-OptiScaler InitializeASI caller is rejected");
    if (mode == L"fakenvapi")
        Check(Has(json, "fakeNvapiObserved", "true"), "game-folder fakenvapi is not treated as NVIDIA's NVAPI");

    // Effective control: Follow and no overrides while OptiScaler's menu owns
    // the controls; the saved choices everywhere else.
    Check(Has(json, "followGame", delegated ? "true" : "false"), "effective Follow");
    Check(Has(json, "mode", delegated ? "\"follow\"" : "\"fixed\""), "effective mode");
    Check(Has(json, "vsyncMode", delegated ? "0" : "1"), "V-Sync override");
    Check(Has(json, "reflexFrameLimitFps", delegated ? "0" : "90"), "Reflex frame limit override");
    // XeSS MFG runs under OptiScaler only; the saved file's older XeSS keys still load and are ignored.
    Check(json.find("\"xessMultiplier\"") == std::string::npos && json.find("\"xessForceEnable\"") == std::string::npos
        && json.find("\"xessFrameLimitFps\"") == std::string::npos && json.find("\"xessLowLatencyMode\"") == std::string::npos,
        "no XeSS multiplier, force or XeLL keys in the status");
    Check(Has(json, "generatedOnlyDebug", delegated ? "false" : "true"), "debug presentation override");
    // Shared UI mode: the DLSS SR/RR presets, the DLSS-G preset and DLSS 5 stay RTXMFG's; hidden delegates them too,
    // unless RTXMFG's tab in OptiScaler's menu offers them.
    const bool dlssApplies = !hidden || apiExpected;
    Check(Has(json, "dlssgPresetRequested", dlssApplies ? "1" : "0"),
        "saved DLSS-G preset applies unless the menu is hidden without OptiScaler's plugin API");
    // dlssgtab.2: a new DLSS-G preset restarts OptiScaler's DLSSG output on its own calls, as it does the game's, so
    // OptiScaler is never the reason it waits for a restart (reason 1, the last code is 8).
    Check(!Has(json, "dlssgPresetLiveSwitch", "1"), "OptiScaler does not make the DLSS-G preset restart-only");
    Check(Has(json, "dlssSrPresetRequested", dlssApplies ? "11" : "0"),
        "saved DLSS SR preset applies unless the menu is hidden without OptiScaler's plugin API");
    Check(Has(json, "dlss5", dlssApplies ? "true" : "false"),
        "saved DLSS 5 applies unless the menu is hidden without OptiScaler's plugin API");

    // Menu factories: wrapped for RTXMFG's own UI, untouched for OptiScaler's.
    const uint64_t wrappers = FactoryWrappersAfterCreate(rtxmfg);
    Check(hidden ? wrappers == 0 : wrappers > 0, hidden
        ? "no RTXMFG factory proxy while the RTXMFG menu is hidden"
        : "RTXMFG factory proxy created for its menu");

    if (menuTab)
    {
        // OptiPluginInit registers the tab inside OptiScaler's DllMain; the worker only for a later switch to hidden.
        using TabFn = int(__cdecl*)(char*, size_t, int, char*, size_t, int*, int*, int*);
        const auto tab = optiscaler ? reinterpret_cast<TabFn>(GetProcAddress(optiscaler, "OptiFixtureMenuTab")) : nullptr;
        Check(tab != nullptr, "stand-in exports its menu tab probe");
        char label[64]{};
        int registered = 0, initResult = -2;
        for (unsigned waited = 0; tab && waited <= 3000; waited += 50)
        {
            registered = tab(label, sizeof(label), 0, nullptr, 0, nullptr, nullptr, &initResult);
            if (registered == (tabExpected ? 1 : 0) && (tabExpected || waited >= 1000)) break;
            Sleep(50);
        }
        Check(initResult == (apiExpected ? 1 : -1), apiExpected
            ? "OptiScaler's plugin loader hands RTXMFG its API and RTXMFG accepts it"
            : "no OptiPluginInit call for a sibling proxy");
        Check(registered == (tabExpected ? 1 : 0), tabExpected
            ? "RTXMFG registers its tab in OptiScaler's menu while its own menu is hidden"
            : "no tab in OptiScaler's menu while RTXMFG's own menu is shared or OptiScaler gave no API");
        if (tabExpected)
        {
            Check(std::strcmp(label, "MFG Unlocker") == 0, "tab label");
            std::vector<char> drawn(16384);
            int calls = 0, bad = 0;
            tab(nullptr, 0, 1, drawn.data(), drawn.size(), &calls, &bad, nullptr);
            const std::string text = drawn.data();
            Check(calls > 0 && bad == 0, "tab draws through OptiScaler's widget functions, inside the draw, Disabled closed");
            Check(text.find("Frame generation is set in OptiScaler's menu.") != std::string::npos,
                "tab names OptiScaler's menu as the frame-generation owner");
            // No DLSS feature runs in the harness.
            Check(text.find("Waiting for DLSS") != std::string::npos, "tab reads the backend status (waiting for DLSS)");
            // GitHub issue 122: the log toggle on its own Diagnostics card, last in the tab, also while waiting for DLSS.
            const size_t diagnostics = text.rfind("Diagnostics\nWrite log file##runtimelog\nWrites the MFG Unlocker log");
            Check(diagnostics != std::string::npos && diagnostics > text.find("Waiting for DLSS"),
                "tab offers Write log file under Diagnostics, after the features");
            if (bad || text.find("Waiting for DLSS") == std::string::npos || diagnostics == std::string::npos)
                std::printf("tab drew %d calls, %d bad:\n%s", calls, bad, text.c_str());
        }
    }

    Sleep(400);
    const std::string log = ReadText(RuntimeLog());
    Check(!log.empty(), "runtime log readable");
    bool facadeClassified = false;
    for (size_t at = log.find("Loaded module:"); at != std::string::npos; at = log.find("Loaded module:", at + 1))
    {
        const size_t end = log.find('\n', at);
        if (log.substr(at, end - at).find("OptiScaler.dll") != std::string::npos) facadeClassified = true;
    }
    Check(!facadeClassified, "OptiScaler's NGX facade is never inventoried as an NVIDIA module");
    if (expectHost)
        Check(log.find("OptiScaler") != std::string::npos, "runtime log records OptiScaler mode");
    if (expectHost)
        Check((log.find("ForzaTech executable") != std::string::npos) == forza,
            forza ? "runtime log records the ForzaTech default" : "no ForzaTech default outside a Forza executable");
    if (mode == L"impostor")
        Check(log.find("OptiPluginInit refused") != std::string::npos, "runtime log records the refused plugin API");
    if (menuTab)
    {
        Check(log.find(apiExpected ? "menuTabApi=1" : "menuTabApi=0") != std::string::npos,
            "runtime log records whether OptiScaler handed its plugin API");
        Check((log.find("OptiScaler plugin API accepted (OptiPluginInit).") != std::string::npos) == apiExpected,
            "runtime log records the accepted plugin API");
        Check((log.find("OptiScaler menu tab registered") != std::string::npos) == tabExpected,
            "runtime log records the menu tab registration");
        const char* layout = fork2Api ? "OptiScaler plugin API layout: MFG Unlocker fork version 2"
            : "OptiScaler plugin API layout: upstream.";
        Check((log.find(layout) != std::string::npos) == apiExpected, fork2Api
            ? "runtime log records the fork's version 2 API table, adapted"
            : apiExpected ? "runtime log records upstream's API table" : "no API table layout without the API");
    }

    if (mode == L"streamline" || mode == L"second-streamline")
        Check(Has(WaitStatus(status, [](const std::string& text) {
            return Has(text, "mainResolverDiscoveryInstalled", "true"); }), "mainResolverDiscoveryInstalled", "true"),
            "Streamline resolver gateway installed before the interposers load");
    if (mode == L"streamline")
    {
        const auto folder = here / L"OptiScaler" / L"streamline";
        std::filesystem::create_directories(folder);
        if (!CopyFileW((here / L"sl.interposer.dll").c_str(), (folder / L"sl.interposer.dll").c_str(), FALSE)) return 3;
        CheckStreamlineInstances(here, folder / L"sl.interposer.dll", true, true);
    }
    else if (mode == L"second-streamline")
    {
        CheckStreamlineInstances(here, here / L"hotpatch" / L"sl.interposer.dll", false, false);
    }
    else if (privateStartup)
    {
        CheckPrivateStartupOrder(here, status, mode == L"streamline-warp");
    }

    Check(ReadText(config) == saved, "saved control remains byte-identical");
    (void)optiscaler;
    std::printf("COMPLETE optiscaler %ls failures=%d\n", mode.c_str(), gFailures);
    return gFailures ? 1 : 0;
}
