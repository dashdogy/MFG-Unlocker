#include "optiscaler_host.h"

#include <winternl.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>

namespace optiscaler_host
{
namespace
{
std::atomic<uintptr_t> gBase{0};
std::atomic<uint32_t> gImageSize{0};
std::atomic<uint32_t> gRoute{static_cast<uint32_t>(Route::None)};
std::atomic<uint32_t> gUiMode{static_cast<uint32_t>(UiMode::Default)};
constexpr uint32_t kUndecided = ~0u;
std::atomic<uint32_t> gDefaultUiMode{kUndecided};
std::atomic<bool> gMenuTabOffered{false};
std::atomic<bool> gPluginCallerAccepted{false};
std::atomic<bool> gPluginCallerRejected{false};
std::atomic<bool> gStarted{false};
std::atomic<uintptr_t> gSelf{0};
// Written once by the thread that wins gBase, before gPublished is released.
wchar_t gFileVersion[32]{};
wchar_t gProductVersion[96]{};
wchar_t gPath[MAX_PATH]{};
std::atomic<bool> gPublished{false};

bool ImageBounds(HMODULE module, const uint8_t*& base, uint32_t& size) noexcept
{
    base = reinterpret_cast<const uint8_t*>(module);
    size = 0;
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || dos->e_lfanew > 0x1000) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    size = nt->OptionalHeader.SizeOfImage;
    return size > static_cast<uint32_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64);
}

bool Inside(uint32_t size, uint64_t offset, uint64_t bytes) noexcept
{
    return offset <= size && bytes <= size - offset;
}

// Resource directory: RT_VERSION -> first name -> first language.
bool FindVersionResource(HMODULE module, const uint8_t*& data, uint32_t& bytes) noexcept
{
    const uint8_t* base = nullptr;
    uint32_t size = 0;
    data = nullptr;
    bytes = 0;
    if (!ImageBounds(module, base, size)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE) return false;
    const IMAGE_DATA_DIRECTORY directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    if (!directory.VirtualAddress || !directory.Size
        || !Inside(size, directory.VirtualAddress, directory.Size)) return false;
    const uint32_t rootOffset = directory.VirtualAddress;
    const uint32_t rootSize = directory.Size;

    auto entries = [&](uint32_t directoryOffset, const IMAGE_RESOURCE_DIRECTORY_ENTRY*& first,
                       uint32_t& count) noexcept -> bool {
        if (!Inside(rootSize, directoryOffset, sizeof(IMAGE_RESOURCE_DIRECTORY))) return false;
        const auto* dir = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY*>(
            base + rootOffset + directoryOffset);
        count = static_cast<uint32_t>(dir->NumberOfNamedEntries) + dir->NumberOfIdEntries;
        if (!count || !Inside(rootSize, directoryOffset + sizeof(IMAGE_RESOURCE_DIRECTORY),
                static_cast<uint64_t>(count) * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY))) return false;
        first = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(dir + 1);
        return true;
    };

    const IMAGE_RESOURCE_DIRECTORY_ENTRY* level = nullptr;
    uint32_t count = 0;
    if (!entries(0, level, count)) return false;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY* type = nullptr;
    for (uint32_t i = 0; i < count; ++i)
        if (!level[i].NameIsString && level[i].Id == 16) { type = &level[i]; break; }
    if (!type || !type->DataIsDirectory) return false;
    if (!entries(type->OffsetToDirectory, level, count) || !level[0].DataIsDirectory) return false;
    if (!entries(level[0].OffsetToDirectory, level, count) || level[0].DataIsDirectory) return false;
    const uint32_t dataEntryOffset = level[0].OffsetToData;
    if (!Inside(rootSize, dataEntryOffset, sizeof(IMAGE_RESOURCE_DATA_ENTRY))) return false;
    const auto* entry = reinterpret_cast<const IMAGE_RESOURCE_DATA_ENTRY*>(
        base + rootOffset + dataEntryOffset);
    if (!entry->OffsetToData || entry->Size < 6 || entry->Size > 64 * 1024
        || !Inside(size, entry->OffsetToData, entry->Size)) return false;
    data = base + entry->OffsetToData;
    bytes = entry->Size;
    return true;
}

