// dlss5.1 provider harness: NVIDIA's DLSSNR provider (NGX feature 18) on synthetic
// frames, with the production key mapping (dlss5_policy.h Build, dlss5_parameters.h).
//
// Dlss5ProviderHarness --dir <folder with nvngx_dlssnr.dll> --out <folder>
//     [--width W --height H] [--render-scale S] [--frames N] [--quality 0|1]
//
// Each mode creates one feature and evaluates N frames of a scene that moves one
// pixel per frame with exact motion vectors:
//   separate  Color = scene (non-pixel-shader resource), Output = its own UAV texture
//   inplace   the scene is copied into Output first; Color = Output (UAV), as dlss5.cpp runs it
// --integrated runs dlss5.cpp's D3D12 stage on the harness's device; --d3d11 [--d3d11-depth r32|r24g8] runs the
// same frames through the DirectX 11 bridge on a D3D11 device (dx11nr.1). --dump/--compare compare two runs.
// Checks: Create/Evaluate succeed, outputs finite and not a copy of the input, and
// the in-place output matches the separate one. Writes PNG previews and timings.
// Exit codes: 0 pass, 1 check failed, 2 setup failure, 3 provider unavailable on this GPU.
#include "dlss5.h"
#include "dlss5_d3d11.h"
#include "dlss5_settings.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
struct Options
{
    std::wstring dir, out;
    bool direct = false, integrated = false, hdr=false;
    // dlss5.4: the Present stage (the output texture stands in for the swapchain
    // image) with this evaluation's depth and motion vectors; UI Correction choice.
    bool present = false;
    // dlss5.5: Hook Method Auto, Resolution Scale Auto and Adaptive exposure;
    // without it the harness keeps the fixed After upscaling stage and scale.
    bool automatic = false;
    // Control audit: settings JSON (dlss5* keys), a static real frame, raw output dump.
    std::string settingsJson;
    std::wstring image, dump;
    // halfcarry.1: the last frame's scene color, raw float RGB like the output dump.
    std::wstring dumpInput;
    bool imagePq = false;
    bool motionRgba = false;
    uint32_t uiCorrection = 0, resources = 1;
    uint32_t intensity=100;
    uint32_t width = 1920, height = 1080;
    float renderScale = 0.6667f;
    uint32_t frames = 12;
    uint32_t quality = 0;
    // dlss5.3 settings for the integrated route (dlss5_policy.h ranges).
    float scale = 0.0f, structure = -1.0f;
    uint32_t passes = 1, model = 0, preset = 0;
    // autoscale.1: at this integrated frame, Resolution Scale 75 % and the next
    // Model; every frame from the first evaluation on must still be evaluated.
    uint32_t step = 0;
    // autorect.1 --auto-step N: Resolution Scale Auto at 100 %, its level pinned (testing::SetAutoLevel) and moved
    // to 75 % at integrated frame N. --reset-at N: the game's Reset at frame N as well as frame 0 (a fixed-scale
    // reference whose history starts where the step's does).
    uint32_t autoStep = 0, resetAt = 0;
    // dx11nr.1 --d3d11 (RunD3D11); --d3d11-depth r32: R32_TYPELESS, r24g8: R24G8_TYPELESS, both depth-stencil + shader resource.
    bool d3d11 = false;
    std::wstring d3d11Depth = L"r32";
    // --compare: a raw float RGB dump (--dump) of another run on the same image, e.g. --integrated against --d3d11.
    std::wstring compare;
};

std::FILE* gLog = nullptr;
bool gNoShutdown=false;
bool gAnimate=false;
// halfcarry.1 --ghost: see Scene and GhostGeometry.
bool gGhost=false;
float GhostPostX(uint32_t width, float shift) { return 0.1f * width + 5.0f * shift; }
float GhostPostWidth(uint32_t width) { return 0.06f * width; }
void Print(const char* format, ...)
{
    char buffer[2048]{};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::fputs(buffer, stdout);std::fflush(stdout);
    if (gLog) { std::fputs(buffer, gLog); std::fflush(gLog); }
}

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature source)
{
    if (!message) return;
    if (gLog) { std::fprintf(gLog, "[ngx %d] %s", static_cast<int>(source), message); std::fflush(gLog); }
    if (std::strstr(message, "DLSSNR") || std::strstr(message, "rror") || std::strstr(message, "rchitecture"))
        std::printf("[ngx %d] %s", static_cast<int>(source), message);
}

uint16_t Half(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) return static_cast<uint16_t>(sign);
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
}

float FromHalf(uint16_t half)
{
    const uint32_t sign = (half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1F;
    const uint32_t mantissa = half & 0x3FF;
    uint32_t bits = 0;
    if (exponent == 0) bits = sign; // flush denormals
    else if (exponent == 31) bits = sign | 0x7F800000u | (mantissa << 13);
    else bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    float value = 0;
    std::memcpy(&value, &bits, 4);
    return value;
}

// A lit outdoor-ish scene: sky gradient, ground, three shaded spheres, a face-coloured
// ellipse with features and fine stripes. `shift` moves everything right by pixels.
void Scene(uint32_t width, uint32_t height, float shift, std::vector<uint16_t>& rgba)
{
    rgba.assign(size_t{width} * height * 4, 0);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f - shift) / width;
            const float v = (static_cast<float>(y) + 0.5f) / height;
            float r = 0.35f + 0.4f * (1 - v), g = 0.5f + 0.35f * (1 - v), b = 0.85f;
            if (v > 0.62f)
            {
                const float checker = (static_cast<int>(u * 24) + static_cast<int>(v * 18)) & 1 ? 0.18f : 0.32f;
                r = checker * 0.9f; g = checker * 0.75f; b = checker * 0.55f;
            }
            const float sx[3]{0.22f, 0.5f, 0.78f}, sr[3]{0.12f, 0.09f, 0.14f};
            const float sc[3][3]{{0.9f, 0.2f, 0.15f}, {0.2f, 0.8f, 0.3f}, {0.85f, 0.85f, 0.9f}};
            for (int s = 0; s < 3; ++s)
            {
                const float dx = (u - sx[s]) * width / height, dy = v - 0.55f;
                const float d2 = dx * dx + dy * dy;
                if (d2 < sr[s] * sr[s])
                {
                    const float nz = std::sqrt(1 - d2 / (sr[s] * sr[s]));
                    const float light = 0.15f + 0.85f * std::max(0.0f, (-dx / sr[s] * 0.4f - dy / sr[s] * 0.6f + nz * 0.7f));
                    r = sc[s][0] * light; g = sc[s][1] * light; b = sc[s][2] * light;
                }
            }
            // Face-like ellipse with eyes and mouth.
            const float fx = (u - 0.5f) * width / height / 0.07f, fy = (v - 0.25f) / 0.1f;
            if (fx * fx + fy * fy < 1)
            {
                r = 0.85f; g = 0.62f; b = 0.5f;
                const float shade = 0.75f + 0.25f * (1 - fx * fx);
                r *= shade; g *= shade; b *= shade;
                if ((std::abs(fx - 0.35f) < 0.12f || std::abs(fx + 0.35f) < 0.12f) && std::abs(fy + 0.2f) < 0.08f)
                { r = 0.08f; g = 0.06f; b = 0.05f; }
                if (std::abs(fx) < 0.35f && std::abs(fy - 0.45f) < 0.05f) { r = 0.5f; g = 0.15f; b = 0.15f; }
            }
            if (v > 0.05f && v < 0.12f && u > 0.05f && u < 0.4f)
            {
                const float stripe = (x % 3) == 0 ? 1.0f : 0.1f;
                r = g = b = stripe;
            }
            // dlss5boost.3 --animate: a band of waves animated in place each frame,
            // which the motion vectors (the whole scene's shift) do not describe, like
            // a shader-animated water surface.
            if (gAnimate && v > 0.15f && v < 0.3f)
            {
                const float wave = 0.5f + 0.4f * std::sin(u * 90.0f + shift * 2.1f) * std::sin(v * 70.0f - shift * 1.7f);
                r = 0.2f * wave; g = 0.45f * wave; b = 0.7f * wave;
            }
            // halfcarry.1 --ghost: grass blades swaying sideways each frame, which the
            // motion vectors (the whole scene's shift) do not describe, like wind
            // animation; and a post moving 5 pixels a frame in front of everything, with
            // its own motion vectors and nearer depth (GhostGeometry), which uncovers the
            // scene behind it.
            if (gGhost)
            {
                if (v > 0.7f)
                {
                    const float sway = 3.0f * std::sin(0.8f * shift + 9.0f * v);
                    const float blade = std::fmod(static_cast<float>(x) + 0.5f - shift - sway + 7000.0f, 7.0f) / 7.0f;
                    const float tip = 0.6f + 0.4f * (v - 0.7f) / 0.3f;
                    if (blade < 0.35f) { r = 0.55f * tip; g = 0.68f * tip; b = 0.18f * tip; }
                    else { r = 0.10f; g = 0.18f; b = 0.05f; }
                }
                const float post = static_cast<float>(x) + 0.5f - GhostPostX(width, shift);
                if (post >= 0 && post < GhostPostWidth(width) && v > 0.2f)
                {
                    const float grain = ((static_cast<int>(v * height) / 3) & 1) ? 0.42f : 0.30f;
                    const float edge = post < 2 || post > GhostPostWidth(width) - 2 ? 0.15f : 1.0f;
                    r = grain * edge; g = grain * 0.62f * edge; b = grain * 0.35f * edge;
                }
            }
            const size_t i = (size_t{y} * width + x) * 4;
            rgba[i] = Half(r); rgba[i + 1] = Half(g); rgba[i + 2] = Half(b); rgba[i + 3] = Half(1);
        }
}

