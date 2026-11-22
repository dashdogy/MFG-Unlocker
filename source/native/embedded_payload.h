#pragma once
// GPU payloads: each one is its own uncompressed RCDATA resource at its own id, as up to V1.7.0. A generated payload
// list is one group (pack_embedded_payloads.py): its ids in order, each one's size, and the SHA-256 of their bytes
// joined in that order, checked once per process before any payload is handed out.
// V1.7.2 nolzma.1 dropped V1.7.1 slim.1's LZMA packing (one compressed stream per group, decoded at run time); the
// loaders' interface did not change.
#include <Windows.h>
#include <cstdint>
#include <span>

namespace embedded_payload
{
inline constexpr uint32_t kSlots = 8;

struct Group
{
    uint32_t slot;         // Below kSlots, one per group in a module.
    uint32_t rawBytes;     // The group's payloads together.
    const char* rawSha256; // Lower-case hex of the payloads joined in table order.
    const char* name;
};

struct Item
{
    uint32_t id;           // The payload's RCDATA id.
    uint32_t offset;       // Where its bytes start in the joined group the SHA-256 covers.
    uint32_t bytes;
};

// Payload `id` of `group` in `module`: its bytes inside the module's resource data, valid while the module stays loaded.
// The first call for a group finds every payload in `items` and checks each size and the group's size and SHA-256;
// the result stays for the process. Empty when a payload is missing or a size or the digest differs (the whole group
// then stays unavailable, logged once), or when `id` is not one of `items`.
std::span<const uint8_t> Find(HMODULE module, const Group& group, std::span<const Item> items, uint32_t id) noexcept;

// The module this code is linked into, which holds its payload resources.
HMODULE Owner() noexcept;

// Where a group that fails its check is reported (single_module::Log in the module).
using LogFunction = void (*)(const wchar_t* text) noexcept;
void SetLog(LogFunction log) noexcept;
}
