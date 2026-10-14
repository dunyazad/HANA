#include "support/diagnostic.h"

#include <string_view>
#include <utility>

namespace hana {

void DiagnosticEngine::error(const SourceLocation& location, std::string message)
{
    diagnostics_.push_back(Diagnostic{Severity::Error, location, std::move(message)});
    ++error_count_;
}

void DiagnosticEngine::note(const SourceLocation& location, std::string message)
{
    diagnostics_.push_back(Diagnostic{Severity::Note, location, std::move(message)});
}

static std::string_view line_containing(const std::string& text, std::uint32_t offset)
{
    std::size_t begin = offset < text.size() ? offset : text.size();
    while (begin > 0 && text[begin - 1] != '\n') {
        --begin;
    }
    std::size_t end = begin;
    while (end < text.size() && text[end] != '\n') {
        ++end;
    }
    std::string_view line(text.data() + begin, end - begin);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

static std::string caret_line(std::string_view line, std::uint32_t column)
{
    // Reproduce tabs so the caret lines up with the source line.
    std::string out;
    std::uint32_t current = 1;
    for (char c : line) {
        if (current >= column) {
            break;
        }
        if ((static_cast<unsigned char>(c) & 0xC0) == 0x80) {
            continue;
        }
        out += (c == '\t') ? '\t' : ' ';
        ++current;
    }
    out += '^';
    return out;
}

std::string format_diagnostic(const SourceFile& file, const Diagnostic& diagnostic)
{
    const char* label = diagnostic.severity == Severity::Error ? "error" : "note";
    std::string out = file.path + ":" + std::to_string(diagnostic.location.line) + ":" +
                      std::to_string(diagnostic.location.column) + ": " + label + ": " +
                      diagnostic.message + "\n";
    const std::string_view line = line_containing(file.text, diagnostic.location.offset);
    out += "    ";
    out += line;
    out += "\n    ";
    out += caret_line(line, diagnostic.location.column);
    out += "\n";
    return out;
}

} // namespace hana
