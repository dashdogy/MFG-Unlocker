#pragma once
// vkdlss5.1 harnesses (local tools, not shipped): what the Vulkan harness and its D3D12 twin share, so both run the
// same scene, settings, control file and checks.
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] inline const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

namespace harness
{
inline std::wstring Directory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value = path;
    return value.substr(0, value.find_last_of(L"\\/"));
}

// extra (dlss5adv.1): more dlss5* keys, from the argument extra=key:value,key:value.
struct Nr { bool on = false; unsigned stage = 2, passes = 1, model = 1; float scale = 100.0f; std::string extra; };

// "key:value,key:value" -> ",\"key\":value,\"key\":value" (values are numbers or true/false).
inline std::string ExtraFields(const char* argument)
{
    std::string fields, item;
    for (const char* at = argument;; ++at)
    {
        if (*at && *at != ',') { item += *at; continue; }
        const size_t colon = item.find(':');
        if (colon != std::string::npos) fields += ",\"" + item.substr(0, colon) + "\":" + item.substr(colon + 1);
        item.clear();
        if (!*at) break;
    }
    return fields;
}

inline bool WriteControl(const Nr& nr)
{
    const std::wstring path = Directory() + L"\\RTXMFG-Universal.json";
    const std::wstring temporary = path + L".tmp";
    char text[1024]{};
    // A strong look (Model B, Structure/Tone 1) so the change is measurable; Auto scale off.
    snprintf(text, sizeof(text), "{\"followGame\":true,\"mode\":\"follow\",\"multiplier\":2,\"dynamicTargetFrameRate\":0,"
        "\"dlssgPreset\":2,\"vsyncMode\":0,\"reflexFrameLimitFps\":0,\"generatedOnlyDebug\":false,\"intervalLogging\":true,"
        "\"version\":13,\"dlss5\":%s,\"dlss5HookStage\":%u,\"dlss5PassCount\":%u,\"dlss5Model\":%u,"
        "\"dlss5StructureIntensity\":1.0,\"dlss5ToneIntensity\":1.0,\"dlss5OverallIntensity\":1.0,"
        "\"dlss5ResolutionScale\":%.1f,\"dlss5ResolutionScaleAuto\":false,\"dlss5AdaptiveExposure\":false",
        nr.on ? "true" : "false", nr.stage, nr.passes, nr.model, nr.scale);
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << text << (nr.on ? nr.extra : std::string()) << "}\n";
        if (!file) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

inline std::string ReadStatus()
{
    std::ifstream file(Directory() + L"\\RTXMFG-Universal.status.json", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

inline std::string Field(const std::string& json, const char* name)
{
    const std::string key = std::string("\"") + name + "\":";
    const size_t at = json.find(key);
    if (at == std::string::npos) return "?";
    size_t end = at + key.size();
    while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
    return json.substr(at + key.size(), end - at - key.size());
}

struct Status { long long state = -1, route = -1, live = -1, evaluations = -1, creates = -1, failures = -1; };
inline Status PrintStatus(const char* when)
{
    const std::string json = ReadStatus();
    const auto f = [&](const char* name) { return Field(json, name); };
    printf("STATUS %s: dlss5State=%s result=%s route=%s stage=%s live=%s pending=%s size=%sx%s extent=%sx%s passes=%s creates=%s "
        "evaluations=%s failures=%s gpuMs=%s detected=%s graphicsApi=%s dlssSrRunning=%s\n", when,
        f("dlss5State").c_str(), f("dlss5Result").c_str(), f("dlss5Route").c_str(), f("dlss5Stage").c_str(), f("dlss5Live").c_str(),
        f("dlss5Pending").c_str(), f("dlss5Width").c_str(), f("dlss5Height").c_str(), f("dlss5ExtentWidth").c_str(),
        f("dlss5ExtentHeight").c_str(), f("dlss5Passes").c_str(), f("dlss5Creates").c_str(), f("dlss5Evaluations").c_str(),
        f("dlss5Failures").c_str(), f("dlss5GpuMs").c_str(), f("dlss5Detected").c_str(), f("graphicsApi").c_str(),
        f("dlssSrRunning").c_str());
    fflush(stdout);
    const auto number = [&](const char* name) { const auto value = f(name); return value == "?" ? -1LL : atoll(value.c_str()); };
    Status status;
    status.state = number("dlss5State"); status.route = number("dlss5Route"); status.live = number("dlss5Live");
    status.evaluations = number("dlss5Evaluations"); status.creates = number("dlss5Creates"); status.failures = number("dlss5Failures");
    return status;
}

inline uint16_t Half(float value)
{
    uint32_t bits = 0; std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    int exponent = int((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFF;
    if (exponent <= 0) return static_cast<uint16_t>(sign);
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | (exponent << 10) | ((mantissa + 0x1000) >> 13));
}
inline float Float(uint16_t half)
{
    const uint32_t sign = (half & 0x8000u) << 16, exponent = (half >> 10) & 0x1F, mantissa = half & 0x3FF;
    if (!exponent) { const float f = std::ldexp(static_cast<float>(mantissa), -24); return sign ? -f : f; }
    const uint32_t bits = exponent == 31 ? sign | 0x7F800000 | (mantissa << 13) : sign | ((exponent + 112) << 23) | (mantissa << 13);
    float result = 0; std::memcpy(&result, &bits, 4); return result;
}

// The scene: a colour gradient, a checkerboard and a bright (HDR) disc, linear BT.709.
inline std::vector<uint16_t> Scene(uint32_t width, uint32_t height)
{
    std::vector<uint16_t> texels(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const float u = (x + 0.5f) / width, v = (y + 0.5f) / height;
            const bool check = ((x / 24) + (y / 24)) & 1;
            float r = 0.15f + 0.6f * u, g = 0.1f + 0.5f * v, b = check ? 0.55f : 0.08f;
            const float dx = u - 0.62f, dy = v - 0.42f;
            if (dx * dx + dy * dy < 0.012f) { r = 3.5f; g = 2.8f; b = 1.6f; }
            const float stripe = 0.5f + 0.5f * std::sin(x * 0.21f + y * 0.07f);
            r *= 0.8f + 0.2f * stripe; g *= 0.8f + 0.2f * stripe;
            uint16_t* t = &texels[(size_t(y) * width + x) * 4];
            t[0] = Half(r); t[1] = Half(g); t[2] = Half(b); t[3] = Half(1.0f);
        }
    return texels;
}

struct Stats { double meanAbs = 0, maxAbs = 0, changed = 0, meanLuma = 0; size_t nonFinite = 0; };
inline Stats Compare(const std::vector<float>& a, const std::vector<float>& b)
{
    Stats s;
    const size_t pixels = a.size() / 4;
    size_t changed = 0;
    for (size_t i = 0; i < pixels; ++i)
    {
        double luma = 0;
        for (int c = 0; c < 3; ++c)
        {
            const float x = a[i * 4 + c], y = b[i * 4 + c];
            if (!std::isfinite(y)) { ++s.nonFinite; continue; }
            const double d = std::fabs(double(x) - y);
            s.meanAbs += d; s.maxAbs = std::max(s.maxAbs, d);
            if (d > 2e-3) ++changed;
            luma += (c == 0 ? 0.2126 : c == 1 ? 0.7152 : 0.0722) * y;
        }
        s.meanLuma += luma;
    }
    s.meanAbs /= double(pixels) * 3; s.changed = double(changed) / (double(pixels) * 3); s.meanLuma /= double(pixels);
    return s;
}

// The read-back output's halfs as floats into `texels`; with `dump`, the halfs also go to output-<name>.rgba16f beside
// the exe.
inline void Decode(const uint16_t* halfs, std::vector<float>& texels, bool dump, const char* name)
{
    for (size_t i = 0; i < texels.size(); ++i) texels[i] = Float(halfs[i]);
    if (!dump) return;
    std::ofstream out(Directory() + L"\\output-" + std::wstring(name, name + strlen(name)) + L".rgba16f", std::ios::binary);
    out.write(reinterpret_cast<const char*>(halfs), std::streamsize(texels.size() * 2));
}
}
