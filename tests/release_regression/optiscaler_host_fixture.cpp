// OptiScaler stand-in for the release harness. Its version resource carries
// OptiScaler's identity strings (ProductName "OptiScaler", OriginalFilename
// "OptiScaler.dll", InternalName "OptiScaler") and it exports the NGX facade
// shape OptiScaler has (D3D12 CreateFeature/EvaluateFeature/
// GetFeatureRequirements/Init_ProjectID) plus a fakenvapi-style
// nvapi_QueryInterface, so RTXMFG's classification of those exports is
// exercised. Like OptiScaler's [Plugins] LoadAsiPlugins, DllMain loads the
// .asi named by RTXMFG_TEST_OPTISCALER_PLUGIN with the loader lock held and
// calls its InitializeASI on the same thread.
// Like OptiScaler's XeFG proxy, DllMain first loads the XeSS pair
// from RTXMFG_TEST_OPTISCALER_XEFG (libxell.dll, libxess_fg.dll) and resolves
// xefgSwapChainD3D12CreateContext, before any plug-in loads. The creation is
// later called from this module's code, so RTXMFG sees OptiScaler as the caller.
//
// OPTISCALER_FIXTURE_XEFG (with the XeSS SDK headers): OptiScaler's
// XeFG output swapchain as XeFG_Dx12::CreateSwapchain1Internal builds it
// (context, InitFromSwapChainDesc with the game's queue and the real DXGI
// factory, GetSwapChainPtr), every call from this image; and a detour on DXGI's
// CreateSwapChainForHwnd that, like OptiScaler's factory hook with FGOutput
// XeFG, answers a game's creation with that chain (XeFG's own creation inside
// passes through, as ScopedSkipFGSCCreation does).
//
// Built three times: without OPTISCALER_FIXTURE_NO_IDENTITY as the host, with it
// as a fakenvapi stand-in (copied to nvapi64.dll) that has no OptiScaler
// identity, and with OPTISCALER_FIXTURE_MENU_TAB as a host that also offers
// OptiScaler's plugin API to the plugin it loads: upstream's merged layout, or
// with RTXMFG_TEST_OPTISCALER_API=fork2 our fork's earlier version 2 layout.
#include <Windows.h>
#include <cstdint>
#include <cstring>
#if defined(OPTISCALER_FIXTURE_MENU_TAB)
#include <string>
#include "optiscaler_plugin.h"
#include "optiscaler_plugin_fork2.h"
#endif
#if defined(OPTISCALER_FIXTURE_XEFG)
#include <cwchar>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell_d3d12.h>
#endif

namespace
{
HMODULE gPlugin = nullptr;
int gInitializeCalled = 0;
int gPluginLoadError = 0;
HMODULE gXefg = nullptr;
HMODULE gXell = nullptr;
void* gXefgCreate = nullptr;
unsigned char gXefgCreateBytes[16]{};
volatile int gXefgCreateResult = 0;
}

extern "C" __declspec(dllexport) void* __cdecl OptiFixtureXefgCreate(unsigned char* resolvedBytes)
{
    if (resolvedBytes) std::memcpy(resolvedBytes, gXefgCreateBytes, sizeof(gXefgCreateBytes));
    return gXell && gXefg ? gXefgCreate : nullptr;
}

// The store after the call keeps it a call (no tail jump), so the provider entry
// returns into OptiScaler's image as it does for OptiScaler's XeFG output.
extern "C" __declspec(dllexport) __declspec(noinline) int __cdecl OptiFixtureCallXefgCreate(void* device, void** output)
{
    using Create = int(*)(void*, void**);
    const int result = gXefgCreate ? reinterpret_cast<Create>(gXefgCreate)(device, output) : -1;
    gXefgCreateResult = result;
    return result;
}

