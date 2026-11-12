#include "runtime_payload.h"
#include "../sha256_util.h"
#include "embedded_xess_programs.generated.h"
#include "embedded_xess_shaders.generated.h"
#include "xess_programs.packed.h"
#include "xess_shaders.packed.h"
#include "xefg_cuda.packed.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace xess_payload {
std::string Hash(std::span<const unsigned char> bytes) {
    sha256_util::Digest digest{};
    if(!sha256_util::Compute(bytes.data(),bytes.size(),digest))throw std::runtime_error("hash_failed");
    return sha256_util::LowerHex(digest);
}
std::span<const Asset> Assets() noexcept { return embedded::assets; }
// Resource `id` from its packed group (embedded_payload.h): the portable programs, our shaders or the CUDA layers.
static std::span<const unsigned char> Packed(HMODULE module,unsigned id) noexcept {
    namespace packed=embedded_payload::packed;
    const std::pair<const embedded_payload::Group*,std::span<const embedded_payload::Item>> groups[]{
        {&packed::xess_programs,packed::xess_programs_items},{&packed::xess_shaders,packed::xess_shaders_items},
        {&packed::xefg_cuda,packed::xefg_cuda_items}};
    for(const auto& [group,items]:groups)
        if(std::any_of(items.begin(),items.end(),[&](const auto& item){return item.id==id;}))
            return embedded_payload::Find(module,*group,items,id);
    return {};
}
std::span<const unsigned char> Read(HMODULE module,const Asset& asset) {
    if(!module)throw std::runtime_error("payload_module_missing");
    const auto data=Packed(module,asset.id);
    if(data.empty()||data.size()!=asset.bytes)throw std::runtime_error("embedded_payload_shape");
    if(Hash(data)!=asset.hash)throw std::runtime_error("embedded_payload_hash");
    return data;
}
Programs LoadPrograms(HMODULE module) {
    const auto all=Assets();
    const auto manifest=std::find_if(all.begin(),all.end(),[](const auto& a){return !strcmp(a.name,"programs/programs.tsv");});
    if(manifest==all.end())throw std::runtime_error("embedded_manifest_missing");
    const auto bytes=Read(module,*manifest);
    std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
    Programs result;std::string line;
    while(std::getline(input,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        std::istringstream row(line);std::string key,hash,extra;
        if(!(row>>key>>hash)||(row>>extra)||key.size()<4||key.size()>24||hash.size()!=64
            ||!std::all_of(key.begin(),key.end(),[](char c){return c>='0'&&c<='9';}))throw std::runtime_error("embedded_manifest_row");
        const auto name="programs/"+key+".dxil";
        const auto found=std::find_if(all.begin(),all.end(),[&](const auto& a){return name==a.name;});
        if(found==all.end()||hash!=found->hash)throw std::runtime_error("embedded_program_manifest");
        const auto data=Read(module,*found);uint32_t size{};
        if(data.size()>=28)memcpy(&size,data.data()+24,4);
        if(data.size()<32||data.size()>1000000||memcmp(data.data(),"DXBC",4)||size!=data.size()
            ||!result.emplace(key,std::vector<unsigned char>(data.begin(),data.end())).second)throw std::runtime_error("embedded_program_shape");
    }
    if(result.size()!=54)throw std::runtime_error("embedded_program_count");
    return result;
}
std::span<const Asset> ShaderAssets() noexcept { return embedded::shaders; }
std::vector<Replacement> LoadReplacements(HMODULE module) {
    // The Intel program each shader replaces: XeFG's motion-vector splat in libxess_fg 3.0.2 (ec5e0c65), its
    // inverted-depth variant, which XeFG uses when started with INVERTED_DEPTH (OptiScaler's default), and five
    // U-Net conv programs of the same build (the CUDA route recognizes both bytes, xess/cuda/cuda_route.cpp).
    struct Native { const char* name; const char* hash; size_t bytes; };
    static constexpr Native natives[]{
        {"shaders/splat_motion_aware.dxil","3f3f91d2ecd4345042f0519f7455776314be9e7730453a3fcb42c57f0acb49b5",4008},
        {"shaders/splat_motion_aware_inverted.dxil","c3649191906bdd300462dbafe6add8f82b09e8e10c41e92784a209d99513224b",4012},
        {"shaders/unet_3ed03daf.dxil","01c8beac27d3f3f82ecba8f3dc2d9d8b60d4c175a41df3c24a24e8643f5578f5",35852},
        {"shaders/unet_748b0baf.dxil","8291bbc36b1100b07d0788f35a3a9fadfbfcc2267ff0ccb7afb114d76f5d69fe",57376},
        {"shaders/unet_cb391582.dxil","a60f7488872a675d66afd2bec02b0b89e3cc38c1c4ed1d25d745a7b2fe677412",57372},
        {"shaders/unet_aa016078.dxil","8c2efb9e955f39379a91887ed3d90412c4b7ae2c142a874e3da5f699de197f7c",47728},
        {"shaders/unet_efbe3788.dxil","40e159414d5b839ba7e0e07cbf8ff4ef425ee026c216b2928ef3bb6dd0e20d40",54224}};
    const auto all=ShaderAssets();
    if(all.size()!=std::size(natives))throw std::runtime_error("embedded_replacement_set");
    std::vector<Replacement> result;
    for(const auto& native:natives){
        const auto found=std::find_if(all.begin(),all.end(),[&](const auto& a){return !strcmp(a.name,native.name);});
        if(found==all.end())throw std::runtime_error("embedded_replacement_set");
        const auto data=Read(module,*found);uint32_t size{};
        if(data.size()>=28)memcpy(&size,data.data()+24,4);
        if(data.size()<32||data.size()>1000000||memcmp(data.data(),"DXBC",4)||size!=data.size())
            throw std::runtime_error("embedded_replacement_shape");
        result.push_back({native.name,native.hash,native.bytes,std::vector<unsigned char>(data.begin(),data.end())});
    }
    return result;
}
}
