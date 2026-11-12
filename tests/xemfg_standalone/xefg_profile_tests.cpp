// fg95prov.1: the XeMFG Nvngx replacement provider's XeFG image profiles (source/native/xess/vulkan/
// xefg_internal_profiles.h) and Resolve's routing (xefg_internal.cpp), on the CPU.
//
//   XemfgXefgProfileTests [table|resolve|all]   the table and synthetic images (CTest)
//   XemfgXefgProfileTests image <libxess_fg.dll> <fg78|fg95|none>
//                                               maps a real image (DONT_RESOLVE_DLL_REFERENCES: none of its code runs,
//                                               no imports load) and checks Resolve picks that family, or refuses it
#include "xess/vulkan/xefg_internal.h"
#include "xess/vulkan/xefg_internal_profiles.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int gFailures = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok) {
        ++gFailures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

using namespace xefg_internal;

const Profile* Family(const char* family)
{
    for (const Profile& p : kProfiles)
        if (std::strcmp(p.family, family) == 0) return &p;
    return nullptr;
}

// The rip-relative cookie load in an entry's witness: its target RVA, or 0 without one.
uint32_t CookieTarget(const Entry& e)
{
    for (size_t i = 0; i + 7 <= sizeof(e.bytes); ++i) {
        if (e.bytes[i] != 0x48 || e.bytes[i + 1] != 0x8b || e.bytes[i + 2] != 0x05) continue;
        int32_t disp = 0;
        std::memcpy(&disp, e.bytes + i + 3, sizeof(disp));
        return uint32_t(int64_t(e.rva) + int64_t(i) + 7 + disp);
    }
    return 0;
}

void Table()
{
    const Profile* fg78 = Family("fg78");
    const Profile* fg95 = Family("fg95");
    Check(fg78 && fg95 && std::size(kProfiles) == 2, "table: fg78 and fg95, nothing else");
    if (!fg78 || !fg95) return;
    Check(std::strcmp(fg78->version, "1.3.1.78") == 0 && std::strcmp(fg95->version, "1.3.4.95") == 0, "table: versions");

    // The fg78 pins vkprov.1 shipped with are unchanged.
    Check(fg78->timeDateStamp == 0x69CB0F4D && fg78->sizeOfImage == 0x015ED000 && fg78->checkSum == 0x015F3160, "fg78: identity");
    Check(fg78->entries[kCreateContext].rva == 0x1853B0 && fg78->entries[kInit].rva == 0x1871C0 &&
          fg78->entries[kExecute].rva == 0x185800 && fg78->entries[kDestroy].rva == 0x1874C0, "fg78: entry RVAs");
    Check(fg95->timeDateStamp == 0x6A95ED0F && fg95->sizeOfImage == 0x015D7000 && fg95->checkSum == 0x015D0F53, "fg95: identity");
    Check(fg95->entries[kCreateContext].rva == 0x1856A0 && fg95->entries[kInit].rva == 0x1874B0 &&
          fg95->entries[kExecute].rva == 0x185AF0 && fg95->entries[kDestroy].rva == 0x1877B0, "fg95: entry RVAs");

    static const char* const kNames[kEntryCount] = {"create_context", "init", "execute", "destroy"};
    for (const Profile& p : kProfiles) {
        const std::string f = p.family;
        // Exact identity only: any one field off, or another profile's field, is no profile.
        Check(FindProfile(p.timeDateStamp, p.sizeOfImage, p.checkSum) == &p, f + ": FindProfile exact");
        Check(!FindProfile(p.timeDateStamp + 1, p.sizeOfImage, p.checkSum), f + ": TimeDateStamp + 1 refused");
        Check(!FindProfile(p.timeDateStamp, p.sizeOfImage + 0x1000, p.checkSum), f + ": SizeOfImage + 0x1000 refused");
        Check(!FindProfile(p.timeDateStamp, p.sizeOfImage, p.checkSum - 1), f + ": CheckSum - 1 refused");
        for (const Profile& q : kProfiles) {
            if (&q == &p) continue;
            Check(!FindProfile(p.timeDateStamp, q.sizeOfImage, q.checkSum) && !FindProfile(q.timeDateStamp, p.sizeOfImage, q.checkSum) &&
                  !FindProfile(q.timeDateStamp, q.sizeOfImage, p.checkSum), f + ": mixed identity with " + q.family + " refused");
        }
        for (size_t i = 0; i < kEntryCount; ++i) {
            const Entry& e = p.entries[i];
            Check(std::strcmp(e.name, kNames[i]) == 0, f + ": entry " + std::to_string(i) + " is " + kNames[i]);
            Check(e.rva && e.rva % 16 == 0 && e.rva + sizeof(e.bytes) <= p.sizeOfImage, f + ": " + e.name + " RVA in the image");
            // Witness: the entry's cookie load reads this image's __security_cookie (the bytes belong at this RVA).
            Check(CookieTarget(e) == p.securityCookie, f + ": " + e.name + " cookie load -> securityCookie");
            for (size_t j = 0; j < i; ++j) Check(p.entries[j].rva != e.rva, f + ": " + e.name + " RVA distinct");
        }
    }
    // The RE finding the table encodes: the same code, moved. Each entry's bytes are fg78's but for the cookie load's
    // displacement (4 bytes).
    for (size_t i = 0; i < kEntryCount; ++i) {
        const Entry& a = fg78->entries[i];
        const Entry& b = fg95->entries[i];
        size_t differing = 0;
        for (size_t k = 0; k < sizeof(a.bytes); ++k) differing += a.bytes[k] != b.bytes[k];
        Check(differing >= 1 && differing <= 4, std::string("fg78/fg95 ") + a.name + ": only the cookie displacement differs");
    }
}

