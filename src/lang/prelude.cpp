#include "lang/prelude.h"

#include "lang/outer_parser.h"

namespace hana::lang {

namespace {

// Declarations every program has, written in HANA. 'Ok(v)' and 'Err(e)'
// make values of 'Result'; the '?' operator returns its 'Err' early.
constexpr const char* kPreludeSource = R"HANA({
  Result<T, E>: enum {
    Ok: { value: T },
    Err: { error: E },

    is_ok: fn<bool(self)> '''
      match self:
        Ok(_): return true
        Err(_): return false
    ''',

    is_err: fn<bool(self)> not self.is_ok(),

    value_or: fn<T(sink self, sink fallback: T)> '''
      match self:
        Ok(v): return v
        Err(_): return fallback
    ''',
  },
}
)HANA";

} // namespace

const SourceFile& prelude_source()
{
    static const SourceFile file{"<prelude>", kPreludeSource};
    return file;
}

std::unique_ptr<Module> parse_prelude(DiagnosticEngine& diagnostics)
{
    OuterParser parser(prelude_source(), diagnostics, true);
    return parser.parse();
}

} // namespace hana::lang
