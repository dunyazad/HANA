#include "lang/lexer.h"

#include <utility>

#include "support/utf8.h"

namespace hana::lang {

namespace {

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_identifier_start(char c)
{
    return is_alpha(c) || c == '_';
}

bool is_identifier_continue(char c)
{
    return is_identifier_start(c) || is_digit(c);
}

int digit_value(char c)
{
    if (is_digit(c)) {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 99;
}

struct KeywordEntry {
    std::string_view text;
    TokenKind kind;
};

constexpr KeywordEntry kKeywords[] = {
    {"fn", TokenKind::KwFn},
    {"let", TokenKind::KwLet},
    {"var", TokenKind::KwVar},
    {"return", TokenKind::KwReturn},
    {"if", TokenKind::KwIf},
    {"else", TokenKind::KwElse},
    {"while", TokenKind::KwWhile},
    {"loop", TokenKind::KwLoop},
    {"for", TokenKind::KwFor},
    {"in", TokenKind::KwIn},
    {"match", TokenKind::KwMatch},
    {"break", TokenKind::KwBreak},
    {"continue", TokenKind::KwContinue},
    {"struct", TokenKind::KwStruct},
    {"enum", TokenKind::KwEnum},
    {"trait", TokenKind::KwTrait},
    {"impl", TokenKind::KwImpl},
    {"pub", TokenKind::KwPub},
    {"import", TokenKind::KwImport},
    {"as", TokenKind::KwAs},
    {"unsafe", TokenKind::KwUnsafe},
    {"extern", TokenKind::KwExtern},
    {"true", TokenKind::KwTrue},
    {"false", TokenKind::KwFalse},
    {"inout", TokenKind::KwInout},
    {"sink", TokenKind::KwSink},
    {"bind", TokenKind::KwBind},
    {"yield", TokenKind::KwYield},
    {"subscript", TokenKind::KwSubscript},
};

TokenKind keyword_or_identifier(std::string_view text)
{
    for (const KeywordEntry& entry : kKeywords) {
        if (entry.text == text) {
            return entry.kind;
        }
    }
    return TokenKind::Identifier;
}

constexpr std::string_view kIntegerSuffixes[] = {
    "i8", "i16", "i32", "i64", "isize", "u8", "u16", "u32", "u64", "usize",
};

constexpr std::string_view kFloatSuffixes[] = {"f32", "f64"};

bool is_integer_suffix(std::string_view suffix)
{
    for (std::string_view candidate : kIntegerSuffixes) {
        if (candidate == suffix) {
            return true;
        }
    }
    return false;
}

bool is_float_suffix(std::string_view suffix)
{
    for (std::string_view candidate : kFloatSuffixes) {
        if (candidate == suffix) {
            return true;
        }
    }
    return false;
}

bool should_drop_newline(const std::vector<Token>& tokens)
{
    return tokens.empty() || tokens.back().kind == TokenKind::Newline;
}

// Tokens after which an expression or statement cannot end, so a following
// line break continues the current line.
bool is_line_continuation_token(TokenKind kind)
{
    switch (kind) {
    case TokenKind::Comma:
    case TokenKind::Colon:
    case TokenKind::Dot:
    case TokenKind::DotDot:
    case TokenKind::DotDotEq:
    case TokenKind::Arrow:
    case TokenKind::FatArrow:
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::Amp:
    case TokenKind::Pipe:
    case TokenKind::Caret:
    case TokenKind::Tilde:
    case TokenKind::Shl:
    case TokenKind::Shr:
    case TokenKind::Bang:
    case TokenKind::AmpAmp:
    case TokenKind::PipePipe:
    case TokenKind::Eq:
    case TokenKind::EqEq:
    case TokenKind::BangEq:
    case TokenKind::Lt:
    case TokenKind::LtEq:
    case TokenKind::Gt:
    case TokenKind::GtEq:
    case TokenKind::PlusEq:
    case TokenKind::MinusEq:
    case TokenKind::StarEq:
    case TokenKind::SlashEq:
    case TokenKind::PercentEq:
    case TokenKind::AmpEq:
    case TokenKind::PipeEq:
    case TokenKind::CaretEq:
    case TokenKind::ShlEq:
    case TokenKind::ShrEq:
        return true;
    default:
        return false;
    }
}

char matching_open_bracket(char close)
{
    switch (close) {
    case ')': return '(';
    case ']': return '[';
    default: return '{';
    }
}

} // namespace

Lexer::Lexer(std::string_view source, DiagnosticEngine& diagnostics)
    : source_(source)
    , diagnostics_(diagnostics)
{
    // Skip a UTF-8 byte order mark if present.
    if (source_.substr(0, 3) == "\xEF\xBB\xBF") {
        location_.offset = 3;
    }
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;
    for (;;) {
        Token token = next_token();
        if (token.kind == TokenKind::Newline &&
            (should_drop_newline(tokens) || continues_line(tokens))) {
            continue;
        }
        const bool done = token.kind == TokenKind::EndOfFile;
        tokens.push_back(std::move(token));
        if (done) {
            break;
        }
    }
    return tokens;
}

Token Lexer::next_token()
{
    skip_whitespace_and_comments();
    token_start_ = location_;

    if (at_end()) {
        return lex_end_of_file();
    }

    const char c = peek();
    if (c == '\n') {
        return lex_newline();
    }
    if (is_identifier_start(c)) {
        return lex_identifier();
    }
    if (is_digit(c)) {
        return lex_number();
    }
    if (c == '"') {
        advance();
        return lex_string_segment(true);
    }
    if (c == '\'') {
        advance();
        return lex_char();
    }
    if (c == '}' && closes_interpolation()) {
        advance();
        close_interpolation();
        return lex_string_segment(false);
    }
    return lex_punctuation();
}

Token Lexer::lex_end_of_file()
{
    if (!interpolation_stack_.empty()) {
        abandon_interpolations();
        diagnostics_.error(token_start_, "unterminated string interpolation at end of file");
    }
    return make_token(TokenKind::EndOfFile, "");
}

Token Lexer::lex_newline()
{
    if (!interpolation_stack_.empty()) {
        // Strings cannot span lines, so the open interpolation is broken.
        // Do not consume the newline; it is emitted on the next call.
        abandon_interpolations();
        return make_error("unterminated string interpolation");
    }
    advance();
    return make_token(TokenKind::Newline, "");
}

Token Lexer::lex_identifier()
{
    while (is_identifier_continue(peek())) {
        advance();
    }
    return make_lexeme_token(keyword_or_identifier(lexeme()));
}

Token Lexer::lex_number()
{
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B')) {
        advance();
        const char marker = advance();
        const int radix = (marker == 'x' || marker == 'X') ? 16 : 2;
        const bool has_digits = consume_radix_digits(radix);
        const std::size_t digits_end = location_.offset;
        Token token = finish_number(TokenKind::IntLiteral, digits_end);
        if (token.kind == TokenKind::FloatLiteral) {
            diagnostics_.error(token_start_, "float suffix on a hexadecimal or binary literal");
            token.kind = TokenKind::Error;
        } else if (!has_digits && token.kind != TokenKind::Error) {
            diagnostics_.error(token_start_, radix == 16 ? "expected digits after '0x'"
                                                         : "expected digits after '0b'");
            token.kind = TokenKind::Error;
        }
        return token;
    }

