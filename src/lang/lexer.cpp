#include "lang/lexer.h"

#include <utility>

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

bool is_utf8_continuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

struct KeywordEntry {
    std::string_view text;
    TokenKind kind;
};

constexpr KeywordEntry kKeywords[] = {
    {"app", TokenKind::KwApp},
    {"component", TokenKind::KwComponent},
    {"scene", TokenKind::KwScene},
    {"state", TokenKind::KwState},
    {"action", TokenKind::KwAction},
    {"if", TokenKind::KwIf},
    {"else", TokenKind::KwElse},
    {"for", TokenKind::KwFor},
    {"in", TokenKind::KwIn},
    {"true", TokenKind::KwTrue},
    {"false", TokenKind::KwFalse},
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
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::Assign:
    case TokenKind::Equal:
    case TokenKind::NotEqual:
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
    case TokenKind::AndAnd:
    case TokenKind::OrOr:
    case TokenKind::Bang:
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

Lexer::Lexer(std::string_view source)
    : source_(source)
{
    // Skip a UTF-8 byte order mark if present.
    if (source_.size() >= 3 && source_.substr(0, 3) == "\xEF\xBB\xBF") {
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
        report(token_start_, "unterminated string interpolation at end of file");
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
    const std::string_view text =
        source_.substr(token_start_.offset, location_.offset - token_start_.offset);
    return make_token(keyword_or_identifier(text), std::string(text));
}

Token Lexer::lex_number()
{
    const bool is_float = consume_digits_and_fraction();
    TokenKind kind = is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral;

    if (peek() == 'x' && is_digit(peek(1))) {
        advance();
        consume_digits_and_fraction();
        kind = TokenKind::SizeLiteral;
    }

    if (is_identifier_continue(peek())) {
        while (is_identifier_continue(peek()) || peek() == '.') {
            advance();
        }
        return make_error("invalid numeric literal");
    }
    return make_lexeme_token(kind);
}

Token Lexer::lex_string_segment(bool is_first_segment)
{
    std::string text;
    for (;;) {
        if (at_end() || peek() == '\n') {
            report(token_start_, "unterminated string literal");
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
            if (!read_escape_sequence(text)) {
                report(location_, "invalid escape sequence in string literal");
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
    case '.': return make_lexeme_token(TokenKind::Dot);
    case '+': return make_lexeme_token(TokenKind::Plus);
    case '-': return make_lexeme_token(TokenKind::Minus);
    case '*': return make_lexeme_token(TokenKind::Star);
    case '/': return make_lexeme_token(TokenKind::Slash);
    case '%': return make_lexeme_token(TokenKind::Percent);
    case '=': return make_one_or_two('=', TokenKind::Equal, TokenKind::Assign);
    case '!': return make_one_or_two('=', TokenKind::NotEqual, TokenKind::Bang);
    case '<': return make_one_or_two('=', TokenKind::LessEqual, TokenKind::Less);
    case '>': return make_one_or_two('=', TokenKind::GreaterEqual, TokenKind::Greater);
    case '&':
        if (match('&')) {
            return make_lexeme_token(TokenKind::AndAnd);
        }
        return make_error("expected '&&'");
    case '|':
        if (match('|')) {
            return make_lexeme_token(TokenKind::OrOr);
        }
        return make_error("expected '||'");
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
    const std::string_view text =
        source_.substr(token_start_.offset, location_.offset - token_start_.offset);
    return make_error("unexpected character '" + std::string(text) + "'");
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
            report(start, "unterminated block comment");
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

bool Lexer::consume_digits_and_fraction()
{
    while (is_digit(peek())) {
        advance();
    }
    if (peek() == '.' && is_digit(peek(1))) {
        advance();
        while (is_digit(peek())) {
            advance();
        }
        return true;
    }
    return false;
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
    case '{': out += '{'; return true;
    case '}': out += '}'; return true;
    default: break;
    }
    // Keep the unknown character so the decoded text stays readable.
    out += c;
    return false;
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
    const std::string_view text =
        source_.substr(token_start_.offset, location_.offset - token_start_.offset);
    return make_token(kind, std::string(text));
}

Token Lexer::make_one_or_two(char second, TokenKind two_char_kind, TokenKind one_char_kind)
{
    return make_lexeme_token(match(second) ? two_char_kind : one_char_kind);
}

Token Lexer::make_error(std::string message)
{
    report(token_start_, std::move(message));
    return make_lexeme_token(TokenKind::Error);
}

void Lexer::report(const SourceLocation& location, std::string message)
{
    diagnostics_.push_back(Diagnostic{location, std::move(message)});
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
