#include "ampere_policy.h"
#include <algorithm>
#include <iostream>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
int main()
{
    using ampere_policy::DecideOptions;
    using Mode = ampere_policy::OptionsMode;
    // Follow game: the menu stores multiplier 2 for Follow game.
    auto d = DecideOptions(true, 2, false, Mode::eOn, 2, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 2 && !d.gameDynamicTarget,
        "Oblivion: Follow game keeps the game's 3x instead of the stored 2x");
    d = DecideOptions(true, 2, false, Mode::eOn, 3, 6, true);
    Check(d.mode == Mode::eOn && d.generatedFrames == 3, "Follow game keeps the game's fixed 4x");
    d = DecideOptions(true, 2, false, Mode::eDynamic, 5, 6, true);
    Check(d.mode == Mode::eDynamic && d.generatedFrames == 5 && d.gameDynamicTarget,
        "Follow game keeps the game's Dynamic request and its target");
    d = DecideOptions(true, 2, false, Mode::eDynamic, 5, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 5 && !d.gameDynamicTarget,
        "game Dynamic without full-capacity proof becomes fixed at the game's count");
    d = DecideOptions(true, 2, false, Mode::eDynamic, 5, 2, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 1, "game Dynamic on an uncertified route stays 2x");
    d = DecideOptions(true, 2, false, Mode::eOn, 5, 2, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 1, "game 6x is limited to the 2x safe maximum");
    d = DecideOptions(true, 2, false, Mode::eOn, 7, 6, false);
    Check(d.generatedFrames == 5, "game count above the six-buffer program is limited to 6x");
    d = DecideOptions(true, 2, false, Mode::eOn, 0, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 1, "game count 0 is submitted as 2x");
    d = DecideOptions(true, 2, false, Mode::eOff, 2, 6, true);
    Check(d.mode == Mode::eOff && !d.gameDynamicTarget, "game Off stays Off");
    d = DecideOptions(true, 1, false, Mode::eOn, 2, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 2, "Follow game ignores a stale stored multiplier of 1");
    d = DecideOptions(true, 6, true, Mode::eOn, 2, 6, true);
    Check(d.mode == Mode::eOn && d.generatedFrames == 2 && !d.gameDynamicTarget,
        "Follow game ignores a stale stored Dynamic choice");
    // Fixed and Dynamic menu choices replace the game's request.
    d = DecideOptions(false, 4, false, Mode::eDynamic, 5, 6, true);
    Check(d.mode == Mode::eOn && d.generatedFrames == 3 && !d.gameDynamicTarget, "fixed 4x replaces game Dynamic");
    d = DecideOptions(false, 4, false, Mode::eOn, 1, 2, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 1, "fixed 4x on a 2x safe maximum submits 2x");
    d = DecideOptions(false, 1, false, Mode::eOn, 2, 6, false);
    Check(d.mode == Mode::eOff, "fixed 1x is Off");
    d = DecideOptions(false, 4, false, Mode::eOff, 2, 6, false);
    Check(d.mode == Mode::eOff, "game Off overrides a fixed choice");
    d = DecideOptions(false, 6, true, Mode::eOn, 1, 6, true);
    Check(d.mode == Mode::eDynamic && d.generatedFrames == 5 && !d.gameDynamicTarget,
        "menu Dynamic uses the menu target");
    d = DecideOptions(false, 6, true, Mode::eOn, 1, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 5, "menu Dynamic without proof stays fixed");
    // Fixed and Dynamic choices match the previous submission exactly.
    bool same = true, bounded = true;
    for (uint32_t safe = 2; safe <= 6; ++safe)
        for (uint32_t multiplier = 0; multiplier <= 7; ++multiplier)
            for (int dynamic = 0; dynamic < 2; ++dynamic)
                for (int supported = 0; supported < 2; ++supported)
                    for (Mode game : {Mode::eOff, Mode::eOn, Mode::eDynamic})
                    {
                        const auto fixed = DecideOptions(false, multiplier, dynamic, game, 3, safe, supported);
                        const uint32_t effective = std::clamp(multiplier, 1u, safe);
                        const Mode previous = multiplier == 1 || game == Mode::eOff ? Mode::eOff
                            : dynamic && supported ? Mode::eDynamic : Mode::eOn;
                        same = same && fixed.mode == previous && !fixed.gameDynamicTarget
                            && fixed.generatedFrames == std::max(effective, 2u) - 1u;
                        for (uint32_t frames = 0; frames <= 7; ++frames)
                        {
                            const auto follow = DecideOptions(true, multiplier, dynamic, game, frames, safe, supported);
                            bounded = bounded && follow.generatedFrames >= 1
                                && follow.generatedFrames <= std::max(safe, 2u) - 1u
                                && follow.gameDynamicTarget == (follow.mode == Mode::eDynamic)
                                && (follow.mode != Mode::eDynamic || supported)
                                && (follow.mode == Mode::eOff) == (game == Mode::eOff);
                        }
                    }
    Check(same, "fixed and menu Dynamic decisions match the previous submission for every input");
    Check(bounded, "Follow game never exceeds the safe maximum or admits unproven Dynamic");
    // The maximum the game is offered (numFramesToGenerateMax).
    using ampere_policy::AdvertisedGeneratedFrames;
    Check(AdvertisedGeneratedFrames(5, 4) == 3,
        "Where Winds Meet: a 4X title is offered 4x, not the 6x the options path would clamp");
    Check(AdvertisedGeneratedFrames(5, 6) == 5, "a 6X title is offered 6x");
    Check(AdvertisedGeneratedFrames(5, 2) == 1, "a title without an NVIDIA MFG override is offered 2x");
    Check(AdvertisedGeneratedFrames(1, 6) == 1, "a legacy 2x wrapper is offered 2x on any title");
    Check(AdvertisedGeneratedFrames(0, 6) == 1, "no startup capacity is offered 2x");
    bool matches = true;
    for (uint32_t policy = 0; policy <= 7; ++policy)
    {
        const uint32_t offered = AdvertisedGeneratedFrames(5, policy);
        // Once certified, Follow game must submit what the game was offered.
        const auto follow = DecideOptions(true, 2, false, Mode::eOn, offered, std::clamp(policy, 2u, 6u), false);
        matches = matches && offered >= 1 && offered <= 5 && follow.generatedFrames == offered;
    }
    Check(matches, "every offered maximum is submitted unclamped once the route is certified");
    // ringcap.1: the certified ring caps the modes at N+1.
    using ampere_policy::RingMaximumMultiplier;
    bool ringMultiplier = true;
    for (uint32_t n = 1; n <= 5; ++n) ringMultiplier = ringMultiplier && RingMaximumMultiplier(5, n) == n + 1;
    Check(ringMultiplier, "a certified ring of N generated frames carries (N+1)x on an MFG startup");
    Check(RingMaximumMultiplier(5, 0) == 2 && RingMaximumMultiplier(5, 6) == 2,
        "no certified ring, or an impossible one, stays at the 2x safe maximum");
    Check(RingMaximumMultiplier(1, 1) == 2 && RingMaximumMultiplier(1, 3) == 2 && RingMaximumMultiplier(0, 3) == 2,
        "a legacy or missing startup never rises above 2x");
    // FF7 Rebirth, RTX 2070 SUPER: startup 5, ring 3 (4 buffers), NVIDIA 4X.
    const uint32_t ff7Safe = std::min(RingMaximumMultiplier(5, 3), 4u);
    d = DecideOptions(true, 2, false, Mode::eOn, 5, ff7Safe, false);
    Check(ff7Safe == 4 && d.mode == Mode::eOn && d.generatedFrames == 3, "FF7 Rebirth: game 6x submits the ring's 4x");
    d = DecideOptions(true, 2, false, Mode::eOn, 3, ff7Safe, false);
    Check(d.generatedFrames == 3, "FF7 Rebirth: the game's 4x is submitted unclamped");
    d = DecideOptions(true, 2, false, Mode::eOn, 1, ff7Safe, false);
    Check(d.generatedFrames == 1, "FF7 Rebirth: the game's 2x stays 2x");
    d = DecideOptions(false, 6, false, Mode::eOn, 3, ff7Safe, false);
    Check(d.generatedFrames == 3, "FF7 Rebirth: a fixed 6x choice submits 4x");
    d = DecideOptions(false, 6, true, Mode::eOn, 3, ff7Safe, ff7Safe == 6);
    Check(d.mode == Mode::eOn && d.generatedFrames == 3, "a partial ring keeps menu Dynamic off (it needs the full six-buffer proof)");
    Check(AdvertisedGeneratedFrames(5, 4, 3) == 3, "FF7 Rebirth is offered 4x before and after its ring is certified");
    Check(AdvertisedGeneratedFrames(5, 6, 3) == 3, "a 6X title with a four-buffer ring is offered 4x once the ring is certified");
    Check(AdvertisedGeneratedFrames(5, 6, 0) == 5 && AdvertisedGeneratedFrames(5, 6, 5) == 5
        && AdvertisedGeneratedFrames(5, 6, 6) == 5, "no ring yet, a full ring or an impossible ring leave the startup offer");
    Check(AdvertisedGeneratedFrames(5, 4, 5) == 3 && AdvertisedGeneratedFrames(1, 6, 3) == 1,
        "the NVIDIA ceiling and a legacy wrapper still bound the offer");
    bool ringBounded = true, ringOffered = true;
    for (uint32_t ring = 1; ring <= 5; ++ring)
        for (uint32_t policy = 2; policy <= 6; ++policy)
        {
            const uint32_t safe = std::min(RingMaximumMultiplier(5, ring), policy);
            const uint32_t offered = AdvertisedGeneratedFrames(5, policy, ring);
            ringOffered = ringOffered && offered <= ring && offered == std::max(safe, 2u) - 1u;
            for (uint32_t frames = 0; frames <= 7; ++frames)
                for (uint32_t multiplier = 0; multiplier <= 7; ++multiplier)
                    for (bool follow : {true, false})
                    {
                        const auto decision = DecideOptions(follow, multiplier, false, Mode::eOn, frames, safe, false);
                        ringBounded = ringBounded && decision.generatedFrames >= 1 && decision.generatedFrames <= ring
                            && ampere_policy::EvaluationFitsCapacity(static_cast<int>(decision.generatedFrames), ring);
                    }
        }
    Check(ringBounded, "no Follow game or fixed submission exceeds the certified ring on any title");
    Check(ringOffered, "the game is offered exactly what the ring and the title allow");
    // admitoff.1: after a failed startup admission every Create is refused, so
    // every request is held at Off instead of retried by Streamline each frame.
    using ampere_policy::HoldOffWithoutAdmission;
    bool held = true, untouched = true;
    for (bool follow : {true, false})
        for (uint32_t multiplier = 1; multiplier <= 6; ++multiplier)
            for (int dynamic = 0; dynamic < 2; ++dynamic)
                for (Mode game : {Mode::eOff, Mode::eOn, Mode::eDynamic})
                {
                    const auto decided = DecideOptions(follow, multiplier, dynamic, game, 3, 6, true);
                    const auto off = HoldOffWithoutAdmission(decided, true);
                    const auto kept = HoldOffWithoutAdmission(decided, false);
                    held = held && off.mode == Mode::eOff && !off.gameDynamicTarget
                        && off.generatedFrames == decided.generatedFrames;
                    untouched = untouched && kept.mode == decided.mode
                        && kept.gameDynamicTarget == decided.gameDynamicTarget
                        && kept.generatedFrames == decided.generatedFrames;
                }
    Check(held, "without admission every Follow game, fixed or Dynamic request is Off and keeps its frame count");
    Check(untouched, "with admission, or before it is known, the decision is unchanged");
    d = HoldOffWithoutAdmission(DecideOptions(true, 2, false, Mode::eOn, 1, 2, false), true);
    Check(d.mode == Mode::eOff && d.generatedFrames == 1,
        "Doom TDA, RTX 3080 Ti: the game's 2x request after a failed startup reaches Streamline as Off");

    // rtx30ngx.1 (DW3 = Dynasty Warriors 3): OptiScaler's own DLSSG output with MFGUnlocker's XeMFG provider as its
    // Nvngx replacement. No Ampere Create certifies a ring there, so the ring rule alone would hold 6x at 2x.
    using ampere_policy::ReplacementOwnsFrameGeneration;
    using ampere_policy::SafeMaximumMultiplier;
    Check(ReplacementOwnsFrameGeneration(true, 0, 0), "the provider offered its maximum and NVIDIA's created nothing");
    Check(!ReplacementOwnsFrameGeneration(false, 0, 0), "without the provider's offer the Ampere ring governs");
    Check(!ReplacementOwnsFrameGeneration(true, 1, 0) && !ReplacementOwnsFrameGeneration(true, 0, 1),
        "a FrameGeneration Create on NVIDIA's provider hands the decision back to the certified ring");
    Check(SafeMaximumMultiplier(false, 5, true, 5, 0) == 2, "control: the uncertified ring is the 2x of the DW3 run");
    bool replacementSafe = true;
    for (bool ready : {false, true})
        for (uint32_t startup : {0u, 1u, 5u})
            for (uint32_t certified = 0; certified <= 6; ++certified)
                replacementSafe = replacementSafe && SafeMaximumMultiplier(true, 5, ready, startup, certified) == 6
                    && SafeMaximumMultiplier(false, 5, ready, startup, certified)
                        == (ready ? RingMaximumMultiplier(startup, certified) : 2u);
    Check(replacementSafe, "a replacement's maximum is 6x on any ring state; otherwise the ring rule is unchanged");
    Check(SafeMaximumMultiplier(true, 0, true, 5, 0) == 2 && SafeMaximumMultiplier(true, 9, true, 5, 0) == 6
        && SafeMaximumMultiplier(true, 3, false, 5, 0) == 4, "a replacement maximum is bounded to 2x-6x");
    d = DecideOptions(true, 2, false, Mode::eOn, 5, SafeMaximumMultiplier(true, 5, true, 5, 0), false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 5, "DW3: OptiScaler's 6x request is submitted as 6x");
    d = DecideOptions(false, 6, false, Mode::eOn, 1, SafeMaximumMultiplier(true, 5, true, 5, 0), false);
    Check(d.generatedFrames == 5, "a fixed 6x menu choice reaches the replacement as 6x");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
