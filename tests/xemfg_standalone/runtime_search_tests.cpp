// CPU checks of XeMFG standalone's runtime search (source/native/xess/runtime_search.h), on folder
// fixtures beside the test (CTest's working folder; test children may not write %TEMP%).
#include "xess/runtime_search.h"
#include <cstdio>
#include <fstream>
#include <string>

namespace
{
namespace fs = std::filesystem;
using namespace xess_runtime_search;
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what, int line)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x), #x, __LINE__)

fs::path gRoot;

void Touch(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "MZ";
}

// A fresh game folder (the EXE's) and a module folder: the same folder (version.dll beside the game's exe) or another.
struct Layout
{
    fs::path game, module;
    explicit Layout(const wchar_t* name, bool separate = false)
    {
        game = gRoot / name / L"Binaries" / L"Win64";
        module = separate ? gRoot / name / L"plugins" : game;
        fs::create_directories(game);
        fs::create_directories(module);
    }
    Bases Read() const { return ReadBases((module / L"version.dll").wstring(), (game / L"Game-Win64-Shipping.exe").wstring()); }
    Result Search(Order order = Order::eD3D12) const { return Find(Read(), order); }
};

bool Is(const fs::path& left, const fs::path& right) { return _wcsicmp(left.c_str(), right.c_str()) == 0; }

void Orders()
{
    const Layout same(L"orders-same");
    auto folders = Folders(same.Read(), Order::eD3D12);
    CHECK(folders.size() == 3);
    CHECK(Is(folders[0].path, same.game / L"RTXMFG-XeSS") && Is(folders[1].path, same.game)
        && Is(folders[2].path, same.game / L"OptiScaler"));
    folders = Folders(same.Read(), Order::eVulkan);
    CHECK(folders.size() == 3);
    CHECK(Is(folders[0].path, same.game) && Is(folders[1].path, same.game / L"RTXMFG-XeSS")
        && Is(folders[2].path, same.game / L"OptiScaler"));
    // Separate folders: D3D12 interleaves the bases per subfolder, Vulkan searches the module's three first.
    const Layout separate(L"orders-separate", true);
    folders = Folders(separate.Read(), Order::eD3D12);
    CHECK(folders.size() == 6);
    CHECK(Is(folders[0].path, separate.module / L"RTXMFG-XeSS") && Is(folders[1].path, separate.game / L"RTXMFG-XeSS")
        && Is(folders[2].path, separate.module) && Is(folders[3].path, separate.game)
        && Is(folders[4].path, separate.module / L"OptiScaler") && Is(folders[5].path, separate.game / L"OptiScaler"));
    CHECK(!wcscmp(folders[0].base, L"module") && !wcscmp(folders[1].base, L"game") && !wcscmp(folders[3].sub, L""));
    folders = Folders(separate.Read(), Order::eVulkan);
    CHECK(folders.size() == 6);
    CHECK(Is(folders[0].path, separate.module) && Is(folders[1].path, separate.module / L"RTXMFG-XeSS")
        && Is(folders[2].path, separate.module / L"OptiScaler") && Is(folders[3].path, separate.game)
        && Is(folders[4].path, separate.game / L"RTXMFG-XeSS") && Is(folders[5].path, separate.game / L"OptiScaler"));
}

void Found()
{
    // The RTXMFG-XeSS folder wins over the game's own copy beside its exe (D3D12), the module's folder on Vulkan.
    const Layout both(L"found-both");
    for (const auto* folder : {L"RTXMFG-XeSS", L""})
    {
        Touch(both.game / folder / kFg);
        Touch(both.game / folder / kLl);
    }
    auto result = both.Search();
    CHECK(result.found == 0 && result.reason.empty() && Is(result.folders[result.found].path, both.game / L"RTXMFG-XeSS"));
    result = both.Search(Order::eVulkan);
    CHECK(result.found == 0 && Is(result.folders[result.found].path, both.game));
    // Only beside the exe.
    const Layout exe(L"found-exe");
    Touch(exe.game / kFg);
    Touch(exe.game / kLl);
    result = exe.Search();
    CHECK(result.found == 1 && result.reason.empty() && result.misnamed.empty());
    CHECK(Is(result.recommended, exe.module / L"RTXMFG-XeSS") && Is(result.gameFolder, exe.game));
    // Another file named like the pair beside it does not stop the pair loading.
    Touch(exe.game / L"libxess_fg.lib");
    result = exe.Search();
    CHECK(result.found == 1 && result.reason.empty() && result.misnamed == L"libxess_fg.lib");
    // The Intel upscaler beside the game is not a near miss.
    const Layout upscaler(L"found-upscaler");
    Touch(upscaler.game / L"libxess.dll");
    result = upscaler.Search();
    CHECK(result.reason == "runtime_not_found" && result.misnamed.empty());
}

