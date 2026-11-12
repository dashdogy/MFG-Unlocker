#include "embedded_payload.h"
#include "sha256_util.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
// Last: 7zTypes.h defines True and False as macros.
#include "third_party/lzma/LzmaDec.h"

namespace embedded_payload
{
namespace
{
struct Slot
{
    std::once_flag once;
    HMODULE module = nullptr;
    uint32_t resource = 0;
    std::unique_ptr<uint8_t[]> bytes;
};
Slot gSlots[kSlots];
std::atomic<LogFunction> gLog{nullptr};

void* Allocate(ISzAllocPtr, size_t bytes) { return ::operator new(bytes, std::nothrow); }
void Release(ISzAllocPtr, void* address) { ::operator delete(address); }
const ISzAlloc kAllocator{Allocate, Release};

// .lzma header: LZMA_PROPS_SIZE property bytes, then the 8-byte size, which the packer leaves unknown.
constexpr size_t kHeaderBytes = LZMA_PROPS_SIZE + 8;

// Compares without allocating (Decode is noexcept).
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

// The decoded group, or null with the reason in `why`. Nothing is returned unless every byte matched.
std::unique_ptr<uint8_t[]> Decode(HMODULE module, const Group& group, wchar_t (&why)[160]) noexcept
{
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(group.resource), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const auto* packed = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
    const DWORD size = resource ? SizeofResource(module, resource) : 0;
    if (!packed)
    {
        swprintf_s(why, L"resource missing");
        return {};
    }
    if (size != group.packedBytes || size <= kHeaderBytes || !group.rawBytes)
    {
        swprintf_s(why, L"packed size %lu, expected %u", size, group.packedBytes);
        return {};
    }
    std::unique_ptr<uint8_t[]> raw(new (std::nothrow) uint8_t[group.rawBytes]);
    if (!raw)
    {
        swprintf_s(why, L"no memory for %u bytes", group.rawBytes);
        return {};
    }
    SizeT rawBytes = group.rawBytes, packedBytes = size - kHeaderBytes;
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes result = LzmaDecode(raw.get(), &rawBytes, packed + kHeaderBytes, &packedBytes, packed, LZMA_PROPS_SIZE,
        LZMA_FINISH_END, &status, &kAllocator);
    if (result != SZ_OK || rawBytes != group.rawBytes
        || (status != LZMA_STATUS_FINISHED_WITH_MARK && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK))
    {
        swprintf_s(why, L"LZMA decode result %d, status %d, %zu of %u bytes", result, static_cast<int>(status),
            static_cast<size_t>(rawBytes), group.rawBytes);
        return {};
    }
    sha256_util::Digest digest{};
    if (!sha256_util::Compute(raw.get(), rawBytes, digest) || !MatchesHex(digest, group.rawSha256))
    {
        swprintf_s(why, L"decoded SHA-256 differs from the build's");
        return {};
    }
    return raw;
}
}

std::span<const uint8_t> Find(HMODULE module, const Group& group, std::span<const Item> items, uint32_t id) noexcept
{
    // A null module never claims the slot, so a later call with the real module still decodes the group.
    if (!module || group.slot >= kSlots) return {};
    Slot& slot = gSlots[group.slot];
    std::call_once(slot.once, [&]() noexcept {
        slot.module = module;
        slot.resource = group.resource;
        wchar_t why[160]{};
        slot.bytes = Decode(module, group, why);
        if (slot.bytes) return;
        wchar_t line[320]{};
        swprintf_s(line, L"Embedded payloads: group %hs (resource %u) refused: %ls; its payloads stay unavailable",
            group.name, group.resource, why);
        if (const LogFunction log = gLog.load(std::memory_order_acquire)) log(line);
    });
    if (!slot.bytes || slot.module != module || slot.resource != group.resource) return {};
    for (const Item& item : items)
    {
        if (item.id != id) continue;
        if (item.offset > group.rawBytes || item.bytes > group.rawBytes - item.offset) return {};
        return {slot.bytes.get() + item.offset, item.bytes};
    }
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
