#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
// sl1pass.1: a Streamline 1.x shaped interposer (Dead Space ships 1.1.1, A Plague Tale: Requiem 1.5.4.7). Minimal
// ABI fixture, not an NVIDIA binary. It exports the Streamline 1 names with the Streamline 1 signatures and has no
// slGetFeatureFunction, which is how the candidate tells the two generations apart. Each entry records what
// reached it, so the test can prove the candidate passed the game's calls on untouched.
namespace {
struct Seen {
    unsigned initCalls, tagCalls, constantsCalls, evaluateCalls;
    const void* preferences; int applicationId;
    const void* resource; uint32_t tag, tagId; const void* extent;
    const void* constants; uint32_t frameIndex, constantsId;
};
Seen seen{};
std::atomic<unsigned> initCalls{0}, tagCalls{0}, constantsCalls{0}, evaluateCalls{0};
}
extern "C" __declspec(dllexport) bool slInit(const void* preferences, int applicationId) {
    seen.preferences = preferences; seen.applicationId = applicationId;
    ++initCalls;
    return true;
}
extern "C" __declspec(dllexport) bool slShutdown() { return true; }
extern "C" __declspec(dllexport) bool slSetTag(const void* resource, uint32_t tag, uint32_t id, const void* extent) {
    seen.resource = resource; seen.tag = tag; seen.tagId = id; seen.extent = extent;
    ++tagCalls;
    return true;
}
extern "C" __declspec(dllexport) bool slSetConstants(const void* values, uint32_t frameIndex, uint32_t id) {
    seen.constants = values; seen.frameIndex = frameIndex; seen.constantsId = id;
    ++constantsCalls;
    return true;
}
extern "C" __declspec(dllexport) bool slEvaluateFeature(void*, uint32_t, uint32_t, uint32_t) {
    ++evaluateCalls;
    return true;
}
extern "C" __declspec(dllexport) void WINAPI FixtureSeen(void* out, unsigned size) {
    Seen copy = seen;
    copy.initCalls = initCalls.load(); copy.tagCalls = tagCalls.load();
    copy.constantsCalls = constantsCalls.load(); copy.evaluateCalls = evaluateCalls.load();
    if (out && size == sizeof(copy)) memcpy(out, &copy, sizeof(copy));
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
