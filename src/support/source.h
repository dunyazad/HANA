#pragma once

#include <cstdint>
#include <string>

namespace hana {

// Position inside a source file. Line and column are 1-based, column counts
// UTF-8 code points, offset is the 0-based byte offset.
struct SourceLocation {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::uint32_t offset = 0;
};

struct SourceFile {
    std::string path;
    std::string text;
};

} // namespace hana