#if defined(OPTISCALER_FIXTURE_XEFG)
namespace
{
using Microsoft::WRL::ComPtr;
using CreateForHwndFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
CreateForHwndFn gCreateForHwnd = nullptr; // trampoline
void* gCreateForHwndTarget = nullptr;
thread_local bool tInsideXefg = false;    // OptiScaler's ScopedSkipFGSCCreation
void* gHookedContext = nullptr;
int gHookCalls = 0;
int gHookResult = 0;
volatile int gXefgDestroyResult = 0;
xell_context_handle_t gXellContext = nullptr; // the XeLL context paired with the live XeFG context

template<class T> T XefgExport(const char* name)
{
    return gXefg ? reinterpret_cast<T>(GetProcAddress(gXefg, name)) : nullptr;
}

// dxgi.dll's export by its export table, so no resolver can hand back a factory proxy:
// OptiScaler passes XeFG its real (unwrapped) factory.
void* SystemExport(const wchar_t* module, const char* name)
{
    const auto* base = reinterpret_cast<const unsigned char*>(LoadLibraryExW(module, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!base) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i)
        if (!std::strcmp(reinterpret_cast<const char*>(base + names[i]), name))
            return const_cast<unsigned char*>(base + functions[ordinals[i]]);
    return nullptr;
}

ComPtr<IDXGIFactory2> RealFactory()
{
    using CreateFn = HRESULT (WINAPI*)(UINT, REFIID, void**);
    ComPtr<IDXGIFactory2> factory;
    if (auto create = reinterpret_cast<CreateFn>(SystemExport(L"dxgi.dll", "CreateDXGIFactory2")))
        create(0, IID_PPV_ARGS(&factory));
    return factory;
}

// XeFG_Dx12::CreateSwapchain1Internal (with CreateSwapchainContext's XeLL pairing: XeFG 1.3 refuses its
// initialization without a latency-reduction context). Results are used after every call, so each call returns
// into this image and RTXMFG sees OptiScaler as the caller.
__declspec(noinline) int XefgSwapchain(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc, void** context, IDXGISwapChain1** chain)
{
    *context = nullptr;
    *chain = nullptr;
    const auto init = XefgExport<decltype(&xefgSwapChainD3D12InitFromSwapChainDesc)>("xefgSwapChainD3D12InitFromSwapChainDesc");
    const auto get = XefgExport<decltype(&xefgSwapChainD3D12GetSwapChainPtr)>("xefgSwapChainD3D12GetSwapChainPtr");
    const auto destroy = XefgExport<decltype(&xefgSwapChainDestroy)>("xefgSwapChainDestroy");
    const auto pair = XefgExport<decltype(&xefgSwapChainSetLatencyReduction)>("xefgSwapChainSetLatencyReduction");
    const auto createLatency = gXell ? reinterpret_cast<decltype(&xellD3D12CreateContext)>(GetProcAddress(gXell, "xellD3D12CreateContext")) : nullptr;
    const auto destroyLatency = gXell ? reinterpret_cast<decltype(&xellDestroyContext)>(GetProcAddress(gXell, "xellDestroyContext")) : nullptr;
    if (!gXefgCreate || !init || !get || !destroy || !pair || !createLatency || !destroyLatency || !factory || !queue) return -100;
    if (gXellContext) return -104;
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return -101;
    xefg_swapchain_handle_t handle = nullptr;
    int result = reinterpret_cast<decltype(&xefgSwapChainD3D12CreateContext)>(gXefgCreate)(device.Get(), &handle);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || !handle) return result ? result : -102;
    xell_context_handle_t latency = nullptr;
    const int latencyResult = createLatency(device.Get(), &latency);
    if (latencyResult != XELL_RESULT_SUCCESS || !latency)
    {
        destroy(handle);
        return -105;
    }
    result = pair(handle, latency);
    gXellContext = latency;
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        destroy(handle);
        destroyLatency(latency);
        gXellContext = nullptr;
        return result;
    }
    xefg_swapchain_d3d12_init_params_t params{};
    params.maxInterpolatedFrames = 1;
    params.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
    tInsideXefg = true;
    result = init(handle, window, desc, nullptr, queue, factory, &params);
    tInsideXefg = false;
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS) result = get(handle, IID_PPV_ARGS(chain));
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        const int destroyed = destroy(handle);
        destroyLatency(latency);
        gXellContext = nullptr;
        return destroyed == XEFG_SWAPCHAIN_RESULT_SUCCESS ? result : -103;
    }
    *context = handle;
    return 0;
}

