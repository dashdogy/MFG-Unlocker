#include "ampere_native_cache.h"
#include "sha256_util.h"
#if MFG_AMPERE_EMBEDDED_KERNELS
#include "ampere_kernels.packed.h"
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>

#ifndef MFG_AMPERE_NATIVE_CACHE
#define MFG_AMPERE_NATIVE_CACHE 0
#endif
#ifndef MFG_AMPERE_KERNEL_IMAGE
#define MFG_AMPERE_KERNEL_IMAGE 0
#endif
#ifndef MFG_AMPERE_NATIVE_CACHE_3109
#define MFG_AMPERE_NATIVE_CACHE_3109 0
#endif
#ifndef MFG_AMPERE_NATIVE_CACHE_ADDITIONAL
#define MFG_AMPERE_NATIVE_CACHE_ADDITIONAL 0
#endif

namespace ampere_native_cache
{
namespace
{
inline constexpr size_t kMaximumBytes = 64u * 1024u * 1024u;
struct Entry
{
    const char* ptxSha256;
    const char* fatbinSha256;
    uint32_t fatbinBytes;
};
#if MFG_AMPERE_NATIVE_CACHE
// Generated locally from the independently validated provider PTX, compiler
// output and binary-utility target checks. Contains hashes only, no vendor code.
#include "ampere_native_manifest.inc"
static_assert(kNativeManifest.size() == kSlots);
#ifdef MFG_AMPERE_NATIVE_MANIFEST_HAS_CONTRACTS
static_assert(kNativeContracts.size() == kSlots);
constexpr auto* kPrimaryContracts = &kNativeContracts;
#undef MFG_AMPERE_NATIVE_MANIFEST_HAS_CONTRACTS
#else
constexpr const std::array<NativeContract, kSlots>* kPrimaryContracts = nullptr;
#endif
#if MFG_AMPERE_NATIVE_CACHE_3109
namespace provider3109
{
#include "ampere_native_manifest_3109.inc"
#ifdef MFG_AMPERE_NATIVE_MANIFEST_HAS_CONTRACTS
static_assert(kNativeContracts.size() == kSlots);
constexpr auto* kContracts = &kNativeContracts;
#undef MFG_AMPERE_NATIVE_MANIFEST_HAS_CONTRACTS
#else
constexpr const std::array<NativeContract, kSlots>* kContracts = nullptr;
#endif
static_assert(kNativeManifest.size() == kSlots);
}
#endif
struct NativeProgram
{
    std::span<const Entry> entries;
    std::span<const NativeContract> contracts;
    uint32_t firstResource; // Layout of the pinned manifests; embedded kernels are found by SHA-256.
};
#if MFG_AMPERE_NATIVE_CACHE_ADDITIONAL
#include "ampere_native_manifest_additional.inc"
#endif
constexpr NativeProgram kNativePrograms[]{
    {kNativeManifest, kPrimaryContracts ? std::span<const NativeContract>(*kPrimaryContracts)
        : std::span<const NativeContract>{}, 4000},
#if MFG_AMPERE_NATIVE_CACHE_3109
    {provider3109::kNativeManifest, provider3109::kContracts
        ? std::span<const NativeContract>(*provider3109::kContracts) : std::span<const NativeContract>{}, 4100},
#endif
};
#endif
template<class T> T Read(const uint8_t* data, size_t offset) noexcept
{
    T value{};
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}
bool SingleImage(const std::vector<uint8_t>& bytes, uint16_t kind,
    size_t& payload, size_t& length) noexcept
{
    payload = length = 0;
    if (bytes.size() < 80 || bytes.size() > kMaximumBytes) return false;
    const auto* data = bytes.data();
    if (Read<uint32_t>(data, 0) != 0xba55ed50 || Read<uint16_t>(data, 4) != 1) return false;
    const size_t outer = Read<uint16_t>(data, 6);
    const auto total = Read<uint64_t>(data, 8);
    if (outer != 16 || total != bytes.size() - outer || total < 64) return false;
    const auto* entry = data + outer;
    const size_t header = Read<uint32_t>(entry, 4);
    const auto padded = Read<uint64_t>(entry, 8);
    if (Read<uint16_t>(entry, 0) != kind || Read<uint16_t>(entry, 2) != 0x101
        || header < 64 || header > total || padded != total - header || !padded
        || (padded & 7u) || Read<uint32_t>(entry, 28) != 86
        || Read<uint64_t>(entry, 40) != 0x41u || (kind == 2 && header != 64)) return false;
    payload = outer + header;
    length = static_cast<size_t>(padded);
    return true;
}
bool MatchesHash(const uint8_t* data, size_t bytes, const char* expected) noexcept
{
    return sha256_util::EqualsUpperHex(data, bytes, expected, kMaximumBytes);
}
#include "ampere_native_elf.inl"
struct File
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool ReadExact(const std::wstring& path, uint32_t expectedBytes, std::vector<uint8_t>& output)
{
    if (expectedBytes < 80 || expectedBytes > kMaximumBytes) return false;
    File file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &attributes, sizeof(attributes))
        || (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        || !GetFileSizeEx(file.value, &size) || size.QuadPart != expectedBytes) return false;
    output.resize(expectedBytes);
    DWORD read = 0;
    return ReadFile(file.value, output.data(), expectedBytes, &read, nullptr) && read == expectedBytes;
}
}

