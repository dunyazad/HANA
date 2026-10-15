#pragma once

#include <string>
#include <vector>

#include "support/source.h"

namespace hana {

enum class Severity {
    Error,
    Note,
};

struct Diagnostic {
    Severity severity = Severity::Error;
    SourceLocation location;
    std::string message;
};

// Collects diagnostics from every compiler stage. Stages never stop on the
// first error; they report and continue so that one run shows all problems.
class DiagnosticEngine {
public:
    void error(const SourceLocation& location, std::string message);
    void note(const SourceLocation& location, std::string message);

    bool has_errors() const { return error_count_ > 0; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

private:
    std::vector<Diagnostic> diagnostics_;
    int error_count_ = 0;
};

// "path:line:col: error: message" followed by the source line and a caret.
std::string format_diagnostic(const SourceFile& file, const Diagnostic& diagnostic);

} // namespace hana