// halfcarry.1 --ghost: this frame's depth (inverted, the post nearest) and motion
// vectors (render pixels; the post's own speed) at render size.
void GhostGeometry(uint32_t width, uint32_t height, uint32_t renderWidth, uint32_t renderHeight, float shift,
    size_t motionChannels, std::vector<float>& depth, std::vector<uint16_t>& motion)
{
    depth.assign(size_t{renderWidth} * renderHeight, 0.0f);
    motion.assign(size_t{renderWidth} * renderHeight * motionChannels, 0);
    for (uint32_t y = 0; y < renderHeight; ++y)
        for (uint32_t x = 0; x < renderWidth; ++x)
        {
            const size_t i = size_t{y} * renderWidth + x;
            const float ox = (static_cast<float>(x) + 0.5f) * width / renderWidth;
            const float v = (static_cast<float>(y) + 0.5f) / renderHeight;
            const float post = ox - GhostPostX(width, shift);
            const bool inPost = post >= 0 && post < GhostPostWidth(width) && v > 0.2f;
            depth[i] = inPost ? 0.97f : 0.1f + 0.8f * y / renderHeight;
            const float speed = inPost ? 5.0f : 1.0f;
            motion[i * motionChannels] = Half(-speed * renderWidth / width);
            motion[i * motionChannels + 1] = Half(0);
            if (motionChannels == 4) { motion[i * 4 + 2] = Half(0); motion[i * 4 + 3] = Half(1); }
        }
}

// Minimal PNG (stored deflate blocks), 8-bit RGB, gamma 2.2 of linear input.
uint32_t Crc(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu)
{
    static uint32_t table[256]{};
    if (!table[1])
        for (uint32_t n = 0; n < 256; ++n)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

// A static real frame as RGBA16F scene color: SDR sRGB bytes to linear, or PQ
// BT.2020 16-bit to linear BT.709 with 1.0 = 100 nits.
bool LoadFrame(const std::wstring& path, bool pq, uint32_t width, uint32_t height, std::vector<uint16_t>& rgba)
{
    std::FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) return false;
    const size_t texels = size_t{width} * height, bytes = texels * 3 * (pq ? 2 : 1);
    std::vector<uint8_t> raw(bytes);
    const bool complete = std::fread(raw.data(), 1, bytes, file) == bytes && std::fgetc(file) == EOF;
    std::fclose(file);
    if (!complete) return false;
    rgba.assign(texels * 4, 0);
    for (size_t i = 0; i < texels; ++i)
    {
        float c[3]{};
        for (int k = 0; k < 3; ++k)
        {
            if (pq)
            {
                const float e = (raw[(i * 3 + k) * 2] | (raw[(i * 3 + k) * 2 + 1] << 8)) / 65535.0f;
                const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
                const float p = std::pow(e, 1.0f / m2);
                c[k] = 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1) / 100.0f;
            }
            else
            {
                const float v = raw[i * 3 + k] / 255.0f;
                c[k] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
            }
        }
        if (pq)
        {
            // BT.2020 -> BT.709 primaries (linear).
            const float r = 1.6605f * c[0] - 0.5876f * c[1] - 0.0728f * c[2];
            const float g = -0.1246f * c[0] + 1.1329f * c[1] - 0.0083f * c[2];
            const float b = -0.0182f * c[0] - 0.1006f * c[1] + 1.1187f * c[2];
            c[0] = std::max(r, 0.0f); c[1] = std::max(g, 0.0f); c[2] = std::max(b, 0.0f);
        }
        for (int k = 0; k < 3; ++k) rgba[i * 4 + k] = Half(c[k]);
        rgba[i * 4 + 3] = Half(1.0f);
    }
    return true;
}

void WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<float>& rgb)
{
    std::vector<uint8_t> raw;
    raw.reserve(size_t{height} * (width * 3 + 1));
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0);
        for (uint32_t x = 0; x < width * 3; ++x)
        {
            const float value = std::clamp(rgb[size_t{y} * width * 3 + x], 0.0f, 1.0f);
            raw.push_back(static_cast<uint8_t>(std::lround(std::pow(value, 1 / 2.2f) * 255)));
        }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (const uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
    for (size_t offset = 0; offset < raw.size(); offset += 65535)
    {
        const size_t length = std::min<size_t>(65535, raw.size() - offset);
        z.push_back(offset + length == raw.size() ? 1 : 0);
        z.push_back(length & 0xFF); z.push_back(length >> 8);
        z.push_back(~length & 0xFF); z.push_back((~length >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + offset, raw.begin() + offset + length);
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((adler >> s) & 0xFF);
    std::FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) return;
    const auto chunk = [&](const char* type, const std::vector<uint8_t>& data)
    {
        const uint32_t length = static_cast<uint32_t>(data.size());
        const uint8_t be[4]{uint8_t(length >> 24), uint8_t(length >> 16), uint8_t(length >> 8), uint8_t(length)};
        std::fwrite(be, 1, 4, file);
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        std::fwrite(body.data(), 1, body.size(), file);
        const uint32_t crc = Crc(body.data(), body.size()) ^ 0xFFFFFFFFu;
        const uint8_t c[4]{uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
        std::fwrite(c, 1, 4, file);
    };
    const uint8_t signature[8]{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(signature, 1, 8, file);
    chunk("IHDR", {uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width),
        uint8_t(height >> 24), uint8_t(height >> 16), uint8_t(height >> 8), uint8_t(height), 8, 2, 0, 0, 0});
    chunk("IDAT", z);
    chunk("IEND", {});
    std::fclose(file);
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t value = 0;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> queryReadback;
    uint64_t frequency = 0;

    bool Submit()
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++value);
        if (fence->GetCompletedValue() < value)
        {
            fence->SetEventOnCompletion(value, event);
            if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) return false;
        }
        if (device->GetDeviceRemovedReason() != S_OK) return false;
        allocator->Reset();
        return SUCCEEDED(list->Reset(allocator.Get(), nullptr));
    }
};

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
    D3D12_RESOURCE_STATES state, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
            IID_PPV_ARGS(&resource)))) return {};
    resource->SetName(name);
    return resource;
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{type};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    const auto state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
            IID_PPV_ARGS(&resource)))) return {};
    return resource;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

// Uploads tightly packed texels of `pixelBytes` into `texture` (in COPY_DEST).
bool Upload(Gpu& gpu, ID3D12Resource* texture, const void* data, uint32_t pixelBytes,
    std::vector<ComPtr<ID3D12Resource>>& keep)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    auto upload = Buffer(gpu.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD);
    if (!upload) return false;
    uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(mapped + footprint.Offset + size_t{y} * footprint.Footprint.RowPitch,
            static_cast<const uint8_t*>(data) + size_t{y} * desc.Width * pixelBytes, desc.Width * pixelBytes);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = footprint;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    keep.push_back(upload);
    return true;
}

// Reads an RGBA16F texture (in COPY_SOURCE) back as linear RGB floats.
bool Readback(Gpu& gpu, ID3D12Resource* texture, std::vector<float>& rgb)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    auto readback = Buffer(gpu.device.Get(), total, D3D12_HEAP_TYPE_READBACK);
    if (!readback) return false;
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (!gpu.Submit()) return false;
    uint8_t* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    const uint32_t width = static_cast<uint32_t>(desc.Width);
    rgb.resize(size_t{width} * desc.Height * 3);
    for (UINT y = 0; y < rows; ++y)
    {
        const auto* row = reinterpret_cast<const uint16_t*>(mapped + footprint.Offset + size_t{y} * footprint.Footprint.RowPitch);
        for (uint32_t x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) rgb[(size_t{y} * width + x) * 3 + c] = FromHalf(row[x * 4 + c]);
    }
    readback->Unmap(0, nullptr);
    return true;
}