Mode ConfiguredMode() noexcept
{
    static_assert(MFG_AMPERE_KERNEL_IMAGE >= 0 && MFG_AMPERE_KERNEL_IMAGE <= 2);
    return static_cast<Mode>(MFG_AMPERE_KERNEL_IMAGE);
}
bool HasManifest() noexcept { return MFG_AMPERE_NATIVE_CACHE != 0; }
std::wstring DefaultDirectory()
{
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&DefaultDirectory), &owner)) return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash + 1);
    return path + L"RTX30MFG-Kernels";
}
bool ValidateNativeContainer(const std::vector<uint8_t>& data) noexcept
{
    try
    {
        size_t offset = 0, size = 0;
        return SingleImage(data, 2, offset, size)
            && ValidateNativeElf(data.data() + offset, size, nullptr);
    }
    catch (...) { return false; }
}
bool ValidateNativeContainer(const std::vector<uint8_t>& data, const NativeContract& contract) noexcept
{
    try
    {
        size_t offset = 0, size = 0;
        return SingleImage(data, 2, offset, size)
            && ValidateNativeElf(data.data() + offset, size, &contract);
    }
    catch (...) { return false; }
}
template<class Reader>
Status LoadProgramUsing(ProgramView validatedPtx, Reader&& readSlot, CompleteProgram& output) noexcept
{
    output.clear();
    if (validatedPtx.size() < kSlots || validatedPtx.size() > kMaximumSlots) return Status::eSourceShape;
#if !MFG_AMPERE_NATIVE_CACHE
    (void)validatedPtx; (void)readSlot;
    return Status::eNoManifest;
#else
    try
    {
        // Prove the complete source shape before selecting a program. Each
        // program owns its entire registered set; source hashes cannot mix
        // across providers, nor can a matching prefix admit a partial cache.
        for (size_t i = 0; i < validatedPtx.size(); ++i)
        {
            size_t offset = 0, length = 0;
            if (!SingleImage(validatedPtx[i], 1, offset, length)) return Status::eSourceShape;
        }
        const NativeProgram* selected = nullptr;
        const auto consider = [&](const NativeProgram& program) {
            if (program.entries.size() != validatedPtx.size()
                || (!program.contracts.empty() && program.contracts.size() != program.entries.size())) return true;
            bool matches = true;
            for (size_t i = 0; matches && i < validatedPtx.size(); ++i)
            {
                size_t offset = 0, length = 0;
                if (!SingleImage(validatedPtx[i], 1, offset, length)) return false;
                const auto* ptx = validatedPtx[i].data() + offset;
                while (length && !ptx[length - 1]) --length;
                matches = MatchesHash(ptx, length, program.entries[i].ptxSha256);
            }
            if (!matches) return true;
            if (selected) return false;
            selected = &program;
            return true;
        };
        for (const auto& program : kNativePrograms)
        {
            if (!consider(program)) return Status::eSourceMismatch;
        }
#if MFG_AMPERE_NATIVE_CACHE_ADDITIONAL
        for (const auto& program : kAdditionalNativePrograms)
            if (!consider(program)) return Status::eSourceMismatch;
#endif
        if (!selected) return Status::eSourceMismatch;
        CompleteProgram candidate(validatedPtx.size());
        for (size_t i = 0; i < validatedPtx.size(); ++i)
        {
            const auto& entry = selected->entries[i];
            if (!readSlot(selected->firstResource + static_cast<uint32_t>(i), entry, candidate[i]))
                return Status::eFileUnavailable;
            if (!MatchesHash(candidate[i].data(), candidate[i].size(), entry.fatbinSha256)) return Status::eHashMismatch;
            if (!selected->contracts.empty()
                ? !ValidateNativeContainer(candidate[i], selected->contracts[i])
                : !ValidateNativeContainer(candidate[i]))
                return Status::eInvalidContainer;
        }
        output = std::move(candidate);
        return Status::eLoaded;
    }
    catch (...) { return Status::eAllocation; }
#endif
}