HRESULT STDMETHODCALLTYPE HookedCreateForHwnd(IDXGIFactory2* self, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full, IDXGIOutput* output,
    IDXGISwapChain1** chain)
{
    ComPtr<ID3D12CommandQueue> queue;
    if (tInsideXefg || !desc || !chain || !device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
        return gCreateForHwnd(self, device, window, desc, full, output, chain);
    ++gHookCalls;
    void* context = nullptr;
    gHookResult = XefgSwapchain(self, queue.Get(), window, desc, &context, chain);
    if (gHookResult) return E_FAIL;
    gHookedContext = context;
    return S_OK;
}
}

extern "C" __declspec(dllexport) int __cdecl OptiFixtureXefgSwapchain(ID3D12CommandQueue* queue, HWND window,
    UINT width, UINT height, void** context, IDXGISwapChain1** chain)
{
    if (!context || !chain) return -1;
    const auto factory = RealFactory();
    if (!factory) return -110;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    return XefgSwapchain(factory.Get(), queue, window, &desc, context, chain);
}

// 1 when the factory OptiScaler passes is DXGI's own object (vtable in System32\dxgi.dll).
extern "C" __declspec(dllexport) int __cdecl OptiFixtureRealFactory()
{
    const auto factory = RealFactory();
    HMODULE owner = nullptr;
    wchar_t path[MAX_PATH]{}, expected[MAX_PATH]{};
    return factory && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            *reinterpret_cast<LPCWSTR*>(factory.Get()), &owner)
        && GetModuleFileNameW(owner, path, MAX_PATH) && GetSystemDirectoryW(expected, MAX_PATH)
        && !wcscat_s(expected, L"\\dxgi.dll") && !_wcsicmp(path, expected) ? 1 : 0;
}

extern "C" __declspec(dllexport) __declspec(noinline) int __cdecl OptiFixtureXefgDestroy(void* context)
{
    const auto destroy = XefgExport<decltype(&xefgSwapChainDestroy)>("xefgSwapChainDestroy");
    const auto destroyLatency = gXell ? reinterpret_cast<decltype(&xellDestroyContext)>(GetProcAddress(gXell, "xellDestroyContext")) : nullptr;
    const int result = destroy && context ? destroy(static_cast<xefg_swapchain_handle_t>(context)) : -1;
    gXefgDestroyResult = result;
    // As OptiScaler's release_xefg_xell: the paired XeLL context after its XeFG context.
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS && gXellContext && destroyLatency)
    {
        const int latency = destroyLatency(gXellContext);
        gXellContext = nullptr;
        if (latency != XELL_RESULT_SUCCESS) return -106;
    }
    return result;
}

