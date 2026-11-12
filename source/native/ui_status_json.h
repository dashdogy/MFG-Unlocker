#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

namespace ui_status_json
{
inline bool Space(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

inline bool ValueEnd(std::string_view text, size_t offset) noexcept
{
    while (offset < text.size() && Space(text[offset])) ++offset;
    return offset == text.size() || text[offset] == ',' || text[offset] == '}';
}

// The most top-level keys one status may carry. rtx30ngx.1: an RTX 30/20 status
// reached 515 keys, past the old 512, and was rejected.
constexpr size_t kMaximumKeys = 2048;

// Status protocols contain one flat object of strings, numeric measurements,
// counters, booleans and nulls. Validate the entire document before the field
// reader can publish it; a complete prefix is not a complete snapshot.
inline bool CompleteObject(std::string_view text)
{
    size_t offset = 0;
    auto space = [&] { while (offset < text.size() && Space(text[offset])) ++offset; };
    auto string = [&](bool key, std::string_view& value) {
        if (offset == text.size() || text[offset++] != '"') return false;
        const size_t start = offset;
        while (offset < text.size())
        {
            const unsigned char ch = static_cast<unsigned char>(text[offset++]);
            if (ch == '"') { value = text.substr(start, offset-start-1); return true; }
            if (ch < 0x20) return false;
            if (ch != '\\') continue;
            // Producer field names are literal ASCII identifiers. Escaped
            // aliases must not bypass the duplicate-key or required-key checks.
            if (key || offset == text.size()) return false;
            const char escape = text[offset++];
            if (escape == 'u')
            {
                for (int digit = 0; digit < 4; ++digit)
                {
                    if (offset == text.size()) return false;
                    const char hex = text[offset++];
                    if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f')
                        || (hex >= 'A' && hex <= 'F'))) return false;
                }
            }
            else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
                return false;
        }
        return false;
    };
    space();
    if (offset == text.size() || text[offset++] != '{') return false;
    space();
    std::vector<std::string_view> keys;
    keys.reserve(192);
    if (offset < text.size() && text[offset] != '}')
    {
        for (;;)
        {
            std::string_view key;
            if (keys.size() == kMaximumKeys || !string(true, key)) return false;
            keys.push_back(key);
            space();
            if (offset == text.size() || text[offset++] != ':') return false;
            space();
            if (offset == text.size()) return false;
            if (text[offset] == '"')
            {
                std::string_view ignored;
                if (!string(false, ignored)) return false;
            }
            else if (text.substr(offset, 4) == "true" || text.substr(offset, 4) == "null")
                offset += 4;
            else if (text.substr(offset, 5) == "false")
                offset += 5;
            else
            {
                if (text[offset] == '-') ++offset;
                if (offset == text.size() || text[offset] < '0' || text[offset] > '9') return false;
                if (text[offset++] != '0')
                    while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') ++offset;
                if (offset < text.size() && text[offset] == '.')
                {
                    ++offset;
                    const size_t fraction = offset;
                    while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') ++offset;
                    if (offset == fraction) return false;
                }
                if (offset < text.size() && (text[offset] == 'e' || text[offset] == 'E'))
                {
                    ++offset;
                    if (offset < text.size() && (text[offset] == '+' || text[offset] == '-')) ++offset;
                    const size_t exponent = offset;
                    while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') ++offset;
                    if (offset == exponent) return false;
                }
            }
            space();
            if (offset == text.size()) return false;
            if (text[offset] == '}') break;
            if (text[offset++] != ',') return false;
            space();
        }
    }
    if (offset == text.size() || text[offset++] != '}') return false;
    space();
    if (offset != text.size()) return false;
    std::sort(keys.begin(), keys.end());
    return std::adjacent_find(keys.begin(), keys.end()) == keys.end();
}
}
