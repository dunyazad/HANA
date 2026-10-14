#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lang/token.h"
#include "support/diagnostic.h"

namespace hana::lang {

// Converts HANA source text (UTF-8) into a flat token stream.
//
// Comments: "// ..." to end of line, "/* ... */" block (may nest).
// Statements end at a newline or ';'. A run of line breaks becomes a single
// Newline token. No Newline is emitted when the line continues:
//   - inside an open '(' or '[' (the innermost open bracket decides)
//   - after a token that cannot end an expression (binary operator, prefix
//     operator, ',', ':', '.', '->', '=>')
// String literals must end on the line where they start.
// The lexer never stops on an error: it reports a diagnostic, emits an Error
// token and continues.
class Lexer {
public:
    Lexer(std::string_view source, DiagnosticEngine& diagnostics);

    std::vector<Token> tokenize();

private:
    Token next_token();
    Token lex_end_of_file();
    Token lex_newline();
    Token lex_identifier();
    Token lex_number();
    Token lex_char();
    Token lex_string_segment(bool is_first_segment);
    Token lex_punctuation();
    Token lex_unexpected_character();

    bool consume_radix_digits(int radix);
    bool consume_decimal_part();
    bool consume_exponent();
    Token finish_number(TokenKind kind, std::size_t digits_end);

    void skip_whitespace_and_comments();
    void skip_line_comment();
    void skip_block_comment();
    bool read_escape_sequence(std::string& out);
    bool read_unicode_escape(std::string& out);

    bool closes_interpolation() const;
    void close_interpolation();
    void abandon_interpolations();
    void track_open_bracket(char open);
    void track_close_bracket(char close);
    bool continues_line(const std::vector<Token>& tokens) const;

    Token make_token(TokenKind kind, std::string text) const;
    Token make_lexeme_token(TokenKind kind) const;
    Token make_with_assign(TokenKind with_eq, TokenKind plain);
    Token make_error(std::string message);
    std::string_view lexeme() const;

    bool at_end() const;
    char peek(std::size_t ahead = 0) const;
    char advance();
    bool match(char expected);

    std::string_view source_;
    DiagnosticEngine& diagnostics_;
    SourceLocation location_;
    SourceLocation token_start_;
    // Currently open '(', '[' and '{' in source order.
    std::vector<char> bracket_stack_;
    // One entry per open string interpolation: the size of bracket_stack_
    // when the interpolation started.
    std::vector<std::size_t> interpolation_stack_;
};

} // namespace hana::lang
