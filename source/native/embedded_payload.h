#pragma once
// slim.1: GPU payloads the build packs into one LZMA-compressed RCDATA resource per group (pack_embedded_payloads.py).
// Loaders still ask by each payload's old resource id; the group is decoded once per process.
#include <Windows.h>
#include <cstdint>
#include <span>

namespace embedded_payload
{
inline constexpr uint32_t kSlots = 8;

struct Group
{
    uint32_t slot;         // Below kSlots, one per group in a module.
    uint32_t resource;     // RCDATA id of the packed .lzma stream.
    uint32_t packedBytes;
    uint32_t rawBytes;
    const char* rawSha256; // Lower-case hex of the decoded group.
    const char* name;
};

struct Item
{
    uint32_t id;           // The payload's resource id before packing.
    uint32_t offset;
    uint32_t bytes;
};

// Payload `id` of `group` in `module`. The first call for a group decodes it and checks its size and SHA-256; the
// decoded bytes stay for the process. Empty when the resource is missing or fails to decode or match (the whole group
// then stays unavailable, logged once), or when `id` is not one of `items`.
std::span<const uint8_t> Find(HMODULE module, const Group& group, std::span<const Item> items, uint32_t id) noexcept;

// The module this code is linked into, which holds its packed resources.
HMODULE Owner() noexcept;

// Where a group that fails to decode is reported (single_module::Log in the module).
using LogFunction = void (*)(const wchar_t* text) noexcept;
void SetLog(LogFunction log) noexcept;
}
