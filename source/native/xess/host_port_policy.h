#pragma once
// hostport.1: whether OptiScaler's own XeFG (FGOutput xefg, or the Nvngx providers) may publish the shader and model
// port on the provider image (xess_mod.cpp PublishHostPort). Header only, so tests/xemfg_standalone checks it on the CPU.

namespace xess_host_port
{
struct PortState
{
    bool trackedContexts = false;   // a tracked context (the game's own XeFG) is alive
    bool model = false;             // a model port is published on the image
    bool unlock = false;            // the frame-count unlock is applied (it stays for the process)
    bool unlockSameImage = false;   // ... on the image the host would publish on
    bool unlockKeeps2x = false;     // ... with the 2x-model site (XeFG's 2x network for every count)
};

// The tracked route restores its model when its last context goes and keeps its unlock. That unlock leaves out the
// 2x-model site, so XeFG asks for the ported model 16 above one generated frame. When only that idle unlock is left on
// this image, the host publishes the port on it again. A live tracked context or model is adopted by the caller
// instead; a 2x-model unlock or one on another image is refused (the host then falls back to the 2x model).
constexpr bool MayPublishHostPort(const PortState& state) noexcept
{
    if (state.trackedContexts || state.model) return false;
    return !state.unlock || (state.unlockSameImage && !state.unlockKeeps2x);
}
}
