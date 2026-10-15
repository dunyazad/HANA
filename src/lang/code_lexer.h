#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lang/token.h"
#include "support/diagnostic.h"

namespace hana::lang {

// A piece of source text that holds code: the inside of a ''' block or a
// one-line code value such as 'count > 0'.
struct CodeRegion {
    std::uint32_t begin = 0; // byte offset of the first character
    std::uint32_t end = 0;   // byte offset one past the last character
    SourceLocation start;    // location of 'begin'
    // Multi-line ''' blocks: every line starts with this many spaces, taken
    // from the indentation of the closing '''. They are not part of the code.
    std::uint32_t base_indent = 0;
    bool multiline = false;
};

// Tokenizes one code region.
//
// Multi-line regions use Python-style indentation: Indent and Dedent tokens
// mark blocks relative to the first code line, which may be indented further
// than the closing ''' (a line indented less than the closing ''' is an
// error). A Newline token ends each logical line. Line breaks inside
// '(', '[' and '{' do not end the line. Blank lines and lines holding only a
// '#' comment are ignored. Tabs in indentation are an error.
// The lexer never stops on an error: it reports a diagnostic, emits an Error
// token and continues.
class CodeLexer {
public:
    CodeLexer(std::string_view source, const CodeRegion& region, DiagnosticEngine& diagnostics);

    std::vector<Token> tokenize();

private:
    void lex_line_start();
    bool skip_blank_line();
    void apply_indentation(std::uint32_t indent, const SourceLocation& location);
    void lex_token();
    void lex_newline();
    void lex_identifier();
    void lex_number();
    void lex_char();
    void lex_string(bool is_format);
    void lex_string_segment(bool is_format, bool is_first_segment);
    void lex_punctuation();
    void lex_unexpected_character();
    void finish();

    bool consume_radix_digits(int radix);
    bool consume_decimal_part();
    bool consume_exponent();
    void finish_number(TokenKind kind, std::size_t digits_end, int radix);

    void skip_spaces_and_comments();
    bool read_escape_sequence(std::string& out);
    bool read_braced_unicode_escape(std::string& out);
    bool read_json_unicode_escape(std::string& out);
    bool read_hex_digits(int count, std::uint32_t& value);

    bool closes_interpolation() const;
    void track_close_bracket(char close);

    void emit(TokenKind kind, std::string text);
    void emit_lexeme(TokenKind kind);
    void emit_with_assign(TokenKind with_eq, TokenKind plain);
    void emit_error(const std::string& message);
    std::string_view lexeme() const;

    bool at_end() const;
    char peek(std::size_t ahead = 0) const;
    char advance();
    bool match(char expected);

    std::string_view source_;
    CodeRegion region_;
    DiagnosticEngine& diagnostics_;
    SourceLocation location_;
    SourceLocation token_start_;
    bool at_line_start_ = false;
    bool seen_code_line_ = false;
    std::vector<std::uint32_t> indent_stack_{0};
    std::vector<char> bracket_stack_;
    // One entry per open f-string interpolation: the size of bracket_stack_
    // when it started.
    std::vector<std::size_t> interpolation_stack_;
    std::vector<Token> tokens_;
};

} // namespace hana::lang
