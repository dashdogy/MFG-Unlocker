// GitHub issue 149 follow-up: the RTX 40 2X hold (ada_gate_hold.h). A game's old nvngx_dlssg.dll (no multi-frame
// check) beside NVIDIA's OTA 310.x (check restored): a mid-game inspection of the old file must not hold 2X again.
#include "ada_gate_hold.h"

#include <iostream>
#include <thread>
#include <vector>

namespace
{
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{
    ++checks;
    failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << text << '\n';
}
using ada_gate_hold::Change;
using ada_gate_hold::Hold;
using ada_gate_hold::Source;
constexpr bool kMissing = true, kHasCheck = false;
}

int main()
{
    {
        // The reported order: old file, OTA (check restored), old file again, Create on OTA, old file mid-game.
        Hold hold;
        Check(hold.Note(kMissing, Source::eInspection) == Change::eHeld && hold.Held(), "an old provider alone holds 2X before any Create");
        Check(hold.Note(kHasCheck, Source::eInspection) == Change::eLifted && !hold.Held(), "a provider with the check lifts it");
        Check(hold.Note(kMissing, Source::eInspection) == Change::eNone && !hold.Held(),
            "the old provider inspected again beside one with the check does not hold");
        Check(hold.Note(kHasCheck, Source::eCreate) == Change::eNone && !hold.Held(), "the Create on the provider with the check keeps it lifted");
        bool stayed = true;
        for (int i = 0; i < 100; ++i) stayed &= hold.Note(kMissing, Source::eInspection) == Change::eNone && !hold.Held();
        Check(stayed, "mid-game inspections of the old provider never bring the hold back (the reported bug)");
    }
    {
        // Issue 149 itself (AION 2): only an old provider, and the Create uses it.
        Hold hold;
        Check(hold.Note(kMissing, Source::eInspection) == Change::eHeld, "AION 2: the old provider holds 2X at inspection");
        Check(hold.Note(kMissing, Source::eCreate) == Change::eNone && hold.Held(), "AION 2: and the Create on it keeps the hold");
        Check(hold.Note(kHasCheck, Source::eInspection) == Change::eNone && hold.Held(),
            "a later inspection of a provider with the check does not lift what the Create decided");
        Check(hold.Note(kHasCheck, Source::eCreate) == Change::eLifted && !hold.Held(), "a later Create on a provider with the check lifts it");
    }
    {
        // A Create on the old provider beside one with the check: the provider in use decides.
        Hold hold;
        hold.Note(kHasCheck, Source::eInspection);
        hold.Note(kMissing, Source::eInspection);
        Check(!hold.Held(), "before the Create, a provider with the check keeps it lifted whatever the order");
        Check(hold.Note(kMissing, Source::eCreate) == Change::eHeld && hold.Held(), "a Create that uses the old provider holds 2X");
    }
    {
        // Inspections and a Create from different threads: the Create's decision stands.
        Hold hold;
        hold.Note(kMissing, Source::eInspection);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&hold] {
                for (int i = 0; i < 10000; ++i) hold.Note(kMissing, Source::eInspection);
            });
        hold.Note(kHasCheck, Source::eCreate);
        for (auto& thread : threads) thread.join();
        Check(!hold.Held(), "inspections racing a Create cannot undo it");
    }
    {
        Hold hold;
        Check(!hold.Held(), "nothing noted: no hold");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
