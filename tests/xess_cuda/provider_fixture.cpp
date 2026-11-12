// Real XeFG internal API behind the candidate's public Nvngx provider API.
// Reuse only the established native-device/upload helpers; the legacy entry point is not run.
#define main XefgLegacyMain
#include "cuda_route_tests.cpp"
#undef main
#include "xess/vulkan/mfg_xemfg_provider.h"
#include <nvsdk_ngx_params.h>
#include <variant>
#include <chrono>

struct Parameters final : NVSDK_NGX_Parameter
{
    using Value = std::variant<unsigned long long, float, double, unsigned int, int, ID3D11Resource*, ID3D12Resource*, void*>;
    std::map<std::string, Value> values;
    template<class T> void Put(const char* name, T value) { values.insert_or_assign(name, Value(value)); }
    template<class T> NVSDK_NGX_Result Read(const char* name, T* value) const {
        const auto it = values.find(name);
        if (it == values.end() || !std::holds_alternative<T>(it->second)) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *value = std::get<T>(it->second); return NVSDK_NGX_Result_Success;
    }
#define FIELD(T) void Set(const char* name, T value) override { Put(name,value); } \
    NVSDK_NGX_Result Get(const char* name, T* value) const override { return Read(name,value); }
    FIELD(unsigned long long) FIELD(float) FIELD(double) FIELD(unsigned int) FIELD(int)
    FIELD(ID3D11Resource*) FIELD(ID3D12Resource*) FIELD(void*)
#undef FIELD
    void Reset() override { values.clear(); }
};