void Missing()
{
    const Layout none(L"missing-none");
    auto result = none.Search();
    CHECK(result.found == SIZE_MAX && result.reason == "runtime_not_found");
    CHECK(Is(result.recommended, none.game / L"RTXMFG-XeSS") && Is(result.gameFolder, none.game));
    const Layout ll(L"missing-fg");
    Touch(ll.game / L"RTXMFG-XeSS" / kLl);
    result = ll.Search();
    CHECK(result.reason == "runtime_missing_libxess_fg" && result.folders[0].ll && !result.folders[0].fg);
    CHECK(ll.Search(Order::eVulkan).reason == "runtime_missing_libxess_fg");
    const Layout fg(L"missing-ll");
    Touch(fg.game / kFg);
    CHECK(fg.Search().reason == "runtime_missing_libxell");
    // Both exist, never in one folder.
    const Layout split(L"split");
    Touch(split.game / L"RTXMFG-XeSS" / kFg);
    Touch(split.game / kLl);
    result = split.Search();
    CHECK(result.reason == "runtime_split" && result.found == SIZE_MAX);
    CHECK(split.Search(Order::eVulkan).reason == "runtime_split");
}

void Misnamed()
{
    // A hidden-extension double name: kept, never loaded.
    const Layout doubled(L"misnamed-double");
    Touch(doubled.game / L"libxess_fg.dll.dll");
    Touch(doubled.game / kLl);
    auto result = doubled.Search();
    CHECK(result.reason == "runtime_misnamed" && result.found == SIZE_MAX && result.misnamed == L"libxess_fg.dll.dll");
    CHECK(result.folders[1].misses.size() == 1 && result.folders[1].ll && !result.folders[1].fg);
    CHECK(!wcscmp(Wanted(result.misnamed), kFg));
    // The SDK's import library and a "(1)" copy.
    const Layout lib(L"misnamed-lib");
    Touch(lib.game / L"RTXMFG-XeSS" / L"libxess_fg.lib");
    Touch(lib.game / L"RTXMFG-XeSS" / L"libxell (1).dll");
    result = lib.Search();
    CHECK(result.reason == "runtime_misnamed" && result.folders[0].misses.size() == 2);
    CHECK(!wcscmp(Wanted(L"libxell (1).dll"), kLl));
    // A folder with the DLL's name ("Extract all").
    const Layout folder(L"misnamed-folder");
    fs::create_directories(folder.game / kFg);
    Touch(folder.game / kLl);
    result = folder.Search();
    CHECK(result.reason == "runtime_misnamed" && result.misnamed == kFg && !result.folders[1].fg);
}

void TooLong()
{
    const std::wstring longPath = (gRoot / std::wstring(280, L'x') / L"version.dll").wstring();
    auto bases = ReadBases(longPath, longPath);
    CHECK(bases.module.empty() && bases.game.empty() && bases.moduleTooLong && bases.gameTooLong);
    auto result = Find(bases, Order::eD3D12);
    CHECK(result.folders.empty() && result.reason == "runtime_path_too_long" && result.recommended.empty());
    // One base too long, the other searched: not found there.
    const Layout game(L"toolong-game");
    bases = ReadBases(longPath, (game.game / L"Game.exe").wstring());
    result = Find(bases, Order::eVulkan);
    CHECK(bases.moduleTooLong && !bases.gameTooLong && result.folders.size() == 3 && result.reason == "runtime_not_found");
    CHECK(Is(result.recommended, game.game / L"RTXMFG-XeSS"));
    // An empty path is unknown, not too long.
    bases = ReadBases(L"", L"");
    CHECK(Find(bases, Order::eD3D12).reason == "runtime_not_found");
}

void Reasons()
{
    for (const char* reason : {"runtime_not_found", "runtime_misnamed", "runtime_split", "runtime_missing_libxell",
             "runtime_missing_libxess_fg", "runtime_path_too_long"})
        CHECK(SearchReason(reason));
    for (const char* reason : {"runtime_load_failed", "runtime_route_rejected", "runtime_exports_missing", "", "active"})
        CHECK(!SearchReason(reason));
}

