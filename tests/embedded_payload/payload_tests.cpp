// embedded_payload::Find on fixture groups listed by pack_embedded_payloads.py (CMakeLists.txt, make_fixtures.py).
#include "embedded_payload.h"
#include "fixtures.packed.h"
#include "damaged.h"
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
using embedded_payload::Group;
using embedded_payload::Item;
using embedded_payload::packed::fixtures;
using embedded_payload::packed::fixtures_items;

int gFailures = 0;
std::vector<std::wstring> gLog;

void Capture(const wchar_t* text) noexcept
{
    try { gLog.emplace_back(text); }
    catch (...) {}
}

void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++gFailures;
}

std::vector<uint8_t> ReadFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// The module's own resource bytes for `id`, where Find must point.
const uint8_t* ResourceBytes(HMODULE module, uint32_t id)
{
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    return loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
}

// Every payload of a refused group comes back empty, on the first call and on later ones.
bool Refused(HMODULE module, const Group& group, std::span<const Item> items)
{
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& item : items)
            if (!embedded_payload::Find(module, group, items, item.id).empty()) return false;
    return true;
}

size_t LinesFor(const Group& group)
{
    wchar_t key[96]{};
    swprintf_s(key, L"group %hs refused: ", group.name);
    return static_cast<size_t>(std::count_if(gLog.begin(), gLog.end(),
        [&](const std::wstring& line) { return line.find(key) != std::wstring::npos; }));
}

// The good table with one change.
std::vector<Item> Changed(void (*change)(std::vector<Item>&))
{
    std::vector<Item> items(std::begin(fixtures_items), std::end(fixtures_items));
    change(items);
    return items;
}
}

int main()
{
    embedded_payload::SetLog(&Capture);
    const HMODULE self = embedded_payload::Owner();
    Expect(self && self == GetModuleHandleW(nullptr), "Owner() is the test executable");
    Expect(std::size(fixtures_items) == std::size(kFixtureFiles), "the group lists every fixture file");

    for (const auto& file : kFixtureFiles)
    {
        const auto bytes = embedded_payload::Find(self, fixtures, fixtures_items, file.id);
        const auto expected = ReadFile(file.path);
        Expect(!expected.empty() && std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()),
            "a payload of the good group equals its fixture file");
        Expect(bytes.data() && bytes.data() == ResourceBytes(self, file.id),
            "a payload is its own uncompressed resource, returned in place");
    }
    Expect(embedded_payload::Find(self, fixtures, fixtures_items, 9999).empty(), "an id outside the table is empty");
    Expect(gLog.empty(), "the good group logs nothing");

    const auto wrongOffset = Changed([](std::vector<Item>& items) { items[1].offset += 4; });
    const auto missing = Changed([](std::vector<Item>& items) { items[2].id = 9999; });
    struct Damaged { Group group; std::span<const Item> items; };
    const Damaged damaged[] = {
        {{1u, fixtures.rawBytes, fixtures.rawSha256, "flipped"}, kFlippedItems},
        {{2u, fixtures.rawBytes, fixtures.rawSha256, "truncated"}, kTruncatedItems},
        {{3u, fixtures.rawBytes, "0000000000000000000000000000000000000000000000000000000000000000", "wrong_sha256"},
            fixtures_items},
        {{4u, fixtures.rawBytes + 4, fixtures.rawSha256, "wrong_raw_size"}, fixtures_items},
        {{5u, fixtures.rawBytes, fixtures.rawSha256, "wrong_offset"}, wrongOffset},
        {{6u, fixtures.rawBytes, fixtures.rawSha256, "missing"}, missing},
    };
    for (const Damaged& entry : damaged)
    {
        char what[96]{};
        std::snprintf(what, sizeof(what), "%s: every payload refused, logged once", entry.group.name);
        Expect(Refused(self, entry.group, entry.items) && LinesFor(entry.group) == 1, what);
    }
    Expect(gLog.size() == std::size(damaged), "one log line per refused group, none more");

    // A call without a module is refused and leaves the slot free for the real module.
    const Group late{7u, fixtures.rawBytes, fixtures.rawSha256, "late"};
    Expect(embedded_payload::Find(nullptr, late, fixtures_items, kFixtureFiles[2].id).empty()
        && embedded_payload::Find(self, late, fixtures_items, kFixtureFiles[2].id).size() == ReadFile(kFixtureFiles[2].path).size(),
        "a null module does not claim the slot");

    // A slot belongs to the first group that used it: another table or digest on the good group's slot gets nothing.
    Expect(embedded_payload::Find(self, fixtures, kFlippedItems, 9011).empty(), "a slot keeps its table");
    Expect(embedded_payload::Find(self, Group{fixtures.slot, fixtures.rawBytes,
        "1111111111111111111111111111111111111111111111111111111111111111", "other"}, fixtures_items,
        kFixtureFiles[0].id).empty(), "a slot keeps its digest");
    Expect(embedded_payload::Find(self, Group{embedded_payload::kSlots, fixtures.rawBytes, fixtures.rawSha256,
        "out_of_range"}, fixtures_items, kFixtureFiles[0].id).empty(), "a slot beyond kSlots is refused");
    Expect(gLog.size() == std::size(damaged), "slot checks log nothing");

    std::printf("%s (%d failed)\n", gFailures ? "EMBEDDED_PAYLOAD_TESTS_FAIL" : "EMBEDDED_PAYLOAD_TESTS_PASS", gFailures);
    return gFailures ? 1 : 0;
}
