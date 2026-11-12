#pragma once

#include "dlss5_policy.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// dlss5presets.1: DLSS 5 presets. A preset is the DLSS 5 tab's settings under a name, written as a share code that
// players paste to each other ("DLSS5-" and Crockford base32, so chat apps neither format nor break it). The menu
// saves presets one code per line in MFG_PRODUCT "-Dlss5Presets.txt" beside the game.
//
// A preset holds the look and how it runs: Hook Method (not Off), every simple and Advanced setting. It leaves out
// what is not part of the look, as Reset to defaults does: Compare (Split view, its position, the hold key, Swap
// sides) and Inspect's Debug view. Loading one turns DLSS 5 on.
//
// Code bytes, before base32:
//   [0]       format (kFormat)
//   [1]       name length n (0..kMaxName), then n bytes of UTF-8
//   records   one per setting that differs from the baseline (Baseline): a tag byte, id = tag & 63 (kKeys index),
//             kind = tag >> 6: 0 false, 1 true, 2 a byte follows (a choice), 3 two bytes follow (an IEEE half float,
//             little endian). The kind gives each record's length, so a newer code's unknown ids are skipped.
//   last 4    CRC-32 (IEEE, little endian) of every byte before it
namespace dlss5::presets
{
inline constexpr std::string_view kPrefix = "DLSS5-";
inline constexpr uint8_t kFormat = 1;
inline constexpr size_t kMaxName = 48;
inline constexpr size_t kMaxPresets = 64;
// The longest code read (a full preset with the longest name is about 190 characters).
inline constexpr size_t kMaxCodeLength = 512;

// Record ids: the index here. Append only: never reorder, remove or reuse an id, or shared codes change meaning.
inline constexpr const char* kKeys[] = {
    "dlss5HookStage",            // 0
    "dlss5UseDlssResources",     // 1
    "dlss5UiCorrection",         // 2
    "dlss5Encoding",             // 3
    "dlss5DiffuseWhite",         // 4
    "dlss5DiffuseWhiteOverride", // 5
    "dlss5OverallIntensity",     // 6
    "dlss5Model",                // 7
    "dlss5StructureIntensity",   // 8
    "dlss5ToneIntensity",        // 9
    "dlss5CharacterMask",        // 10
    "dlss5CharacterIntensity",   // 11
    "dlss5ResolutionScale",      // 12
    "dlss5ResolutionScaleAuto",  // 13
    "dlss5ScalingDomain",        // 14
    "dlss5DownsampleFilter",     // 15
    "dlss5UpsampleFilter",       // 16
    "dlss5ScaleReconstruction",  // 17
    "dlss5Preset",               // 18
    "dlss5PassCount",            // 19
    "dlss5SignalEncoding",       // 20
    "dlss5BlockHudless",         // 21
    "dlss5Exposure",             // 22
    "dlss5UseExposureValue",     // 23
    "dlss5AdaptiveExposure",     // 24
    "dlss5Boost",                // 25
    "dlss5LaterPassesCustom",    // 26
    "dlss5LaterModel",           // 27
    "dlss5LaterIntensity",       // 28
    "dlss5LaterStructure",       // 29
    "dlss5LaterTone",            // 30
    "dlss5LaterCharacter",       // 31
    "dlss5BlendStrength",        // 32
    "dlss5ColorStrength",        // 33
    "dlss5HighlightGuard",       // 34
    "dlss5HighlightLimit",       // 35
    "dlss5HaloDark",             // 36
    "dlss5HaloBright",           // 37
    "dlss5HaloRadius",           // 38
    "dlss5SupersampleFilter",    // 39
    "dlss5HdrKnee",              // 40
    "dlss5AdaptiveTarget",       // 41
    "dlss5BoostTolerance",       // 42
};
inline constexpr size_t kKeyCount = std::size(kKeys);
static_assert(kKeyCount <= 64, "a record tag holds ids 0-63; a new format is needed for more");

constexpr const settings_json::Field* FieldFor(std::string_view key) noexcept
{
    for (const settings_json::Field& field : settings_json::kFields)
        if (std::string_view(field.key) == key) return &field;
    return nullptr;
}

constexpr bool KeysValid() noexcept
{
    for (size_t index = 0; index != kKeyCount; ++index)
    {
        const settings_json::Field* field = FieldFor(kKeys[index]);
        if (!field || (field->kind == settings_json::Kind::eUnsigned && field->maximum > 255.0f)) return false;
        for (size_t other = 0; other != index; ++other)
            if (std::string_view(kKeys[other]) == kKeys[index]) return false;
    }
    return true;
}
static_assert(KeysValid(), "every preset id is a dlss5* setting of kFields, choices up to 255, each once");

// Codes store the settings that differ from these: generation 3's defaults, frozen so every code keeps its meaning.
// Before changing a default in Settings, write generation 3's value here.
static_assert(kDefaultsVersion == 3, "preset codes diff against generation 3's defaults: freeze them in Baseline() first");
inline Settings Baseline() noexcept
{
    return Settings{};
}

// IEEE half float, rounded to nearest even. The ranges here (-1 to 500) are exact to about 0.05 %.
inline uint16_t ToHalf(float value) noexcept
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t biased = (bits >> 23) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (biased == 0xFFu) return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
    const int32_t exponent = static_cast<int32_t>(biased) - 127 + 15;
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exponent <= 0)
    {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);
        if (rest > halfway || (rest == halfway && (half & 1u))) ++half;
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFu;
    // A carry out of the mantissa raises the exponent, which is the correct rounding.
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;
    return static_cast<uint16_t>(sign | half);
}