void NgxCheck(int32_t value, const char* what)
{
    if (NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(value))) {
        char text[120]; std::snprintf(text,sizeof text,"%s: %08x",what,uint32_t(value)); throw std::runtime_error(text);
    }
}
uint64_t StatusNumber(const fs::path& path, const char* key)
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        try {
            const auto bytes = ReadFile(path); const std::string text(bytes.begin(), bytes.end());
            const auto at = text.find(std::string("\"")+key+"\":");
            if (at != std::string::npos) return std::stoull(text.substr(at + std::strlen(key) + 3));
        } catch (...) {}
        Sleep(100);
    }
    throw std::runtime_error(std::string("status missing ")+key);
}
ComPtr<ID3D12Resource> Texture2D(Gpu& g, uint32_t width, uint32_t height, DXGI_FORMAT format,
    D3D12_RESOURCE_STATES state, bool uav = false)
{
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,width,height,1,1,format,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,
        uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> resource;
    Check(g.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"fixture texture");
    return resource;
}
void Upload(Gpu& g, ID3D12Resource* dst, uint32_t width, uint32_t height, DXGI_FORMAT format,
    uint32_t bytesPerPixel, const void* pixels)
{
    const uint32_t pitch = (width*bytesPerPixel+255)&~255u;
    auto buffer = g.Buffer(uint64_t(pitch)*height,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped{}; Check(buffer->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"fixture upload map");
    for(uint32_t y=0;y<height;++y) std::memcpy(mapped+size_t(y)*pitch,static_cast<const uint8_t*>(pixels)+size_t(y)*width*bytesPerPixel,size_t(width)*bytesPerPixel);
    buffer->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};
    from.pResource=buffer.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint={format,width,height,1,pitch};
    to.pResource=dst;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&to,0,0,0,&from,nullptr); g.staging.push_back(buffer);
}
std::string HashOutput(Gpu& g, ID3D12Resource* image, uint32_t width, uint32_t height)
{
    Barrier(g.list.Get(),image,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint32_t pitch=(width*4+255)&~255u;
    auto buffer=g.Buffer(uint64_t(pitch)*height,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=image;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=buffer.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,width,height,1,pitch};
    g.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    Barrier(g.list.Get(),image,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.Submit();g.staging.clear();
    uint8_t* mapped{};Check(buffer->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"readback map");
    std::vector<uint8_t> tight(size_t(width)*height*4);
    for(uint32_t y=0;y<height;++y)std::memcpy(tight.data()+size_t(y)*width*4,mapped+size_t(y)*pitch,size_t(width)*4);
    buffer->Unmap(0,nullptr);return Sha256(tight);
}

int wmain(int argc,wchar_t** argv)
{
    try {
        if(argc!=5)throw std::runtime_error("arguments: candidate host runtime on|off");
        const fs::path candidate=fs::absolute(argv[1]),host=fs::absolute(argv[2]),runtime=fs::absolute(argv[3]);
        const bool on=std::wstring(argv[4])==L"on";const auto here=fs::current_path();
        const auto config=here/L"fixture-config.json",status=here/L"fixture-status.json";
        std::ofstream(config)<<"{\"version\":1,\"enabled\":true,\"maxFrames\":5,\"optiScalerUi\":\"optiscaler\"}";
        std::ofstream(here/L"OptiScaler.ini")<<"[XeFG]\nRtxmfgModel=port\nXeFGTensorCores="<<(on?"on":"off")<<"\n";
        SetEnvironmentVariableW(L"RTX_MFG_CONFIG_PATH",config.c_str());SetEnvironmentVariableW(L"RTX_MFG_STATUS_PATH",status.c_str());
        SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT",L"1");
        SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_PLUGIN",candidate.c_str());
        SetEnvironmentVariableW(L"RTXMFG_TEST_OPTISCALER_XEFG",runtime.c_str());
        Gpu g;g.MakeList(false);
        HMODULE opti=LoadLibraryW(host.c_str());if(!opti)throw std::runtime_error("host fixture failed to load");
        HMODULE dll=GetModuleHandleW(candidate.c_str());if(!dll)dll=LoadLibraryW(candidate.c_str());
        if(!dll)throw std::runtime_error("candidate failed to load");
        auto get=reinterpret_cast<PFN_MfgXeMFGProviderGetApi>(GetProcAddress(dll,MFG_XEMFG_PROVIDER_GET_API));
        const auto* api=get?get(MFG_XEMFG_PROVIDER_API_VERSION):nullptr;
        if(!api||api->size<sizeof(*api)||!api->D3D12Init)throw std::runtime_error("provider API missing");
        Sleep(500);NgxCheck(api->D3D12Init(g.device.Get()),"D3D12Init");
        std::ofstream hashes(here/L"frame-hashes.txt");uint64_t beforeResize=0,afterResize=0;
        for(uint32_t count:{1u,2u,5u}) {
            Parameters parameters;void* feature{};NgxCheck(api->D3D12CreateFeature(g.list.Get(),&parameters,&feature),"D3D12CreateFeature");
            for(uint32_t frame=0;frame<6;++frame) {
                const uint32_t width=frame<3?640:768,height=frame<3?360:432;
                auto color=Texture2D(g,width,height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST);
                auto depth=Texture2D(g,width,height,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
                auto motion=Texture2D(g,width,height,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
                auto output=Texture2D(g,width,height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
                std::vector<uint32_t> pixels(size_t(width)*height);std::vector<float> depths(pixels.size(),0.75f),vectors(pixels.size()*2,0);
                for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)pixels[size_t(y)*width+x]=0xff000000u|((x+frame*3)&255)|((y&255)<<8)|(((x+y+frame)&255)<<16);
                Upload(g,color.Get(),width,height,DXGI_FORMAT_R8G8B8A8_UNORM,4,pixels.data());
                Upload(g,depth.Get(),width,height,DXGI_FORMAT_R32_FLOAT,4,depths.data());
                Upload(g,motion.Get(),width,height,DXGI_FORMAT_R32G32_FLOAT,8,vectors.data());
                Barrier(g.list.Get(),color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                g.Submit();g.staging.clear();
                parameters.Reset();parameters.Set("DLSSG.Backbuffer",color.Get());parameters.Set("DLSSG.Depth",depth.Get());parameters.Set("DLSSG.MVecs",motion.Get());
                parameters.Set("DLSSG.OutputInterpolated",output.Get());parameters.Set("DLSSG.MultiFrameCount",count);
                parameters.Set("DLSSG.CameraAspectRatio",float(width)/height);parameters.Set("DLSSG.CameraNear",0.1f);parameters.Set("DLSSG.CameraFar",100.0f);parameters.Set("DLSSG.CameraFOV",1.0471976f);
                parameters.Set("DLSSG.MvecScaleX",1.0f);parameters.Set("DLSSG.MvecScaleY",1.0f);
                for(uint32_t index=1;index<=count;++index) {
                    parameters.Set("DLSSG.MultiFrameIndex",index);NgxCheck(api->D3D12Evaluate(g.list.Get(),feature,&parameters),"D3D12Evaluate");
                    hashes<<count<<' '<<frame<<' '<<index<<' '<<width<<'x'<<height<<' '<<HashOutput(g,output.Get(),width,height)<<'\n';
                }
                if(frame==2||frame==5) {
                    Sleep(1300);const auto launches=StatusNumber(status,"xessTensorCoreLaunches"),fallbacks=StatusNumber(status,"xessTensorCoreFallbacks");
                    std::printf("PROVIDER_SAMPLE count=%u frame=%u launches=%llu fallbacks=%llu\n",count,frame,launches,fallbacks);
                    if(fallbacks||(!on&&launches)||(on&&!launches))throw std::runtime_error("provider tensor counters differ");
                    if(frame==2)beforeResize=launches;else {afterResize=launches;if(on&&afterResize<=beforeResize)throw std::runtime_error("no launches after resize");}
                }
            }
            NgxCheck(api->D3D12ReleaseFeature(feature),"D3D12ReleaseFeature");
        }
        hashes.close();NgxCheck(api->D3D12Shutdown(),"D3D12Shutdown");Sleep(2200);
        std::printf("D3D12_PROVIDER_FIXTURE_PASS tensor=%s resize_launches=%llu->%llu\n",on?"on":"off",beforeResize,afterResize);
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"PROVIDER_FIXTURE_FAILURE %s\n",error.what());return 2;}
}
