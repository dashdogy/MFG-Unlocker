#include "common.h"
#include <atomic>
#include <fstream>
using namespace harness;
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

// sl1pass.1: a Streamline 1.x game (Dead Space, A Plague Tale: Requiem). The candidate's Streamline hooks are written
// for Streamline 2. Before the fix slSetTag(resource, tag, 0, extent) never reached Streamline (the id of 0 was read
// as "no tags"), so DLSS had no inputs, and each slSetConstants call raised an access violation reading the frame
// index as a frame token. Here the game's lookups must return the interposer's own entries and every call must
// arrive unchanged, with no fault.
namespace {
// Mirrors streamline_one_fixture.cpp.
struct Seen {
    unsigned initCalls, tagCalls, constantsCalls, evaluateCalls;
    const void* preferences; int applicationId;
    const void* resource; uint32_t tag, tagId; const void* extent;
    const void* constants; uint32_t frameIndex, constantsId;
};
using InitFn = bool (*)(const void*, int);
using TagFn = bool (*)(const void*, uint32_t, uint32_t, const void*);
using ConstantsFn = bool (*)(const void*, uint32_t, uint32_t);
using SeenFn = void (WINAPI*)(void*, unsigned);

// Access violations raised on the thread that makes the game's calls (the candidate's worker threads may probe
// memory under their own handlers; those are not this test's subject).
std::atomic<unsigned> gAccessViolations{0};
DWORD gCallingThread = 0;
LONG CALLBACK CountFaults(EXCEPTION_POINTERS* info) {
    if (info && info->ExceptionRecord && info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
        && GetCurrentThreadId() == gCallingThread)
        ++gAccessViolations;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The runtime log is UTF-8 or UTF-16: look for the text in both forms.
bool LogHas(const char* text) {
    std::error_code error;
    std::string wide;
    for (const char* at = text; *at; ++at) { wide.push_back(*at); wide.push_back('\0'); }
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path(), error)) {
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"RTXMFG-", 0) != 0 || entry.path().extension() != L".log") continue;
        // The candidate keeps the log open for writing: read it shared.
        const HANDLE file = CreateFileW(entry.path().c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;
        std::string content;
        char block[65536];
        DWORD read = 0;
        while (ReadFile(file, block, sizeof(block), &read, nullptr) && read) content.append(block, read);
        CloseHandle(file);
        if (content.find(text) != std::string::npos || content.find(wide) != std::string::npos) return true;
    }
    return false;
}
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 3) return 2;
    if (!Check(LoadCandidate(argv[2]) != nullptr, "exact candidate loads")) return 1;
    // Wait for the candidate's hook on this executable's GetProcAddress, the route a game's lookups take.
    bool ready = false;
    for (unsigned i = 0; i < 200 && !ready; ++i) {
        std::ifstream file("RTXMFG-Universal.status.json");
        const std::string status{std::istreambuf_iterator<char>(file), {}};
        file.close();
        ready = status.find("\"mainResolverDiscoveryInstalled\":true") != std::string::npos
            && status.find("\"pid\":" + std::to_string(GetCurrentProcessId()) + ",") != std::string::npos;
        if (!ready) Sleep(25);
    }
    if (!Check(ready, "main resolver hook is active before the Streamline 1 lookups")) return 1;

    // A second mapping of the same file under a name the candidate never treats as an interposer: the reference
    // for "entry bytes untouched".
    if (!Check(CopyFileW(L"streamline1\\sl.interposer.dll", L"StreamlineOneReference.dll", FALSE) != FALSE,
            "reference copy of the Streamline 1 fixture made")) return 1;
    const HMODULE reference = LoadLibraryW(L"StreamlineOneReference.dll");
    gCallingThread = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, &CountFaults);
    const HMODULE interposer = LoadLibraryW(L"streamline1\\sl.interposer.dll");
    if (!Check(interposer != nullptr && reference != nullptr && interposer != reference,
            "Streamline 1 interposer and its reference copy load")) return 1;
    Check(GetProcAddress(interposer, "slGetFeatureFunction") == nullptr, "fixture has no slGetFeatureFunction, as Streamline 1");

    const auto seenOf = reinterpret_cast<SeenFn>(GetProcAddress(interposer, "FixtureSeen"));
    if (!Check(seenOf != nullptr, "fixture reports what reached it")) return 1;
    // Give the discovery worker time to look at the new module, as a game's start-up does. run.py asserts the log
    // line after this process exits; here it only decides how long to wait.
    bool logged = false;
    for (unsigned i = 0; i < 200 && !logged; ++i) {
        logged = LogHas("Streamline 1.x interposer: no Streamline hooks installed");
        if (!logged) Sleep(25);
    }
    printf("NOTE streamlineOneLogLineSeenWhileRunning=%u\n", logged);

    for (const char* name : {"slInit", "slSetTag", "slSetConstants"}) {
        const auto* resolved = reinterpret_cast<const unsigned char*>(GetProcAddress(interposer, name));
        const auto* pristine = reinterpret_cast<const unsigned char*>(GetProcAddress(reference, name));
        const bool inside = resolved && pristine
            && reinterpret_cast<uintptr_t>(resolved) - reinterpret_cast<uintptr_t>(interposer)
                == reinterpret_cast<uintptr_t>(pristine) - reinterpret_cast<uintptr_t>(reference);
        printf("NOTE %s resolved=%p reference=%p\n", name, static_cast<const void*>(resolved), static_cast<const void*>(pristine));
        Check(inside, "the game's lookup returns the interposer's own entry, not a hook");
        Check(inside && memcmp(resolved, pristine, 16) == 0, "the entry's first bytes are untouched (no detour)");
    }
    const auto init = reinterpret_cast<InitFn>(GetProcAddress(interposer, "slInit"));
    const auto tag = reinterpret_cast<TagFn>(GetProcAddress(interposer, "slSetTag"));
    const auto constants = reinterpret_cast<ConstantsFn>(GetProcAddress(interposer, "slSetConstants"));
    if (!Check(init && tag && constants, "Streamline 1 entries resolve")) return 1;

    unsigned char preferences[96];
    for (unsigned i = 0; i < sizeof(preferences); ++i) preferences[i] = static_cast<unsigned char>(0xA0 + i);
    unsigned char expected[sizeof(preferences)];
    memcpy(expected, preferences, sizeof(preferences));
    unsigned char resource[32]{}, extent[16]{}, values[256]{};
    Seen seen{};

    Check(init(preferences, 0x6006603), "slInit returns Streamline's answer");
    seenOf(&seen, sizeof(seen));
    Check(seen.initCalls == 1 && seen.preferences == preferences && seen.applicationId == 0x6006603,
        "slInit reaches Streamline once with the game's own preferences pointer and application id");
    Check(memcmp(preferences, expected, sizeof(preferences)) == 0, "the game's preferences are not modified");

    // The case that broke DLSS: viewport id 0, which the Streamline 2 hook read as "no tags".
    Check(tag(resource, 3, 0, extent), "slSetTag with id 0 returns Streamline's answer");
    seenOf(&seen, sizeof(seen));
    Check(seen.tagCalls == 1 && seen.resource == resource && seen.tag == 3 && seen.tagId == 0 && seen.extent == extent,
        "slSetTag with id 0 reaches Streamline with every argument unchanged");
    Check(tag(resource, 1, 2, nullptr), "slSetTag with a nonzero id and no extent returns Streamline's answer");
    seenOf(&seen, sizeof(seen));
    Check(seen.tagCalls == 2 && seen.tag == 1 && seen.tagId == 2 && seen.extent == nullptr,
        "slSetTag with a nonzero id reaches Streamline unchanged");

    // Frame indexes as Dead Space sent them: small integers that are not readable addresses.
    unsigned calls = 2;
    for (const uint32_t frameIndex : {0xFAu, 0x19B9u, 0x19BAu}) {
        Check(constants(values, frameIndex, 0), "slSetConstants returns Streamline's answer");
        seenOf(&seen, sizeof(seen));
        ++calls;
        Check(seen.constantsCalls == calls - 2 && seen.constants == values && seen.frameIndex == frameIndex
            && seen.constantsId == 0, "slSetConstants reaches Streamline with the frame index unchanged");
    }
    Check(gAccessViolations.load() == 0, "no access violation is raised by the Streamline 1 calls");
    printf("NOTE accessViolations=%u\n", gAccessViolations.load());
    return gFailures ? 1 : 0;
}
