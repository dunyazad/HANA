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
    if (diagnostic.location.in_prelude) {
        return "<prelude>:" + std::to_string(diagnostic.location.line) + ": " + label + ": " +
               diagnostic.message + "\n";
    }
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

std::string json_escape(std::string_view text)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += "\\u00";
                out += kHex[(c >> 4) & 0xF];
                out += kHex[c & 0xF];
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

static std::string json_position(const Diagnostic& diagnostic)
{
    return "\"line\": " + std::to_string(diagnostic.location.line) +
           ", \"column\": " + std::to_string(diagnostic.location.column) +
           (diagnostic.location.in_prelude ? ", \"in_prelude\": true" : "") +
           ", \"message\": \"" + json_escape(diagnostic.message) + "\"";
}

std::string diagnostics_to_json(const SourceFile& file, const std::vector<Diagnostic>& diagnostics)
{
    std::string items;
    std::string notes;
    int error_count = 0;
    bool open_item = false;
    const auto close_item = [&]() {
        if (open_item) {
            items += ", \"notes\": [" + notes + "]}";
            notes.clear();
            open_item = false;
        }
    };
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == Severity::Note && open_item) {
            notes += (notes.empty() ? "" : ", ") + std::string("{") + json_position(diagnostic) + "}";
            continue;
        }
        close_item();
        if (!items.empty()) {
            items += ",\n    ";
        }
        const bool is_error = diagnostic.severity == Severity::Error;
        error_count += is_error ? 1 : 0;
        items += std::string("{\"severity\": \"") + (is_error ? "error" : "note") + "\", " +
                 json_position(diagnostic);
        open_item = true;
    }
    close_item();
    return "{\"file\": \"" + json_escape(file.path) + "\", \"error_count\": " +
           std::to_string(error_count) + ", \"diagnostics\": [" +
           (items.empty() ? "" : "\n    " + items + "\n") + "]}\n";
}

} // namespace hana