struct Stats { double meanAbsDiff = 0, maxAbsDiff = 0, meanValue = 0; size_t nonFinite = 0; };
Stats Compare(const std::vector<float>& a, const std::vector<float>& b)
{
    Stats stats{};
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (!std::isfinite(a[i])) { ++stats.nonFinite; continue; }
        const double diff = std::abs(static_cast<double>(a[i]) - b[i]);
        stats.meanAbsDiff += diff;
        stats.maxAbsDiff = std::max(stats.maxAbsDiff, diff);
        stats.meanValue += a[i];
    }
    stats.meanAbsDiff /= std::max<size_t>(1, a.size());
    stats.meanValue /= std::max<size_t>(1, a.size());
    return stats;
}

bool ParseOptions(int argc, wchar_t** argv, Options& options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        const bool more = i + 1 < argc;
        if (arg == L"--direct") options.direct = true;
        else if(arg==L"--integrated") options.integrated=true;
        else if(arg==L"--present") options.integrated=options.present=true;
        else if(arg==L"--auto") options.integrated=options.automatic=true;
        else if(arg==L"--settings" && more){const std::wstring w=argv[++i];options.settingsJson.assign(w.begin(),w.end());}
        else if(arg==L"--image" && more) options.image=argv[++i];
        else if(arg==L"--image-pq" && more){options.image=argv[++i];options.imagePq=true;}
        else if(arg==L"--dump" && more) options.dump=argv[++i];
        else if(arg==L"--motion-rgba16f") options.motionRgba=true;
        else if(arg==L"--ui-correction" && more) options.uiCorrection=static_cast<uint32_t>(_wtoi(argv[++i]));
        else if(arg==L"--resources" && more) options.resources=static_cast<uint32_t>(_wtoi(argv[++i]));
        else if(arg==L"--hdr") options.hdr=true;
        else if(arg==L"--intensity" && more) options.intensity=static_cast<uint32_t>(_wtoi(argv[++i]));
        else if(arg==L"--no-shutdown") gNoShutdown=true;
        else if(arg==L"--animate") gAnimate=true;
        else if(arg==L"--ghost") gGhost=true;
        else if(arg==L"--dump-input" && more) options.dumpInput=argv[++i];
        else if (arg == L"--dir" && more) options.dir = argv[++i];
        else if (arg == L"--out" && more) options.out = argv[++i];
        else if (arg == L"--width" && more) options.width = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (arg == L"--height" && more) options.height = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (arg == L"--render-scale" && more) options.renderScale = static_cast<float>(_wtof(argv[++i]));
        else if (arg == L"--frames" && more) options.frames = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (arg == L"--quality" && more) options.quality = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (arg == L"--scale" && more) options.scale = static_cast<float>(_wtof(argv[++i]));
        else if (arg == L"--passes" && more) options.passes = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (arg == L"--model" && more) options.model = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (arg == L"--preset" && more) options.preset = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (arg == L"--structure" && more) options.structure = static_cast<float>(_wtof(argv[++i]));
        else if(arg==L"--step" && more) options.integrated=true,options.step=static_cast<uint32_t>(_wtoi(argv[++i]));
        else if(arg==L"--auto-step" && more) options.integrated=options.automatic=true,options.autoStep=static_cast<uint32_t>(_wtoi(argv[++i]));
        else if(arg==L"--reset-at" && more) options.resetAt=static_cast<uint32_t>(_wtoi(argv[++i]));
        else if(arg==L"--d3d11") options.integrated=options.d3d11=true;
        else if(arg==L"--d3d11-depth" && more) options.d3d11Depth=argv[++i];
        else if(arg==L"--compare" && more) options.compare=argv[++i];
        else return false;
    }
    // The bridge runs Auto, After and Before upscaling on its own device: no Present stage, no direct-only run.
    if (options.d3d11 && (options.present || options.direct)) return false;
    // --auto-step steps the D3D12 integrated route after its first frames; --step changes the feature instead.
    if (options.autoStep && (options.d3d11 || options.step || options.autoStep >= options.frames - 1)) return false;
    if (options.d3d11Depth != L"r32" && options.d3d11Depth != L"r24g8") return false;
    return !options.dir.empty() && !options.out.empty() && options.width >= 64 && options.height >= 64
        && options.width<=7680 && options.height<=4320 && options.frames>=2 && options.frames<=128
        && options.quality<=1 && options.intensity<=100 && options.renderScale > 0.2f && options.renderScale <= 1.0f
        && options.uiCorrection<=2 && options.resources<=2;
}

// The settings every route of the harness runs with; false when --settings is rejected.
bool HarnessSettings(const Options& options, dlss5::Settings& settings)
{
    settings = {};
    settings.enabled = true;
    // --quality 1: the former 0.85 network extent; --intensity in percent.
    settings.scale = options.quality ? 85.0f : 100.0f;settings.intensity=static_cast<float>(options.intensity)/100.0f;
    if (options.scale > 0.0f) settings.scale = options.scale;
    settings.passes = options.passes; settings.model = options.model; settings.preset = options.preset;
    if (options.structure >= 0.0f) settings.structure = options.structure;
    if (!options.automatic) {
        settings.stage = static_cast<uint32_t>(dlss5::Stage::eUpscaled);
        settings.scaleAuto = false; settings.adaptiveExposure = false;
    }
    // autorect.1 --auto-step: Resolution Scale Auto under the slider (100 % unless --scale), plain exposure.
    if (options.autoStep) { settings.scaleAuto = true; settings.adaptiveExposure = false; }
    if (options.present) settings.stage = static_cast<uint32_t>(dlss5::Stage::ePresent);
    settings.uiCorrection = options.uiCorrection; settings.resources = options.resources;
    if (!options.settingsJson.empty() && !dlss5::settings_json::Parse(options.settingsJson, settings))
    {
        Print("settings JSON rejected: %s\n", options.settingsJson.c_str());
        return false;
    }
    settings = dlss5::Sanitized(settings);
    if (!options.settingsJson.empty()) Print("settings JSON %s\n", options.settingsJson.c_str());
    Print("settings scale %.1f (auto %d) passes %u model %u preset %u structure %.2f stage %u adaptive exposure %d uiCorrection %u (provider value %d)\n",
        settings.scale, settings.scaleAuto ? 1 : 0, settings.passes, settings.model, settings.preset, settings.structure, settings.stage,
        settings.adaptiveExposure ? 1 : 0,
        settings.uiCorrection, dlss5::UiCorrectionValue(settings, static_cast<dlss5::Stage>(settings.stage)));
    return true;
}

// The game DLSS feature's Create keys: render size in, output size out, motion vectors at render size, inverted depth.
dlss_sr::CreateKeys HarnessCreate(const Options& options, uint32_t width, uint32_t height, uint32_t renderWidth, uint32_t renderHeight)
{
    dlss_sr::CreateKeys create{};
    create.present = dlss_sr::CreateKeys::eRequired | dlss_sr::CreateKeys::eCreateFlags;
    create.width = renderWidth; create.height = renderHeight; create.outWidth = width; create.outHeight = height;
    create.createFlags = dlss5::kMotionLowResFlag | dlss5::kDepthInvertedFlag | (options.hdr?1:0);
    return create;
}

bool CompareReference(const Options& options, const char* mode, const std::vector<float>& out)
{
    if (options.compare.empty()) return true;
    std::vector<float> reference(out.size());
    std::FILE* file = _wfopen(options.compare.c_str(), L"rb");
    const bool complete = file && std::fread(reference.data(), sizeof(float), reference.size(), file) == reference.size()
        && std::fgetc(file) == EOF;
    if (file) std::fclose(file);
    if (!complete)
    {
        Print("[%s] reference %ls does not hold this image's %zu floats\n", mode, options.compare.c_str(), out.size());
        return false;
    }
    const Stats same = Compare(out, reference);
    Print("[%s] vs reference %ls: mean |diff| %.6f max %.5f\n", mode, options.compare.c_str(), same.meanAbsDiff, same.maxAbsDiff);
    return !same.nonFinite && same.meanAbsDiff <= 2e-3;
}

// --d3d11: the game's DLSS. After upscaling its output is already in place; Before upscaling it copies the Color it
// was handed into the output's corner (render size), and counts the calls whose Color DLSS 5 substituted.
ID3D11Resource* gGameColor = nullptr;
ID3D11Resource* gGameOutput = nullptr;
bool gFakeCopiesColor = false;
unsigned long long gFakeCalls = 0, gFakeSubstituted = 0;
NVSDK_NGX_Result NVSDK_CONV FakeEvaluate(ID3D11DeviceContext* context, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback)
{
    ++gFakeCalls;
    ID3D11Resource* color = nullptr;
    if (parameters->Get("Color", &color) != NVSDK_NGX_Result_Success) color = nullptr;
    if (color && color != gGameColor) ++gFakeSubstituted;
    if (gFakeCopiesColor && color) context->CopySubresourceRegion(gGameOutput, 0, 0, 0, 0, color, 0, nullptr);
    return NVSDK_NGX_Result_Success;
}