struct Node
{
    uint32_t length{};
    uint32_t valueLength{};
    uint32_t type{};
    const wchar_t* key{};
    size_t keyLength{};
    const uint8_t* value{};
    const uint8_t* children{};
    const uint8_t* end{};
};

const uint8_t* Align4(const uint8_t* origin, const uint8_t* p) noexcept
{
    const size_t offset = static_cast<size_t>(p - origin);
    return origin + ((offset + 3) & ~size_t{3});
}

bool ReadNode(const uint8_t* origin, const uint8_t* p, const uint8_t* limit, Node& node) noexcept
{
    if (p < origin || p + 6 > limit) return false;
    const auto* words = reinterpret_cast<const uint16_t*>(p);
    node.length = words[0];
    node.valueLength = words[1];
    node.type = words[2];
    if (node.length < 6 || p + node.length > limit) return false;
    node.end = p + node.length;
    node.key = reinterpret_cast<const wchar_t*>(p + 6);
    const size_t maxKey = static_cast<size_t>(node.end - (p + 6)) / sizeof(wchar_t);
    node.keyLength = wcsnlen(node.key, maxKey);
    if (node.keyLength == maxKey) return false;
    node.value = Align4(origin, p + 6 + (node.keyLength + 1) * sizeof(wchar_t));
    const size_t valueBytes = node.type == 1
        ? static_cast<size_t>(node.valueLength) * sizeof(wchar_t)
        : node.valueLength;
    if (node.value > node.end || valueBytes > static_cast<size_t>(node.end - node.value)) return false;
    node.children = Align4(origin, node.value + valueBytes);
    if (node.children > node.end) node.children = node.end;
    return true;
}

bool KeyIs(const Node& node, const wchar_t* key) noexcept
{
    const size_t length = wcslen(key);
    return node.keyLength == length && _wcsnicmp(node.key, key, length) == 0;
}

bool CopyString(const wchar_t* text, size_t available, wchar_t* output, size_t count) noexcept
{
    if (!output || !count) return false;
    const size_t length = wcsnlen(text, available);
    if (!length || length >= count) { output[0] = L'\0'; return false; }
    wmemcpy(output, text, length);
    output[length] = L'\0';
    return true;
}

bool Equals(const wchar_t* a, const wchar_t* b) noexcept
{
    return a && b && _wcsicmp(a, b) == 0;
}

bool Contains(const wchar_t* text, const wchar_t* part) noexcept
{
    const size_t length = wcslen(part);
    for (; text && *text; ++text)
        if (_wcsnicmp(text, part, length) == 0) return true;
    return false;
}

// The executable's file name from the process parameters (fixed for the process; no loader list walk).
bool ExecutableName(wchar_t* output, size_t count) noexcept
{
    output[0] = L'\0';
    const auto* peb = reinterpret_cast<const PEB*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    if (!peb || !peb->ProcessParameters) return false;
    const UNICODE_STRING& path = peb->ProcessParameters->ImagePathName;
    if (!path.Buffer || !path.Length) return false;
    const size_t length = path.Length / sizeof(wchar_t);
    size_t start = length;
    while (start && path.Buffer[start - 1] != L'\\' && path.Buffer[start - 1] != L'/') --start;
    return CopyString(path.Buffer + start, length - start, output, count);
}

uint32_t Mode() noexcept
{
    const uint32_t mode = gUiMode.load(std::memory_order_acquire);
    return mode == static_cast<uint32_t>(UiMode::Default) ? static_cast<uint32_t>(DefaultUiMode()) : mode;
}

