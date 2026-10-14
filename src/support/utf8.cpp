#include "support/utf8.h"

namespace hana {

bool is_utf8_continuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

bool append_utf8(std::string& out, std::uint32_t code_point)
{
    if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
        return false;
    }
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
    return true;
}

static std::size_t sequence_length(unsigned char lead)
{
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 0;
}

std::uint32_t decode_utf8(std::string_view text, std::size_t& length)
{
    length = 0;
    if (text.empty()) {
        return kInvalidCodePoint;
    }
    const auto lead = static_cast<unsigned char>(text[0]);
    const std::size_t count = sequence_length(lead);
    if (count == 0 || count > text.size()) {
        length = 1;
        return kInvalidCodePoint;
    }
    static constexpr unsigned char kLeadMask[] = {0, 0x7F, 0x1F, 0x0F, 0x07};
    std::uint32_t value = lead & kLeadMask[count];
    for (std::size_t i = 1; i < count; ++i) {
        if (!is_utf8_continuation(text[i])) {
            length = i;
            return kInvalidCodePoint;
        }
        value = (value << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
    }
    length = count;
    static constexpr std::uint32_t kMinimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < kMinimum[count] || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return kInvalidCodePoint;
    }
    return value;
}

} // namespace hana