// dx11nr.1 --d3d11: the integrated route's frames through the DirectX 11 bridge (dlss5_d3d11.h), the harness as the
// game: NGX D3D11 on a D3D11 device, the image, depth-stencil depth, motion vectors and color in D3D11, evaluated
// through the bridge around FakeEvaluate. Same frames, keys and summary line as --integrated, so --integrated --dump
// a.bin and --d3d11 --compare a.bin compare the same network path. GPU ms include the bridge's copies and waits.
int RunD3D11(const Options& options, IDXGIAdapter1* adapter, uint32_t width, uint32_t height, uint32_t renderWidth,
    uint32_t renderHeight)
{
    const char* const mode = "d3d11";
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr,
            &context))) { Print("D3D11CreateDevice failed\n"); return 2; }
    // NGX on the game's D3D11 device, as a game initializes it.
    const std::wstring appData = (std::filesystem::path(options.out) / L"ngx").wstring();
    std::filesystem::create_directories(appData);
    const wchar_t* paths[]{options.dir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = &NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID("b5a8a8d2-3d5c-4f11-9e0f-5d1a6c7e8f90",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", appData.c_str(), device.Get(), &info);
    Print("NGX D3D11 init 0x%08X\n", static_cast<uint32_t>(result));
    if (NVSDK_NGX_FAILED(result)) return 2;
    const HMODULE runtime = GetModuleHandleW(L"_nvngx.dll");
    if (!runtime) { Print("_nvngx.dll is not loaded\n"); return 2; }
    dlss5::d3d11::Api api{};
    api.runtime = runtime;
    api.evaluate = &FakeEvaluate;
    api.d3d12.runtime = runtime;
    api.d3d12.create = reinterpret_cast<dlss5::CreateFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_CreateFeature"));
    api.d3d12.evaluate = reinterpret_cast<dlss5::EvaluateFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_EvaluateFeature"));
    api.d3d12.release = reinterpret_cast<dlss5::ReleaseFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_ReleaseFeature"));
    api.d3d12.allocate = reinterpret_cast<dlss5::AllocateFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_AllocateParameters"));
    api.d3d12.destroy = reinterpret_cast<dlss5::DestroyFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_DestroyParameters"));
    api.init = [](HMODULE module) noexcept {
        return reinterpret_cast<dlss5::d3d11::InitFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_Init_Ext"));
    };
    if (!api.d3d12.Complete()) { Print("_nvngx.dll lacks the D3D12 entries the bridge uses\n"); return 2; }
    Print("bridge runtime=%p NVSDK_NGX_D3D12_Init_Ext=%s\n", static_cast<void*>(runtime), api.init(runtime) ? "exported" : "absent");

    const auto texture = [&](uint32_t w, uint32_t h, DXGI_FORMAT format, UINT bind) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1; desc.Format = format; desc.SampleDesc.Count = 1;
        desc.Usage = bind ? D3D11_USAGE_DEFAULT : D3D11_USAGE_STAGING; desc.BindFlags = bind;
        desc.CPUAccessFlags = bind ? 0 : D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> made;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &made)) ? made : ComPtr<ID3D11Texture2D>{};
    };
    const bool r24 = options.d3d11Depth == L"r24g8";
    const DXGI_FORMAT depthFormat = r24 ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
    const size_t motionChannels = options.motionRgba ? 4 : 2;
    const DXGI_FORMAT motionFormat = options.motionRgba ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
    // The game's DLSS output and input color, its depth-stencil buffer and motion vectors.
    auto output = texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    auto color = texture(renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    auto depth = texture(renderWidth, renderHeight, depthFormat, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    // A depth-stencil texture takes no UpdateSubresource: its depth arrives through a plain texture of its family.
    auto depthUpload = texture(renderWidth, renderHeight, depthFormat, D3D11_BIND_SHADER_RESOURCE);
    auto motion = texture(renderWidth, renderHeight, motionFormat, D3D11_BIND_SHADER_RESOURCE);
    auto staging = texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
    if (!output || !color || !depth || !depthUpload || !motion || !staging) { Print("D3D11 textures failed\n"); return 2; }
    Print("[%s] depth %s, motion vectors %s\n", mode, r24 ? "R24G8_TYPELESS" : "R32_TYPELESS", options.motionRgba ? "RGBA16F" : "RG16F");
    const auto uploadDepth = [&](const std::vector<float>& values) {
        std::vector<uint32_t> texels(values.size());
        for (size_t i = 0; i < values.size(); ++i)
        {
            // R24G8: 24-bit unorm depth in the low bits, stencil 0.
            if (r24) texels[i] = static_cast<uint32_t>(std::lround(std::clamp(values[i], 0.0f, 1.0f) * 16777215.0));
            else std::memcpy(&texels[i], &values[i], 4);
        }
        context->UpdateSubresource(depthUpload.Get(), 0, nullptr, texels.data(), renderWidth * 4, 0);
        context->CopyResource(depth.Get(), depthUpload.Get());
    };
    // Depth and motion vectors as --integrated makes them.
    std::vector<float> depthData(size_t{renderWidth} * renderHeight);
    for (uint32_t y = 0; y < renderHeight; ++y)
        for (uint32_t x = 0; x < renderWidth; ++x)
            depthData[size_t{y} * renderWidth + x] = 0.1f + 0.8f * y / renderHeight;
    std::vector<uint16_t> motionData(size_t{renderWidth} * renderHeight * motionChannels);
    const float motionX = options.image.empty() ? -1.0f * renderWidth / width : 0.0f;
    std::vector<uint16_t> imagePixels;
    if (!options.image.empty() && !LoadFrame(options.image, options.imagePq, width, height, imagePixels))
    {
        Print("image %ls does not hold %ux%u %s texels\n", options.image.c_str(), width, height, options.imagePq ? "PQ rgb48le" : "rgb24");
        return 2;
    }
    for (size_t i = 0; i < motionData.size(); i += motionChannels)
    {
        motionData[i] = Half(motionX); motionData[i + 1] = Half(0);
        if (motionChannels == 4) { motionData[i + 2] = Half(0); motionData[i + 3] = Half(1); }
    }
    uploadDepth(depthData);
    context->UpdateSubresource(motion.Get(), 0, nullptr, motionData.data(), static_cast<UINT>(renderWidth * motionChannels * 2), 0);

    NVSDK_NGX_Parameter* parameters = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&parameters)) || !parameters) return 2;
    parameters->Set("Output", static_cast<ID3D11Resource*>(output.Get()));
    parameters->Set("Color", static_cast<ID3D11Resource*>(color.Get()));
    parameters->Set("Depth", static_cast<ID3D11Resource*>(depth.Get()));
    parameters->Set("MotionVectors", static_cast<ID3D11Resource*>(motion.Get()));
    gGameColor = color.Get(); gGameOutput = output.Get();

    dlss5::Settings settings{};
    if (!HarnessSettings(options, settings)) return 2;
    if (settings.stage == static_cast<uint32_t>(dlss5::Stage::ePresent)) { Print("Present does not run through the bridge\n"); return 2; }
    gFakeCopiesColor = settings.stage == static_cast<uint32_t>(dlss5::Stage::eRender);
    const dlss_sr::CreateKeys create = HarnessCreate(options, width, height, renderWidth, renderHeight);
    dlss5::testing::SetProviderPath((std::filesystem::path(options.dir) / L"nvngx_dlssnr.dll").c_str());
    // The bridge's create path, with a fake game handle.
    const auto* gameHandle = reinterpret_cast<const NVSDK_NGX_Handle*>(0x1234);
    dlss5::d3d11::GameCreated(gameHandle, create);
    dlss5::Configure(settings);
    if (!dlss5::Owns(dlss5::kD3D11Route, gameHandle)) { Print("[%s] the bridge did not track the feature\n", mode); return 1; }

    ComPtr<ID3D11Query> disjoint, begin, end, idle;
    D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT};
    const bool queries = SUCCEEDED(device->CreateQuery(&query, &disjoint))
        && (query.Query = D3D11_QUERY_TIMESTAMP, SUCCEEDED(device->CreateQuery(&query, &begin)) && SUCCEEDED(device->CreateQuery(&query, &end)))
        && (query.Query = D3D11_QUERY_EVENT, SUCCEEDED(device->CreateQuery(&query, &idle)));
    if (!queries) return 2;
    // Everything submitted so far completed (the bridge's D3D11 waits include its queue's work).
    const auto finish = [&]() {
        context->End(idle.Get());
        context->Flush();
        const ULONGLONG start = GetTickCount64();
        BOOL done = FALSE;
        while (context->GetData(idle.Get(), &done, sizeof(done), 0) == S_FALSE)
        {
            if (GetTickCount64() - start > 20000) return false;
            Sleep(0);
        }
        return device->GetDeviceRemovedReason() == S_OK;
    };
    if (!finish()) return 2;

    std::vector<uint16_t> pixels, colorPixels(size_t{renderWidth} * renderHeight * 4);
    std::vector<float> lastScene;
    std::vector<double> timesMs;
    int exitCode = 0;
    unsigned long long evaluated = 0; unsigned gaps = 0;
    for (uint32_t frame = 0; frame < options.frames; ++frame)
    {
        if (options.step && frame == options.step)
        {
            auto next = settings; next.scale = 75.0f; next.model = (settings.model + 1) % 3; dlss5::Configure(next);
            Print("[%s] frame %u: Resolution Scale 75, model %u\n", mode, frame, next.model);
        }
        if (imagePixels.empty()) Scene(width, height, static_cast<float>(frame), pixels);
        else pixels = imagePixels;
        if (gGhost && imagePixels.empty())
        {
            std::vector<float> ghostDepth;
            std::vector<uint16_t> ghostMotion;
            GhostGeometry(width, height, renderWidth, renderHeight, static_cast<float>(frame), motionChannels, ghostDepth, ghostMotion);
            uploadDepth(ghostDepth);
            context->UpdateSubresource(motion.Get(), 0, nullptr, ghostMotion.data(), static_cast<UINT>(renderWidth * motionChannels * 2), 0);
        }
        if (options.hdr && imagePixels.empty()) for (size_t n = 0; n < pixels.size(); ++n) if (n % 4 != 3) pixels[n] = Half(FromHalf(pixels[n]) * 4.0f);
        context->UpdateSubresource(output.Get(), 0, nullptr, pixels.data(), width * 8, 0);
        // The DLSS input: the frame at render size (nearest texel).
        for (uint32_t y = 0; y < renderHeight; ++y)
            for (uint32_t x = 0; x < renderWidth; ++x)
                std::memcpy(&colorPixels[(size_t{y} * renderWidth + x) * 4],
                    &pixels[(size_t{y * height / renderHeight} * width + size_t{x} * width / renderWidth) * 4], 8);
        context->UpdateSubresource(color.Get(), 0, nullptr, colorPixels.data(), renderWidth * 8, 0);
        parameters->Set("Reset", frame == 0 ? 1 : 0);
        context->Begin(disjoint.Get());
        context->End(begin.Get());
        result = dlss5::d3d11::Evaluate(api, context.Get(), gameHandle, parameters, nullptr);
        context->End(end.Get());
        context->End(disjoint.Get());
        if (!finish()) { Print("[%s] device lost at frame %u\n", mode, frame); return 1; }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
        UINT64 first = 0, last = 0;
        if (frame && context->GetData(disjoint.Get(), &clock, sizeof(clock), 0) == S_OK && !clock.Disjoint && clock.Frequency
            && context->GetData(begin.Get(), &first, sizeof(first), 0) == S_OK && context->GetData(end.Get(), &last, sizeof(last), 0) == S_OK
            && last >= first)
            timesMs.push_back(static_cast<double>(last - first) * 1000.0 / static_cast<double>(clock.Frequency));
        const auto state = dlss5::Read();
        Print("d3d11 frame=%u state=%u route=%u bridge=%d refusal=%u creates=%llu evaluations=%llu processed=%llu result=0x%08X stage=%u scale=%.0f gpuMs=%.2f budgetMs=%.2f gain=%.3f\n",
            frame, static_cast<unsigned>(state.state), state.route, state.d3d11 ? 1 : 0, state.d3d11Refusal,
            static_cast<unsigned long long>(state.creates), static_cast<unsigned long long>(state.evaluations),
            static_cast<unsigned long long>(dlss5::d3d11::testing::Processed()), state.result, state.stage, state.scale, state.gpuMs,
            state.budgetMs, state.exposureGain);
        if (NVSDK_NGX_FAILED(result) || state.state == dlss5::State::eEvaluateFailed || state.state == dlss5::State::eCreateFailed
            || state.state == dlss5::State::eUnsupported || state.state == dlss5::State::eDriverUpdate)
        {
            Print("[%s] frame %u: game result 0x%08X, DLSS 5 state %u result 0x%08X\n", mode, frame, static_cast<uint32_t>(result),
                static_cast<unsigned>(state.state), state.result);
            // Unsupported without a bridge refusal: the provider refused this GPU (as exit code 3 elsewhere).
            exitCode = state.state == dlss5::State::eUnsupported
                && !dlss5::d3d11::Blocking(static_cast<dlss5::d3d11::Refusal>(state.d3d11Refusal)) ? 3 : 1;
            break;
        }
        if (evaluated && state.evaluations != evaluated + 1) { ++gaps; Print("[%s] frame %u went without an evaluation\n", mode, frame); }
        evaluated = state.evaluations;
        if (frame == options.frames - 1)
        {
            std::vector<float> rgb;
            for (size_t i = 0; i < pixels.size() / 4; ++i)
                for (int c = 0; c < 3; ++c) rgb.push_back(FromHalf(pixels[i * 4 + c]));
            lastScene = rgb;
            if (!options.dumpInput.empty())
                if (std::FILE* file = _wfopen(options.dumpInput.c_str(), L"wb")) { std::fwrite(rgb.data(), sizeof(float), rgb.size(), file); std::fclose(file); }
        }
    }
    if (exitCode)
    {
        const bool owned = dlss5::d3d11::BeforeRelease(gameHandle);
        dlss5::d3d11::AfterRelease(gameHandle, NVSDK_NGX_Result_Success, owned);
        NVSDK_NGX_D3D11_DestroyParameters(parameters);
        Print("result FAIL\n");
        return exitCode;
    }
    context->CopyResource(staging.Get(), output.Get());
    if (!finish()) return 1;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 2;
    std::vector<float> out(size_t{width} * height * 3);
    for (uint32_t y = 0; y < height; ++y)
    {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) + size_t{y} * mapped.RowPitch);
        for (uint32_t x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) out[(size_t{y} * width + x) * 3 + c] = FromHalf(row[x * 4 + c]);
    }
    context->Unmap(staging.Get(), 0);
    std::sort(timesMs.begin(), timesMs.end());
    const Stats stats = Compare(out, lastScene);
    Print("[%s] GPU ms median %.3f min %.3f max %.3f | output mean %.4f, vs input mean |diff| %.5f max %.4f, non-finite %zu\n",
        mode, timesMs.empty() ? 0.0 : timesMs[timesMs.size() / 2], timesMs.empty() ? 0.0 : timesMs.front(),
        timesMs.empty() ? 0.0 : timesMs.back(), stats.meanValue, stats.meanAbsDiff, stats.maxAbsDiff, stats.nonFinite);
    const auto bridge = dlss5::Read();
    const uint64_t processed = dlss5::d3d11::testing::Processed();
    Print("[%s] bridge: stage gpuMs %.3f on its own queue (the GPU ms above add the D3D11 copies and waits), %llu of %llu evaluations reached the game, game DLSS calls %llu, Color substituted %llu\n",
        mode, bridge.gpuMs, static_cast<unsigned long long>(processed), static_cast<unsigned long long>(bridge.evaluations), gFakeCalls,
        gFakeSubstituted);
    WritePng(std::filesystem::path(options.out) / (std::string(mode) + "-output.png"), width, height, out);
    WritePng(std::filesystem::path(options.out) / "input.png", width, height, lastScene);
    if (stats.nonFinite || stats.meanValue <= 0.01 || (options.intensity?stats.meanAbsDiff<1e-5:stats.meanAbsDiff>1e-5)) exitCode=1;
    // Every DLSS 5 evaluation reached the game (copied back, or its Color substituted), and the game's own DLSS ran
    // exactly once per frame.
    if (!bridge.d3d11 || !bridge.evaluations || processed != bridge.evaluations || gFakeCalls != options.frames
        || (gFakeCopiesColor && gFakeSubstituted != processed) || (!gFakeCopiesColor && gFakeSubstituted)) exitCode = 1;
    if (!options.dump.empty())
        if (std::FILE* file = _wfopen(options.dump.c_str(), L"wb")) { std::fwrite(out.data(), sizeof(float), out.size(), file); std::fclose(file); }
    if (!CompareReference(options, mode, out)) exitCode = 1;
    if (options.step)
    {
        const uint32_t extent = static_cast<uint32_t>(std::lround(width * 0.75));
        Print("[%s] step: extent %ux%u creates %llu live %u pending %d gaps %u\n", mode, bridge.extentWidth, bridge.extentHeight,
            static_cast<unsigned long long>(bridge.creates), bridge.live, bridge.pending ? 1 : 0, gaps);
        if (gaps || bridge.extentWidth != extent || bridge.creates != 2ull * settings.passes || bridge.pending || bridge.live != 1) exitCode = 1;
    }
    if (options.automatic)
    {
        Print("[%s] auto stage %u scale %.0f gpuMs %.3f budgetMs %.3f gain %.3f\n", mode, bridge.stage, bridge.scale, bridge.gpuMs,
            bridge.budgetMs, bridge.exposureGain);
        if (bridge.stage != static_cast<uint32_t>(dlss5::Stage::eUpscaled) || !(bridge.gpuMs >= 0.0f && bridge.gpuMs < 1000.0f)
            || !(bridge.budgetMs > 0.0f) || !(bridge.exposureGain >= 0.125f && bridge.exposureGain <= 8.0f)
            || (!options.hdr && bridge.exposureGain != 1.0f)) exitCode = 1;
    }
    // The bridge's release path: the slot goes once its work completed (every frame above finished).
    const bool owned = dlss5::d3d11::BeforeRelease(gameHandle);
    dlss5::d3d11::AfterRelease(gameHandle, NVSDK_NGX_Result_Success, owned);
    const auto released = dlss5::Read();
    const size_t sets = dlss5::d3d11::testing::Sets();
    if (!owned || released.live != 0 || released.d3d11 || sets) exitCode = 1;
    Print("[%s] ReleaseFeature owned=%d live=%u shared sets held=%zu\n", mode, owned ? 1 : 0, released.live, sets);
    NVSDK_NGX_D3D11_DestroyParameters(parameters);
    // The bridge keeps NGX on its own device for the process; the game's D3D11 NGX shuts down as a game's would.
    if (!gNoShutdown) NVSDK_NGX_D3D11_Shutdown1(device.Get());
    Print("result %s\n", exitCode ? "FAIL" : "PASS");
    return exitCode;
}
}