    consume_radix_digits(10);
    bool is_float = consume_decimal_part();
    is_float = consume_exponent() || is_float;
    return finish_number(is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral,
                         location_.offset);
}

Token Lexer::lex_char()
{
    std::string text;
    if (peek() == '\'') {
        advance();
        return make_error("empty character literal");
    }
    if (at_end() || peek() == '\n') {
        return make_error("unterminated character literal");
    }
    if (peek() == '\\') {
        advance();
        if (!read_escape_sequence(text)) {
            diagnostics_.error(location_, "invalid escape sequence in character literal");
        }
    } else {
        text += advance();
        while (!at_end() && is_utf8_continuation(peek())) {
            text += advance();
        }
    }
    if (match('\'')) {
        return make_token(TokenKind::CharLiteral, std::move(text));
    }
    // Recover by skipping to the closing quote on the same line.
    while (!at_end() && peek() != '\n' && peek() != '\'') {
        advance();
    }
    const bool closed = match('\'');
    return make_error(closed ? "character literal must contain exactly one character"
                             : "unterminated character literal");
}

Token Lexer::lex_string_segment(bool is_first_segment)
{
    std::string text;
    for (;;) {
        if (at_end() || peek() == '\n') {
            diagnostics_.error(token_start_, "unterminated string literal");
            return make_token(TokenKind::Error, std::move(text));
        }
        const char c = advance();
        if (c == '"') {
            return make_token(is_first_segment ? TokenKind::StringLiteral : TokenKind::InterpEnd,
                              std::move(text));
        }
        if (c == '{') {
            interpolation_stack_.push_back(bracket_stack_.size());
            return make_token(is_first_segment ? TokenKind::InterpStart : TokenKind::InterpMid,
                              std::move(text));
        }
        if (c == '\\') {
            const SourceLocation escape_location = location_;
            if (!read_escape_sequence(text)) {
                diagnostics_.error(escape_location, "invalid escape sequence in string literal");
            }
            continue;
        }
        text += c;
    }
}

Token Lexer::lex_punctuation()
{
    const char c = advance();
    switch (c) {
    case '(': track_open_bracket(c); return make_lexeme_token(TokenKind::LParen);
    case ')': track_close_bracket(c); return make_lexeme_token(TokenKind::RParen);
    case '[': track_open_bracket(c); return make_lexeme_token(TokenKind::LBracket);
    case ']': track_close_bracket(c); return make_lexeme_token(TokenKind::RBracket);
    case '{': track_open_bracket(c); return make_lexeme_token(TokenKind::LBrace);
    case '}': track_close_bracket(c); return make_lexeme_token(TokenKind::RBrace);
    case ',': return make_lexeme_token(TokenKind::Comma);
    case ':': return make_lexeme_token(TokenKind::Colon);
    case ';': return make_lexeme_token(TokenKind::Semicolon);
    case '?': return make_lexeme_token(TokenKind::Question);
    case '~': return make_lexeme_token(TokenKind::Tilde);
    case '.':
        if (match('.')) {
            return make_lexeme_token(match('=') ? TokenKind::DotDotEq : TokenKind::DotDot);
        }
        return make_lexeme_token(TokenKind::Dot);
    case '-':
        if (match('>')) {
            return make_lexeme_token(TokenKind::Arrow);
        }
        return make_with_assign(TokenKind::MinusEq, TokenKind::Minus);
    case '=':
        if (match('>')) {
            return make_lexeme_token(TokenKind::FatArrow);
        }
        return make_with_assign(TokenKind::EqEq, TokenKind::Eq);
    case '+': return make_with_assign(TokenKind::PlusEq, TokenKind::Plus);
    case '*': return make_with_assign(TokenKind::StarEq, TokenKind::Star);
    case '/': return make_with_assign(TokenKind::SlashEq, TokenKind::Slash);
    case '%': return make_with_assign(TokenKind::PercentEq, TokenKind::Percent);
    case '^': return make_with_assign(TokenKind::CaretEq, TokenKind::Caret);
    case '!': return make_with_assign(TokenKind::BangEq, TokenKind::Bang);
    case '&':
        if (match('&')) {
            return make_lexeme_token(TokenKind::AmpAmp);
        }
        return make_with_assign(TokenKind::AmpEq, TokenKind::Amp);
    case '|':
        if (match('|')) {
            return make_lexeme_token(TokenKind::PipePipe);
        }
        return make_with_assign(TokenKind::PipeEq, TokenKind::Pipe);
    case '<':
        if (match('<')) {
            return make_with_assign(TokenKind::ShlEq, TokenKind::Shl);
        }
        return make_with_assign(TokenKind::LtEq, TokenKind::Lt);
    case '>':
        if (match('>')) {
            return make_with_assign(TokenKind::ShrEq, TokenKind::Shr);
        }
        return make_with_assign(TokenKind::GtEq, TokenKind::Gt);
    default:
        break;
    }
    return lex_unexpected_character();
}

Token Lexer::lex_unexpected_character()
{
    // The first byte was already consumed; consume the rest of a UTF-8
    // sequence so the error covers one whole code point.
    while (!at_end() && is_utf8_continuation(peek())) {
        advance();
    }
    return make_error("unexpected character '" + std::string(lexeme()) + "'");
}

bool Lexer::consume_radix_digits(int radix)
{
    bool has_digits = false;
    for (;;) {
        const char c = peek();
        if (c == '_') {
            advance();
        } else if (digit_value(c) < radix) {
            advance();
            has_digits = true;
        } else {
            return has_digits;
        }
    }
}

bool Lexer::consume_decimal_part()
{
    // "1..2" is a range, so '.' starts a fraction only when a digit follows.
    if (peek() != '.' || !is_digit(peek(1))) {
        return false;
    }
    advance();
    consume_radix_digits(10);
    return true;
}

bool Lexer::consume_exponent()
{
    if (peek() != 'e' && peek() != 'E') {
        return false;
    }
    const bool signed_exponent = (peek(1) == '+' || peek(1) == '-') && is_digit(peek(2));
    if (!is_digit(peek(1)) && !signed_exponent) {
        return false;
    }
    advance();
    if (signed_exponent) {
        advance();
    }
    consume_radix_digits(10);
    return true;
}

Token Lexer::finish_number(TokenKind kind, std::size_t digits_end)
{
    while (is_identifier_continue(peek())) {
        advance();
    }
    const std::string_view digits =
        source_.substr(token_start_.offset, digits_end - token_start_.offset);
    const std::string_view suffix = source_.substr(digits_end, location_.offset - digits_end);

    Token token = make_token(kind, std::string(digits));
    if (suffix.empty()) {
        return token;
    }
    if (is_float_suffix(suffix)) {
        token.kind = TokenKind::FloatLiteral;
    } else if (!is_integer_suffix(suffix)) {
        return make_error("invalid suffix '" + std::string(suffix) + "' on numeric literal");
    } else if (kind == TokenKind::FloatLiteral) {
        return make_error("integer suffix '" + std::string(suffix) + "' on a float literal");
    }
    token.suffix = std::string(suffix);
    return token;
}

void Lexer::skip_whitespace_and_comments()
{
    for (;;) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            skip_line_comment();
        } else if (c == '/' && peek(1) == '*') {
            skip_block_comment();
        } else {
            return;
        }
    }
}

