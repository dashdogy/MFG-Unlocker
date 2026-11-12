#pragma once
// pluginprov.2: host processes that get proxy forwarding only, decided from what the process was started with.
// Header only, so tests/xemfg_standalone checks it on the CPU (single_module.cpp's DllMain uses it).
#include <string_view>

namespace host_process_policy
{
// A Chromium-family helper process (CEF, Electron, Chrome). The browser process starts its own executable, or a helper
// beside it, again with a --type= switch (renderer, gpu-process, utility, crashpad-handler, ...); the browser process
// itself never has one. A game's CEF UI (BeamNG.drive) can load this module into each helper through a proxy DLL.
// True when "--type=" with a value is a switch of its own: at the start or after white space, outside quotes.
inline bool HasChromiumHelperSwitch(std::wstring_view commandLine) noexcept
{
    constexpr std::wstring_view kSwitch = L"--type=";
    const auto blank = [](wchar_t c) { return c == L' ' || c == L'\t'; };
    bool quoted = false;
    for (size_t i = 0; i < commandLine.size(); ++i) {
        const wchar_t c = commandLine[i];
        if (c == L'"') {
            quoted = !quoted;
            continue;
        }
        if (quoted || (i && !blank(commandLine[i - 1])) || commandLine.substr(i, kSwitch.size()) != kSwitch) continue;
        const size_t value = i + kSwitch.size();
        if (value < commandLine.size() && !blank(commandLine[value]) && commandLine[value] != L'"') return true;
    }
    return false;
}
}