// OptiScaler's DXGI factory hook with FGOutput XeFG: DXGI's CreateSwapChainForHwnd, detoured.
extern "C" __declspec(dllexport) int __cdecl OptiFixtureHookFactory()
{
    if (gCreateForHwndTarget) return 0;
    const auto factory = RealFactory();
    if (!factory) return -1;
    void* target = (*reinterpret_cast<void***>(factory.Get()))[15];
    const auto initialized = MH_Initialize();
    if ((initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
        || MH_CreateHook(target, reinterpret_cast<void*>(&HookedCreateForHwnd), reinterpret_cast<void**>(&gCreateForHwnd)) != MH_OK
        || MH_EnableHook(target) != MH_OK) return -2;
    gCreateForHwndTarget = target;
    return 0;
}

extern "C" __declspec(dllexport) void* __cdecl OptiFixtureHookedXefg(int* calls, int* result)
{
    if (calls) *calls = gHookCalls;
    if (result) *result = gHookResult;
    return gHookedContext;
}
#endif

#if defined(OPTISCALER_FIXTURE_MENU_TAB)
// menutab.2: OptiScaler's plugin API. Like OptiScaler's plugin loader, DllMain hands it to the plugin's OptiPluginInit
// right after InitializeASI. The stand-in keeps one tab and draws it on demand (OptiFixtureMenuTab) through widget
// functions that record what the tab drew, and count widget calls outside a draw, invalid arguments and a Disabled
// block left open as bad. A tab reading one table layout as the other calls the widget next to the one it means, and
// its text goes missing or its arguments count as bad.
struct OptiPlugin
{
    int unused;
};

namespace
{
SRWLOCK gTabLock = SRWLOCK_INIT;
OptiPlugin gPluginHandle{};
int gPluginInitResult = -1; // -1 not called, 0 refused, 1 accepted
bool gTabRegistered = false;
char gTabLabel[64]{};
OptiDrawTabFn gTabDraw = nullptr;
void* gTabUser = nullptr;
bool gInDraw = false;
bool gDisabledOpen = false;
std::string gTabText; // every text, section, label and tooltip drawn, one per line
int gTabCalls = 0;
int gTabBad = 0;

void Record(const char* text)
{
    ++gTabCalls;
    if (!gInDraw || !text) { ++gTabBad; return; }
    gTabText += text;
    gTabText += '\n';
}
void Widget()
{
    ++gTabCalls;
    if (!gInDraw) ++gTabBad;
}
// From OptiPluginInit (inside DllMain) or later from the plugin's own thread.
bool TabRegister(OptiPlugin* plugin, const char* name, const char*, OptiDrawTabFn draw, void* userData)
{
    if (plugin != &gPluginHandle || !name || !name[0] || !draw) return false;
    AcquireSRWLockExclusive(&gTabLock);
    const bool added = !gTabRegistered;
    if (added)
    {
        strncpy_s(gTabLabel, name, _TRUNCATE);
        gTabDraw = draw;
        gTabUser = userData;
        gTabRegistered = true;
    }
    ReleaseSRWLockExclusive(&gTabLock);
    return added;
}
void TabText(const char* text) { Record(text); }
void TabNothing() { Widget(); }
void TabDisabled(bool disabled) { Widget(); gDisabledOpen = disabled; }
void TabTextColored(float, float, float, float, const char* text) { Record(text); }
bool TabButton(const char* label) { Record(label); return false; }
bool TabCheckbox(const char* label, bool* value) { Record(label); if (!value) ++gTabBad; return false; }
bool TabSliderFloat(const char* label, float* value, float, float, const char*) { Record(label); if (!value) ++gTabBad; return false; }
bool TabSliderInt(const char* label, int32_t* value, int32_t, int32_t, const char*) { Record(label); if (!value) ++gTabBad; return false; }
bool TabComboItems(const char* label, int32_t* current, const char* const* items, int32_t count)
{
    Record(label);
    if (!current || !items || count <= 0 || *current < 0 || *current >= count) { ++gTabBad; return false; }
    for (int32_t i = 0; i < count; ++i)
        if (!items[i]) ++gTabBad;
    return false;
}
// Upstream: itemDisabled is null or count bools.
bool TabCombo(const char* label, int32_t* current, const char* const* items, int32_t count, const bool* itemDisabled)
{
    const bool result = TabComboItems(label, current, items, count);
    const auto* bytes = reinterpret_cast<const unsigned char*>(itemDisabled);
    for (int32_t i = 0; bytes && i < count && i < 64; ++i)
        if (bytes[i] > 1) ++gTabBad;
    return result;
}
// The fork: Combo without disabled items, ComboEx with a mask of the first 32.
bool TabComboFork2(const char* label, int32_t* current, const char* const* items, int32_t count)
{
    return TabComboItems(label, current, items, count);
}
bool TabComboEx(const char* label, int32_t* current, const char* const* items, int32_t count, uint32_t)
{
    return TabComboItems(label, current, items, count);
}
bool TabInputText(const char* label, char* buffer, size_t size) { Record(label); if (!buffer || !size) ++gTabBad; return false; }
bool TabColorEdit(const char* label, float* rgba, bool) { Record(label); if (!rgba) ++gTabBad; return false; }
float TabMenuScale() { return 1.0f; }
bool TabMenuOpen() { return gInDraw; }
bool TabFalse() { Widget(); return false; }
// Settings and logging: RTXMFG keeps its own; these stand-ins return the defaults.
bool TabGetBool(OptiPlugin*, const char*, bool value) { return value; }
int32_t TabGetInt(OptiPlugin*, const char*, int32_t value) { return value; }
float TabGetFloat(OptiPlugin*, const char*, float value) { return value; }
bool TabGetString(OptiPlugin*, const char*, char* buffer, size_t size, const char* value)
{
    if (buffer && size) strncpy_s(buffer, size, value ? value : "", _TRUNCATE);
    return false;
}
void TabSetBool(OptiPlugin*, const char*, bool) {}
void TabSetInt(OptiPlugin*, const char*, int32_t) {}
void TabSetFloat(OptiPlugin*, const char*, float) {}
void TabSetString(OptiPlugin*, const char*, const char*) {}
void TabReset(OptiPlugin*, const char*) {}
bool TabSave(OptiPlugin*) { return true; }
void TabLog(OptiPlugin*, OptiLogLevel, const char*) {}

// Field order of optiscaler_plugin.h: ItemDeactivatedAfterEdit after ItemTooltip, Combo with itemDisabled, no
// GetMenuScale.
const OptiPluginApi gPluginApi = {sizeof(OptiPluginApi), OPTI_PLUGIN_API_VERSION, TabRegister, TabText, TabNothing,
    TabNothing, TabNothing, TabDisabled, TabText, TabFalse, TabText, TabText, TabTextColored, TabButton, TabCheckbox,
    TabSliderFloat, TabSliderInt, TabCombo, TabInputText, TabColorEdit, TabMenuOpen, TabGetBool, TabGetInt, TabGetFloat,
    TabGetString, TabSetBool, TabSetInt, TabSetFloat, TabSetString, TabReset, TabSave, TabLog};

// Field order of optiscaler_plugin_fork2.h.
const OptiPluginApiFork2 gPluginApiFork2 = {sizeof(OptiPluginApiFork2), OPTI_PLUGIN_FORK2_API_VERSION, TabRegister,
    TabText, TabNothing, TabNothing, TabNothing, TabDisabled, TabText, TabText, TabText, TabTextColored, TabButton,
    TabCheckbox, TabSliderFloat, TabSliderInt, TabComboFork2, TabInputText, TabColorEdit, TabMenuScale, TabMenuOpen,
    TabGetBool, TabGetInt, TabGetFloat, TabGetString, TabSetBool, TabSetInt, TabSetFloat, TabSetString, TabReset, TabSave,
    TabLog, TabComboEx, TabFalse};

void InitPlugin(HMODULE plugin)
{
    const auto init = reinterpret_cast<PFN_OptiPluginInit>(GetProcAddress(plugin, OPTI_PLUGIN_INIT_NAME));
    if (!init) return;
    wchar_t layout[16]{};
    const DWORD length = GetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_API", layout, 16);
    const bool fork2 = length && length < 16 && wcscmp(layout, L"fork2") == 0;
    const auto* api = fork2 ? reinterpret_cast<const OptiPluginApi*>(&gPluginApiFork2) : &gPluginApi;
    // Not under gTabLock: the plugin registers from inside.
    const bool accepted = init(api, &gPluginHandle);
    AcquireSRWLockExclusive(&gTabLock);
    gPluginInitResult = accepted ? 1 : 0;
    // OptiScaler keeps nothing a plugin registered when it declines the API.
    if (!accepted) gTabRegistered = false;
    ReleaseSRWLockExclusive(&gTabLock);
}
}

// 1 while a tab is registered; label receives its label and initResult OptiPluginInit's result (-1 not called). With
// draw, the tab is drawn once on this thread, text receives what it drew, calls the widget calls and bad the misuses.
extern "C" __declspec(dllexport) int __cdecl OptiFixtureMenuTab(char* label, size_t labelSize, int draw, char* text,
    size_t textSize, int* calls, int* bad, int* initResult)
{
    AcquireSRWLockExclusive(&gTabLock);
    const int registered = gTabRegistered ? 1 : 0;
    if (label && labelSize) strncpy_s(label, labelSize, gTabLabel, _TRUNCATE);
    if (initResult) *initResult = gPluginInitResult;
    if (registered && draw)
    {
        gTabText.clear();
        gTabCalls = gTabBad = 0;
        gDisabledOpen = false;
        gInDraw = true;
        gTabDraw(gTabUser);
        gInDraw = false;
        if (gDisabledOpen) ++gTabBad;
    }
    if (text && textSize) strncpy_s(text, textSize, gTabText.c_str(), _TRUNCATE);
    if (calls) *calls = gTabCalls;
    if (bad) *bad = gTabBad;
    ReleaseSRWLockExclusive(&gTabLock);
    return registered;
}
#endif

extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_CreateFeature() { return -1; }
extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_EvaluateFeature() { return -1; }
extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_GetFeatureRequirements() { return -1; }
extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_Init_ProjectID() { return -1; }
extern "C" __declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_ReleaseFeature() { return -1; }

extern "C" __declspec(dllexport) void* __cdecl nvapi_QueryInterface(uint32_t id)
{
    static int marker = 0;
    return id == 0x21382138u ? &marker : nullptr;
}

extern "C" __declspec(dllexport) void __cdecl OptiFixtureReport(int* loaded, int* initialized, int* error)
{
    if (loaded) *loaded = gPlugin != nullptr;
    if (initialized) *initialized = gInitializeCalled;
    if (error) *error = gPluginLoadError;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
#if !defined(OPTISCALER_FIXTURE_NO_IDENTITY)
    if (reason == DLL_PROCESS_ATTACH)
    {
        wchar_t xefg[MAX_PATH]{};
        const DWORD xefgLength = GetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_XEFG", xefg, MAX_PATH);
        if (xefgLength && xefgLength < MAX_PATH - 16)
        {
            wchar_t path[MAX_PATH]{};
            const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
            if (!wcscpy_s(path, xefg) && !wcscat_s(path, L"\\libxell.dll"))
                gXell = LoadLibraryExW(path, nullptr, flags);
            if (!wcscpy_s(path, xefg) && !wcscat_s(path, L"\\libxess_fg.dll"))
                gXefg = LoadLibraryExW(path, nullptr, flags);
            if (gXefg)
                gXefgCreate = reinterpret_cast<void*>(GetProcAddress(gXefg, "xefgSwapChainD3D12CreateContext"));
            if (gXefgCreate)
                std::memcpy(gXefgCreateBytes, gXefgCreate, sizeof(gXefgCreateBytes));
        }
        wchar_t plugin[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_PLUGIN", plugin, MAX_PATH);
        if (length && length < MAX_PATH)
        {
            gPlugin = LoadLibraryExW(plugin, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!gPlugin) gPluginLoadError = static_cast<int>(GetLastError());
            using InitializeAsi = void(*)();
            if (gPlugin)
                if (auto initialize = reinterpret_cast<InitializeAsi>(GetProcAddress(gPlugin, "InitializeASI")))
                {
                    initialize();
                    gInitializeCalled = 1;
                }
#if defined(OPTISCALER_FIXTURE_MENU_TAB)
            if (gPlugin) InitPlugin(gPlugin);
#endif
        }
    }
#endif
    return TRUE;
}