void LogFields()
{
    const Layout doubled(L"json");
    Touch(doubled.game / L"libxess_fg.dll.dll");
    Touch(doubled.game / kLl);
    const auto bases = doubled.Read();
    const auto json = Json(bases, Find(bases, Order::eD3D12));
    CHECK(json.find("\"game\":\"same\"") != std::string::npos);
    CHECK(json.find("\"searched\":[{\"folder\":\"module/RTXMFG-XeSS\",\"fg\":false,\"ll\":false},{\"folder\":\"module\",\"fg\":false,"
        "\"ll\":true,\"near\":[\"libxess_fg.dll.dll\"]},{\"folder\":\"module/OptiScaler\",\"fg\":false,\"ll\":false}]")
        != std::string::npos);
    // Backslashes escaped; a typical Steam path with both bases fits a queued log line (752 characters).
    CHECK(json.find("\\\\Binaries\\\\Win64\"") != std::string::npos && json.size() < 600);
    const Layout separate(L"json-separate", true);
    const auto apart = Json(separate.Read(), separate.Search());
    CHECK(apart.find("\"game\":\"same\"") == std::string::npos && apart.find("game/OptiScaler") != std::string::npos);
    // A non-ASCII folder is UTF-8.
    const Layout unicode(L"json-\u00e9");
    CHECK(Json(unicode.Read(), unicode.Search()).find("json-\xc3\xa9") != std::string::npos);
    CHECK(Utf8(L"\u00e9") == "\xc3\xa9");
}

// oldruntime.1: a pair short of exports (Cyberpunk's own libxess_fg 1.1.0.19 beside its exe, no RTXMFG-XeSS folder).
void OutdatedPair()
{
    // ASI in plugins\: a newer pair goes in plugins\RTXMFG-XeSS, which both orders search before the game's folder.
    const Layout plugin(L"outdated-plugin", true);
    Touch(plugin.game / kFg);
    Touch(plugin.game / kLl);
    auto result = plugin.Search();
    CHECK(result.found == 3 && Is(NewerPairFolder(result), plugin.module / L"RTXMFG-XeSS"));
    result = plugin.Search(Order::eVulkan);
    CHECK(result.found == 3 && Is(NewerPairFolder(result), plugin.module / L"RTXMFG-XeSS"));
    // Proxy beside the exe: D3D12 still reaches RTXMFG-XeSS first; Vulkan loads the exe folder's pair first, so the
    // newer pair replaces it there.
    const Layout proxy(L"outdated-proxy");
    Touch(proxy.game / kFg);
    Touch(proxy.game / kLl);
    result = proxy.Search();
    CHECK(result.found == 1 && Is(NewerPairFolder(result), proxy.game / L"RTXMFG-XeSS"));
    result = proxy.Search(Order::eVulkan);
    CHECK(result.found == 0 && Is(NewerPairFolder(result), proxy.game));
    // An old pair in RTXMFG-XeSS itself is replaced there; nothing found names no folder.
    const Layout own(L"outdated-own");
    Touch(own.game / L"RTXMFG-XeSS" / kFg);
    Touch(own.game / L"RTXMFG-XeSS" / kLl);
    CHECK(Is(NewerPairFolder(own.Search()), own.game / L"RTXMFG-XeSS"));
    const Layout none(L"outdated-none");
    CHECK(NewerPairFolder(none.Search()).empty());

    // The first file short of an export is kept with the first missing name; the log fields come before "searched".
    Outdated outdated;
    CHECK(!outdated.file && OutdatedJson(outdated).empty());
    outdated.Miss(kFg, "xefgSwapChainSetNumInterpolatedFrames");
    outdated.Miss(kLl, "xellAddMarkerData");
    CHECK(outdated.file == kFg && std::string(outdated.first) == "xefgSwapChainSetNumInterpolatedFrames" && outdated.missing == 2);
    outdated.version = "1.1.0.19";
    CHECK(OutdatedJson(outdated) == ",\"outdated\":\"libxess_fg.dll\",\"outdated_version\":\"1.1.0.19\",\"missing\":2,"
        "\"first_missing\":\"xefgSwapChainSetNumInterpolatedFrames\"");
    Outdated ll;
    ll.Miss(kLl, "xellAddMarkerData");
    CHECK(ll.file == kLl && ll.missing == 1);

    // A loaded module's version resource reads as four numbers; this test EXE has none; nullptr is never the EXE.
    const std::string kernel = ModuleVersion(GetModuleHandleW(L"kernel32.dll"));
    size_t dots = 0;
    for (const char c : kernel) if (c == '.') ++dots;
    CHECK(dots == 3 && !kernel.empty() && kernel.front() >= '1' && kernel.front() <= '9');
    CHECK(ModuleVersion(GetModuleHandleW(nullptr)).empty());
    CHECK(ModuleVersion(nullptr).empty());
}
}

int main()
{
    gRoot = fs::current_path() / L"runtime-search-work";
    std::error_code removed;
    fs::remove_all(gRoot, removed);
    fs::create_directories(gRoot);
    Orders();
    Found();
    Missing();
    Misnamed();
    TooLong();
    Reasons();
    LogFields();
    OutdatedPair();
    fs::remove_all(gRoot, removed);
    std::printf("xemfg runtime search: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