void Lexer::skip_line_comment()
{
    // Leave the newline in place so it is still emitted as a token.
    while (!at_end() && peek() != '\n') {
        advance();
    }
}

void Lexer::skip_block_comment()
{
    const SourceLocation start = location_;
    advance();
    advance();
    int depth = 1;
    while (depth > 0) {
        if (at_end()) {
            diagnostics_.error(start, "unterminated block comment");
            return;
        }
        if (peek() == '/' && peek(1) == '*') {
            advance();
            advance();
            ++depth;
        } else if (peek() == '*' && peek(1) == '/') {
            advance();
            advance();
            --depth;
        } else {
            advance();
        }
    }
}

bool Lexer::read_escape_sequence(std::string& out)
{
    if (at_end() || peek() == '\n') {
        return false;
    }
    const char c = advance();
    switch (c) {
    case 'n': out += '\n'; return true;
    case 't': out += '\t'; return true;
    case 'r': out += '\r'; return true;
    case '0': out += '\0'; return true;
    case '\\': out += '\\'; return true;
    case '"': out += '"'; return true;
    case '\'': out += '\''; return true;
    case '{': out += '{'; return true;
    case '}': out += '}'; return true;
    case 'u': return read_unicode_escape(out);
    default: break;
    }
    // Keep the unknown character so the decoded text stays readable.
    out += c;
    return false;
}

