#include "ampere_wrapper_capacity.h"
#include "ampere_legacy_wrapper_contracts.h"
#include "ampere_ota_wrapper_contracts.h"
#include <cstring>
#include <iostream>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
template<class T> void Put(uintptr_t base, size_t offset, T value)
{ std::memcpy(reinterpret_cast<void*>(base + offset), &value, sizeof(value)); }
// diag171.1: the step ReadFailure() names for the 171 log line.
bool Stage(const ampere_wrapper::Layout& layout, const char* expected)
{
    const char* actual = ampere_wrapper::ReadFailure(layout);
    if (std::strcmp(actual, expected)) std::cout << "  ReadFailure: " << actual << " (expected " << expected << ")\n";
    return !std::strcmp(actual, expected);
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    const unsigned expected = wcstoul(argv[2], nullptr, 10);
    HMODULE module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Check(module != nullptr, "exact local wrapper maps without executing its DLL entry or plugin startup");
    if (!module) return 2;
    ampere_wrapper::Layout layout{};
    Check(ampere_wrapper::Discover(module, layout), "four independently decoded code contracts match the real wrapper");
    if (failures) { FreeLibrary(module); return 1; }
    Check(static_cast<unsigned>(layout.profile) == expected, "actual wrapper selects the expected allocation profile");
    const bool pacer = layout.profile == ampere_wrapper::Profile::eLegacyPacer;
    const bool small = layout.profile == ampere_wrapper::Profile::eLegacySmall;
    const bool large = layout.profile == ampere_wrapper::Profile::eLegacyLarge;
    const bool direct = layout.profile == ampere_wrapper::Profile::eLegacyDirect;
    const bool legacy = pacer || small || large || layout.profile == ampere_wrapper::Profile::eLegacyCompact;
    // Streamline 2.12's direct ring sits on the context like the compact one: MFG, not 2x (GitHub issue 153).
    Check(layout.maximumGeneratedFrames == (legacy ? 1u : 5u),
        "pacer-ring legacy wrapper falls back to 2x; compact, extended and direct wrappers retain MFG capacity");
    Check(layout.frameIdAvailable == !pacer, "only the audited pre-frame-ID wrapper uses native-call interval ordinals");
    Check(layout.createEnd > layout.createBegin, "Create caller range is established by the image unwind table");
    size_t maximum, swapchain, count, buffers;
    if (pacer) { maximum = 0x10d00; swapchain = 0x3538; count = 0x60; buffers = 0x48; }
    else if (small) { maximum = 0x39f8; swapchain = 0x3538; count = 0x60; buffers = 0x48; }
    else if (large) { maximum = 0x45c8; swapchain = 0x4138; count = 0x60; buffers = 0x48; }
    else if (direct) { maximum = 0x45e4; swapchain = 0x4150; count = 0x4158; buffers = 0x42c0; }
    else if (legacy) { maximum = 0x45a4; swapchain = 0x4120; count = 0x58; buffers = 0x40; }
    else if (layout.profile == ampere_wrapper::Profile::eCompact)
    { maximum = 0x45dc; swapchain = 0x4150; count = 0x4158; buffers = 0x42c0; }
    else { maximum = 0x460c; swapchain = 0x4180; count = 0x4188; buffers = 0x42f0; }
    const uintptr_t context = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 0x12000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    const uintptr_t ring = legacy ? reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) : context;
    const uintptr_t storage = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(context && ring && storage, "fixture owns private context, pacer and buffer allocations");
    if (!context || !ring || !storage) return 2;
    auto wrongOwner = layout; wrongOwner.contextSlot = storage;
    Check(!ampere_wrapper::BeforeSwapchain(wrongOwner), "private storage cannot impersonate an image-owned context slot");
    uintptr_t original = 0; std::memcpy(&original, reinterpret_cast<void*>(layout.contextSlot), sizeof(original));
    ampere_wrapper::Capacity early{};
    Put(layout.contextSlot, 0, uintptr_t{0});
    Check(!ampere_wrapper::BeforeSwapchain(layout), "missing context cannot bootstrap a recognized wrapper");
    Check(Stage(layout, "context-null"), "171 diagnostics name a missing wrapper context");
    Put(layout.contextSlot, 0, context);
    if (legacy)
    {
        Check(ampere_wrapper::BeforeSwapchain(layout), "legacy capabilities bootstrap before plugin startup allocates its pacer");
        Check(!ampere_wrapper::Read(layout, early), "early capabilities do not admit Create without a pacer and allocated resources");
        Check(Stage(layout, "ring-null"), "171 diagnostics name a missing pacer");
        Put(context, swapchain, uintptr_t{0x1000});
        Check(!ampere_wrapper::BeforeSwapchain(layout), "an allocated swapchain forbids bootstrap even without a pacer");
        const auto observation = ampere_wrapper::InspectFailure(layout);
        Check(observation.contextReadable && observation.capacity.swapchain == 0x1000
            && !observation.capacity.ringOwner, "startup diagnostics preserve an existing swapchain even when the pacer is absent");
        Put(context, swapchain, uintptr_t{0});
        Put(context, 0x18, uintptr_t{1});
        Check(!ampere_wrapper::BeforeSwapchain(layout), "an unreadable existing pacer cannot bootstrap");
    }
    if (legacy) Put(context, 0x18, ring);
    Check(ampere_wrapper::BeforeSwapchain(layout), "early bootstrap requires an unallocated current context");
    Check(Stage(layout, "count-out-of-range"), "171 diagnostics name an empty ring (no swap chain set up) count-out-of-range");
    Put(ring, buffers, storage); Put(ring, buffers + 8, storage + sizeof(uintptr_t));
    Check(!ampere_wrapper::BeforeSwapchain(layout), "allocated presentation resources close the bootstrap window");
    Put(ring, buffers, uintptr_t{0}); Put(ring, buffers + 8, uintptr_t{0});
    Put(context, maximum, layout.maximumGeneratedFrames);
    Put(context, swapchain, uintptr_t{0x1000});
    Check(!ampere_wrapper::BeforeSwapchain(layout), "allocated swapchain closes the bootstrap window");
    for (unsigned n = legacy ? 2 : 6; n <= 6; ++n)
    {
        Put(ring, count, n); Put(ring, buffers, storage);
        Put(ring, buffers + 8, storage + n * sizeof(uintptr_t));
        Put(ring, buffers + 16, storage + n * sizeof(uintptr_t));
        for (unsigned i = 0; i < n; ++i) Put(storage, i * sizeof(uintptr_t), uintptr_t{0x2000 + i * 0x100});
        ampere_wrapper::Capacity capacity{};
        Check(ampere_wrapper::Read(layout, capacity) && capacity.bufferCount == n
            && capacity.maximumGeneratedFrames == layout.maximumGeneratedFrames, "actual ring count certifies only its selected fixed generated capacity");
        Check(Stage(layout, "readable"), "171 diagnostics call a certifiable allocation readable");
        Put(context, swapchain, uintptr_t{0});
        ampere_wrapper::Capacity noSwapchain{};
        Check(!ampere_wrapper::Read(layout, noSwapchain) && Stage(layout, "swapchain-null"),
            "a filled ring without its swap chain is refused and named swapchain-null");
        Put(context, swapchain, uintptr_t{0x1000});
        Check(ampere_wrapper::Matches(layout, capacity), "unchanged exact allocation survives frame-path revalidation");
        Check(ampere_wrapper::RefreshedBuffers(capacity, capacity) == 0, "an unchanged allocation is not a buffer refresh");
        // A swap-chain resize re-acquires every buffer object into the same vector.
        for (unsigned i = 0; i < n; ++i) Put(storage, i * sizeof(uintptr_t), uintptr_t{0x7000 + i * 0x100});
        ampere_wrapper::Capacity refreshed{};
        Check(!ampere_wrapper::Matches(layout, capacity) && ampere_wrapper::Read(layout, refreshed)
            && ampere_wrapper::RefreshedBuffers(capacity, refreshed) == n,
            "a complete refill of the same ring is a refresh of every buffer");
        Check(ampere_wrapper::Matches(layout, refreshed), "the refreshed snapshot revalidates on the frame path");
        Put(storage, 0, uintptr_t{0x7777});
        ampere_wrapper::Capacity partial{};
        Check(ampere_wrapper::Read(layout, partial) && ampere_wrapper::RefreshedBuffers(refreshed, partial) == 1,
            "a refill that replaces one buffer object reports one changed entry");
        Put(context, swapchain, uintptr_t{0x1100});
        ampere_wrapper::Capacity otherSwapchain{};
        Check(ampere_wrapper::Read(layout, otherSwapchain) && ampere_wrapper::RefreshedBuffers(refreshed, otherSwapchain) == 0,
            "buffers of another swap chain are never a refresh");
        Put(context, swapchain, uintptr_t{0x1000});
        auto changed = refreshed; changed.buffersBegin += sizeof(uintptr_t); changed.buffersEnd += sizeof(uintptr_t);
        Check(ampere_wrapper::RefreshedBuffers(capacity, changed) == 0, "moved vector storage is never a refresh");
        changed = refreshed; changed.buffersCapacity += sizeof(uintptr_t);
        Check(ampere_wrapper::RefreshedBuffers(capacity, changed) == 0, "reallocated vector capacity is never a refresh");
        changed = refreshed; changed.bufferCount = n - 1;
        Check(ampere_wrapper::RefreshedBuffers(capacity, changed) == 0, "a changed buffer count is never a refresh");
        changed = refreshed; changed.maximumGeneratedFrames ^= 4u;
        Check(ampere_wrapper::RefreshedBuffers(capacity, changed) == 0, "a changed generated maximum is never a refresh");
        changed = refreshed; changed.context += 0x10; changed.ringOwner += 0x10;
        Check(ampere_wrapper::RefreshedBuffers(capacity, changed) == 0, "another wrapper context is never a refresh");
        auto empty = capacity; empty.context = 0;
        Check(ampere_wrapper::RefreshedBuffers(empty, refreshed) == 0, "no stored allocation proof means no refresh");
        for (unsigned i = 0; i < n; ++i) Put(storage, i * sizeof(uintptr_t), uintptr_t{0x2000 + i * 0x100});
        Check(ampere_wrapper::Matches(layout, capacity), "fixture restores the original buffer objects");
        Put(storage, 0, uintptr_t{0x9999});
        Check(!ampere_wrapper::Matches(layout, capacity), "replacing one presentation resource invalidates the feature snapshot");
        Put(storage, 0, uintptr_t{0});
        Check(!ampere_wrapper::Read(layout, capacity), "null presentation resource cannot certify capacity");
        Check(Stage(layout, "entry-null"), "171 diagnostics name a null presentation resource");
        Put(storage, 0, uintptr_t{0x2100});
        Check(!ampere_wrapper::Read(layout, capacity), "aliased presentation resources cannot certify capacity");
        Check(Stage(layout, "entry-duplicate"), "171 diagnostics name aliased presentation resources");
        Put(storage, 0, uintptr_t{0x2000});
        Put(ring, buffers + 16, storage + (n - 1) * sizeof(uintptr_t));
        Check(!ampere_wrapper::Read(layout, capacity), "allocation end beyond reserved capacity is rejected");
        Check(Stage(layout, "vector-capacity"), "171 diagnostics name an end beyond the reserved capacity");
        Put(ring, buffers + 16, storage + n * sizeof(uintptr_t));
        Put(ring, buffers + 8, storage + (n - 1) * sizeof(uintptr_t));
        Check(!ampere_wrapper::Read(layout, capacity) && Stage(layout, "vector-end-mismatch"),
            "a vector end that disagrees with the ring count is refused and named vector-end-mismatch");
    }
    ampere_wrapper::Capacity capacity{};
    Put(ring, count, 1u);
    Check(!ampere_wrapper::Read(layout, capacity), "insufficient ring capacity cannot admit even 2x");
    Check(Stage(layout, "count-out-of-range"), "171 diagnostics name a one-entry ring");
    Put(ring, count, 7u);
    Check(!ampere_wrapper::Read(layout, capacity), "unbounded ring counts remain rejected");
    Check(Stage(layout, "count-out-of-range"), "171 diagnostics name an oversized ring");
    Put(ring, count, 6u); Put(ring, buffers + 8, storage + 48); Put(ring, buffers + 16, storage + 48);
    // ringcap.1: a legacy layout holds exactly one generated frame; an MFG
    // layout holds the N (1-5) its startup cached, with exactly N+1 buffers.
    Put(context, maximum, legacy ? 5u : 6u);
    Check(!ampere_wrapper::Read(layout, capacity) && Stage(layout, "maximum-out-of-range"),
        "a generated maximum the layout cannot hold never establishes capacity");
    Put(context, maximum, 0u);
    Check(!ampere_wrapper::Read(layout, capacity) && Stage(layout, "maximum-out-of-range"),
        "a zero generated maximum never establishes capacity");
    if (!legacy)
    {
        Put(context, maximum, 1u);
        Check(!ampere_wrapper::Read(layout, capacity) && Stage(layout, "count-not-maximum-plus-one"),
            "a ring with more buffers than the cached maximum needs is refused");
        bool ringCertified = true, ringExact = true;
        for (uint32_t n = 1; n <= 5; ++n)
        {
            Put(context, maximum, n);
            Put(ring, count, n + 1); Put(ring, buffers + 8, storage + (n + 1) * sizeof(uintptr_t));
            Put(ring, buffers + 16, storage + (n + 1) * sizeof(uintptr_t));
            ampere_wrapper::Capacity partial{};
            const bool read = ampere_wrapper::Read(layout, partial);
            ringCertified = ringCertified && read && partial.maximumGeneratedFrames == n && partial.bufferCount == n + 1
                && ampere_wrapper::Matches(layout, partial) && Stage(layout, "readable");
            if (n < 5)
            {
                Put(ring, count, n + 2); Put(ring, buffers + 8, storage + (n + 2) * sizeof(uintptr_t));
                Put(ring, buffers + 16, storage + (n + 2) * sizeof(uintptr_t));
                ringExact = ringExact && !ampere_wrapper::Read(layout, partial) && !ampere_wrapper::Matches(layout, partial)
                    && Stage(layout, "count-not-maximum-plus-one");
            }
        }
        Check(ringCertified, "a ring of N+1 distinct buffers certifies the N (1-5) its wrapper cached and revalidates on the frame path");
        Check(ringExact, "a ring with N+2 buffers for a cached N is refused");
        // The FF7 Rebirth ring: maximum 3, four buffers.
        Put(context, maximum, 3u);
        Put(ring, count, 4u); Put(ring, buffers + 8, storage + 32); Put(ring, buffers + 16, storage + 32);
        ampere_wrapper::Capacity ff7{};
        Check(ampere_wrapper::Read(layout, ff7) && ff7.maximumGeneratedFrames == 3 && ff7.bufferCount == 4,
            "FF7 Rebirth's ring (maximum 3, four buffers) certifies three generated frames");
        Put(context, maximum, layout.maximumGeneratedFrames);
        Check(!ampere_wrapper::Read(layout, capacity) && Stage(layout, "count-not-maximum-plus-one"),
            "a short MFG ring is refused and named count-not-maximum-plus-one");
    }
    if (legacy)
    {
        Put(context, maximum, 1u); Put(context, 0x18, uintptr_t{0});
        Check(!ampere_wrapper::Read(layout, capacity), "missing pacer owner remains blocked");
        Check(Stage(layout, "ring-null"), "171 diagnostics name a missing pacer owner");
    }
    Put(layout.contextSlot, 0, original);
    // The same real image must stop matching when its audited Create window
    // is corrupted, and recover after exact byte/protection restoration.
    const uint8_t* createPattern = pacer ? kPacerCreateWindow : small ? kLegacySmallCreateWindow
        : large ? kLegacyLargeCreateWindow : direct ? kLegacyDirectCreateWindow
        : legacy ? kLegacyCompactCreateWindow : nullptr;
    // The late pacer (Streamline 2.7.32) keeps the pacer fields with the small wrapper's Create.
    const uint8_t* alternatePattern = pacer ? kLegacySmallCreateWindow : nullptr;
    const size_t patternBytes = createPattern ? 23u : 1u;
    uintptr_t contract = createPattern ? 0 : layout.createBegin;
    for (const uint8_t* pattern : {createPattern, alternatePattern})
        for (uintptr_t p = layout.createBegin; pattern && !contract && p + patternBytes <= layout.createEnd; ++p)
            if (!std::memcmp(reinterpret_cast<void*>(p), pattern, patternBytes)) contract = p;
    // The 2.12 Create contract is a window inside the function (entry + 0x2f in 2.12.0 and 2.12.129);
    // the caller range is the one unwind entry that contains it, not a range starting at the window.
    if (direct)
        Check(contract > layout.createBegin && contract + patternBytes <= layout.createEnd,
            "the direct Create contract lies inside its unwind entry, which gives the caller range");
    DWORD protection = 0, ignored = 0;
    Check(contract && VirtualProtect(reinterpret_cast<void*>(contract), 1, PAGE_EXECUTE_READWRITE, &protection),
        "negative fixture changes only a bounded mapped-image contract");
    if (contract && protection)
    {
        const uint8_t saved = *reinterpret_cast<uint8_t*>(contract);
        *reinterpret_cast<uint8_t*>(contract) = saved ^ 1;
        VirtualProtect(reinterpret_cast<void*>(contract), 1, protection, &ignored);
        ampere_wrapper::Layout rejected{};
        Check(!ampere_wrapper::Discover(module, rejected), "missing Create contract blocks the otherwise recognized real wrapper");
        VirtualProtect(reinterpret_cast<void*>(contract), 1, PAGE_EXECUTE_READWRITE, &ignored);
        *reinterpret_cast<uint8_t*>(contract) = saved;
        VirtualProtect(reinterpret_cast<void*>(contract), 1, protection, &ignored);
        Check(ampere_wrapper::Discover(module, rejected), "exact restoration recovers admission without editing the NVIDIA file");
    }
    VirtualFree(reinterpret_cast<void*>(storage), 0, MEM_RELEASE);
    if (legacy) VirtualFree(reinterpret_cast<void*>(ring), 0, MEM_RELEASE);
    VirtualFree(reinterpret_cast<void*>(context), 0, MEM_RELEASE);
    FreeLibrary(module);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