Status LoadProgram(ProgramView validatedPtx, const std::wstring& directory, CompleteProgram& output) noexcept
{
    return LoadProgramUsing(validatedPtx, [&](size_t, const Entry& entry, std::vector<uint8_t>& slot)
    {
        if (directory.empty()) return false;
        std::wstring filename;
        for (size_t c = 0; c < 64; ++c)
        {
            const char value = entry.fatbinSha256[c];
            if (!((value >= '0' && value <= '9') || (value >= 'A' && value <= 'F'))) return false;
            filename += static_cast<wchar_t>(value);
        }
        return ReadExact(directory + L"\\" + filename + L".fatbin", entry.fatbinBytes, slot);
    }, output);
}

#if MFG_AMPERE_EMBEDDED_KERNELS
struct KernelResource
{
    const char* fatbinSha256;
    uint32_t id;
};
#include "ampere_kernel_resources.inc"
#endif

Status LoadEmbeddedProgram(ProgramView validatedPtx, void* module, CompleteProgram& output) noexcept
{
    return LoadProgramUsing(validatedPtx, [&](size_t, const Entry& entry, std::vector<uint8_t>& slot)
    {
#if MFG_AMPERE_EMBEDDED_KERNELS
        if (!module) return false;
        // Each distinct kernel is embedded once; programs sharing it share the resource.
        uint32_t resourceId = 0;
        for (const auto& kernel : kKernelResources)
            if (std::strcmp(kernel.fatbinSha256, entry.fatbinSha256) == 0) { resourceId = kernel.id; break; }
        if (!resourceId) return false;
        const auto bytes = embedded_payload::Find(static_cast<HMODULE>(module), embedded_payload::packed::ampere_kernels,
            embedded_payload::packed::ampere_kernels_items, resourceId);
        if (bytes.size() != entry.fatbinBytes) return false;
        slot.assign(bytes.begin(), bytes.end());
        return true;
#else
        (void)module; (void)entry; (void)slot;
        return false;
#endif
    }, output);
}

Status LoadConfiguredProgram(ProgramView validatedPtx, CompleteProgram& output) noexcept
{
#if MFG_AMPERE_EMBEDDED_KERNELS
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&DefaultDirectory), &owner);
    return LoadEmbeddedProgram(validatedPtx, owner, output);
#else
    return LoadProgram(validatedPtx, DefaultDirectory(), output);
#endif
}

template<class Load>
Status LoadLegacyProgram(const Program& validatedPtx, Program& output, Load&& load) noexcept
{
    for (auto& slot : output) slot.clear();
    CompleteProgram complete;
    const auto status = load(ProgramView(validatedPtx), complete);
    if (status != Status::eLoaded) return status;
    if (complete.size() != output.size()) return Status::eSourceShape;
    for (size_t i = 0; i < output.size(); ++i) output[i] = std::move(complete[i]);
    return status;
}

Status LoadProgram(const Program& validatedPtx, const std::wstring& directory, Program& output) noexcept
{
    return LoadLegacyProgram(validatedPtx, output, [&](ProgramView input, CompleteProgram& complete) {
        return LoadProgram(input, directory, complete); });
}
Status LoadEmbeddedProgram(const Program& validatedPtx, void* module, Program& output) noexcept
{
    return LoadLegacyProgram(validatedPtx, output, [&](ProgramView input, CompleteProgram& complete) {
        return LoadEmbeddedProgram(input, module, complete); });
}
Status LoadConfiguredProgram(const Program& validatedPtx, Program& output) noexcept
{
    return LoadLegacyProgram(validatedPtx, output, [&](ProgramView input, CompleteProgram& complete) {
        return LoadConfiguredProgram(input, complete); });
}
}
