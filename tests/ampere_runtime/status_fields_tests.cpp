#include "ampere_status_fields.h"
#include "ui_status_json.h"
#include <iostream>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
int main()
{
    // The menu rejects an RTX 30 status whole when these two blocks are nested
    // objects, so they stay flat.
    ampere_provider_arch::Snapshot declaration{};
    declaration.armed = true; declaration.gpuArchitecture = 0x170; declaration.gpuImplementation = 6;
    declaration.declared = 2; declaration.last = ampere_provider_arch::Outcome::eDeclared;
    dlssg_provider_selection::Snapshot selection{};
    selection.prepared = true; selection.selected.version = {310, 9, 0}; selection.selected.nvidiaSigned = true;
    selection.attempts = 1; selection.loaded = 1; selection.examined = 3;
    const std::string path = "C:\\\\ProgramData\\\\NVIDIA\\\\NGX\\\\models\\\\dlssg\\\\versions\\\\20318464\\\\files\\\\160_E658700.bin";
    const std::string status = std::string("{\"version\":1,\"ampereFailure\":0")
        + ampere_status_fields::Declaration(declaration)
        + ampere_status_fields::ProviderSelection(selection, path) + "}";
    std::cout << status << '\n';
    Check(ui_status_json::CompleteObject(status), "RTX 30 status with both blocks is one complete flat object");
    Check(status.find("\"ampereDeclarationLast\":\"declared\"") != std::string::npos, "declaration outcome is published");
    Check(status.find("\"ampereProviderVersion\":\"310.9.0\"") != std::string::npos, "selected provider version is published");
    Check(status.find("\"ampereProviderPath\":\"C:\\\\ProgramData") != std::string::npos, "selected provider path is published escaped");
    Check(status.find(":{") == std::string::npos, "no nested object remains");
    // The nested layout, a negative control for the reader.
    Check(!ui_status_json::CompleteObject("{\"version\":1,\"ampereArchitectureDeclaration\":{\"declared\":1}}"),
        "control: the reader rejects a nested object (the 1.4.2-dev.1 symptom)");
    const std::string empty = std::string("{\"version\":1")
        + ampere_status_fields::ProviderSelection(dlssg_provider_selection::Snapshot{}, "") + "}";
    Check(ui_status_json::CompleteObject(empty), "an empty selection stays a complete object");

    // rtx30ngx.1: a full RTX 30 status has 515 top-level keys, past the reader's old 512-key cap. The same
    // shape: about 459 base fields, then the declaration and provider-selection blocks.
    auto keyCount = [](const std::string& text) {
        size_t count = 0;
        for (size_t at = text.find("\":"); at != std::string::npos; at = text.find("\":", at + 2)) ++count;
        return count;
    };
    auto filler = [](size_t first, size_t count) {
        std::string text;
        for (size_t i = first; i < first + count; ++i) text += ",\"field" + std::to_string(i) + "\":" + std::to_string(i);
        return text;
    };
    const std::string fullRtx30 = std::string("{\"version\":41,\"gpuFamily\":2") + filler(0, 496)
        + ampere_status_fields::Declaration(declaration)
        + ampere_status_fields::ProviderSelection(selection, path) + "}";
    std::cout << "full RTX 30 status keys: " << keyCount(fullRtx30) << '\n';
    Check(keyCount(fullRtx30) > 512, "fixture: the RTX 30 status shape is past the old 512-key cap");
    Check(ui_status_json::CompleteObject(fullRtx30), "a full RTX 30 status past 512 keys is one complete object");
    const std::string atLimit = std::string("{\"version\":41") + filler(0, ui_status_json::kMaximumKeys - 1) + "}";
    Check(keyCount(atLimit) == ui_status_json::kMaximumKeys && ui_status_json::CompleteObject(atLimit),
        "a status with exactly the maximum number of keys is accepted");
    const std::string overLimit = std::string("{\"version\":41") + filler(0, ui_status_json::kMaximumKeys) + "}";
    Check(!ui_status_json::CompleteObject(overLimit), "one key past the maximum is still rejected");
    const std::string duplicate = std::string("{\"version\":41") + filler(0, 600) + ",\"field599\":1}";
    Check(!ui_status_json::CompleteObject(duplicate), "a duplicate key past 512 keys is still rejected");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
