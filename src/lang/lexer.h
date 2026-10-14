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
// Statements end at a newline or ';'. A run of line breaks becomes a single
// Newline token. No Newline is emitted when the line continues:
//   - inside an open '(' or '[' (the innermost open bracket decides)
//   - after a token that cannot end an expression (binary operator, '!',
//     ',', ':', '.')
// String literals must end on the line where they start.
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
    Token lex_color();
    Token lex_string_segment(bool is_first_segment);
    Token lex_punctuation();
    Token lex_unexpected_character();

    void skip_whitespace_and_comments();
    void skip_line_comment();
    void skip_block_comment();
    bool consume_digits_and_fraction();
    bool read_escape_sequence(std::string& out);
    bool closes_interpolation() const;
    void close_interpolation();
    void abandon_interpolations();
    void track_open_bracket(char open);
    void track_close_bracket(char close);
    bool continues_line(const std::vector<Token>& tokens) const;

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
    // Currently open '(', '[' and '{' in source order.
    std::vector<char> bracket_stack_;
    // One entry per open string interpolation: the size of bracket_stack_
    // when the interpolation started.
    std::vector<std::size_t> interpolation_stack_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace hana::lang