int Run(int argc, wchar_t** argv)
{
    Options options{};
    if (!ParseOptions(argc, argv, options))
    {
        std::fputs("usage: Dlss5ProviderHarness --dir <provider folder> --out <folder> [--width W --height H] "
            "[--render-scale S] [--frames N] [--quality 0|1] [--integrated | --d3d11 [--d3d11-depth r32|r24g8]] "
            "[--dump F] [--compare F]\n", stderr);
        return 2;
    }
    std::filesystem::create_directories(options.out);
    gLog = _wfopen((std::filesystem::path(options.out) / L"harness.log").c_str(), L"w");
    dlss5::SetLogCallback([](const wchar_t* message) { Print("%ls\n", message); });
    const uint32_t width = options.width, height = options.height;
    const uint32_t renderWidth = std::max(1u, static_cast<uint32_t>(std::lround(width * options.renderScale)));
    const uint32_t renderHeight = std::max(1u, static_cast<uint32_t>(std::lround(height * options.renderScale)));
    Print("output %ux%u render %ux%u frames %u quality %u\n", width, height, renderWidth, renderHeight,
        options.frames, options.quality);

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 2;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE)
        {
            Print("adapter %ls\n", desc.Description);
            break;
        }
        adapter.Reset();
    }
    if (!adapter) { Print("no NVIDIA adapter\n"); return 2; }
    if (options.d3d11)
    {
        const int code = RunD3D11(options, adapter.Get(), width, height, renderWidth, renderHeight);
        if (gLog) { std::fclose(gLog); gLog = nullptr; }
        return code;
    }
    Gpu gpu{};
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device)))) return 2;
    const LUID luid=gpu.device->GetAdapterLuid();
    Print("pid=%lu adapterLuid=%08X:%08X\n",GetCurrentProcessId(),static_cast<uint32_t>(luid.HighPart),luid.LowPart);
    D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (FAILED(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.queue)))
        || FAILED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)))
        || FAILED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
            IID_PPV_ARGS(&gpu.list)))
        || FAILED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)))) return 2;
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gpu.queue->GetTimestampFrequency(&gpu.frequency);
    D3D12_QUERY_HEAP_DESC queryDesc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 512};
    gpu.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&gpu.queries));
    gpu.queryReadback = Buffer(gpu.device.Get(), 512 * 8, D3D12_HEAP_TYPE_READBACK);

    // NGX on this device, with the provider folder as its feature path.
    const std::wstring appData = (std::filesystem::path(options.out) / L"ngx").wstring();
    std::filesystem::create_directories(appData);
    const wchar_t* paths[]{options.dir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = &NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID("b5a8a8d2-3d5c-4f11-9e0f-5d1a6c7e8f90",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", appData.c_str(), gpu.device.Get(), &info);
    Print("NGX init 0x%08X\n", static_cast<uint32_t>(result));
    if (NVSDK_NGX_FAILED(result)) return 2;

    NVSDK_NGX_Parameter* capability = nullptr;
    result = NVSDK_NGX_D3D12_GetCapabilityParameters(&capability);
    const dlss5::Capability cap = dlss5::ReadCapability(capability);
    Print("capability 0x%08X DLSSNR.Available=%d NeedsUpdatedDriver=%d MinDriver=%d.%d FeatureInitResult=0x%08X\n",
        static_cast<uint32_t>(result), cap.available, cap.needsUpdatedDriver, cap.minDriverMajor, cap.minDriverMinor,
        static_cast<uint32_t>(cap.featureInitResult));
    if (capability) NVSDK_NGX_D3D12_DestroyParameters(capability);

    dlss5::DirectProvider directProvider;
    if (options.direct && !directProvider.Open((std::filesystem::path(options.dir)/L"nvngx_dlssnr.dll").c_str(),GetModuleHandleW(L"_nvngx.dll"))) {
        Print("direct provider admission failed (core=%p)\n",GetModuleHandleW(L"_nvngx.dll")); return 2;
    }
    // Scene frames, depth and motion vectors (the scene moves +1 output pixel per frame;
    // motion vectors point to the previous position, in render pixels, MV scale 1).
    std::vector<ComPtr<ID3D12Resource>> keep;
    auto scene = Texture(gpu.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_COPY_DEST, L"scene");
    auto depth = Texture(gpu.device.Get(), renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_STATE_COPY_DEST, L"depth");
    auto motion = Texture(gpu.device.Get(), renderWidth, renderHeight,
        options.motionRgba ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, L"motion");
    if (!scene || !depth || !motion) return 2;
    std::vector<float> depthData(size_t{renderWidth} * renderHeight);
    for (uint32_t y = 0; y < renderHeight; ++y)
        for (uint32_t x = 0; x < renderWidth; ++x)
            depthData[size_t{y} * renderWidth + x] = 0.1f + 0.8f * y / renderHeight; // inverted-Z style: far at top
    const size_t motionChannels = options.motionRgba ? 4 : 2;
    std::vector<uint16_t> motionData(size_t{renderWidth} * renderHeight * motionChannels);
    const float motionX = options.image.empty() ? -1.0f * renderWidth / width : 0.0f;
    std::vector<uint16_t> imagePixels;
    if (!options.image.empty() && !LoadFrame(options.image, options.imagePq, width, height, imagePixels))
    {
        Print("image %ls does not hold %ux%u %s texels\n", options.image.c_str(), width, height, options.imagePq ? "PQ rgb48le" : "rgb24");
        return 2;
    }
    for (size_t i = 0; i < motionData.size(); i += motionChannels)
    {
        motionData[i] = Half(motionX); motionData[i + 1] = Half(0);
        if (motionChannels == 4) { motionData[i + 2] = Half(0); motionData[i + 3] = Half(1); }
    }
    if (!Upload(gpu, depth.Get(), depthData.data(), 4, keep)
        || !Upload(gpu, motion.Get(), motionData.data(), static_cast<uint32_t>(motionChannels * 2), keep))
        return 2;
    Barrier(gpu.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(gpu.list.Get(), motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!gpu.Submit()) return 2;

    dlss5::Settings settings{};
    if (!HarnessSettings(options, settings)) return 2;
    const dlss_sr::CreateKeys create = HarnessCreate(options, width, height, renderWidth, renderHeight);

    dlss5::Api nrApi{};
    const auto* gameHandle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x1234);
    if(options.integrated){
        nrApi.runtime=GetModuleHandleW(L"_nvngx.dll");
        nrApi.create=reinterpret_cast<dlss5::CreateFn>(GetProcAddress(nrApi.runtime,"NVSDK_NGX_D3D12_CreateFeature"));
        nrApi.evaluate=reinterpret_cast<dlss5::EvaluateFn>(GetProcAddress(nrApi.runtime,"NVSDK_NGX_D3D12_EvaluateFeature"));
        nrApi.release=reinterpret_cast<dlss5::ReleaseFn>(GetProcAddress(nrApi.runtime,"NVSDK_NGX_D3D12_ReleaseFeature"));
        nrApi.allocate=reinterpret_cast<dlss5::AllocateFn>(GetProcAddress(nrApi.runtime,"NVSDK_NGX_D3D12_AllocateParameters"));
        nrApi.destroy=reinterpret_cast<dlss5::DestroyFn>(GetProcAddress(nrApi.runtime,"NVSDK_NGX_D3D12_DestroyParameters"));
        if(!nrApi.Complete())return 2;
        dlss5::testing::SetProviderPath((std::filesystem::path(options.dir)/L"nvngx_dlssnr.dll").c_str());
        dlss5::GameCreated(0,gameHandle,create);dlss5::Configure(settings);
    }
    std::vector<std::vector<float>> finals;
    std::vector<float> lastScene;
    std::vector<double> presentMs;
    int exitCode = 0;
    for (const bool inPlace : {false, true})
    {
        if(options.integrated && inPlace)break;
        const char* mode = options.present?"present":options.integrated?"integrated":inPlace ? "inplace" : "separate";
        auto output = Texture(gpu.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"output");
        NVSDK_NGX_Parameter* parameters = nullptr;
        if (!output || NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&parameters))) return 2;
        dlss5::WriteCreate(parameters, width, height, settings);
        NVSDK_NGX_Handle* handle = nullptr;
        if(options.integrated)result=NVSDK_NGX_Result_Success;
        else if (options.direct) {
            result=directProvider.Initialize(gpu.device.Get(),parameters);
            Print("direct init 0x%08X\n",static_cast<uint32_t>(result));
            if(result==NVSDK_NGX_Result_Success) result=directProvider.Create(gpu.list.Get(),parameters,&handle);
        } else result = NVSDK_NGX_D3D12_CreateFeature(gpu.list.Get(), static_cast<NVSDK_NGX_Feature>(dlss5::kFeature),
            parameters, &handle);
        Print(options.integrated?"[%s] source fixture ready 0x%08X handle=%p state=%u\n":"[%s] CreateFeature 0x%08X handle=%p state=%u\n", mode, static_cast<uint32_t>(result), handle,
            static_cast<uint32_t>(NVSDK_NGX_FAILED(result) ? dlss5::CreateFailureState(static_cast<uint32_t>(result))
                : dlss5::State::eRunning));
        if (!gpu.Submit()) { Print("device lost after Create\n"); return 1; }
        if (NVSDK_NGX_FAILED(result))
        {
            NVSDK_NGX_D3D12_DestroyParameters(parameters);
            if(!gNoShutdown)NVSDK_NGX_D3D12_Shutdown1(gpu.device.Get());
            const auto state = dlss5::CreateFailureState(static_cast<uint32_t>(result));
            return state == dlss5::State::eUnsupported || state == dlss5::State::eNotInstalled ? 3 : 1;
        }
        std::vector<uint16_t> pixels;
        std::vector<double> timesMs;
        D3D12_RESOURCE_STATES sceneState = D3D12_RESOURCE_STATE_COPY_DEST;
        // --step: evaluations so far and the frames that went without one after the first.
        unsigned long long evaluated=0;unsigned gaps=0;
        for (uint32_t frame = 0; frame < options.frames; ++frame)
        {
            if(options.integrated && options.step && frame==options.step){
                auto next=settings;next.scale=75.0f;next.model=(settings.model+1)%3;dlss5::Configure(next);
                Print("[%s] frame %u: Resolution Scale 75, model %u\n",mode,frame,next.model);
            }
            if(options.integrated && options.autoStep && (frame==0 || frame==options.autoStep)){
                // Auto's level: 100 % (kLevels[0]) from the first frame, 75 % (kLevels[2]) from the step.
                dlss5::testing::SetAutoLevel(frame?2:0);
                if(frame)Print("[%s] frame %u: Resolution Scale Auto steps to 75 %%\n",mode,frame);
            }
            if (imagePixels.empty()) Scene(width, height, static_cast<float>(frame), pixels);
            else pixels = imagePixels;
            if (gGhost && imagePixels.empty())
            {
                // halfcarry.1: this frame's depth and motion vectors (the post moves on its own).
                std::vector<float> ghostDepth;
                std::vector<uint16_t> ghostMotion;
                GhostGeometry(width, height, renderWidth, renderHeight, static_cast<float>(frame), motionChannels, ghostDepth, ghostMotion);
                Barrier(gpu.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                Barrier(gpu.list.Get(), motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                if (!Upload(gpu, depth.Get(), ghostDepth.data(), 4, keep)
                    || !Upload(gpu, motion.Get(), ghostMotion.data(), static_cast<uint32_t>(motionChannels * 2), keep)) return 2;
                Barrier(gpu.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(gpu.list.Get(), motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            Barrier(gpu.list.Get(), scene.Get(), sceneState, D3D12_RESOURCE_STATE_COPY_DEST);
            if(options.hdr && imagePixels.empty())for(size_t n=0;n<pixels.size();++n)if(n%4!=3)pixels[n]=Half(FromHalf(pixels[n])*4.0f);
            if (!Upload(gpu, scene.Get(), pixels.data(), 8, keep)) return 2;
            sceneState = D3D12_RESOURCE_STATE_COPY_DEST;
            if (inPlace || options.integrated)
            {
                Barrier(gpu.list.Get(), scene.Get(), sceneState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(gpu.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                gpu.list->CopyResource(output.Get(), scene.Get());
                Barrier(gpu.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                sceneState = D3D12_RESOURCE_STATE_COPY_SOURCE;
            }
            else
            {
                Barrier(gpu.list.Get(), scene.Get(), sceneState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                sceneState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            // The game's DLSS evaluation, as dlss5.cpp reads it.
            dlss5::Source source{};
            source.output = reinterpret_cast<uintptr_t>(output.Get());
            source.depth = reinterpret_cast<uintptr_t>(depth.Get());
            source.motion = reinterpret_cast<uintptr_t>(motion.Get());
            source.renderWidth = renderWidth; source.renderHeight = renderHeight;
            source.reset = frame == 0 || (options.resetAt && frame == options.resetAt);
            dlss5::Frame evaluation{};
            if (!dlss5::Build(source, create, dlss5::Stage::eUpscaled, false, evaluation)) return 2;
            if (!inPlace) evaluation.color = reinterpret_cast<uintptr_t>(scene.Get());
            dlss5::WriteEvaluate(parameters, evaluation, settings);
            gpu.list->EndQuery(gpu.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * 2);
            if(options.integrated){
                parameters->Set("Output",output.Get());parameters->Set("Depth",depth.Get());parameters->Set("MotionVectors",motion.Get());
                parameters->Set("Reset",frame==0 || (options.resetAt && frame==options.resetAt)?1:0);
                dlss5::AfterEvaluate(0,nrApi,gpu.list.Get(),gameHandle,parameters,NVSDK_NGX_Result_Success);
                if(options.present){
                    // The game presents its image (in PRESENT) after submitting the frame.
                    Barrier(gpu.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PRESENT);
                    if(!gpu.Submit()){Print("[%s] device lost at frame %u\n",mode,frame);return 1;}
                    LARGE_INTEGER start{},stop{},ticks{};QueryPerformanceFrequency(&ticks);QueryPerformanceCounter(&start);
                    dlss5::PresentTarget target{gpu.queue.Get(),output.Get(),options.hdr?1u:0u,0x7000};
                    dlss5::Present(target);
                    if(!gpu.Submit()){Print("[%s] device lost after present %u\n",mode,frame);return 1;}
                    QueryPerformanceCounter(&stop);if(frame)presentMs.push_back((stop.QuadPart-start.QuadPart)*1000.0/ticks.QuadPart);
                    Print("present frame=%u hudlessBlocked=%d\n",frame,dlss5::BlockHudless()?1:0);
                    Barrier(gpu.list.Get(),output.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                const auto state=dlss5::Read();
                Print("integrated frame=%u state=%u route=%u creates=%llu evaluations=%llu result=0x%08X stage=%u scale=%.0f gpuMs=%.2f budgetMs=%.2f gain=%.3f\n",frame,
                    static_cast<unsigned>(state.state),state.route,static_cast<unsigned long long>(state.creates),
                    static_cast<unsigned long long>(state.evaluations),state.result,state.stage,state.scale,state.gpuMs,
                    state.budgetMs,state.exposureGain);
                result=state.state==dlss5::State::eEvaluateFailed || state.state==dlss5::State::eCreateFailed
                    ? NVSDK_NGX_Result_Fail:NVSDK_NGX_Result_Success;
                if(evaluated && state.evaluations!=evaluated+1){++gaps;Print("[%s] frame %u went without an evaluation\n",mode,frame);}
                evaluated=state.evaluations;
            }else result = options.direct ? directProvider.Evaluate(gpu.list.Get(),handle,parameters)
                : NVSDK_NGX_D3D12_EvaluateFeature_C(gpu.list.Get(), handle, parameters, nullptr);
            gpu.list->EndQuery(gpu.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * 2 + 1);
            if (NVSDK_NGX_FAILED(result))
            {
                Print("[%s] frame %u EvaluateFeature 0x%08X\n", mode, frame, static_cast<uint32_t>(result));
                exitCode = 1;
                break;
            }
            if (!gpu.Submit()) { Print("[%s] device lost at frame %u\n", mode, frame); return 1; }
            if (frame == options.frames - 1)
            {
                std::vector<float> rgb;
                for (size_t i = 0; i < pixels.size() / 4; ++i)
                    for (int c = 0; c < 3; ++c) rgb.push_back(FromHalf(pixels[i * 4 + c]));
                lastScene = rgb;
                if (!options.dumpInput.empty())
                    if (std::FILE* file = _wfopen(options.dumpInput.c_str(), L"wb")) { std::fwrite(rgb.data(), sizeof(float), rgb.size(), file); std::fclose(file); }
            }
        }
        gpu.list->ResolveQueryData(gpu.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, options.frames * 2,
            gpu.queryReadback.Get(), 0);
        Barrier(gpu.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        std::vector<float> out;
        if (!Readback(gpu, output.Get(), out)) return 2;
        uint64_t* stamps = nullptr;
        gpu.queryReadback->Map(0, nullptr, reinterpret_cast<void**>(&stamps));
        for (uint32_t frame = 1; frame < options.frames; ++frame)
            timesMs.push_back(static_cast<double>(stamps[frame * 2 + 1] - stamps[frame * 2]) * 1000.0 / gpu.frequency);
        gpu.queryReadback->Unmap(0, nullptr);
        // Frame n's GPU time is perFrameMs[n - 1] (--auto-step compares both sides of its step).
        const std::vector<double> perFrameMs = timesMs;
        std::sort(timesMs.begin(), timesMs.end());
        const Stats stats = Compare(out, lastScene);
        Print("[%s] GPU ms median %.3f min %.3f max %.3f | output mean %.4f, vs input mean |diff| %.5f max %.4f, non-finite %zu\n",
            mode, timesMs.empty() ? 0.0 : timesMs[timesMs.size() / 2], timesMs.empty() ? 0.0 : timesMs.front(),
            timesMs.empty() ? 0.0 : timesMs.back(), stats.meanValue, stats.meanAbsDiff, stats.maxAbsDiff, stats.nonFinite);
        WritePng(std::filesystem::path(options.out) / (std::string(mode) + "-output.png"), width, height, out);
        if (!inPlace) WritePng(std::filesystem::path(options.out) / "input.png", width, height, lastScene);
        if (stats.nonFinite || stats.meanValue <= 0.01 || (options.intensity?stats.meanAbsDiff<1e-5:stats.meanAbsDiff>1e-5)) exitCode=1;
        finals.push_back(out);
        if (!options.dump.empty())
            if (std::FILE* file = _wfopen(options.dump.c_str(), L"wb")) { std::fwrite(out.data(), sizeof(float), out.size(), file); std::fclose(file); }
        if (!CompareReference(options, mode, out)) exitCode = 1;
        if(!presentMs.empty()){
            std::sort(presentMs.begin(),presentMs.end());
            Print("[%s] present wall ms (record + GPU) median %.3f min %.3f max %.3f\n",mode,
                presentMs[presentMs.size()/2],presentMs.front(),presentMs.back());
        }
        if(options.integrated&&options.step){
            // The replacement took over: 75 % and the next model, the old feature
            // released, no frame left out after the first evaluation.
            const auto stepped=dlss5::Read();
            const uint32_t extent=static_cast<uint32_t>(std::lround(width*0.75));
            Print("[%s] step: extent %ux%u creates %llu live %u pending %d gaps %u\n",mode,stepped.extentWidth,stepped.extentHeight,
                static_cast<unsigned long long>(stepped.creates),stepped.live,stepped.pending?1:0,gaps);
            if(gaps || stepped.extentWidth!=extent || stepped.creates!=2ull*settings.passes || stepped.pending || stepped.live!=1)exitCode=1;
        }
        if(options.integrated&&options.autoStep){
            // autorect.1: the feature created at the first frame (its 100 % capacity) still evaluates, the network
            // on 75 % of it since the step, nothing pending and no frame left out. The GPU time on each side of
            // the step (frames 1 .. N-1 and N+1 ..) is reported, not judged: harness GPU clocks vary (P2/P3).
            const auto stepped=dlss5::Read();
            const uint32_t extentWidth=static_cast<uint32_t>(std::lround(width*0.75)),extentHeight=static_cast<uint32_t>(std::lround(height*0.75));
            const auto median=[&](size_t first,size_t last){
                if(first>=last || last>perFrameMs.size())return 0.0;
                std::vector<double> part(perFrameMs.begin()+first,perFrameMs.begin()+last);
                std::sort(part.begin(),part.end());return part[part.size()/2];
            };
            const double before=median(0,options.autoStep-1),after=median(options.autoStep,perFrameMs.size());
            Print("[%s] auto step: extent %ux%u scale %.0f creates %llu live %u pending %d gaps %u | GPU ms median before %.3f after %.3f (x%.2f)\n",
                mode,stepped.extentWidth,stepped.extentHeight,stepped.scale,static_cast<unsigned long long>(stepped.creates),stepped.live,
                stepped.pending?1:0,gaps,before,after,before>0.0?after/before:0.0);
            if(gaps || stepped.extentWidth!=extentWidth || stepped.extentHeight!=extentHeight || stepped.scale!=75.0f
                || stepped.creates!=settings.passes || stepped.pending || stepped.live!=1)exitCode=1;
            dlss5::testing::SetAutoLevel(SIZE_MAX);
        }
        if(options.integrated&&options.automatic){
            // Auto resolves to After upscaling, and the stage's GPU time and
            // luminance came back from completed descriptor sets.
            const auto automatic=dlss5::Read();
            Print("[%s] auto stage %u scale %.0f gpuMs %.3f budgetMs %.3f gain %.3f\n",mode,automatic.stage,automatic.scale,
                automatic.gpuMs,automatic.budgetMs,automatic.exposureGain);
            if(automatic.stage!=static_cast<uint32_t>(dlss5::Stage::eUpscaled)||!(automatic.gpuMs>=0.0f&&automatic.gpuMs<1000.0f)
                ||!(automatic.budgetMs>0.0f)||!(automatic.exposureGain>=0.125f&&automatic.exposureGain<=8.0f)
                ||(!options.hdr&&automatic.exposureGain!=1.0f))exitCode=1;
        }
        if(options.integrated){
            // Another Hook Method releases the Present-stage feature once its work completed.
            if(options.present){auto other=settings;other.stage=static_cast<uint32_t>(dlss5::Stage::eUpscaled);
                dlss5::Configure(other);dlss5::testing::Collect();}
            dlss5::BeforeRelease(0,gameHandle);dlss5::AfterRelease(0,gameHandle,NVSDK_NGX_Result_Success);
            if(dlss5::Read().live!=0 || dlss5::Read().evaluations==0)exitCode=1;
            result=NVSDK_NGX_Result_Success;
        }else result = options.direct ? directProvider.Release(handle) : NVSDK_NGX_D3D12_ReleaseFeature(handle);
        Print("[%s] ReleaseFeature 0x%08X\n", mode, static_cast<uint32_t>(result));
        NVSDK_NGX_D3D12_DestroyParameters(parameters);
        if (exitCode) break;
    }
    if (finals.size() == 2)
    {
        const Stats same = Compare(finals[1], finals[0]);
        Print("inplace vs separate: mean |diff| %.6f max %.5f\n", same.meanAbsDiff, same.maxAbsDiff);
        if (same.meanAbsDiff > 2e-3) exitCode = 1;
    }
    if(!gNoShutdown)NVSDK_NGX_D3D12_Shutdown1(gpu.device.Get());
    Print("result %s\n", exitCode ? "FAIL" : "PASS");
    if (gLog) std::fclose(gLog);
    return exitCode;
}

int wmain(int argc,wchar_t** argv) {
    int result=2;
    try { result=Run(argc,argv); }
    catch(const std::exception& error) { std::fprintf(stderr,"setup exception: %s\n",error.what()); }
    std::fflush(nullptr);
    // Opt-in harness escape from a driver telemetry shutdown hang. All submitted
    // GPU work has already drained; shutdown itself is not validated in this mode.
    if(gNoShutdown)TerminateProcess(GetCurrentProcess(),static_cast<UINT>(result));
    return result;
}
