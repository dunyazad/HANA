#pragma once

#include <optional>
#include <string>

#include "support/diagnostic.h"

namespace hana::lang {

// Re-indents a module into the canonical layout:
//   - JSON layer: two spaces per open '{', '[' or '('
//   - ''' blocks: code starts two spaces right of the line that opens the
//     block, two spaces per block level; the closing ''' lines up with the
//     opening line
//   - continuation lines inside brackets keep their offset from the start
//     of their logical line
//   - trailing whitespace is removed and the file ends with one newline
// Only whitespace at line starts and ends changes, so the meaning and the
// comments of the program are preserved. Returns nullopt, with diagnostics,
// when the file has syntax errors; such files are left alone.
std::optional<std::string> format_source(const SourceFile& file, DiagnosticEngine& diagnostics);

} // namespace hana::lang