bool Lexer::read_unicode_escape(std::string& out)
{
    // Form: \u{XXXX} with 1 to 6 hexadecimal digits.
    if (!match('{')) {
        return false;
    }
    std::uint32_t value = 0;
    int count = 0;
    while (digit_value(peek()) < 16) {
        value = value * 16 + static_cast<std::uint32_t>(digit_value(advance()));
        ++count;
        if (count > 6) {
            return false;
        }
    }
    if (count == 0 || !match('}')) {
        return false;
    }
    return append_utf8(out, value);
}

bool Lexer::closes_interpolation() const
{
    // A '}' ends the innermost interpolation unless a '{' opened inside the
    // interpolated expression is still waiting for it.
    if (interpolation_stack_.empty()) {
        return false;
    }
    for (std::size_t i = interpolation_stack_.back(); i < bracket_stack_.size(); ++i) {
        if (bracket_stack_[i] == '{') {
            return false;
        }
    }
    return true;
}

void Lexer::close_interpolation()
{
    // Brackets left open inside the expression are dropped together with it.
    bracket_stack_.resize(interpolation_stack_.back());
    interpolation_stack_.pop_back();
}

void Lexer::abandon_interpolations()
{
    bracket_stack_.resize(interpolation_stack_.front());
    interpolation_stack_.clear();
}

