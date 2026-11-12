#include "../../source/native/xess/intel_native.h"
#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

// The XeLL pairing follows the driver's own binding rule and never loads a
// module: loading igxell makes the driver unwrap a built-in libxell context
// into igxell, a crash at launch.
namespace fs=std::filesystem;
static unsigned checks{};
static void Check(bool value,const char* why)
{
    ++checks;
    if (!value) throw std::runtime_error(why);
}
static HMODULE Load(const fs::path& path)
{
    const auto module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Check(module!=nullptr,"load fixture");
    return module;
}
static bool IgxellLoaded() { return GetModuleHandleW(L"igxell.dll")!=nullptr; }
int wmain(int argc,wchar_t** argv)
{
    std::string failure;
    try
    {
        Check(argc==7,"mode, native FG, libxell, ungated libxell, wrong DLL and private output directory required");
        const std::wstring mode=argv[1];
        const fs::path fgPath=argv[2],libxellPath=argv[3],ungatedPath=argv[4],wrongPath=argv[5],output=argv[6];
        fs::create_directories(output);
        Check(!GetModuleHandleW(L"libxell.dll") && !IgxellLoaded(),"clean process");
        // The real FG image is used only for reads/discovery. No FG entry, GPU
        // context or public SDK handle executes in these tests.
        const auto fg=LoadLibraryExW(fgPath.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
        Check(fg!=nullptr,"map verified native FG");
        intel_native::CapacitySite site;
        Check(intel_native::FindCapacity(reinterpret_cast<uint8_t*>(fg),site,failure),"native FG contract qualified");
        HMODULE libxell{},igxell{};
        if (mode==L"builtin" || mode==L"handoff" || mode==L"not-sibling") libxell=Load(libxellPath);
        if (mode==L"ungated") libxell=Load(ungatedPath);
        if (mode==L"wrong-libxell")
        {
            fs::copy_file(wrongPath,output/L"libxell.dll",fs::copy_options::overwrite_existing);
            libxell=Load(output/L"libxell.dll");
        }
        if (mode==L"handoff") igxell=Load(fgPath.parent_path()/L"igxell.dll");
        if (mode==L"not-sibling")
        {
            fs::copy_file(fgPath.parent_path()/L"igxell.dll",output/L"igxell.dll",fs::copy_options::overwrite_existing);
            igxell=Load(output/L"igxell.dll");
        }
        const bool igxellBefore=IgxellLoaded();
        intel_native::Receipt receipt;
        SetLastError(0x5123);
        const bool selected=intel_native::SelectXeLL(fg,libxell,receipt,failure);
        Check(GetLastError()==0x5123,"selection preserves caller LastError");
        Check(IgxellLoaded()==igxellBefore,"selection never loads igxell");
        const std::string now=intel_native::DriverXellBinding();
        if (mode==L"builtin")
        {
            // The 007 First Light shape: OptiScaler's libxell 1.3.2, no igxell.
            Check(selected && receipt.ll==libxell && receipt.llBinding==std::string("libxell"),"built-in libxell bound");
            Check(receipt.llGated && receipt.llRva==0xd1cb,"libxell count gate mapped by the route layout");
            Check(receipt.llMajor==1 && receipt.llMinor==3 && receipt.llPatch==2 && !receipt.igxellLoaded,"receipt version and igxell state");
            Check(!receipt.llPath.empty() && now=="libxell","driver rule agrees");
            auto* gate=reinterpret_cast<uint8_t*>(libxell)+receipt.llRva;
            Check(*gate==3,"selection does not patch libxell");
            Check(intel_native::Raise(libxell,receipt.llRva,3,5,failure),"raise libxell gate with COW/CAS/protection readback");
            intel_native::Receipt raised;
            Check(intel_native::SelectXeLL(fg,libxell,raised,failure) && raised.llRva==receipt.llRva,"raised libxell gate still selected");
            Check(intel_native::Raise(libxell,receipt.llRva,5,3,failure) && *gate==3,"libxell rollback");
            FreeLibrary(raised.ll);
            intel_native::Receipt other;
            Check(!intel_native::SelectXeLL(fg,nullptr,other,failure) && failure=="native_xell_binding_not_route","libxell outside the route refused");
            Check(!other.ll && other.llBinding==std::string("libxell"),"refusal owns no reference");
            FreeLibrary(receipt.ll);
        }
        else if (mode==L"handoff")
        {
            // libxell wrapped a driver context and igxell is resident: the driver binds igxell.
            Check(selected && receipt.ll==igxell && receipt.llBinding==std::string("igxell"),"resident sibling igxell bound");
            Check(receipt.llGated && receipt.llRva==0xcedb && receipt.igxellLoaded && now=="igxell","igxell gate and rule");
            Check(*(reinterpret_cast<uint8_t*>(igxell)+receipt.llRva)==3,"selection does not patch igxell");
            FreeLibrary(receipt.ll);
            Check(GetModuleHandleW(L"igxell.dll")==igxell,"selection held exactly one reference");
        }
        else if (mode==L"ungated")
        {
            // XeLL 1.2.x has no count gate: nothing to raise, setter verified.
            Check(selected && receipt.ll==libxell && !receipt.llGated && !receipt.llRva,"ungated libxell bound without a write site");
            Check(receipt.llMajor==1 && receipt.llMinor==2 && now=="libxell","ungated version and rule");
            FreeLibrary(receipt.ll);
        }
        else
        {
            Check(!selected && !receipt.ll,"refusal gives no owned module to publication");
            if (mode==L"missing")
                Check(failure=="native_xell_not_loaded" && now=="none" && !receipt.igxellLoaded,"no libxell: refused, nothing loaded");
            else if (mode==L"not-sibling")
                Check(failure=="native_igxell_not_sibling" && now=="igxell","foreign igxell copy refused");
            else if (mode==L"wrong-libxell")
                Check(failure=="native_xell_version" && now=="unknown","libxell without a version refused");
            else Check(false,"unknown mode");
        }
        if (igxell)
        {
            FreeLibrary(igxell);
            Check(!IgxellLoaded(),"no leaked igxell references");
        }
        Check(*(reinterpret_cast<uint8_t*>(fg)+site.immediate)==3,"all selection paths leave FG stock");
        FreeLibrary(fg);
        std::printf("PASS %u selection checks, %ls; no provider context or GPU calls\n",checks,mode.c_str());
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr,"FAIL %s; %s\n",e.what(),failure.c_str()); return 1; }
}
