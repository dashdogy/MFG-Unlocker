#include "embedded_payload.h"
#include "sha256_util.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

namespace embedded_payload
{
namespace
{
struct Slot
{
    std::once_flag once;
    HMODULE module = nullptr;
    Group group{};
    std::span<const Item> items;
    std::unique_ptr<const uint8_t*[]> data; // Each item's resource bytes; null unless the whole group matched.
};
Slot gSlots[kSlots];
std::atomic<LogFunction> gLog{nullptr};

// Compares without allocating (Check is noexcept).
bool MatchesHex(const sha256_util::Digest& digest, const char* hex) noexcept
{
    const auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < digest.size(); ++i)
    {
        const int high = nibble(hex[2 * i]), low = high < 0 ? -1 : nibble(hex[2 * i + 1]);
        if (low < 0 || digest[i] != static_cast<uint8_t>(high * 16 + low)) return false;
    }
    return hex[2 * digest.size()] == '\0';
}

// Every item's resource bytes, or null with the reason in `why`. Nothing is returned unless every item is present at
// its size and offset and the joined bytes match the group's size and SHA-256.
std::unique_ptr<const uint8_t*[]> Check(HMODULE module, const Group& group, std::span<const Item> items,
    wchar_t (&why)[160]) noexcept
{
    if (items.empty() || !group.rawBytes)
    {
        swprintf_s(why, L"empty table");
        return {};
    }
    std::unique_ptr<const uint8_t*[]> data(new (std::nothrow) const uint8_t*[items.size()]);
    if (!data)
    {
        swprintf_s(why, L"no memory for %zu entries", items.size());
        return {};
    }
    sha256_util::Hasher hasher;
    uint64_t joined = 0;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const Item& item = items[i];
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(item.id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
        const auto* bytes = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
        const DWORD size = resource ? SizeofResource(module, resource) : 0;
        if (!bytes)
        {
            swprintf_s(why, L"payload %u missing", item.id);
            return {};
        }
        if (size != item.bytes || item.offset != joined)
        {
            swprintf_s(why, L"payload %u is %lu bytes at %llu, expected %u at %u", item.id, size,
                static_cast<unsigned long long>(joined), item.bytes, item.offset);
            return {};
        }
        if (!hasher.Update(bytes, size))
        {
            swprintf_s(why, L"SHA-256 unavailable");
            return {};
        }
        data[i] = bytes;
        joined += size;
    }
    if (joined != group.rawBytes)
    {
        swprintf_s(why, L"%llu bytes in all, expected %u", static_cast<unsigned long long>(joined), group.rawBytes);
        return {};
    }
    sha256_util::Digest digest{};
    if (!hasher.Finish(digest) || !MatchesHex(digest, group.rawSha256))
    {
        swprintf_s(why, L"SHA-256 differs from the build's");
        return {};
    }
    return data;
}
}

std::span<const uint8_t> Find(HMODULE module, const Group& group, std::span<const Item> items, uint32_t id) noexcept
{
    // A null module never claims the slot, so a later call with the real module still checks the group.
    if (!module || group.slot >= kSlots || !group.rawSha256) return {};
    Slot& slot = gSlots[group.slot];
    std::call_once(slot.once, [&]() noexcept {
        slot.module = module;
        slot.group = group;
        slot.items = items;
        wchar_t why[160]{};
        slot.data = Check(module, group, items, why);
        if (slot.data) return;
        wchar_t line[320]{};
        swprintf_s(line, L"Embedded payloads: group %hs refused: %ls; its payloads stay unavailable",
            group.name ? group.name : "?", why);
        if (const LogFunction log = gLog.load(std::memory_order_acquire)) log(line);
    });
    // A slot belongs to the group that first used it: same module, table and digest.
    if (!slot.data || slot.module != module || slot.items.data() != items.data() || slot.items.size() != items.size()
        || slot.group.rawBytes != group.rawBytes || std::strcmp(slot.group.rawSha256, group.rawSha256) != 0)
        return {};
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id == id) return {slot.data[i], items[i].bytes};
    return {};
}

HMODULE Owner() noexcept
{
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
               reinterpret_cast<LPCWSTR>(&Owner), &module) ? module : nullptr;
}

void SetLog(LogFunction log) noexcept { gLog.store(log, std::memory_order_release); }
}
