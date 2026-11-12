#pragma once
// vkhair.4: universal hair's Vulkan side as the hair panel reads it (universal_hair.cpp ReadSnapshot). No Vulkan
// types: universal_hair.cpp does not build against the Vulkan headers.
#include <cstdint>

namespace rtx_hair::vk {
struct Status
{
    bool seen{};       // a Vulkan instance or device was created
    bool admitted{};   // a GPU was admitted (the LSS extension is reported on it)
    bool requested{};  // the game enabled the LSS extension at vkCreateDevice
    bool prepared{};   // and that device is converted
    bool failed{};     // its conversion could not start
    uint64_t extensionReports{}, featureReports{}, pipelineFlagsStripped{};
    uint64_t shadersRewritten{}, shadersNeutral{}, shadersFailed{};
    uint64_t sizeQueries{}, builds{}, updates{}, rebuilds{}, invalidGeometries{}, indirectBuilds{}, hostBuildsRefused{}, failures{};
    uint64_t topLevelPatched{}, topLevelCopyBytes{}, hairBlas{}, poolBuffers{}, poolBytes{}, evictions{};
    // hairmem.1: byte figures (topLevelCopyBytes too) are what is held now.
    uint64_t spareBytes{}, retiredBytes{}, reuses{}, allocations{}, hairDestroyed{}, gameHairBlasBytes{}, gameBlasBytes{};
    uint32_t ribbons{};
    bool fastBuild{};
    uint64_t lastBuildTick{}; // GetTickCount64 of the latest converted hair build, 0 if none
    char reason[256]{};       // why no GPU was admitted, or why the conversion failed
    char lastError[256]{};    // the builder's latest error
};
Status ReadStatus() noexcept;
}
