#pragma once

#include "dlss5_policy.h"
#include "ui_status_json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

// Reading and writing the dlss5* keys of kFields (dlss5_policy.h).
namespace dlss5::settings_json
{
// Legacy keys: "dlss5Intensity" in percent and "dlss5Quality" (1 =
// the 0.85 network extent). Read only when the new key is absent.
inline constexpr const char* kLegacyIntensity = "dlss5Intensity";
inline constexpr const char* kLegacyQuality = "dlss5Quality";

namespace detail
{
inline bool Find(std::string_view content, const char* name, size_t& offset) noexcept
{
    const std::string key = std::string("\"") + name + "\"";
    const size_t at = content.find(key);
    if (at == std::string_view::npos) return false;
    const size_t colon = content.find(':', at + key.size());
    if (colon == std::string_view::npos) return false;
    offset = content.find_first_not_of(" \t\r\n", colon + 1);
    return offset != std::string_view::npos;
}

// The value's end offset, or npos when the value is not of the kind.
inline size_t ParseValue(std::string_view content, size_t offset, Kind kind, float& value) noexcept
{
    if (kind == Kind::eBool)
    {
        if (content.compare(offset, 4, "true") == 0 && ui_status_json::ValueEnd(content, offset + 4))
        { value = 1.0f; return offset + 4; }
        if (content.compare(offset, 5, "false") == 0 && ui_status_json::ValueEnd(content, offset + 5))
        { value = 0.0f; return offset + 5; }
        return std::string_view::npos;
    }
    if (kind == Kind::eUnsigned)
    {
        uint32_t parsed = 0;
        const auto result = std::from_chars(content.data() + offset, content.data() + content.size(), parsed);
        const size_t end = static_cast<size_t>(result.ptr - content.data());
        if (result.ec != std::errc{} || !ui_status_json::ValueEnd(content, end)) return std::string_view::npos;
        value = static_cast<float>(parsed);
        return end;
    }
    float parsed = 0.0f;
    const auto result = std::from_chars(content.data() + offset, content.data() + content.size(), parsed);
    const size_t end = static_cast<size_t>(result.ptr - content.data());
    if (result.ec != std::errc{} || !std::isfinite(parsed) || !ui_status_json::ValueEnd(content, end))
        return std::string_view::npos;
    value = parsed;
    return end;
}

inline std::string Format(const Field& field, const Settings& settings)
{
    if (field.kind == Kind::eBool) return settings.*field.flag ? "true" : "false";
    if (field.kind == Kind::eUnsigned) return std::to_string(settings.*field.integer);
    char text[32]{};
    const auto result = std::to_chars(text, text + sizeof(text) - 1, settings.*field.real);
    return result.ec == std::errc{} ? std::string(text, result.ptr) : std::string("0");
}
}

// Reads every present dlss5* key over `settings`. False when a present key is
// malformed or out of range: the caller keeps its last valid settings. Content
// of an older defaults generation (without dlss5DefaultsVersion: generation 1)
// moves to the new defaults where it still held the old ones (MigrateDefaults).
inline bool Parse(std::string_view content, Settings& settings) noexcept
{
    Settings parsed = settings;
    size_t marker = 0;
    const bool current = detail::Find(content, "dlss5DefaultsVersion", marker);
    for (const Field& field : kFields)
    {
        size_t offset = 0;
        if (!detail::Find(content, field.key, offset)) continue;
        float value = 0.0f;
        if (detail::ParseValue(content, offset, field.kind, value) == std::string_view::npos
            || value < field.minimum || value > field.maximum) return false;
        if (field.kind == Kind::eBool) parsed.*field.flag = value != 0.0f;
        else if (field.kind == Kind::eUnsigned) parsed.*field.integer = static_cast<uint32_t>(value);
        else parsed.*field.real = value;
    }
    size_t offset = 0;
    float legacy = 0.0f;
    if (!detail::Find(content, "dlss5OverallIntensity", offset) && detail::Find(content, kLegacyIntensity, offset))
    {
        if (detail::ParseValue(content, offset, Kind::eUnsigned, legacy) == std::string_view::npos || legacy > 100.0f)
            return false;
        parsed.intensity = legacy / 100.0f;
    }
    if (!detail::Find(content, "dlss5ResolutionScale", offset) && detail::Find(content, kLegacyQuality, offset))
    {
        if (detail::ParseValue(content, offset, Kind::eUnsigned, legacy) == std::string_view::npos || legacy > 1.0f)
            return false;
        parsed.scale = legacy == 1.0f ? 85.0f : 100.0f;
    }
    const uint32_t from = current ? parsed.defaultsVersion : 1u;
    if (from < kDefaultsVersion) MigrateDefaults(parsed, from);
    settings = Sanitized(parsed);
    return true;
}

// Writes every dlss5* key into a flat JSON object, replacing present values
// and appending absent ones. Legacy keys are removed.
inline bool Write(std::string& content, const Settings& input)
{
    const Settings settings = Sanitized(input);
    for (const char* legacy : {kLegacyIntensity, kLegacyQuality})
    {
        const std::string key = std::string(",\"") + legacy + "\"";
        size_t at = content.find(key);
        if (at == std::string::npos) continue;
        size_t offset = 0;
        float value = 0.0f;
        if (!detail::Find(content, legacy, offset)) return false;
        const size_t end = detail::ParseValue(content, offset, Kind::eUnsigned, value);
        if (end == std::string::npos) return false;
        content.erase(at, end - at);
    }
    for (const Field& field : kFields)
    {
        const std::string value = detail::Format(field, settings);
        size_t offset = 0;
        if (detail::Find(content, field.key, offset))
        {
            float previous = 0.0f;
            // A stored number of another kind (e.g. 1 for a float) is rewritten too.
            size_t end = detail::ParseValue(content, offset, field.kind, previous);
            if (end == std::string::npos) end = detail::ParseValue(content, offset, Kind::eFloat, previous);
            if (end == std::string::npos) return false;
            content.replace(offset, end - offset, value);
        }
        else
        {
            const size_t close = content.find_last_not_of(" \t\r\n");
            if (close == std::string::npos || content[close] != '}') return false;
            content.insert(close, std::string(",\"") + field.key + "\":" + value);
        }
    }
    return true;
}

// The settings as status fields: ",\"dlss5\":true,..." (no braces).
inline std::string StatusFields(const Settings& settings)
{
    std::string fields;
    fields.reserve(768);
    for (const Field& field : kFields)
        fields += std::string(",\"") + field.key + "\":" + detail::Format(field, settings);
    return fields;
}
}
