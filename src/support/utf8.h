#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hana {

constexpr std::uint32_t kInvalidCodePoint = 0xFFFFFFFF;

bool is_utf8_continuation(char c);

// Appends the UTF-8 encoding of a code point. Returns false for values that
// are not Unicode scalar values (surrogates or above U+10FFFF).
bool append_utf8(std::string& out, std::uint32_t code_point);

// Decodes the first code point of text and stores its byte length in length.
// Returns kInvalidCodePoint for malformed input.
std::uint32_t decode_utf8(std::string_view text, std::size_t& length);

} // namespace hana
