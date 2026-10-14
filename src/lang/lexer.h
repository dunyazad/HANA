#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lang/diagnostic.h"
#include "lang/token.h"

namespace hana::lang {

// Converts HANA source text (UTF-8) into a flat token stream.
//
// Comments: "// ..." to end of line, "/* ... */" block (may nest).
// Newlines are significant and emitted as a single Newline token for any run
// of line breaks; the parser decides where they matter.
// The lexer never stops on an error: it records a Diagnostic, emits an Error
// token and continues, so all problems in a file are reported at once.
class Lexer {
public:
    explicit Lexer(std::string_view source);

    std::vector<Token> tokenize();
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

private:
    Token next_token();
    Token lex_end_of_file();
    Token lex_newline();
    Token lex_identifier();
    Token lex_number();
    Token lex_string_segment(bool is_first_segment);
    Token lex_punctuation();
    Token lex_unexpected_character();

    void skip_whitespace_and_comments();
    void skip_line_comment();
    void skip_block_comment();
    bool consume_digits_and_fraction();
    bool read_escape_sequence(std::string& out);
    bool closes_interpolation() const;
    void track_brace_open();
    void track_brace_close();

    Token make_token(TokenKind kind, std::string text) const;
    Token make_lexeme_token(TokenKind kind) const;
    Token make_one_or_two(char second, TokenKind two_char_kind, TokenKind one_char_kind);
    Token make_error(std::string message);
    void report(const SourceLocation& location, std::string message);

    bool at_end() const;
    char peek(std::size_t ahead = 0) const;
    char advance();
    bool match(char expected);

    std::string_view source_;
    SourceLocation location_;
    SourceLocation token_start_;
    // One entry per open string interpolation; value is the number of '{'
    // opened inside the interpolated expression and not yet closed.
    std::vector<int> interpolation_stack_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace hana::lang