// A synthetic PE64 header with `profile`'s identity in ordinary (non-image) memory.
std::vector<uint8_t> Header(uint32_t stamp, uint32_t size, uint32_t sum)
{
    std::vector<uint8_t> image(0x1000, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.TimeDateStamp = stamp;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = size;
    nt->OptionalHeader.CheckSum = sum;
    return image;
}

std::string ResolveReason(const void* base, Api& api)
{
    std::string reason = "unset";
    api.family = "stale";
    const bool ok = Resolve(reinterpret_cast<HMODULE>(const_cast<void*>(base)), api, reason);
    return ok ? std::string("ok:") + api.family : reason;
}

void ResolveRouting()
{
    Api api;
    Check(ResolveReason(nullptr, api) == "no_module", "resolve: null module");
    std::vector<uint8_t> zeros(0x1000, 0);
    Check(ResolveReason(zeros.data(), api) == "not_pe", "resolve: not a PE");
    std::vector<uint8_t> dosOnly = Header(0, 0, 0);
    reinterpret_cast<IMAGE_NT_HEADERS64*>(dosOnly.data() + 0x80)->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    Check(ResolveReason(dosOnly.data(), api) == "not_pe64", "resolve: PE32 refused");

    for (const Profile& p : kProfiles) {
        const std::string f = p.family;
        // The identity selects the profile; ordinary memory is never an executable image page, so it stops there
        // (fail closed) with nothing published.
        auto image = Header(p.timeDateStamp, p.sizeOfImage, p.checkSum);
        Check(ResolveReason(image.data(), api) == f + "_entry_not_executable_create_context", f + ": resolve routes to its profile");
        Check(!api.module && !api.family && !api.createContext && !api.init && !api.execute && !api.destroy, f + ": nothing published on refusal");
        auto off = Header(p.timeDateStamp, p.sizeOfImage, p.checkSum ^ 1);
        Check(ResolveReason(off.data(), api) == "build_not_profiled", f + ": checksum off -> build_not_profiled");
    }
    // Calls through an unresolved Api never reach XeFG.
    Api none;
    Handle context = nullptr;
    Check(none.CreateContext(reinterpret_cast<ID3D12Device*>(&context), &context) == kException && none.Destroy(&context) == kException,
          "resolve: unresolved calls return kException");
}

int Image(const char* path, const char* expected)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wide(length > 0 ? size_t(length) : 0, L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), length);
    // Mapped as an executable image with section protections; DONT_RESOLVE_DLL_REFERENCES: no DllMain, no TLS
    // callbacks, no imports. (An image-resource mapping would map it without execute, which Resolve rightly refuses.)
    HMODULE mapped = LoadLibraryExW(wide.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!mapped) {
        std::printf("FAIL image: cannot map %s (%lu)\n", path, GetLastError());
        return 2;
    }
    HMODULE base = mapped;
    Api api;
    std::string reason;
    const bool ok = Resolve(base, api, reason);
    const std::string got = ok ? std::string(api.family) : "none:" + reason;
    std::printf("image %s -> %s\n", path, got.c_str());
    if (std::strcmp(expected, "none") == 0) Check(!ok && reason == "build_not_profiled", std::string("image: refused, got ") + got);
    else {
        Check(ok && std::strcmp(api.family, expected) == 0, std::string("image: expected ") + expected + ", got " + got);
        if (const Profile* p = ok ? Family(expected) : nullptr) {
            const auto* image = reinterpret_cast<const uint8_t*>(base);
            Check(reinterpret_cast<const uint8_t*>(api.createContext) == image + p->entries[kCreateContext].rva &&
                  reinterpret_cast<const uint8_t*>(api.init) == image + p->entries[kInit].rva &&
                  reinterpret_cast<const uint8_t*>(api.execute) == image + p->entries[kExecute].rva &&
                  reinterpret_cast<const uint8_t*>(api.destroy) == image + p->entries[kDestroy].rva, "image: entry addresses");
        }
    }
    FreeLibrary(mapped);
    return gFailures ? 1 : 0;
}
}

int main(int argc, char** argv)
{
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "image") {
        if (argc < 4) { std::printf("usage: %s image <libxess_fg.dll> <fg78|fg95|none>\n", argv[0]); return 2; }
        return Image(argv[2], argv[3]);
    }
    if (which == "table" || which == "all") Table();
    if (which == "resolve" || which == "all") ResolveRouting();
    if (which != "table" && which != "resolve" && which != "all") { std::printf("unknown case %s\n", which.c_str()); return 2; }
    std::printf("%s %s\n", which.c_str(), gFailures ? "FAILED" : "passed");
    return gFailures ? 1 : 0;
}