bool ModulePathFromLoader(HMODULE module, wchar_t* output, size_t count) noexcept
{
    // The loader list is stable while the loader lock is held (DllMain and
    // loader notifications); elsewhere GetModuleFileNameW takes the lock.
    output[0] = L'\0';
    const auto* peb = reinterpret_cast<const PEB*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    if (!peb || !peb->Ldr) return false;
    const LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    size_t guard = 0;
    for (const LIST_ENTRY* link = head->Flink; link && link != head && guard < 4096;
         link = link->Flink, ++guard)
    {
        const auto* entry = CONTAINING_RECORD(link, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (entry->DllBase != module) continue;
        const UNICODE_STRING& name = entry->FullDllName;
        if (!name.Buffer || !name.Length) return false;
        return CopyString(name.Buffer, name.Length / sizeof(wchar_t), output, count);
    }
    return false;
}

void Publish(HMODULE module, Route route) noexcept
{
    const uint8_t* base = nullptr;
    uint32_t size = 0;
    if (!ImageBounds(module, base, size)) return;
    uintptr_t expected = 0;
    const uintptr_t wanted = reinterpret_cast<uintptr_t>(module);
    if (!gBase.compare_exchange_strong(expected, wanted, std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        // First OptiScaler image wins; a plugin call upgrades the route only
        // for that same image.
        if (expected == wanted && route == Route::Plugin)
            gRoute.store(static_cast<uint32_t>(Route::Plugin), std::memory_order_release);
        return;
    }
    gImageSize.store(size, std::memory_order_release);
    ReadVersionString(module, L"FileVersion", gFileVersion, _countof(gFileVersion));
    ReadVersionString(module, L"ProductVersion", gProductVersion, _countof(gProductVersion));
    ModulePathFromLoader(module, gPath, _countof(gPath));
    gRoute.store(static_cast<uint32_t>(route), std::memory_order_release);
    gPublished.store(true, std::memory_order_release);
}
}

bool ReadVersionString(HMODULE module, const wchar_t* key, wchar_t* output, size_t count) noexcept
{
    if (output && count) output[0] = L'\0';
    if (!key || !output || !count) return false;
    const uint8_t* data = nullptr;
    uint32_t bytes = 0;
    if (!FindVersionResource(module, data, bytes)) return false;
    const uint8_t* limit = data + bytes;
    Node root{};
    if (!ReadNode(data, data, limit, root) || !KeyIs(root, L"VS_VERSION_INFO")) return false;
    for (const uint8_t* p = root.children; p + 6 <= root.end;)
    {
        Node block{};
        if (!ReadNode(data, p, root.end, block)) return false;
        if (KeyIs(block, L"StringFileInfo"))
        {
            for (const uint8_t* t = block.children; t + 6 <= block.end;)
            {
                Node table{};
                if (!ReadNode(data, t, block.end, table)) return false;
                for (const uint8_t* s = table.children; s + 6 <= table.end;)
                {
                    Node string{};
                    if (!ReadNode(data, s, table.end, string)) return false;
                    if (KeyIs(string, key))
                        return CopyString(reinterpret_cast<const wchar_t*>(string.value),
                            static_cast<size_t>(string.end - string.value) / sizeof(wchar_t),
                            output, count);
                    s = Align4(data, string.end);
                }
                t = Align4(data, table.end);
            }
        }
        p = Align4(data, block.end);
    }
    return false;
}

bool IsOptiScalerImage(HMODULE module) noexcept
{
    if (!module || reinterpret_cast<uintptr_t>(module) == gSelf.load(std::memory_order_acquire))
        return false;
    wchar_t product[64]{}, original[64]{}, internal[64]{};
    if (!ReadVersionString(module, L"ProductName", product, _countof(product))
        || !Equals(product, L"OptiScaler")) return false;
    ReadVersionString(module, L"OriginalFilename", original, _countof(original));
    ReadVersionString(module, L"InternalName", internal, _countof(internal));
    return Equals(original, L"OptiScaler.dll") || Equals(internal, L"OptiScaler");
}

void ObserveProcessStart(HMODULE self) noexcept
{
    gSelf.store(reinterpret_cast<uintptr_t>(self), std::memory_order_release);
    const auto* peb = reinterpret_cast<const PEB*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    if (peb && peb->Ldr)
    {
        const LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
        size_t guard = 0;
        for (const LIST_ENTRY* link = head->Flink; link && link != head && guard < 4096;
             link = link->Flink, ++guard)
        {
            const auto* entry = CONTAINING_RECORD(link, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
            HMODULE module = static_cast<HMODULE>(entry->DllBase);
            if (module && module != self && IsOptiScalerImage(module))
            {
                Publish(module, Route::Proxy);
                break;
            }
        }
    }
    gStarted.store(true, std::memory_order_release);
}

void ObserveMappedModule(HMODULE module) noexcept
{
    if (gBase.load(std::memory_order_acquire) || !IsOptiScalerImage(module)) return;
    Publish(module, gStarted.load(std::memory_order_acquire) ? Route::Late : Route::Proxy);
}

bool AcceptPluginCaller(HMODULE caller) noexcept
{
    if (!caller || !IsOptiScalerImage(caller))
    {
        gPluginCallerRejected.store(true, std::memory_order_release);
        return false;
    }
    Publish(caller, Route::Plugin);
    if (reinterpret_cast<uintptr_t>(caller) != gBase.load(std::memory_order_acquire))
    {
        gPluginCallerRejected.store(true, std::memory_order_release);
        return false;
    }
    gPluginCallerAccepted.store(true, std::memory_order_release);
    return true;
}

void SetUiMode(UiMode mode) noexcept
{
    gUiMode.store(static_cast<uint32_t>(mode), std::memory_order_release);
}

bool ForzaTechExecutable() noexcept
{
    const HMODULE executable = GetModuleHandleW(nullptr);
    for (const wchar_t* key : {L"ProductName", L"FileDescription"})
    {
        wchar_t text[128]{};
        if (ReadVersionString(executable, key, text, _countof(text))
            && (Contains(text, L"Forza Motorsport") || Contains(text, L"Forza Horizon"))) return true;
    }
    wchar_t name[MAX_PATH]{};
    return ExecutableName(name, _countof(name)) && _wcsnicmp(name, L"forza", 5) == 0;
}

UiMode DefaultUiMode() noexcept
{
    uint32_t mode = gDefaultUiMode.load(std::memory_order_acquire);
    if (mode == kUndecided)
    {
        // Racing first callers compute the same value.
        mode = static_cast<uint32_t>(ForzaTechExecutable() ? UiMode::Hidden : UiMode::Shared);
        gDefaultUiMode.store(mode, std::memory_order_release);
    }
    // tabdefault.1: RTXMFG's tab in OptiScaler's menu.
    if (gMenuTabOffered.load(std::memory_order_acquire)) return UiMode::Hidden;
    return static_cast<UiMode>(mode);
}

void NoteMenuTabOffered() noexcept
{
    gMenuTabOffered.store(true, std::memory_order_release);
}

bool Detected() noexcept
{
    return gBase.load(std::memory_order_acquire) != 0;
}

bool UiDelegated() noexcept
{
    return Detected() && Mode() != static_cast<uint32_t>(UiMode::Full);
}

bool UiHidden() noexcept
{
    return Detected() && Mode() == static_cast<uint32_t>(UiMode::Hidden);
}

Route CurrentRoute() noexcept
{
    return static_cast<Route>(gRoute.load(std::memory_order_acquire));
}

HMODULE Module() noexcept
{
    return reinterpret_cast<HMODULE>(gBase.load(std::memory_order_acquire));
}

bool IsHostModule(HMODULE module) noexcept
{
    return module && reinterpret_cast<uintptr_t>(module) == gBase.load(std::memory_order_acquire);
}

bool IsHostAddress(const void* address) noexcept
{
    const uintptr_t base = gBase.load(std::memory_order_acquire);
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    const uint32_t size = gImageSize.load(std::memory_order_acquire);
    return base && size && value >= base && value - base < size;
}

Snapshot Read() noexcept
{
    Snapshot snapshot{};
    snapshot.route = CurrentRoute();
    snapshot.detected = Detected();
    const uint32_t mode = Mode();
    snapshot.uiOptIn = mode == static_cast<uint32_t>(UiMode::Full);
    snapshot.uiDelegated = snapshot.detected && !snapshot.uiOptIn;
    snapshot.uiHidden = snapshot.detected && mode == static_cast<uint32_t>(UiMode::Hidden);
    snapshot.pluginCallerAccepted = gPluginCallerAccepted.load(std::memory_order_acquire);
    snapshot.pluginCallerRejected = gPluginCallerRejected.load(std::memory_order_acquire);
    snapshot.base = gBase.load(std::memory_order_acquire);
    snapshot.imageSize = gImageSize.load(std::memory_order_acquire);
    if (gPublished.load(std::memory_order_acquire))
    {
        wcsncpy_s(snapshot.fileVersion, gFileVersion, _TRUNCATE);
        wcsncpy_s(snapshot.productVersion, gProductVersion, _TRUNCATE);
        wcsncpy_s(snapshot.path, gPath, _TRUNCATE);
    }
    return snapshot;
}

uint32_t ReadXefgModel(std::wstring& setting, std::wstring& path, std::wstring& key) noexcept
{
    try
    {
        setting.clear();
        path.clear();
        key.clear();
        const HMODULE host = Module();
        if (!host) return 0;
        std::wstring module(32768, L'\0');
        const DWORD length = GetModuleFileNameW(host, module.data(), static_cast<DWORD>(module.size()));
        if (!length || length >= module.size()) return 0;
        module.resize(length);
        const size_t slash = module.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return 0;
        path = module.substr(0, slash + 1) + L"OptiScaler.ini";
        for (const wchar_t* name : {L"XeFGModel", L"RtxmfgModel"})
        {
            wchar_t value[16]{};
            GetPrivateProfileStringW(L"XeFG", name, L"", value,
                static_cast<DWORD>(_countof(value)), path.c_str());
            if (!value[0]) continue;
            setting = value;
            key = name;
            if (_wcsicmp(value, L"port") == 0) return 1;
            if (_wcsicmp(value, L"default") == 0 || _wcsicmp(value, L"intel") == 0) return 2;
            return 0;
        }
        return 0;
    }
    catch (...)
    {
        setting.clear();
        path.clear();
        key.clear();
        return 0;
    }
}

std::wstring ReadFrameGenSetting(const wchar_t* key) noexcept
{
    try
    {
        std::wstring ignoredSetting, path, ignoredKey;
        ReadXefgModel(ignoredSetting, path, ignoredKey);
        if (path.empty()) return {};
        wchar_t value[64]{};
        GetPrivateProfileStringW(L"FrameGen", key, L"", value, static_cast<DWORD>(_countof(value)), path.c_str());
        return value;
    }
    catch (...)
    {
        return {};
    }
}

namespace
{
// [XeFG] `key` of the OptiScaler.ini beside the identified module (ReadXefgModel
// resolves it) into `value`, and as `setting`. eNoIni without that ini; eFailed
// when the read threw. `setting` is empty unless eRead.
enum class XefgIniRead { eNoIni, eRead, eFailed };
template<size_t N> XefgIniRead ReadXefgIni(const wchar_t* key, wchar_t (&value)[N], std::wstring& setting) noexcept
{
    try
    {
        setting.clear();
        std::wstring ignoredSetting, path, ignoredKey;
        ReadXefgModel(ignoredSetting, path, ignoredKey);
        if (path.empty()) return XefgIniRead::eNoIni;
        GetPrivateProfileStringW(L"XeFG", key, L"", value, static_cast<DWORD>(N), path.c_str());
        setting = value;
        return XefgIniRead::eRead;
    }
    catch (...)
    {
        setting.clear();
        return XefgIniRead::eFailed;
    }
}
// Switch words: off, 0, false, no, disabled / on, 1, true, yes, enabled (any case).
bool SwitchWord(const wchar_t* value, bool on) noexcept
{
    static constexpr const wchar_t* kWords[2][5]{{L"off", L"0", L"false", L"no", L"disabled"},
        {L"on", L"1", L"true", L"yes", L"enabled"}};
    for (const wchar_t* word : kWords[on ? 1 : 0])
        if (_wcsicmp(value, word) == 0) return true;
    return false;
}
// Off (also false, no, disabled) or intel (also on, stock); 0 stays a threshold.
bool SceneDetectorOffWord(const wchar_t* value) noexcept
{
    return _wcsicmp(value, L"0") != 0 && SwitchWord(value, false);
}
bool SceneDetectorWord(const wchar_t* value) noexcept
{
    if (SceneDetectorOffWord(value)) return true;
    for (const wchar_t* word : {L"intel", L"on", L"stock"})
        if (_wcsicmp(value, word) == 0) return true;
    return false;
}
}

bool XefgSceneDetectorOff(std::wstring& setting) noexcept
{
    wchar_t value[32]{};
    const auto read = ReadXefgIni(L"XeFGSceneChangeThreshold", value, setting);
    if (read != XefgIniRead::eRead) return read == XefgIniRead::eNoIni;
    return !value[0] || _wcsicmp(value, L"auto") == 0 || SceneDetectorOffWord(value);
}

bool ReadXefgSceneChangeThreshold(float& threshold, std::wstring& setting) noexcept
{
    threshold = 0.0f;
    wchar_t value[32]{};
    // The words XefgSceneDetectorOff reads set no threshold.
    if (ReadXefgIni(L"XeFGSceneChangeThreshold", value, setting) != XefgIniRead::eRead
        || !value[0] || _wcsicmp(value, L"auto") == 0 || SceneDetectorWord(value))
    {
        setting.clear();
        return false;
    }
    wchar_t* end{};
    const float parsed = wcstof(value, &end);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (!end || end == value || *end || !(parsed >= 0.0f && parsed <= 1.0f)) return false;
    threshold = parsed;
    return true;
}

bool ReadXefgTensorCores(std::wstring& setting) noexcept
{
    wchar_t value[32]{};
    return ReadXefgIni(L"XeFGTensorCores", value, setting) != XefgIniRead::eRead || !SwitchWord(value, false);
}

bool ReadXefgTensorCoresVulkanBarriers(std::wstring& setting) noexcept
{
    wchar_t value[32]{};
    return ReadXefgIni(L"XeFGTensorCoresVulkanBarriers", value, setting) == XefgIniRead::eRead && SwitchWord(value, true);
}
uint32_t ReadXefgTensorCoresMask(std::wstring& setting) noexcept
{
    constexpr uint32_t all = (1u << 17) - 1;
    wchar_t value[32]{};
    if (ReadXefgIni(L"XeFGTensorCoresMask", value, setting) != XefgIniRead::eRead || !value[0]) return all;
    wchar_t* end{};
    const auto mask = wcstoull(value, &end, 16);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    return end != value && end && !*end && mask <= all ? uint32_t(mask) : all;
}

bool ReadXefgFlipMetering(std::wstring& setting) noexcept
{
    wchar_t value[32]{};
    return ReadXefgIni(L"XeFGFlipMetering", value, setting) != XefgIniRead::eRead || !SwitchWord(value, false);
}

bool XefgFlipMeteringForced(const std::wstring& setting) noexcept
{
    return !setting.empty() && SwitchWord(setting.c_str(), true);
}

bool ReadXefgFlipMeteringLead(uint32_t& microseconds, std::wstring& setting) noexcept
{
    microseconds = 0;
    wchar_t value[16]{};
    if (ReadXefgIni(L"XeFGFlipMeteringLeadUs", value, setting) != XefgIniRead::eRead
        || !value[0] || _wcsicmp(value, L"auto") == 0)
    {
        setting.clear();
        return false;
    }
    wchar_t* end{};
    const unsigned long parsed = wcstoul(value, &end, 10);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (!end || end == value || *end || value[0] == L'-' || parsed > 8000) return false;
    microseconds = static_cast<uint32_t>(parsed);
    return true;
}

int ReadXefgAmdFlipMeter(std::wstring& setting) noexcept
{
    wchar_t value[32]{};
    if (ReadXefgIni(L"XeFGAmdFlipMeter", value, setting) != XefgIniRead::eRead) return 1;
    return SwitchWord(value, false) ? 0 : SwitchWord(value, true) ? 2 : 1;
}

// An [XeFG] key the user set: present, not empty and not "auto".
template<size_t N> bool ReadXefgSetKey(const wchar_t* key, wchar_t (&value)[N], std::wstring& setting) noexcept
{
    return ReadXefgIni(key, value, setting) == XefgIniRead::eRead && value[0] && _wcsicmp(value, L"auto") != 0;
}

uint8_t ReadXefgOutputRing(std::wstring& setting) noexcept
{
    wchar_t value[32]{};
    if (ReadXefgIni(L"XeFGOutputRing", value, setting) != XefgIniRead::eRead) return 2;
    return _wcsicmp(value, L"1") == 0 || _wcsicmp(value, L"stock") == 0 || SwitchWord(value, false) ? 1 : 2;
}

bool ReadXefgHdr10FlagOff(std::wstring& setting) noexcept
{
    wchar_t value[32]{};
    if (ReadXefgIni(L"XeFGHdr10Flag", value, setting) != XefgIniRead::eRead) return true;
    return !(_wcsicmp(value, L"stock") == 0 || _wcsicmp(value, L"intel") == 0 || SwitchWord(value, true));
}

// winequeue.1: Wine/Proton (its ntdll exports wine_get_version); the flip queue's auto default is off there.
bool RunningUnderWine() noexcept
{
    static const bool wine = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return wine;
}

// ChooseFlipQueue reads this as "the ini allows it" (Wine/Proton's auto is off there); off is the escape hatch.
bool ReadXefgFlipQueue(std::wstring& setting) noexcept
{
    wchar_t value[32]{};
    if (!ReadXefgSetKey(L"XeFGFlipQueue", value, setting)) return true;
    return !SwitchWord(value, false);
}

bool ReadXefgPacingDiagnostics(std::wstring& setting) noexcept
{
    wchar_t value[32]{};
    return ReadXefgIni(L"XeFGPacingDiagnostics", value, setting) == XefgIniRead::eRead && SwitchWord(value, true);
}

bool XefgFlipQueueSet() noexcept
{
    wchar_t value[32]{};
    std::wstring setting;
    return ReadXefgSetKey(L"XeFGFlipQueue", value, setting);
}

namespace
{
// The marker and its terminator in a section of the image that is initialized data, readable, not writable,
// not executable and not discardable (OptiScaler's .rdata), within min(VirtualSize, SizeOfRawData).
bool ImageCarriesMenuFix(HMODULE module) noexcept
{
    const uint8_t* base = nullptr;
    uint32_t size = 0;
    if (!ImageBounds(module, base, size)) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    const WORD sections = nt->FileHeader.NumberOfSections;
    if (!Inside(size, static_cast<uint64_t>(reinterpret_cast<const uint8_t*>(section)-base),
            static_cast<uint64_t>(sections)*sizeof(IMAGE_SECTION_HEADER))) return false;
    constexpr size_t length = sizeof(kMenuFixMarker);
    constexpr DWORD excluded = IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE;
    for (WORD i = 0; i < sections; ++i, ++section)
    {
        const DWORD flags = section->Characteristics;
        if (!(flags & IMAGE_SCN_CNT_INITIALIZED_DATA) || !(flags & IMAGE_SCN_MEM_READ) || (flags & excluded)) continue;
        const uint64_t bytes = std::min<uint64_t>(section->Misc.VirtualSize, section->SizeOfRawData);
        if (bytes < length || !Inside(size, section->VirtualAddress, bytes)) continue;
        const uint8_t* at = base+section->VirtualAddress;
        const uint8_t* const last = at+(bytes-length);
        while (at <= last)
        {
            at = static_cast<const uint8_t*>(std::memchr(at, kMenuFixMarker[0], static_cast<size_t>(last-at)+1));
            if (!at) break;
            if (std::memcmp(at, kMenuFixMarker, length) == 0) return true;
            ++at;
        }
    }
    return false;
}

std::atomic<uint32_t> gMenuBuffers{0};
}

uint32_t MenuBackBuffers() noexcept
{
    const uint32_t known = gMenuBuffers.load(std::memory_order_acquire);
    if (known) return known;
    // Nothing identified yet: the limit without the menu fix, not remembered (the first identified image decides).
    if (!gPublished.load(std::memory_order_acquire)) return kMenuBackBuffers;
    const uint32_t buffers = ImageCarriesMenuFix(Module()) ? kFixedMenuBackBuffers : kMenuBackBuffers;
    gMenuBuffers.store(buffers, std::memory_order_release);
    return buffers;
}

uint32_t ReadXefgNativeBuffers(std::wstring& setting, uint32_t& requested) noexcept
{
    const uint32_t limit = MenuBackBuffers();
    const uint32_t fallback = limit >= kFixedMenuBackBuffers ? kDefaultNativeBuffers : kMenuBackBuffers;
    requested = fallback;
    wchar_t value[16]{};
    if (ReadXefgIni(L"XeFGNativeBuffers", value, setting) != XefgIniRead::eRead || !value[0] || _wcsicmp(value, L"auto") == 0)
        return requested;
    if (SwitchWord(value, false)) return requested = 0;
    wchar_t* end{};
    const unsigned long parsed = wcstoul(value, &end, 10);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (!end || end == value || *end || value[0] == L'-' || parsed < 2 || parsed > kFixedMenuBackBuffers) return requested;
    requested = static_cast<uint32_t>(parsed);
    return requested > limit ? limit : requested;
}

namespace
{
std::atomic<bool> gFakeNvapi{false};
}

bool IsFakeNvapi(HMODULE nvapi, NvapiQueryInterface query) noexcept
{
    if (!nvapi || !query) return false;
    bool fake = false;
    wchar_t path[MAX_PATH]{}, system[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nvapi, path, MAX_PATH);
    const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
    if (length && length < MAX_PATH && systemLength && systemLength < MAX_PATH - 1)
    {
        const wchar_t* leaf = wcsrchr(path, L'\\');
        fake = !leaf || static_cast<size_t>(leaf - path) != systemLength
            || _wcsnicmp(path, system, systemLength) != 0;
    }
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(query), &owner) && owner != nvapi)
        fake = true;
    __try { if (query(0x21382138u)) fake = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { fake = true; }
    if (fake) NoteFakeNvapi();
    return fake;
}

void NoteFakeNvapi() noexcept
{
    gFakeNvapi.store(true, std::memory_order_release);
}

bool FakeNvapiObserved() noexcept
{
    return gFakeNvapi.load(std::memory_order_acquire);
}

const char* RouteName(Route route) noexcept
{
    switch (route)
    {
    case Route::Proxy: return "proxy";
    case Route::Plugin: return "plugin";
    case Route::Late: return "late";
    default: return "none";
    }
}
}
