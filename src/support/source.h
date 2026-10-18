#pragma once

#include <cstdint>
#include <string>

namespace hana {

// Position inside a source file. Line and column are 1-based, column counts
// UTF-8 code points, offset is the 0-based byte offset.
// 'in_prelude' marks a place in the prelude the compiler adds to every
// program, rather than in the file being compiled.
struct SourceLocation {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::uint32_t offset = 0;
    bool in_prelude = false;
};

struct SourceFile {
    std::string path;
    std::string text;
};

} // namespace hana
