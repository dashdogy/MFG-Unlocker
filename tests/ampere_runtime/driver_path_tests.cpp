// nvpresent.1: which module paths count as the display driver's own files
// (adapter_discovery DriverFilePath). The menu accepts NVIDIA's driver present
// layer only from a driver-store package; the NGX runtime keeps its rule.
// Paths only: nothing is loaded and no file needs to exist.
#include "adapter_discovery.h"
#include <iostream>
#include <string>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }

int wmain()
{
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) { std::cout << "SETUP no system directory\n"; return 2; }
    const std::wstring root(system, length);
    const std::wstring package = root + L"\\DriverStore\\FileRepository\\nv_dispsi.inf_amd64_ccd7fbb71bb72370";
    auto present = [](const std::wstring& path) {
        return adapter_discovery::DriverFilePath(path.c_str(), L"NvPresent64.dll", false);
    };

    Check(present(package + L"\\NvPresent64.dll"), "a driver-store package's NvPresent64.dll qualifies");
    Check(present(package + L"\\nvpresent64.DLL"), "the file name compares without case");
    Check(present(package + L"/NvPresent64.dll"), "a forward slash is the same path");
    Check(!present(root + L"\\NvPresent64.dll"), "a System32 copy does not qualify for the present layer");
    Check(!present(root + L"\\DriverStore\\FileRepository\\NvPresent64.dll"), "the repository folder itself is no package");
    Check(!present(package + L"\\sub\\NvPresent64.dll"), "a nested folder inside a package does not qualify");
    Check(!present(package + L"\\..\\nv_dispsi.inf_amd64_ccd7fbb71bb72370\\NvPresent64.dll"),
        "a path with a parent alias is refused before normalization");
    Check(!present(package + L"\\.\\NvPresent64.dll"), "a path with a current-folder alias is refused");
    Check(!present(package + L"\\NvPresent64.dll.bak") && !present(package + L"\\_nvngx.dll"),
        "another file name in the package does not qualify");
    Check(!present(L"D:\\Games\\The First Descendant\\M1\\Binaries\\Win64\\NvPresent64.dll"),
        "a game-local file of that name does not qualify");
    Check(!present(L"\\\\server\\share\\Windows\\System32\\DriverStore\\FileRepository\\pkg\\NvPresent64.dll"),
        "a UNC path does not qualify");
    Check(!present(L"NvPresent64.dll") && !present(L"") && !adapter_discovery::DriverFilePath(
        (package + L"\\NvPresent64.dll").c_str(), nullptr, false), "relative, empty and missing names do not qualify");

    // The NGX runtime rule is unchanged: driver store or the System32 copy.
    Check(adapter_discovery::DriverNgxRuntimePath((package + L"\\_nvngx.dll").c_str()),
        "the driver store's _nvngx.dll is the NGX runtime");
    Check(adapter_discovery::DriverNgxRuntimePath((root + L"\\_nvngx.dll").c_str()),
        "an older install's System32\\_nvngx.dll is the NGX runtime");
    Check(!adapter_discovery::DriverNgxRuntimePath((package + L"\\NvPresent64.dll").c_str())
        && !adapter_discovery::DriverNgxRuntimePath((package + L"\\sub\\_nvngx.dll").c_str())
        && !adapter_discovery::DriverNgxRuntimePath(L"D:\\Games\\Engine\\Binaries\\_nvngx.dll"),
        "other files, nested folders and game-local copies are not the NGX runtime");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
