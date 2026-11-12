// embedded_payload::Find on fixture groups packed by pack_embedded_payloads.py (CMakeLists.txt, make_fixtures.py).
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

// Every payload of a refused group comes back empty, on the first call and on later ones.
bool Refused(HMODULE module, const Group& group)
{
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& item : fixtures_items)
            if (!embedded_payload::Find(module, group, fixtures_items, item.id).empty()) return false;
    return true;
}

size_t LinesFor(const Group& group)
{
    wchar_t key[96]{};
    swprintf_s(key, L"group %hs (resource %u) refused: ", group.name, group.resource);
    return static_cast<size_t>(std::count_if(gLog.begin(), gLog.end(),
        [&](const std::wstring& line) { return line.find(key) != std::wstring::npos; }));
}
}

int main()
{
    embedded_payload::SetLog(&Capture);
    const HMODULE self = embedded_payload::Owner();
    Expect(self && self == GetModuleHandleW(nullptr), "Owner() is the test executable");

    for (const auto& file : kFixtureFiles)
    {
        const auto bytes = embedded_payload::Find(self, fixtures, fixtures_items, file.id);
        const auto expected = ReadFile(file.path);
        Expect(!expected.empty() && std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()),
            "a payload of the good group equals its fixture file");
    }
    Expect(embedded_payload::Find(self, fixtures, fixtures_items, 9999).empty(), "an id outside the table is empty");
    Expect(gLog.empty(), "the good group logs nothing");

    const Group damaged[] = {
        {1u, 3951u, kFlippedBytes, fixtures.rawBytes, fixtures.rawSha256, "flipped"},
        {2u, 3952u, kTruncatedBytes, fixtures.rawBytes, fixtures.rawSha256, "truncated"},
        {3u, fixtures.resource, fixtures.packedBytes, fixtures.rawBytes,
            "0000000000000000000000000000000000000000000000000000000000000000", "wrong_sha256"},
        {4u, fixtures.resource, fixtures.packedBytes + 1, fixtures.rawBytes, fixtures.rawSha256, "wrong_packed_size"},
        {5u, fixtures.resource, fixtures.packedBytes, fixtures.rawBytes + 4, fixtures.rawSha256, "wrong_raw_size"},
        {6u, 3999u, fixtures.packedBytes, fixtures.rawBytes, fixtures.rawSha256, "missing"},
    };
    for (const Group& group : damaged)
    {
        char what[96]{};
        std::snprintf(what, sizeof(what), "%s: every payload refused, logged once", group.name);
        Expect(Refused(self, group) && LinesFor(group) == 1, what);
    }
    Expect(gLog.size() == std::size(damaged), "one log line per refused group, none more");

    // A call without a module is refused and leaves the slot free for the real module.
    const Group late{7u, fixtures.resource, fixtures.packedBytes, fixtures.rawBytes, fixtures.rawSha256, "late"};
    Expect(embedded_payload::Find(nullptr, late, fixtures_items, kFixtureFiles[2].id).empty()
        && embedded_payload::Find(self, late, fixtures_items, kFixtureFiles[2].id).size() == ReadFile(kFixtureFiles[2].path).size(),
        "a null module does not claim the slot");

    // A slot belongs to the first group that used it: another resource on the good group's slot gets nothing.
    const Group other{fixtures.slot, 3951u, kFlippedBytes, fixtures.rawBytes, fixtures.rawSha256, "other"};
    Expect(embedded_payload::Find(self, other, fixtures_items, kFixtureFiles[0].id).empty(), "a slot keeps its group");
    Expect(embedded_payload::Find(self, Group{embedded_payload::kSlots, fixtures.resource, fixtures.packedBytes,
        fixtures.rawBytes, fixtures.rawSha256, "out_of_range"}, fixtures_items, kFixtureFiles[0].id).empty(),
        "a slot beyond kSlots is refused");

    std::printf("%s (%d failed)\n", gFailures ? "EMBEDDED_PAYLOAD_TESTS_FAIL" : "EMBEDDED_PAYLOAD_TESTS_PASS", gFailures);
    return gFailures ? 1 : 0;
}
