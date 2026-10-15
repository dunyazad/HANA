#include "lang/formatter.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

#include "lang/outer_parser.h"

namespace hana::lang {

namespace {

constexpr std::size_t kIndentWidth = 2;

enum class LineKind {
    Json,       // part of the JSON layer, possibly holding one-line code
    Code,       // inside a multi-line ''' block
    CodeClose,  // the line holding the closing ''' of a block
    Verbatim,   // inside a multi-line /* */ comment: left as written
};

struct Line {
    std::size_t begin = 0; // offset of the first character
    std::size_t end = 0;   // offset of the line break (or end of text)
    LineKind kind = LineKind::Json;
    std::size_t region = 0;      // for Code and CodeClose
    std::size_t json_depth = 0;  // brackets open at the line start (Json)
    std::size_t new_indent = 0;
};

std::vector<Line> split_lines(std::string_view text)
{
    std::vector<Line> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        Line line;
        line.begin = begin;
        line.end = (end > begin && text[end - 1] == '\r') ? end - 1 : end;
        lines.push_back(line);
        if (end == text.size()) {
            break;
        }
        begin = end + 1;
    }
    return lines;
}

std::string_view trimmed(std::string_view text, const Line& line)
{
    std::string_view content = text.substr(line.begin, line.end - line.begin);
    const std::size_t first = content.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = content.find_last_not_of(" \t");
    return content.substr(first, last - first + 1);
}

std::size_t leading_spaces(std::string_view text, const Line& line)
{
    std::size_t count = 0;
    while (line.begin + count < line.end && text[line.begin + count] == ' ') {
        ++count;
    }
    return count;
}

std::size_t line_of(const std::vector<Line>& lines, std::size_t offset)
{
    auto it = std::upper_bound(lines.begin(), lines.end(), offset,
                               [](std::size_t value, const Line& line) { return value < line.begin; });
    return static_cast<std::size_t>(it - lines.begin()) - 1;
}

void mark_code_lines(std::vector<Line>& lines, const std::vector<CodeRegion>& regions)
{
    for (std::size_t r = 0; r < regions.size(); ++r) {
        const CodeRegion& region = regions[r];
        if (!region.multiline) {
            continue;
        }
        const std::size_t first = line_of(lines, region.begin);
        const std::size_t close = line_of(lines, region.end);
        for (std::size_t i = first; i < close; ++i) {
            lines[i].kind = LineKind::Code;
            lines[i].region = r;
        }
        lines[close].kind = LineKind::CodeClose;
        lines[close].region = r;
    }
}

// Scans the JSON layer and records how many brackets are open at the start
// of each line. Strings, comments and ''' code are skipped.
class JsonDepthScanner {
public:
    JsonDepthScanner(std::string_view text, std::vector<Line>& lines)
        : text_(text)
        , lines_(lines)
    {
    }

    void run()
    {
        std::size_t line = 0;
        std::size_t depth = 0;
        std::size_t i = 0;
        while (i < text_.size()) {
            while (line + 1 < lines_.size() && lines_[line + 1].begin <= i) {
                ++line;
                lines_[line].json_depth = depth;
            }
            i = step(i, depth);
        }
    }

private:
    std::size_t step(std::size_t i, std::size_t& depth)
    {
        const char c = text_[i];
        if (text_.substr(i, 3) == "'''") {
            const std::size_t close = text_.find("'''", i + 3);
            return close == std::string_view::npos ? text_.size() : close + 3;
        }
        if (c == '"' || c == '\'') {
            return skip_quoted(i, c);
        }
        if (text_.substr(i, 2) == "//") {
            const std::size_t end = text_.find('\n', i);
            return end == std::string_view::npos ? text_.size() : end;
        }
        if (text_.substr(i, 2) == "/*") {
            return skip_block_comment(i);
        }
        if (c == '{' || c == '[' || c == '(') {
            ++depth;
        } else if ((c == '}' || c == ']' || c == ')') && depth > 0) {
            --depth;
        }
        return i + 1;
    }

    std::size_t skip_quoted(std::size_t i, char quote)
    {
        for (++i; i < text_.size() && text_[i] != '\n'; ++i) {
            if (text_[i] == '\\') {
                ++i;
            } else if (text_[i] == quote) {
                return i + 1;
            }
        }
        return i;
    }

    // Lines that start inside the comment are kept verbatim.
    std::size_t skip_block_comment(std::size_t i)
    {
        std::size_t end = text_.find("*/", i + 2);
        end = end == std::string_view::npos ? text_.size() : end + 2;
        for (Line& line : lines_) {
            if (line.begin > i && line.begin < end && line.kind == LineKind::Json) {
                line.kind = LineKind::Verbatim;
            }
        }
        return end;
    }

    std::string_view text_;
    std::vector<Line>& lines_;
};

bool starts_with_closer(std::string_view content)
{
    return !content.empty() && (content[0] == '}' || content[0] == ']' || content[0] == ')');
}

// Re-indents the lines of one multi-line block, mapping each logical line to
// its block level the same way the code lexer does. A comment-only line
// takes the indentation of the next code line, as in Python.
class BlockFormatter {
public:
    BlockFormatter(std::string_view text, std::size_t code_base)
        : text_(text)
        , code_base_(code_base)
    {
    }

