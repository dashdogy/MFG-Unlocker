#pragma once
// vkhair.4: universal hair's Vulkan side as the hair panel reads it (universal_hair.cpp ReadSnapshot). No Vulkan
// types: universal_hair.cpp does not build against the Vulkan headers.
#include <cstdint>

namespace rtx_hair::vk {
struct Status
{
    bool requested{};  // the game requested LSS at vkCreateDevice at least once
    bool prepared{};   // a converted device is live
    uint64_t extensionReports{}, featureReports{}, pipelineFlagsStripped{};
    uint64_t shadersRewritten{}, shadersNeutral{}, shadersFailed{};
    uint64_t sizeQueries{}, builds{}, updates{}, rebuilds{}, invalidGeometries{}, indirectBuilds{}, hostBuildsRefused{}, failures{};
    uint64_t topLevelPatched{}, topLevelCopyBytes{}, hairBlas{}, poolBuffers{}, poolBytes{}, evictions{};
    // hairmem.1: byte figures (topLevelCopyBytes too) are what is held now.
    uint64_t spareBytes{}, retiredBytes{}, reuses{}, allocations{}, hairDestroyed{}, gameHairBlasBytes{}, gameBlasBytes{};
    uint32_t ribbons{};
    bool fastBuild{};
    uint64_t lastBuildTick{}; // GetTickCount64 of the latest converted hair build, 0 if none
    char reason[256]{};       // why the game's requested LSS path is unavailable
    char lastError[256]{};    // the builder's latest error
};
Status ReadStatus() noexcept;
}