void Lexer::track_open_bracket(char open)
{
    bracket_stack_.push_back(open);
}

void Lexer::track_close_bracket(char close)
{
    // Unbalanced brackets are reported by the parser. Here a closer pops back
    // to its matching opener, never past the start of the current string
    // interpolation, so one stray bracket does not disturb newline handling
    // for the rest of the file.
    const std::size_t floor = interpolation_stack_.empty() ? 0 : interpolation_stack_.back();
    const char open = matching_open_bracket(close);
    for (std::size_t i = bracket_stack_.size(); i > floor; --i) {
        if (bracket_stack_[i - 1] == open) {
            bracket_stack_.resize(i - 1);
            return;
        }
    }
}

bool Lexer::continues_line(const std::vector<Token>& tokens) const
{
    if (!bracket_stack_.empty() && bracket_stack_.back() != '{') {
        return true;
    }
    return !tokens.empty() && is_line_continuation_token(tokens.back().kind);
}

Token Lexer::make_token(TokenKind kind, std::string text) const
{
    Token token;
    token.kind = kind;
    token.text = std::move(text);
    token.location = token_start_;
    return token;
}

Token Lexer::make_lexeme_token(TokenKind kind) const
{
    return make_token(kind, std::string(lexeme()));
}

Token Lexer::make_with_assign(TokenKind with_eq, TokenKind plain)
{
    return make_lexeme_token(match('=') ? with_eq : plain);
}

Token Lexer::make_error(std::string message)
{
    diagnostics_.error(token_start_, std::move(message));
    return make_lexeme_token(TokenKind::Error);
}

std::string_view Lexer::lexeme() const
{
    return source_.substr(token_start_.offset, location_.offset - token_start_.offset);
}

bool Lexer::at_end() const
{
    return location_.offset >= source_.size();
}

char Lexer::peek(std::size_t ahead) const
{
    const std::size_t index = location_.offset + ahead;
    return index < source_.size() ? source_[index] : '\0';
}

char Lexer::advance()
{
    const char c = source_[location_.offset];
    ++location_.offset;
    if (c == '\n') {
        ++location_.line;
        location_.column = 1;
    } else if (!is_utf8_continuation(c)) {
        ++location_.column;
    }
    return c;
}

bool Lexer::match(char expected)
{
    if (at_end() || peek() != expected) {
        return false;
    }
    advance();
    return true;
}

} // namespace hana::lang