    void format(Line& line)
    {
        const std::string_view content = trimmed(text_, line);
        const std::size_t original = leading_spaces(text_, line);
        if (content.empty()) {
            line.new_indent = 0;
            return;
        }
        if (bracket_depth_ > 0) {
            const std::size_t shifted = logical_new_ + original;
            line.new_indent = shifted >= logical_original_
                                  ? std::max(code_base_, shifted - logical_original_)
                                  : code_base_;
            scan_brackets(content);
            return;
        }
        if (content[0] == '#') {
            pending_comments_.push_back(&line);
            return;
        }
        apply_indentation(original);
        line.new_indent = code_base_ + kIndentWidth * (stack_.size() - 1);
        logical_original_ = original;
        logical_new_ = line.new_indent;
        place_pending_comments(line.new_indent);
        scan_brackets(content);
    }

    // Comments after the last code line of the block line up with it.
    void finish()
    {
        place_pending_comments(stack_.empty() ? code_base_ : logical_new_);
    }

private:
    void apply_indentation(std::size_t original)
    {
        if (stack_.empty() || original > stack_.back()) {
            stack_.push_back(original);
            return;
        }
        while (stack_.size() > 1 && original < stack_.back()) {
            stack_.pop_back();
        }
    }

    void place_pending_comments(std::size_t indent)
    {
        for (Line* comment : pending_comments_) {
            comment->new_indent = indent;
        }
        pending_comments_.clear();
    }

    void scan_brackets(std::string_view content)
    {
        for (std::size_t i = 0; i < content.size(); ++i) {
            const char c = content[i];
            if (c == '#') {
                return;
            }
            if (c == '"' || c == '\'') {
                for (++i; i < content.size() && content[i] != c; ++i) {
                    if (content[i] == '\\') {
                        ++i;
                    }
                }
            } else if (c == '(' || c == '[' || c == '{') {
                ++bracket_depth_;
            } else if ((c == ')' || c == ']' || c == '}') && bracket_depth_ > 0) {
                --bracket_depth_;
            }
        }
    }

    std::string_view text_;
    std::size_t code_base_;
    std::vector<std::size_t> stack_;
    std::size_t bracket_depth_ = 0;
    std::size_t logical_original_ = 0;
    std::size_t logical_new_ = 0;
    std::vector<Line*> pending_comments_;
};

void compute_json_indents(std::string_view text, std::vector<Line>& lines)
{
    for (Line& line : lines) {
        if (line.kind != LineKind::Json) {
            continue;
        }
        const std::string_view content = trimmed(text, line);
        std::size_t depth = line.json_depth;
        if (starts_with_closer(content) && depth > 0) {
            --depth;
        }
        line.new_indent = content.empty() ? 0 : kIndentWidth * depth;
    }
}

void compute_code_indents(std::string_view text, std::vector<Line>& lines,
                          const std::vector<CodeRegion>& regions)
{
    for (std::size_t r = 0; r < regions.size(); ++r) {
        if (!regions[r].multiline) {
            continue;
        }
        const std::size_t open_line = line_of(lines, regions[r].begin - 1);
        const std::size_t open_indent = lines[open_line].new_indent;
        BlockFormatter block(text, open_indent + kIndentWidth);
        for (Line& line : lines) {
            const bool in_block = line.kind == LineKind::Code || line.kind == LineKind::CodeClose;
            if (!in_block || line.region != r) {
                continue;
            }
            if (line.kind == LineKind::Code) {
                block.format(line);
            } else if (line.kind == LineKind::CodeClose) {
                line.new_indent = open_indent;
            }
        }
        block.finish();
    }
}

std::string assemble(std::string_view text, const std::vector<Line>& lines)
{
    std::string out;
    for (const Line& line : lines) {
        if (line.kind == LineKind::Verbatim) {
            std::string_view content = text.substr(line.begin, line.end - line.begin);
            const std::size_t last = content.find_last_not_of(" \t");
            out += last == std::string_view::npos ? std::string_view{} : content.substr(0, last + 1);
        } else {
            const std::string_view content = trimmed(text, line);
            if (!content.empty()) {
                out.append(line.new_indent, ' ');
                out += content;
            }
        }
        out += '\n';
    }
    while (out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') {
        out.pop_back();
    }
    if (out == "\n") {
        out.clear();
    }
    return out;
}

} // namespace

std::optional<std::string> format_source(const SourceFile& file, DiagnosticEngine& diagnostics)
{
    OuterParser parser(file, diagnostics);
    parser.parse();
    if (diagnostics.has_errors()) {
        return std::nullopt;
    }
    // One-line code stays inside its JSON line untouched; only multi-line
    // blocks are re-indented separately.
    const std::vector<CodeRegion>& regions = parser.code_regions();
    std::vector<Line> lines = split_lines(file.text);
    mark_code_lines(lines, regions);
    JsonDepthScanner(file.text, lines).run();
    compute_json_indents(file.text, lines);
    compute_code_indents(file.text, lines, regions);
    return assemble(file.text, lines);
}

} // namespace hana::lang
