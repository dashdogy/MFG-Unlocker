#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>

namespace xess_mod::frame_time_profile
{
enum class Profile
{
    eUnknown,
    eCyberpunkSeconds,
    eWitcher3Seconds,
};

inline constexpr std::string_view kCyberpunkSha256 =
    "a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991";
inline constexpr std::string_view kWitcher3Sha256 =
    "9406eccc12b68e08920931442ef6a57340e910d3e01f2082e88232487433fe51";

constexpr char AsciiLower(char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

constexpr wchar_t AsciiLower(wchar_t value) noexcept
{
    return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
}

template<class Character>
constexpr bool EqualAsciiInsensitive(std::basic_string_view<Character> left,
    std::basic_string_view<Character> right) noexcept
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (AsciiLower(left[index]) != AsciiLower(right[index])) return false;
    return true;
}

// Filename selects a candidate only; the full executable digest admits the
// observed unit contract. An unrecognized build keeps the provider's input.
constexpr Profile MatchExecutableIdentity(std::wstring_view leaf,
    std::string_view sha256) noexcept
{
    if (EqualAsciiInsensitive(leaf, std::wstring_view{L"Cyberpunk2077.exe"})
        && EqualAsciiInsensitive(sha256, kCyberpunkSha256)) return Profile::eCyberpunkSeconds;
    if (EqualAsciiInsensitive(leaf, std::wstring_view{L"witcher3.exe"})
        && EqualAsciiInsensitive(sha256, kWitcher3Sha256)) return Profile::eWitcher3Seconds;
    return Profile::eUnknown;
}

constexpr bool IsSecondsProfile(Profile profile) noexcept
{
    return profile == Profile::eCyberpunkSeconds || profile == Profile::eWitcher3Seconds;
}

constexpr const char* Name(Profile profile) noexcept
{
    switch (profile)
    {
    case Profile::eCyberpunkSeconds: return "cyberpunk_seconds";
    case Profile::eWitcher3Seconds: return "witcher3_seconds";
    default: return "unchanged";
    }
}

struct Correction
{
    float milliseconds{};
    bool converted{};
    bool validInput{};
};

// Keep the existing Cyberpunk nonnegative/finite/overflow policy for every
// seconds source (exact executables and detected units).
inline Correction ConvertSeconds(float source) noexcept
{
    const bool validInput = std::isfinite(source) && source >= 0.0f;
    if (!validInput || source > std::numeric_limits<float>::max() / 1000.0f)
        return {0.0f, true, false};
    const float milliseconds = source * 1000.0f;
    if (!std::isfinite(milliseconds)) return {0.0f, true, false};
    return {milliseconds, true, true};
}

// Game-agnostic units. Both SDK headers want milliseconds; some engines
// pass seconds (Witcher 3 and Cyberpunk do, and a game update changes their
// executable digest). Between 2 and 2000 fps a seconds value lies in 0.0005..0.5
// and a milliseconds value in 0.5..500, so the magnitude separates them. When the
// spacing of the game's TagFrameConstants calls is known it must agree within
// 10x (frameRenderTime may be render time, shorter than the frame), which rejects
// a stray value. kVotes agreeing samples in a row lock the decision; until then
// each sample uses its own magnitude.
enum class Units { eUndecided, eMilliseconds, eSeconds };

constexpr const char* Name(Units units) noexcept
{
    return units == Units::eMilliseconds ? "ms" : units == Units::eSeconds ? "seconds" : "undecided";
}

class UnitDetector
{
public:
    static constexpr int kVotes = 8;
    static constexpr float kBoundary = 0.5f;
    static constexpr double kMinimumGapMs = 0.5, kMaximumGapMs = 1000.0, kMaximumRatio = 10.0;

    Units Decided() const noexcept { return decided_; }
    int Votes() const noexcept { return votes_; }
    // One sample's own reading (eUndecided: invalid, zero or contradicted by the gap).
    static Units Classify(float source, double tagGapMs) noexcept
    {
        if (!std::isfinite(source) || source <= 0.0f) return Units::eUndecided;
        const Units magnitude = source < kBoundary ? Units::eSeconds : Units::eMilliseconds;
        if (tagGapMs >= kMinimumGapMs && tagGapMs <= kMaximumGapMs)
        {
            const double milliseconds = magnitude == Units::eSeconds ? double(source)*1000.0 : double(source);
            const double ratio = milliseconds/tagGapMs;
            if (ratio < 1.0/kMaximumRatio || ratio > kMaximumRatio) return Units::eUndecided;
        }
        return magnitude;
    }
    // The units to apply to this sample.
    Units Observe(float source, double tagGapMs) noexcept
    {
        if (decided_ != Units::eUndecided) return decided_;
        const Units vote = Classify(source, tagGapMs);
        if (vote == Units::eUndecided) { votes_ = 0; candidate_ = Units::eUndecided; }
        else if (vote == candidate_) { if (++votes_ >= kVotes) decided_ = vote; }
        else { candidate_ = vote; votes_ = 1; }
        if (decided_ != Units::eUndecided) return decided_;
        if (!std::isfinite(source) || source <= 0.0f) return Units::eMilliseconds; // nothing to convert
        return source < kBoundary ? Units::eSeconds : Units::eMilliseconds;
    }

private:
    Units decided_{Units::eUndecided}, candidate_{Units::eUndecided};
    int votes_{};
};

inline Correction NormalizeUnits(float source, Units units) noexcept
{
    if (units == Units::eSeconds) return ConvertSeconds(source);
    return {source, false, std::isfinite(source) && source >= 0.0f};
}
}
