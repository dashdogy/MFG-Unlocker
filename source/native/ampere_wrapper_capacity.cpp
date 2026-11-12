#include "ampere_wrapper_capacity.h"
#include "third_party/minhook/src/hde/hde64.h"
#include <algorithm>
#include <cstring>
#include "ampere_legacy_wrapper_contracts.h"
#include "ampere_ota_wrapper_contracts.h"

namespace ampere_wrapper
{
namespace
{
// Independently traced in local production 2.13 and 2.14 wrappers (OTA 2.14.16 included). Every
// window must match one complete profile; version text grants no admission.
struct Fields { size_t maximum, swapchain, count, buffers; };
constexpr Fields kCompactFields{0x45dc, 0x4150, 0x4158, 0x42c0};
constexpr Fields kExtendedFields{0x460c, 0x4180, 0x4188, 0x42f0};
// Legacy rings live in the independently matched pacer object at context+18.
// count/buffers below are pacer offsets; maximum/swapchain are context offsets.
constexpr Fields kPacerFields{0x10d00, 0x3538, 0x60, 0x48};
constexpr Fields kLegacyCompactFields{0x45a4, 0x4120, 0x58, 0x40};
constexpr Fields kLegacySmallFields{0x39f8, 0x3538, 0x60, 0x48};
constexpr Fields kLegacyLargeFields{0x45c8, 0x4138, 0x60, 0x48};
// Streamline 2.12.0 and OTA 2.12.129 ("direct"; GitHub issue 153): an MFG ring on the context, with the compact
// profile's swap-chain, count and buffer fields. Startup caches min(DLSSG.MultiFrameCountMax, 5) at +45e4 (1 when
// NGX has no value); swap-chain setup makes that +1 (at most 6) the BufferCount and resizes the +42c0 vector to it.
constexpr Fields kLegacyDirectFields{0x45e4, 0x4150, 0x4158, 0x42c0};
bool UsesPacer(const Layout& layout) noexcept
{
    return layout.profile == Profile::eLegacyPacer || layout.profile == Profile::eLegacyCompact
        || layout.profile == Profile::eLegacySmall || layout.profile == Profile::eLegacyLarge;
}
const Fields* ProfileFields(const Layout& layout) noexcept
{
    switch (layout.profile)
    {
    case Profile::eCompact: return &kCompactFields;
    case Profile::eExtended: return &kExtendedFields;
    case Profile::eLegacyPacer: return &kPacerFields;
    case Profile::eLegacyCompact: return &kLegacyCompactFields;
    case Profile::eLegacySmall: return &kLegacySmallFields;
    case Profile::eLegacyLarge: return &kLegacyLargeFields;
    case Profile::eLegacyDirect: return &kLegacyDirectFields;
    default: return nullptr;
    }
}
constexpr uint8_t kMaximumWindow[]{
    0x4c,0x8d,0xbe,0x0c,0x46,0x00,0x00,0xba,0x05,0x00,0x00,0x00,0x3b,0xca,0x0f,0x42,
    0xd1,0x41,0x89,0x17,
};
constexpr uint8_t kContextWindow[]{
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,
    0x89,0x7c,0x24,0x20,0x41,0x56,0x48,0x81,0xec,0xa0,0x01,0x00,0x00,0x48,0x8b,0x1d,
    0x6c,0xfb,0x03,0x00,0x49,0x8b,0xf1,0x49,0x8b,0xf8,0x48,0x8b,0xea,0x4c,0x8b,0xf1,
    0x48,0x83,0xbb,0x80,0x41,0x00,0x00,0x00,
};
constexpr uint8_t kSwapchainCountWindow[]{
    0x80,0xbb,0xd8,0x46,0x00,0x00,0x00,0x74,0x08,0x8b,0x8b,0xd4,0x46,0x00,0x00,0xeb,
    0x08,0x8b,0x8b,0x0c,0x46,0x00,0x00,0xff,0xc1,0x89,0x8b,0x88,0x41,0x00,0x00,0x85,
    0xc9,0x75,0x08,0x89,0x83,0x88,0x41,0x00,0x00,0x8b,0xc8,
};
constexpr uint8_t kBufferLoopWindow[]{
    0x41,0x8b,0xdd,0x39,0x9f,0x88,0x41,0x00,0x00,0x76,0x79,0x48,0x8b,0x8f,0x58,0x43,
    0x00,0x00,0x48,0x8b,0x01,0x4c,0x8b,0x90,0x68,0x02,0x00,0x00,0x8b,0xc3,0x48,0x8d,
    0x34,0xc5,0x00,0x00,0x00,0x00,0x4c,0x8b,0x8f,0xf0,0x42,0x00,0x00,0x4c,0x03,0xce,
    0x44,0x8b,0xc3,0x48,0x8b,0x97,0x80,0x41,0x00,0x00,0x41,0xff,0xd2,0x48,0x63,0xc8,
    0x85,0xc0,0x0f,0x85,0xa4,0x01,0x00,0x00,0x48,0x8b,0x87,0xf0,0x42,0x00,0x00,0x48,
    0x8b,0x0c,0x06,0x44,0x89,0x71,0x44,0x44,0x89,0x79,0x48,0x44,0x89,0x61,0x4c,0xc7,
    0x41,0x50,0x01,0x00,0x00,0x00,0xc7,0x41,0x54,0x01,0x00,0x00,0x00,0x8b,0x87,0xb8,
    0x42,0x00,0x00,0x89,0x41,0x64,0x44,0x89,0x69,0x40,0xff,0xc3,0x3b,0x9f,0x88,0x41,
    0x00,0x00,0x72,0x87,
};
constexpr uint8_t kCreateWindow[]{
    0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x41,0x56,0x41,
    0x57,0x48,0x83,0xec,0x70,0x0f,0x29,0x74,0x24,0x60,0x4d,0x8b,0xf8,0x48,0x8b,0xf2,
    0x48,0x83,0x7a,0x08,0x00,
};
constexpr uint8_t kCompactMaximumWindow[]{
    0xba,0x05,0x00,0x00,0x00,0x3b,0xca,0x0f,0x42,0xd1,0x49,0x8d,0xb6,0xdc,0x45,0x00,
    0x00,0x89,0x16,
};
constexpr uint8_t kCompactContextWindow[]{
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,
    0x89,0x7c,0x24,0x20,0x41,0x56,0x48,0x81,0xec,0xa0,0x01,0x00,0x00,0x48,0x8b,0x1d,
    0xbc,0xeb,0x03,0x00,0x49,0x8b,0xf1,0x49,0x8b,0xf8,0x48,0x8b,0xea,0x4c,0x8b,0xf1,
    0x48,0x83,0xbb,0x50,0x41,0x00,0x00,0x00,
};
constexpr uint8_t kCompactSwapchainCountWindow[]{
    0x80,0xbb,0xa8,0x46,0x00,0x00,0x00,0x74,0x08,0x8b,0x8b,0xa4,0x46,0x00,0x00,0xeb,
    0x08,0x8b,0x8b,0xdc,0x45,0x00,0x00,0xff,0xc1,0x89,0x8b,0x58,0x41,0x00,0x00,0x85,
    0xc9,0x75,0x08,0x89,0x83,0x58,0x41,0x00,0x00,0x8b,0xc8,
};
constexpr uint8_t kCompactBufferLoopWindow[]{
    0x41,0x8b,0xdd,0x39,0x9f,0x58,0x41,0x00,0x00,0x76,0x79,0x48,0x8b,0x8f,0x28,0x43,
    0x00,0x00,0x48,0x8b,0x01,0x4c,0x8b,0x90,0x58,0x02,0x00,0x00,0x8b,0xc3,0x48,0x8d,
    0x34,0xc5,0x00,0x00,0x00,0x00,0x4c,0x8b,0x8f,0xc0,0x42,0x00,0x00,0x4c,0x03,0xce,
    0x44,0x8b,0xc3,0x48,0x8b,0x97,0x50,0x41,0x00,0x00,0x41,0xff,0xd2,0x48,0x63,0xc8,
    0x85,0xc0,0x0f,0x85,0xa4,0x01,0x00,0x00,0x48,0x8b,0x87,0xc0,0x42,0x00,0x00,0x48,
    0x8b,0x0c,0x06,0x44,0x89,0x71,0x44,0x44,0x89,0x79,0x48,0x44,0x89,0x61,0x4c,0xc7,
    0x41,0x50,0x01,0x00,0x00,0x00,0xc7,0x41,0x54,0x01,0x00,0x00,0x00,0x8b,0x87,0x88,
    0x42,0x00,0x00,0x89,0x41,0x64,0x44,0x89,0x69,0x40,0xff,0xc3,0x3b,0x9f,0x58,0x41,
    0x00,0x00,0x72,0x87,
};

bool Range(uintptr_t address, size_t bytes, DWORD type, HMODULE owner = nullptr) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || !bytes || bytes > UINTPTR_MAX - address
        || VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT || memory.Type != type
        || (owner && memory.AllocationBase != owner)
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        || (memory.Protect != PAGE_READWRITE && memory.Protect != PAGE_WRITECOPY)) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return address >= begin && address - begin <= memory.RegionSize
        && bytes <= memory.RegionSize - (address - begin);
}
template<class T> T Field(uintptr_t base, size_t offset) noexcept
{
    T value{};
    memcpy(&value, reinterpret_cast<const void*>(base + offset), sizeof(value));
    return value;
}
struct Image
{
    HMODULE module{};
    uintptr_t base = 0;
    uint32_t size = 0;
    const IMAGE_NT_HEADERS64* nt{};
    bool Contains(uintptr_t address, size_t bytes) const noexcept
    { return address >= base && address - base < size && bytes <= size - (address - base); }
    bool Executable(uintptr_t address) const noexcept
    {
        MEMORY_BASIC_INFORMATION memory{};
        return Contains(address, 1) && VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == sizeof(memory)
            && memory.AllocationBase == module && memory.Type == MEM_IMAGE && memory.State == MEM_COMMIT
            && memory.Protect == PAGE_EXECUTE_READ;
    }
    bool Open(HMODULE value) noexcept
    {
        base = reinterpret_cast<uintptr_t>(value); module = value;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
        nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        size = nt->OptionalHeader.SizeOfImage;
        return size >= 4096 && size < 1024u * 1024u * 1024u
            && Contains(reinterpret_cast<uintptr_t>(IMAGE_FIRST_SECTION(nt)),
                nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER));
    }
    template<size_t N> uintptr_t Unique(const uint8_t (&pattern)[N], size_t displacement = SIZE_MAX,
        size_t secondDisplacement = SIZE_MAX) const noexcept
    {
        uintptr_t result = 0;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.VirtualAddress >= size) continue;
            const size_t bytes = std::min<size_t>(size - section.VirtualAddress,
                std::max(section.Misc.VirtualSize, section.SizeOfRawData));
            for (size_t offset = 0; offset + N <= bytes; ++offset)
            {
                const uintptr_t address = base + section.VirtualAddress + offset;
                const auto* candidate = reinterpret_cast<const uint8_t*>(address);
                bool same = true;
                for (size_t j = 0; same && j < N; ++j)
                    same = (displacement != SIZE_MAX && j >= displacement && j - displacement < 4)
                        || (secondDisplacement != SIZE_MAX && j >= secondDisplacement && j - secondDisplacement < 4)
                        || candidate[j] == pattern[j];
                if (!same) continue;
                size_t decoded = 0;
                while (decoded < N)
                {
                    hde64s instruction{};
                    const auto count = hde64_disasm(candidate + decoded, &instruction);
                    if (!count || (instruction.flags & F_ERROR) || count > N - decoded) return 0;
                    const bool shortBranch = instruction.opcode == 0xeb
                        || (instruction.opcode >= 0x70 && instruction.opcode <= 0x7f);
                    const bool longBranch = instruction.opcode == 0xe8 || instruction.opcode == 0xe9
                        || (instruction.opcode == 0x0f && instruction.opcode2 >= 0x80 && instruction.opcode2 <= 0x8f);
                    if (shortBranch || longBranch)
                    {
                        const intptr_t relative = shortBranch
                            ? Field<int8_t>(address, decoded + count - 1)
                            : Field<int32_t>(address, decoded + count - 4);
                        if (!Executable(address + decoded + count + relative)) return 0;
                    }
                    decoded += count;
                }
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(candidate, &memory, sizeof(memory)) != sizeof(memory)
                    || memory.AllocationBase != module || memory.Type != MEM_IMAGE
                    || memory.State != MEM_COMMIT || memory.Protect != PAGE_EXECUTE_READ || result) return 0;
                result = address;
            }
        }
        return result;
    }
};
// A legacy 2x layout holds exactly one generated frame; an MFG layout holds
// 1..5, whatever its startup cached.
bool RingMaximumValid(const Layout& layout, uint32_t maximum) noexcept
{
    return layout.maximumGeneratedFrames == 1 ? maximum == 1
        : maximum >= 1 && maximum <= layout.maximumGeneratedFrames;
}
bool ReadUnchecked(const Layout& layout, Capacity& value) noexcept
{
    value = {};
    const Fields* fields = ProfileFields(layout);
    if (!fields || !layout.contextSlot) return false;
    value.context = Field<uintptr_t>(layout.contextSlot, 0);
    if (!value.context || (layout.maximumGeneratedFrames != 1
            && layout.maximumGeneratedFrames != ampere_mfg::kMaximumGeneratedFrames)) return false;
    // An MFG wrapper sizes its ring from the maximum its startup
    // cached (min(DLSSG.MultiFrameCountMax, 5)), which can be below the layout's
    // five. That maximum N, with exactly N+1 buffers below, is the capacity.
    const uint32_t maximum = Field<uint32_t>(value.context, fields->maximum);
    if (!RingMaximumValid(layout, maximum)) return false;
    value.maximumGeneratedFrames = maximum;
    const bool legacy = UsesPacer(layout);
    value.ringOwner = legacy ? Field<uintptr_t>(value.context, 0x18) : value.context;
    if (!value.ringOwner) return false;
    value.bufferCount = Field<uint32_t>(value.ringOwner, fields->count);
    if (value.bufferCount < value.maximumGeneratedFrames + 1
        || value.bufferCount > value.buffers.size()
        || (layout.maximumGeneratedFrames != 1 && value.bufferCount != value.maximumGeneratedFrames + 1)) return false;
    value.swapchain = Field<uintptr_t>(value.context, fields->swapchain);
    value.buffersBegin = Field<uintptr_t>(value.ringOwner, fields->buffers);
    value.buffersEnd = Field<uintptr_t>(value.ringOwner, fields->buffers + 8);
    value.buffersCapacity = Field<uintptr_t>(value.ringOwner, fields->buffers + 16);
    const size_t bytes = value.bufferCount * sizeof(uintptr_t);
    if (!value.swapchain || !value.buffersBegin || value.buffersBegin % alignof(uintptr_t)
        || value.buffersBegin > UINTPTR_MAX - bytes
        || value.buffersEnd != value.buffersBegin + bytes
        || value.buffersCapacity < value.buffersEnd
        || value.buffersCapacity - value.buffersBegin > 6 * sizeof(uintptr_t)) return false;
    memcpy(value.buffers.data(), reinterpret_cast<void*>(value.buffersBegin), bytes);
    for (size_t i = 0; i < value.bufferCount; ++i)
    {
        if (!value.buffers[i]) return false;
        for (size_t j = 0; j < i; ++j)
            if (value.buffers[i] == value.buffers[j]) return false;
    }
    return true;
}
}
bool Discover(HMODULE module, Layout& layout) noexcept
{
    layout = {};
    __try
    {
        Image image{};
        if (!image.Open(module)) return false;
        const uintptr_t extendedContext = image.Unique(kContextWindow, 32);
        const uintptr_t compactContext = image.Unique(kCompactContextWindow, 32);
        const bool extended = extendedContext && image.Unique(kMaximumWindow)
            && image.Unique(kSwapchainCountWindow) && image.Unique(kBufferLoopWindow);
        // OTA 2.14.16: the extended fields with the moved count override and its own Create frame.
        const bool extendedOverride = extendedContext && image.Unique(kMaximumWindow)
            && image.Unique(kOverrideSwapchainCountWindow) && image.Unique(kBufferLoopWindow);
        const bool compact = compactContext && image.Unique(kCompactMaximumWindow)
            && image.Unique(kCompactSwapchainCountWindow) && image.Unique(kCompactBufferLoopWindow);
        const uintptr_t pacerContext = image.Unique(kPacerContextWindow, 3, 10);
        const uintptr_t otaPacerContext = image.Unique(kOtaPacerContextWindow, 3, 10);
        const uintptr_t legacyCompactContext = image.Unique(kLegacyCompactContextWindow, 3, 10);
        const bool pacer = pacerContext && image.Unique(kPacerMaximumWindow) && image.Unique(kPacerBuffersWindow);
        const bool otaPacer = otaPacerContext && image.Unique(kOtaPacerMaximumWindow) && image.Unique(kPacerBuffersWindow);
        const bool legacyCompact = legacyCompactContext && image.Unique(kLegacyCompactMaximumWindow)
            && image.Unique(kLegacyCompactBuffersWindow);
        const uintptr_t smallContext = image.Unique(kLegacySmallContextWindow, 3, 10);
        const uintptr_t largeContext = image.Unique(kLegacyLargeContextWindow, 3, 10);
        const bool small = smallContext && image.Unique(kLegacySmallMaximumWindow) && image.Unique(kLegacySmallBuffersWindow);
        const bool large = largeContext && image.Unique(kLegacyLargeMaximumWindow) && image.Unique(kLegacyLargeBuffersWindow);
        const bool direct = compactContext && image.Unique(kLegacyDirectMaximumWindow)
            && image.Unique(kLegacyDirectCountWindow) && image.Unique(kLegacyDirectBuffersWindow);
        // Small-wrapper code over the pacer context: the pacer's fields and pre-frame-ID ordinals.
        const bool latePacer = smallContext && image.Unique(kLatePacerMaximumWindow)
            && image.Unique(kLatePacerBuffersWindow);
        if (static_cast<unsigned>(extended) + extendedOverride + compact + pacer + otaPacer + latePacer + legacyCompact + small + large
            + direct != 1) return false;
        const bool pacerFields = pacer || otaPacer || latePacer;
        const bool legacy = pacerFields || legacyCompact || small || large;
        // The direct ring holds N+1 for the N its startup cached, as on the compact profile; Read()
        // certifies N from the allocation. Only the pacer-ring profiles stay 2x.
        const bool fixedTwoX = legacy;
        // These Create contracts are windows inside the function, not its entry; the unwind entry
        // that contains the window gives the caller range.
        const bool interiorCreate = legacy || direct;
        const uintptr_t prologue = compact ? compactContext : extended || extendedOverride ? extendedContext
            : pacer ? pacerContext : otaPacer ? otaPacerContext : small || latePacer ? smallContext
            : large ? largeContext : direct ? compactContext : legacyCompactContext;
        const uintptr_t create = pacer || otaPacer ? image.Unique(kPacerCreateWindow)
            : small || latePacer ? image.Unique(kLegacySmallCreateWindow) : large ? image.Unique(kLegacyLargeCreateWindow)
            : direct ? image.Unique(kLegacyDirectCreateWindow)
            : legacyCompact ? image.Unique(kLegacyCompactCreateWindow)
            : extendedOverride ? image.Unique(kOverrideCreateWindow) : image.Unique(kCreateWindow);
        if (!prologue || !create) return false;
        // mov rbx,[rip+disp32] in the verified swap-chain entry.
        const uintptr_t slot = legacy ? prologue + 7 + Field<int32_t>(prologue, 3)
            : prologue + 36 + Field<int32_t>(prologue, 32);
        if (legacy)
        {
            const uintptr_t auxiliary = prologue + 14 + Field<int32_t>(prologue, 10);
            if (!image.Contains(auxiliary, sizeof(void*)) || !Range(auxiliary, sizeof(void*), MEM_IMAGE, module)) return false;
        }
        if (!image.Contains(slot, sizeof(void*)) || slot % alignof(void*)
            || !Range(slot, sizeof(void*), MEM_IMAGE, module)) return false;
        const auto& directory = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!directory.Size || directory.Size % sizeof(RUNTIME_FUNCTION)
            || !image.Contains(image.base + directory.VirtualAddress, directory.Size)) return false;
        const auto* functions = reinterpret_cast<const RUNTIME_FUNCTION*>(image.base + directory.VirtualAddress);
        uintptr_t begin = 0, end = 0;
        for (size_t i = 0; i < directory.Size / sizeof(RUNTIME_FUNCTION); ++i)
        {
            if (!(functions[i].BeginAddress <= create - image.base && create - image.base < functions[i].EndAddress)) continue;
            if (!interiorCreate && functions[i].BeginAddress != create - image.base) return false;
            if (end || functions[i].EndAddress <= functions[i].BeginAddress
                || functions[i].EndAddress - functions[i].BeginAddress > 4096
                || !image.Contains(image.base + functions[i].BeginAddress,
                    functions[i].EndAddress - functions[i].BeginAddress)) return false;
            begin = image.base + functions[i].BeginAddress;
            end = image.base + functions[i].EndAddress;
        }
        if (!end) return false;
        layout = {module, slot, begin, end, compact ? Profile::eCompact : extended || extendedOverride ? Profile::eExtended
            : pacerFields ? Profile::eLegacyPacer : small ? Profile::eLegacySmall
            : large ? Profile::eLegacyLarge : direct ? Profile::eLegacyDirect : Profile::eLegacyCompact,
            fixedTwoX ? 1u : ampere_mfg::kMaximumGeneratedFrames, !pacerFields};
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BeforeSwapchain(const Layout& layout) noexcept
{
    __try
    {
        const Fields* fields = ProfileFields(layout);
        if (!fields || !Range(layout.contextSlot, sizeof(void*), MEM_IMAGE, layout.module)) return false;
        const uintptr_t context = Field<uintptr_t>(layout.contextSlot, 0);
        if (!context || !Range(context, fields->maximum + sizeof(uint32_t), MEM_PRIVATE)
            || Field<uintptr_t>(context, fields->swapchain)) return false;
        const bool legacy = UsesPacer(layout);
        const uintptr_t ring = legacy ? Field<uintptr_t>(context, 0x18) : context;
        // sl.common queries NGX capabilities before the legacy DLSS-G plugin's
        // onStartup allocates its pacer. A verified, current context with no
        // swapchain/pacer is still before allocation. This only bootstraps the
        // capability; Read() at Create continues to require the actual ring.
        if (legacy && !ring)
            return Field<uintptr_t>(layout.contextSlot, 0) == context
                && !Field<uintptr_t>(context, fields->swapchain)
                && !Field<uintptr_t>(context, 0x18);
        return ring && (!legacy || Range(ring, fields->count + sizeof(uint32_t), MEM_PRIVATE))
            && Field<uintptr_t>(ring, fields->buffers) == Field<uintptr_t>(ring, fields->buffers + 8)
            && Field<uintptr_t>(layout.contextSlot, 0) == context
            && !Field<uintptr_t>(context, fields->swapchain)
            && (!legacy || Field<uintptr_t>(context, 0x18) == ring);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Read(const Layout& layout, Capacity& capacity) noexcept
{
    capacity = {};
    __try
    {
        const Fields* fields = ProfileFields(layout);
        if (!fields || !Range(layout.contextSlot, sizeof(void*), MEM_IMAGE, layout.module)) return false;
        const uintptr_t context = Field<uintptr_t>(layout.contextSlot, 0);
        if (!context || !Range(context, fields->maximum + sizeof(uint32_t), MEM_PRIVATE)) return false;
        const bool legacy = UsesPacer(layout);
        const uintptr_t ring = legacy ? Field<uintptr_t>(context, 0x18) : context;
        if (!ring || (legacy && !Range(ring, fields->count + sizeof(uint32_t), MEM_PRIVATE))) return false;
        const uint32_t count = Field<uint32_t>(ring, fields->count);
        if (count < 2 || count > ampere_mfg::kPresentationBuffers) return false;
        const uintptr_t buffers = Field<uintptr_t>(ring, fields->buffers);
        Capacity candidate{};
        if (!Range(buffers, count * sizeof(uintptr_t), MEM_PRIVATE) || !ReadUnchecked(layout, candidate)) return false;
        // Verify that the fields did not change while taking the snapshot.
        Capacity again{};
        if (!ReadUnchecked(layout, again) || again != candidate) return false;
        capacity = candidate;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { capacity = {}; return false; }
}
bool Matches(const Layout& layout, const Capacity& capacity) noexcept
{
    // No image scans or VirtualQuery loop on the frame path. The pinned
    // wrapper and exact feature own this snapshot until successful Release.
    __try
    {
        if (!capacity.context || Field<uintptr_t>(layout.contextSlot, 0) != capacity.context) return false;
        Capacity current{};
        return ReadUnchecked(layout, current) && current == capacity;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CreationOnStack(const Layout& layout) noexcept
{
    void* frames[32]{};
    const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < count; ++i)
    {
        const uintptr_t frame = reinterpret_cast<uintptr_t>(frames[i]);
        if (frame >= layout.createBegin && frame < layout.createEnd) return true;
    }
    return false;
}
Observation InspectFailure(const Layout& layout) noexcept
{
    Observation result{};
    __try
    {
        const Fields* fields = ProfileFields(layout);
        if (!fields || !Range(layout.contextSlot, sizeof(void*), MEM_IMAGE, layout.module)) return result;
        auto& value = result.capacity;
        value.context = Field<uintptr_t>(layout.contextSlot, 0);
        if (!Range(value.context, fields->maximum + sizeof(uint32_t), MEM_PRIVATE)) return result;
        result.contextReadable = true;
        result.maximum = Field<uint32_t>(value.context, fields->maximum);
        value.swapchain = Field<uintptr_t>(value.context, fields->swapchain);
        const bool legacy = UsesPacer(layout);
        value.ringOwner = legacy ? Field<uintptr_t>(value.context, 0x18) : value.context;
        if (!value.ringOwner || (legacy && !Range(value.ringOwner, fields->count + sizeof(uint32_t), MEM_PRIVATE))) return result;
        result.bufferCount = Field<uint32_t>(value.ringOwner, fields->count);
        value.maximumGeneratedFrames = result.maximum; value.bufferCount = result.bufferCount;
        value.buffersBegin = Field<uintptr_t>(value.ringOwner, fields->buffers);
        value.buffersEnd = Field<uintptr_t>(value.ringOwner, fields->buffers + 8);
        value.buffersCapacity = Field<uintptr_t>(value.ringOwner, fields->buffers + 16);
        if (result.bufferCount < 2 || result.bufferCount > value.buffers.size() || value.buffersBegin % alignof(uintptr_t)
            || !Range(value.buffersBegin, result.bufferCount * sizeof(uintptr_t), MEM_PRIVATE)) return result;
        memcpy(value.buffers.data(), reinterpret_cast<void*>(value.buffersBegin), result.bufferCount * sizeof(uintptr_t));
        result.buffersReadable = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { result.buffersReadable = false; }
    return result;
}
const char* ReadFailure(const Layout& layout) noexcept
{
    __try
    {
        const Fields* fields = ProfileFields(layout);
        if (!fields) return "profile-unknown";
        if (!layout.contextSlot || !Range(layout.contextSlot, sizeof(void*), MEM_IMAGE, layout.module)) return "context-slot-unreadable";
        const uintptr_t context = Field<uintptr_t>(layout.contextSlot, 0);
        if (!context) return "context-null";
        if (!Range(context, fields->maximum + sizeof(uint32_t), MEM_PRIVATE)) return "context-unreadable";
        const bool legacy = UsesPacer(layout);
        const uintptr_t ring = legacy ? Field<uintptr_t>(context, 0x18) : context;
        if (!ring) return "ring-null";
        if (legacy && !Range(ring, fields->count + sizeof(uint32_t), MEM_PRIVATE)) return "ring-unreadable";
        const uint32_t count = Field<uint32_t>(ring, fields->count);
        if (count < 2 || count > ampere_mfg::kPresentationBuffers) return "count-out-of-range";
        const uintptr_t begin = Field<uintptr_t>(ring, fields->buffers);
        if (!Range(begin, count * sizeof(uintptr_t), MEM_PRIVATE)) return "vector-unreadable";
        // ReadUnchecked's checks, in its order.
        const uint32_t maximum = Field<uint32_t>(context, fields->maximum);
        if ((layout.maximumGeneratedFrames != 1 && layout.maximumGeneratedFrames != ampere_mfg::kMaximumGeneratedFrames)
            || !RingMaximumValid(layout, maximum)) return "maximum-out-of-range";
        if (count < maximum + 1 || (layout.maximumGeneratedFrames != 1 && count != maximum + 1)) return "count-not-maximum-plus-one";
        if (!Field<uintptr_t>(context, fields->swapchain)) return "swapchain-null";
        const uintptr_t end = Field<uintptr_t>(ring, fields->buffers + 8);
        const uintptr_t capacity = Field<uintptr_t>(ring, fields->buffers + 16);
        const size_t bytes = count * sizeof(uintptr_t);
        if (begin % alignof(uintptr_t) || begin > UINTPTR_MAX - bytes || end != begin + bytes) return "vector-end-mismatch";
        if (capacity < end || capacity - begin > 6 * sizeof(uintptr_t)) return "vector-capacity";
        std::array<uintptr_t, ampere_mfg::kPresentationBuffers> buffers{};
        memcpy(buffers.data(), reinterpret_cast<void*>(begin), bytes);
        for (size_t i = 0; i < count; ++i)
        {
            if (!buffers[i]) return "entry-null";
            for (size_t j = 0; j < i; ++j)
                if (buffers[i] == buffers[j]) return "entry-duplicate";
        }
        Capacity first{}, again{};
        if (!ReadUnchecked(layout, first) || !ReadUnchecked(layout, again) || again != first) return "changed-while-reading";
        return "readable";
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return "read-fault"; }
}
}
