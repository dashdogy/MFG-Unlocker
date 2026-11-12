// Additional provider-proxy cases; existing D3D12 test recordings remain unchanged.
#define main XefgLegacyMain
#include "cuda_route_tests.cpp"
#undef main
#include "xess/cuda/provider_list.h"
#include "proxy_recording.h"
#include <set>

int main(int argc, char** argv)
{
    try {
        std::vector<fs::path> dirs; fs::path fatbinPath;
        std::string test = "equivalence"; bool dred = false, agility = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&] { if (++i == argc) throw std::runtime_error("missing argument"); return std::string(argv[i]); };
            if (arg == "--fatbin") fatbinPath = value();
            else if (arg == "--programs") dirs.emplace_back(value());
            else if (arg == "--test") test = value();
            else if (arg == "--dred") dred = true;
            else if (arg == "--expect-agility") agility = true;
            else if (arg != "--proxy-list") throw std::runtime_error("unknown argument " + arg);
        }
        if (dred) {
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
            Check(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)), "DRED settings");
            settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
        Gpu g; g.MakeList(false);
        // Agility moves from its class table to the list's own table on the first Reset.
        // Establish that runtime-owned transition before measuring proxy table writes.
        g.Submit();
        if (agility) {
            wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const auto expected = fs::path(exe).parent_path() / L"D3D12" / L"D3D12Core.dll";
            if (_wcsicmp(LoadedRuntime().c_str(), expected.c_str())) throw std::runtime_error("Agility runtime not loaded");
        }
        std::map<std::string, std::vector<uint8_t>> bySha;
        for (const auto& dir : dirs) for (const auto& entry : fs::directory_iterator(dir)) if (entry.is_regular_file()) {
            auto data = ReadFile(entry.path()); if (!data.empty()) bySha.emplace(Sha256(data), std::move(data));
        }
        std::map<std::string, Program> programs;
        for (const auto& layer : xefg_cuda::Layers()) {
            if (!bySha.count(layer.sha256)) throw std::runtime_error("missing program fixture");
            auto& p = programs[layer.program]; p.dxil = bySha.at(layer.sha256);
            Check(g.device->CreateRootSignature(0, p.dxil.data(), p.dxil.size(), IID_PPV_ARGS(&p.root)), "root");
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = p.root.Get(); desc.CS = {p.dxil.data(), p.dxil.size()};
            Check(g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&p.reference)), "reference pipeline");
        }
        const auto fatbin = ReadFile(fatbinPath); std::string reason;
        if (!xefg_cuda::Install(g.device.Get(), GetModuleHandleW(nullptr), fatbin, reason, false)) throw std::runtime_error("install: " + reason);
        for (auto& [name, p] : programs) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = p.root.Get(); desc.CS = {p.dxil.data(), p.dxil.size()};
            Check(g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&p.routed)), "routed pipeline");
            if (!xefg_cuda::LayerOf(p.routed.Get())) throw std::runtime_error("unrecognized routed pipeline");
        }
        if (test == "tokens") {
            std::set<ID3D12PipelineState*> seen;
            std::vector<ComPtr<ID3D12PipelineState>> keep;
            uint64_t previous = 0; ComPtr<ID3D12PipelineState> first;
            for (int context = 0; context < 20; ++context) {
                const auto token = xefg_cuda::NewPipelineToken();
                xefg_cuda::PipelineScope scope(token);
                for (auto& [name, p] : programs) {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = p.root.Get(); desc.CS = {p.dxil.data(), p.dxil.size()};
                    ComPtr<ID3D12PipelineState> pipeline;
                    Check(g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "token pipeline");
                    if (!xefg_cuda::LayerOf(pipeline.Get())) throw std::runtime_error("token pipeline unrecognized");
                    seen.insert(pipeline.Get()); keep.push_back(pipeline);
                    if (!first) first = pipeline;
                }
                if (previous) {
                    auto survivor = keep.back(); xefg_cuda::ForgetPipelines(previous);
                    if (!xefg_cuda::LayerOf(survivor.Get()) || (context == 1 && xefg_cuda::LayerOf(first.Get())))
                        throw std::runtime_error("pipeline token isolation failed");
                }
                previous = token;
            }
            xefg_cuda::ForgetPipelines(previous);
            if (seen.size() <= 256) throw std::runtime_error("pipeline fixture did not create more than 256 identities");
            std::printf("PROXY_TOKENS_PASS distinct=%zu contexts=20\n", seen.size()); return 0;
        }
        xefg_cuda::ProviderList proxy, wrapper;
        void** const table = *reinterpret_cast<void***>(g.list.Get());
        const auto tableSnapshot = std::vector<void*>(table, table + 67);
        size_t cases = 0, failures = 0;
        for (const auto& spec : kSpecs) {
            auto& p = programs.at(spec.name);
            for (const auto size : {std::pair{37u,29u},std::pair{101u,67u},std::pair{16u,8u}})
                for (int mode = 0; mode < 5; ++mode) for (int realistic = 0; realistic < 2; ++realistic) {
                    if (mode == 4 && (spec.family != Family::Flow || spec.nout != 8)) continue;
                    if (test != "equivalence" && cases) break;
                    const auto c = Make(spec, size.first, size.second, 99, realistic != 0, mode);
                    const auto reference = RunProxy(g, spec, c, p, p.reference.Get(), nullptr, 0);
                    if (!proxy.Bind(g.list.Get())) throw std::runtime_error("proxy Bind refused");
                    if (test == "stale") {
                        RunProxy(g, spec, c, p, p.routed.Get(), &proxy, 0);
                        proxy.Unbind(); if (!proxy.Bind(g.list.Get())) throw std::runtime_error("second Bind refused");
                    }
                    if (test == "escape") {
                        void* forwarded{}; const auto hr = proxy.QueryInterface(__uuidof(ID3D12GraphicsCommandList4), &forwarded);
                        void* actual{}; const auto expected = g.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), &actual);
                        if (hr != expected || forwarded != actual) throw std::runtime_error("unknown IID did not forward");
                        if (forwarded) static_cast<IUnknown*>(forwarded)->Release();
                        if (actual) static_cast<IUnknown*>(actual)->Release();
                    }
                    xefg_cuda::ProviderList* recorder = &proxy;
                    if (test == "wrapped") {
                        proxy.Unbind();
                        if (!wrapper.Bind(g.list.Get())) throw std::runtime_error("wrapper Bind refused");
                        void* escaped{}; wrapper.QueryInterface(__uuidof(ID3D12GraphicsCommandList4), &escaped);
                        if (escaped) static_cast<IUnknown*>(escaped)->Release();
                        if (proxy.Bind(wrapper.Interface())) throw std::runtime_error("wrapped list admitted");
                        recorder = &wrapper;
                    }
                    const auto before = xefg_cuda::Snapshot();
                    const auto output = RunProxy(g, spec, c, p, p.routed.Get(), recorder, test == "stale" ? 1 : 0);
                    const auto after = xefg_cuda::Snapshot();
                    proxy.Unbind(); wrapper.Unbind();
                    const auto launches = test == "stale" || test == "escape" || test == "wrapped" ? 0 : 1;
                    const auto fallbacks = test == "stale" ? 1 : 0;
                    size_t poison{}; const auto bad = Mismatches(reference, output, poison);
                    const bool ok = !bad && after.launches - before.launches == launches && after.fallbacks - before.fallbacks == fallbacks;
                    ++cases; failures += !ok;
                    if (!ok) std::printf("FAIL %s bad=%zu launches=%llu fallbacks=%llu\n", c.label.c_str(), bad,
                        after.launches-before.launches, after.fallbacks-before.fallbacks);
                }
            if (test != "equivalence") break;
        }
        if (*reinterpret_cast<void***>(g.list.Get()) != table || std::vector<void*>(table,table+67) != tableSnapshot) {
            std::printf("TABLE_DIAGNOSTIC before=%p after=%p\n", table, *reinterpret_cast<void***>(g.list.Get()));
            for (size_t slot = 0; slot < 67; ++slot) if (table[slot] != tableSnapshot[slot])
                std::printf("TABLE_SLOT %zu before=%p after=%p\n", slot, tableSnapshot[slot], table[slot]);
            throw std::runtime_error("proxy wrote a command list table");
        }
        std::printf("PROXY_%s cases=%zu failures=%zu table_slots=67 untouched=true\n", test.c_str(), cases, failures);
        return failures ? 1 : 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"PROXY_SETUP_FAILURE %s\n",error.what()); return 2; }
}