inline float FromHalf(uint16_t half) noexcept
{
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits = sign;
    if (exponent == 0x1Fu) bits |= 0x7F800000u | (mantissa << 13);
    else if (exponent) bits |= ((exponent + 112u) << 23) | (mantissa << 13);
    else if (mantissa)
    {
        uint32_t biased = 113;
        while (!(mantissa & 0x400u)) { mantissa <<= 1; --biased; }
        bits |= (biased << 23) | ((mantissa & 0x3FFu) << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline uint32_t Crc32(const uint8_t* data, size_t size) noexcept
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index != size; ++index)
    {
        crc ^= data[index];
        for (int bit = 0; bit != 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Crockford base32: digits and capitals without I, L, O and U.
inline constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

inline std::string Base32(const std::vector<uint8_t>& bytes)
{
    std::string text;
    text.reserve((bytes.size() * 8 + 4) / 5);
    uint32_t buffer = 0;
    int bits = 0;
    for (const uint8_t byte : bytes)
    {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5)
        {
            text += kAlphabet[(buffer >> (bits - 5)) & 31u];
            bits -= 5;
        }
    }
    if (bits > 0) text += kAlphabet[(buffer << (5 - bits)) & 31u];
    return text;
}

// A character's base32 value, -1 for none. Lower case reads as upper case; O reads as 0, I and L as 1.
constexpr int Base32Value(char character) noexcept
{
    if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    if (character == 'O') return 0;
    if (character == 'I' || character == 'L') return 1;
    for (int index = 0; index != 32; ++index)
        if (kAlphabet[index] == character) return index;
    return -1;
}

// False for a character outside the alphabet or for bits left over that a whole code never has.
inline bool FromBase32(std::string_view text, std::vector<uint8_t>& bytes)
{
    bytes.clear();
    uint32_t buffer = 0;
    int bits = 0;
    for (const char character : text)
    {
        if (character == '-') continue;
        const int value = Base32Value(character);
        if (value < 0) return false;
        buffer = (buffer << 5) | static_cast<uint32_t>(value);
        bits += 5;
        if (bits >= 8)
        {
            bytes.push_back(static_cast<uint8_t>(buffer >> (bits - 8)));
            bits -= 8;
        }
    }
    return bits < 5 && (buffer & ((1u << bits) - 1u)) == 0;
}

// A name as presets keep it: valid UTF-8 without control characters, spaces trimmed, at most kMaxName bytes (cut on
// a character boundary). "##" becomes "#": ImGui would hide the rest of a label after it.
inline std::string CleanName(std::string_view name)
{
    std::string clean;
    for (size_t index = 0; index < name.size();)
    {
        if (name[index] == '#' && !clean.empty() && clean.back() == '#') { ++index; continue; }
        const uint8_t lead = static_cast<uint8_t>(name[index]);
        size_t length = lead < 0x80 ? 1 : (lead >= 0xC2 && lead <= 0xDF) ? 2 : (lead >= 0xE0 && lead <= 0xEF) ? 3
            : (lead >= 0xF0 && lead <= 0xF4) ? 4 : 0;
        bool valid = length != 0 && index + length <= name.size();
        for (size_t next = 1; valid && next < length; ++next)
            valid = (static_cast<uint8_t>(name[index + next]) & 0xC0u) == 0x80u;
        if (!valid) { ++index; continue; }
        if (length == 1 && (lead < 0x20 || lead == 0x7F)) { ++index; continue; }
        if (clean.size() + length > kMaxName) break;
        clean.append(name.substr(index, length));
        index += length;
    }
    const size_t first = clean.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return clean.substr(first, clean.find_last_not_of(' ') - first + 1);
}

// The records of `settings` (no format, name or checksum): equal records are the same preset.
inline std::string Records(const Settings& input)
{
    const Settings settings = Sanitized(input), baseline = Baseline();
    std::string records;
    for (size_t id = 0; id != kKeyCount; ++id)
    {
        const settings_json::Field& field = *FieldFor(kKeys[id]);
        if (field.kind == settings_json::Kind::eBool)
        {
            const bool value = settings.*field.flag;
            if (value != baseline.*field.flag) records += static_cast<char>(id | (value ? 0x40u : 0u));
        }
        else if (field.kind == settings_json::Kind::eUnsigned)
        {
            const uint32_t value = settings.*field.integer;
            if (value == baseline.*field.integer) continue;
            records += static_cast<char>(id | 0x80u);
            records += static_cast<char>(value);
        }
        else
        {
            const uint16_t value = ToHalf(settings.*field.real);
            if (value == ToHalf(baseline.*field.real)) continue;
            records += static_cast<char>(id | 0xC0u);
            records += static_cast<char>(value & 0xFFu);
            records += static_cast<char>(value >> 8);
        }
    }
    return records;
}

inline std::string Encode(const Settings& settings, std::string_view name)
{
    const std::string clean = CleanName(name), records = Records(settings);
    std::vector<uint8_t> bytes;
    bytes.reserve(2 + clean.size() + records.size() + 4);
    bytes.push_back(kFormat);
    bytes.push_back(static_cast<uint8_t>(clean.size()));
    bytes.insert(bytes.end(), clean.begin(), clean.end());
    bytes.insert(bytes.end(), records.begin(), records.end());
    const uint32_t crc = Crc32(bytes.data(), bytes.size());
    for (int shift = 0; shift != 32; shift += 8) bytes.push_back(static_cast<uint8_t>(crc >> shift));
    return std::string(kPrefix) + Base32(bytes);
}

enum class Error : uint8_t
{
    eNone = 0,
    // No "DLSS5-" in the text.
    eNoCode,
    // A code whose characters, length or checksum are wrong (cut short, mistyped, or another tool's).
    eDamaged,
    // A format this build does not read.
    eNewer,
};

struct Preset
{
    std::string name;
    // The code in its canonical spelling (Encode's).
    std::string code;
    // The baseline with the code's records: apply with Apply.
    Settings settings{};
    // Records this build does not know (from a newer MFG Unlocker), skipped.
    uint32_t skipped = 0;
};

// One code: the characters after the prefix.
inline Error DecodeBody(std::string_view body, Preset& preset)
{
    std::vector<uint8_t> bytes;
    if (body.empty() || body.size() > kMaxCodeLength || !FromBase32(body, bytes) || bytes.size() < 2 + 4)
        return Error::eDamaged;
    const size_t end = bytes.size() - 4;
    uint32_t crc = 0;
    for (int index = 0; index != 4; ++index) crc |= static_cast<uint32_t>(bytes[end + index]) << (8 * index);
    if (crc != Crc32(bytes.data(), end)) return Error::eDamaged;
    if (bytes[0] != kFormat) return bytes[0] > kFormat ? Error::eNewer : Error::eDamaged;
    const size_t nameLength = bytes[1];
    if (nameLength > kMaxName || 2 + nameLength > end) return Error::eDamaged;
    const std::string_view name(reinterpret_cast<const char*>(bytes.data() + 2), nameLength);
    if (CleanName(name) != name) return Error::eDamaged;
    Settings settings = Baseline();
    uint32_t skipped = 0;
    uint64_t seen = 0;
    for (size_t at = 2 + nameLength; at < end;)
    {
        const uint8_t tag = bytes[at++];
        const uint32_t id = tag & 63u, kind = tag >> 6;
        const size_t length = kind == 2 ? 1 : kind == 3 ? 2 : 0;
        if (at + length > end) return Error::eDamaged;
        if (seen & (1ull << id)) return Error::eDamaged;
        seen |= 1ull << id;
        if (id >= kKeyCount) { ++skipped; at += length; continue; }
        const settings_json::Field& field = *FieldFor(kKeys[id]);
        if (field.kind == settings_json::Kind::eBool)
        {
            if (kind > 1) return Error::eDamaged;
            settings.*field.flag = kind == 1;
        }
        else if (field.kind == settings_json::Kind::eUnsigned)
        {
            if (kind != 2) return Error::eDamaged;
            settings.*field.integer = bytes[at];
        }
        else
        {
            if (kind != 3) return Error::eDamaged;
            const float value = FromHalf(static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8)));
            if (!std::isfinite(value)) return Error::eDamaged;
            settings.*field.real = value;
        }
        at += length;
    }
    // A newer build's wider ranges come back into ours, as a saved file's would.
    preset.settings = Sanitized(settings);
    preset.name = std::string(name);
    preset.code = Encode(preset.settings, preset.name);
    preset.skipped = skipped;
    return Error::eNone;
}

// The first code in `text`, which may hold more: a chat message, quotes or a code block around it. Codes end at the
// first character outside the alphabet and "-". eNewer or eDamaged when every code found is so.
inline Error Decode(std::string_view text, Preset& preset)
{
    Error result = Error::eNoCode;
    for (size_t at = 0; at + kPrefix.size() <= text.size(); ++at)
    {
        bool match = true;
        for (size_t index = 0; match && index != kPrefix.size(); ++index)
        {
            char character = text[at + index];
            if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
            match = character == kPrefix[index];
        }
        if (!match) continue;
        size_t end = at + kPrefix.size();
        while (end < text.size() && (text[end] == '-' || Base32Value(text[end]) >= 0)) ++end;
        const Error error = DecodeBody(text.substr(at + kPrefix.size(), end - at - kPrefix.size()), preset);
        if (error == Error::eNone) return error;
        if (result != Error::eNewer) result = error;
        at = end - 1;
    }
    return result;
}

// `current` with a preset's settings: the preset's look and how it runs, DLSS 5 on. Compare and Inspect stay.
inline Settings Apply(const Settings& current, const Settings& preset) noexcept
{
    Settings result = current;
    for (const char* key : kKeys)
    {
        const settings_json::Field& field = *FieldFor(key);
        if (field.kind == settings_json::Kind::eBool) result.*field.flag = preset.*field.flag;
        else if (field.kind == settings_json::Kind::eUnsigned) result.*field.integer = preset.*field.integer;
        else result.*field.real = preset.*field.real;
    }
    result.enabled = true;
    return Sanitized(result);
}

// The saved presets file: one code per line; lines starting with '#' are comments. Lines that are not a readable code
// (a newer format, a hand edit) are kept as they are, so a rewrite never loses them.
struct Entry
{
    Preset preset;
    // The line as read, for a line that is not a readable code.
    std::string raw;
    bool readable = false;
};

inline std::vector<Entry> ParseStore(std::string_view content)
{
    std::vector<Entry> entries;
    while (!content.empty())
    {
        const size_t newline = content.find('\n');
        std::string_view line = content.substr(0, newline);
        content = newline == std::string_view::npos ? std::string_view{} : content.substr(newline + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        // A UTF-8 byte order mark from a text editor.
        if (entries.empty() && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);
        if (line.empty() || line.front() == '#') continue;
        Entry entry;
        entry.readable = Decode(line, entry.preset) == Error::eNone;
        if (!entry.readable) entry.raw = std::string(line.substr(0, kMaxCodeLength * 2));
        entries.push_back(std::move(entry));
        if (entries.size() >= kMaxPresets * 2) break;
    }
    return entries;
}

inline std::string WriteStore(const std::vector<Entry>& entries)
{
    std::string content = "# MFG Unlocker DLSS 5 presets: one code per line, in the menu's order.\r\n"
        "# Share a line with others; they import it under DLSS 5, Presets.\r\n";
    for (const Entry& entry : entries) content += (entry.readable ? entry.preset.code : entry.raw) + "\r\n";
    return content;
}

inline size_t ReadableCount(const std::vector<Entry>& entries) noexcept
{
    size_t count = 0;
    for (const Entry& entry : entries) count += entry.readable ? 1 : 0;
    return count;
}

// ASCII letters compare without case, so "Night" and "night" are one name.
inline bool SameName(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index != left.size(); ++index)
    {
        char a = left[index], b = right[index];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

inline constexpr size_t kNone = static_cast<size_t>(-1);

inline size_t FindName(const std::vector<Entry>& entries, std::string_view name) noexcept
{
    for (size_t index = 0; index != entries.size(); ++index)
        if (entries[index].readable && SameName(entries[index].preset.name, name)) return index;
    return kNone;
}

inline size_t FindSettings(const std::vector<Entry>& entries, const Settings& settings)
{
    const std::string records = Records(settings);
    for (size_t index = 0; index != entries.size(); ++index)
        if (entries[index].readable && Records(entries[index].preset.settings) == records) return index;
    return kNone;
}

// `name`, or "name (2)", "name (3)"... when it is taken.
inline std::string UniqueName(const std::vector<Entry>& entries, std::string_view name)
{
    const std::string base = CleanName(name);
    if (FindName(entries, base) == kNone) return base;
    for (unsigned number = 2;; ++number)
    {
        const std::string suffix = " (" + std::to_string(number) + ")";
        std::string candidate = CleanName(std::string_view(base).substr(0, kMaxName - suffix.size()));
        // CleanName cut on a character boundary; trim again in case that left a trailing space.
        candidate = CleanName(candidate) + suffix;
        if (FindName(entries, candidate) == kNone) return candidate;
    }
}
}
