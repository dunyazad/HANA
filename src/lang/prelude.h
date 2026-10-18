#pragma once

#include <memory>

#include "lang/ast.h"
#include "support/diagnostic.h"
#include "support/source.h"

namespace hana::lang {

// The source of the prelude: declarations every program has, such as
// 'Result<T, E>'. Its locations are marked 'in_prelude'.
const SourceFile& prelude_source();

// Parses the prelude. Diagnostics go to 'diagnostics'; there are none
// unless the compiler itself is wrong.
std::unique_ptr<Module> parse_prelude(DiagnosticEngine& diagnostics);

} // namespace hana::lang
