#include "lang/code_lexer.h"

#include <utility>

#include "support/utf8.h"

namespace hana::lang {

namespace {

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_identifier_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
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
    {"let", TokenKind::KwLet},       {"var", TokenKind::KwVar},
    {"if", TokenKind::KwIf},         {"elif", TokenKind::KwElif},
    {"else", TokenKind::KwElse},     {"while", TokenKind::KwWhile},
    {"for", TokenKind::KwFor},       {"in", TokenKind::KwIn},
    {"match", TokenKind::KwMatch},   {"break", TokenKind::KwBreak},
    {"continue", TokenKind::KwContinue}, {"return", TokenKind::KwReturn},
    {"pass", TokenKind::KwPass},     {"and", TokenKind::KwAnd},
    {"or", TokenKind::KwOr},         {"not", TokenKind::KwNot},
    {"true", TokenKind::KwTrue},     {"false", TokenKind::KwFalse},
    {"null", TokenKind::KwNull},     {"as", TokenKind::KwAs},
    {"fn", TokenKind::KwFn},         {"inout", TokenKind::KwInout},
    {"sink", TokenKind::KwSink},     {"unsafe", TokenKind::KwUnsafe},
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
    return suffix == "f32" || suffix == "f64";
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

CodeLexer::CodeLexer(std::string_view source, const CodeRegion& region,
                     DiagnosticEngine& diagnostics)
    : source_(source)
    , region_(region)
    , diagnostics_(diagnostics)
    , location_(region.start)
    , at_line_start_(region.multiline)
{
}

std::vector<Token> CodeLexer::tokenize()
{
    for (;;) {
        if (at_line_start_ && bracket_stack_.empty()) {
            lex_line_start();
        }
        skip_spaces_and_comments();
        token_start_ = location_;
        if (at_end()) {
            break;
        }
        lex_token();
    }
    finish();
    return std::move(tokens_);
}

// ---------------------------------------------------------------------------
// Lines and indentation

void CodeLexer::lex_line_start()
{
    while (!at_end()) {
        const SourceLocation line_start = location_;
        std::uint32_t spaces = 0;
        bool reported_tab = false;
        while (peek() == ' ' || peek() == '\t') {
            if (peek() == '\t' && !reported_tab) {
                diagnostics_.error(location_, "tabs are not allowed for indentation; use spaces");
                reported_tab = true;
            }
            advance();
            ++spaces;
        }
        if (skip_blank_line()) {
            continue;
        }
        if (region_.multiline && spaces < region_.base_indent) {
            diagnostics_.error(line_start,
                               "this line is indented less than the closing ''' of its block");
        }
        const std::uint32_t indent = spaces >= region_.base_indent ? spaces - region_.base_indent : 0;
        apply_indentation(indent, location_);
        break;
    }
    at_line_start_ = false;
}

// Consumes the rest of a line that holds no code. Returns false when the
// line has code.
bool CodeLexer::skip_blank_line()
{
    if (peek() == '#') {
        while (!at_end() && peek() != '\n') {
            advance();
        }
    }
    if (at_end()) {
        return true;
    }
    if (peek() == '\r' && peek(1) == '\n') {
        advance();
    }
    if (peek() == '\n') {
        advance();
        return true;
    }
    return false;
}

// The first code line sets the block's base level, so code may be indented
// further than the closing ''' as long as it is consistent.
void CodeLexer::apply_indentation(std::uint32_t indent, const SourceLocation& location)
{
    token_start_ = location;
    if (!seen_code_line_) {
        seen_code_line_ = true;
        indent_stack_.front() = indent;
        return;
    }
    if (indent > indent_stack_.back()) {
        indent_stack_.push_back(indent);
        emit(TokenKind::Indent, "");
        return;
    }
    // The bottom entry is the block's base level and is never popped.
    while (indent_stack_.size() > 1 && indent < indent_stack_.back()) {
        indent_stack_.pop_back();
        emit(TokenKind::Dedent, "");
    }
    if (indent < indent_stack_.front()) {
        diagnostics_.error(location, "this line is indented less than the first line of its block");
        return;
    }
    if (indent != indent_stack_.back()) {
        diagnostics_.error(location, "this line's indentation does not match any outer block");
        indent_stack_.push_back(indent);
        emit(TokenKind::Indent, "");
    }
}

void CodeLexer::finish()
{
    token_start_ = location_;
    if (!interpolation_stack_.empty()) {
        diagnostics_.error(location_, "unterminated string interpolation");
    }
    if (!tokens_.empty() && tokens_.back().kind != TokenKind::Newline) {
        emit(TokenKind::Newline, "");
    }
    while (indent_stack_.size() > 1) {
        indent_stack_.pop_back();
        emit(TokenKind::Dedent, "");
    }
    emit(TokenKind::EndOfInput, "");
}

// ---------------------------------------------------------------------------
// Tokens

void CodeLexer::lex_token()
{
    const char c = peek();
    if (c == '\n' || (c == '\r' && peek(1) == '\n')) {
        lex_newline();
    } else if (c == 'f' && peek(1) == '"') {
        advance();
        advance();
        lex_string(true);
    } else if (is_identifier_start(c)) {
        lex_identifier();
    } else if (is_digit(c)) {
        lex_number();
    } else if (c == '"') {
        advance();
        lex_string(false);
    } else if (c == '\'') {
        advance();
        lex_char();
    } else if (c == '}' && closes_interpolation()) {
        advance();
        bracket_stack_.resize(interpolation_stack_.back());
        interpolation_stack_.pop_back();
        lex_string_segment(true, false);
    } else {
        lex_punctuation();
    }
}

void CodeLexer::lex_newline()
{
    if (!interpolation_stack_.empty()) {
        // Strings end on their line, so the open interpolation is broken.
        bracket_stack_.resize(interpolation_stack_.front());
        interpolation_stack_.clear();
        emit_error("unterminated string interpolation");
        return;
    }
    if (peek() == '\r') {
        advance();
    }
    advance();
    if (!bracket_stack_.empty()) {
        return; // implicit line joining inside brackets
    }
    if (!tokens_.empty() && tokens_.back().kind != TokenKind::Newline &&
        tokens_.back().kind != TokenKind::Indent && tokens_.back().kind != TokenKind::Dedent) {
        emit(TokenKind::Newline, "");
    }
    at_line_start_ = region_.multiline;
}

void CodeLexer::lex_identifier()
{
    while (is_identifier_continue(peek())) {
        advance();
    }
    emit_lexeme(keyword_or_identifier(lexeme()));
}

void CodeLexer::lex_number()
{
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B')) {
        advance();
        const char marker = advance();
        const int radix = (marker == 'x' || marker == 'X') ? 16 : 2;
        if (!consume_radix_digits(radix)) {
            while (is_identifier_continue(peek())) {
                advance();
            }
            emit_error(radix == 16 ? "expected digits after '0x'" : "expected digits after '0b'");
            return;
        }
        finish_number(TokenKind::IntLiteral, location_.offset, radix);
        return;
    }
    consume_radix_digits(10);
    // After '.', a number is a position as in 'pair.0.1', never '0.1'.
    if (follows_dot()) {
        finish_number(TokenKind::IntLiteral, location_.offset, 10);
        return;
    }
    bool is_float = consume_decimal_part();
    is_float = consume_exponent() || is_float;
    finish_number(is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral, location_.offset, 10);
}

bool CodeLexer::follows_dot() const
{
    return !tokens_.empty() && tokens_.back().kind == TokenKind::Dot &&
           tokens_.back().location.offset + 1 == token_start_.offset;
}

void CodeLexer::finish_number(TokenKind kind, std::size_t digits_end, int radix)
{
    while (is_identifier_continue(peek())) {
        advance();
    }
    const std::string_view digits =
        source_.substr(token_start_.offset, digits_end - token_start_.offset);
    const std::string_view suffix = source_.substr(digits_end, location_.offset - digits_end);
    if (suffix.empty()) {
        emit(kind, std::string(digits));
        return;
    }
    if (is_float_suffix(suffix)) {
        if (radix != 10) {
            emit_error("a float suffix cannot follow a hexadecimal or binary literal");
            return;
        }
        kind = TokenKind::FloatLiteral;
    } else if (!is_integer_suffix(suffix)) {
        emit_error("invalid suffix '" + std::string(suffix) + "' on a number");
        return;
    } else if (kind == TokenKind::FloatLiteral) {
        emit_error("integer suffix '" + std::string(suffix) + "' on a float literal");
        return;
    }
    emit(kind, std::string(digits));
    tokens_.back().suffix = std::string(suffix);
}

void CodeLexer::lex_char()
{
    std::string text;
    if (peek() == '\'') {
        advance();
        emit_error("empty character literal");
        return;
    }
    if (at_end() || peek() == '\n') {
        emit_error("unterminated character literal");
        return;
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
        emit(TokenKind::CharLiteral, std::move(text));
        return;
    }
    while (!at_end() && peek() != '\n' && peek() != '\'') {
        advance();
    }
    const bool closed = match('\'');
    emit_error(closed ? "a character literal holds exactly one character; use \"...\" for strings"
                      : "unterminated character literal");
}

void CodeLexer::lex_string(bool is_format)
{
    lex_string_segment(is_format, true);
}

void CodeLexer::lex_string_segment(bool is_format, bool is_first_segment)
{
    std::string text;
    for (;;) {
        if (at_end() || peek() == '\n') {
            emit_error("unterminated string literal");
            return;
        }
        const char c = advance();
        if (c == '"') {
            TokenKind kind = TokenKind::StringLiteral;
            if (is_format) {
                kind = is_first_segment ? TokenKind::FStringWhole : TokenKind::FStringEnd;
            }
            emit(kind, std::move(text));
            return;
        }
        if (c == '{' && is_format) {
            interpolation_stack_.push_back(bracket_stack_.size());
            emit(is_first_segment ? TokenKind::FStringStart : TokenKind::FStringMid, std::move(text));
            return;
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

void CodeLexer::lex_punctuation()
{
    const char c = advance();
    switch (c) {
    case '(':
    case '[':
    case '{':
        bracket_stack_.push_back(c);
        emit_lexeme(c == '(' ? TokenKind::LParen : c == '[' ? TokenKind::LBracket : TokenKind::LBrace);
        return;
    case ')':
    case ']':
    case '}':
        track_close_bracket(c);
        emit_lexeme(c == ')' ? TokenKind::RParen : c == ']' ? TokenKind::RBracket : TokenKind::RBrace);
        return;
    case ',': emit_lexeme(TokenKind::Comma); return;
    case ':': emit_lexeme(TokenKind::Colon); return;
    case '?': emit_lexeme(TokenKind::Question); return;
    case '~': emit_lexeme(TokenKind::Tilde); return;
    case '.':
        if (match('.')) {
            emit_lexeme(match('=') ? TokenKind::DotDotEq : TokenKind::DotDot);
        } else {
            emit_lexeme(TokenKind::Dot);
        }
        return;
    case '-':
        if (match('>')) {
            emit_lexeme(TokenKind::Arrow);
            return;
        }
        emit_with_assign(TokenKind::MinusEq, TokenKind::Minus);
        return;
    case '=': emit_with_assign(TokenKind::EqEq, TokenKind::Eq); return;
    case '+': emit_with_assign(TokenKind::PlusEq, TokenKind::Plus); return;
    case '*': emit_with_assign(TokenKind::StarEq, TokenKind::Star); return;
    case '/': emit_with_assign(TokenKind::SlashEq, TokenKind::Slash); return;
    case '%': emit_with_assign(TokenKind::PercentEq, TokenKind::Percent); return;
    case '^': emit_with_assign(TokenKind::CaretEq, TokenKind::Caret); return;
    case '&': emit_with_assign(TokenKind::AmpEq, TokenKind::Amp); return;
    case '|': emit_with_assign(TokenKind::PipeEq, TokenKind::Pipe); return;
    case '!':
        if (match('=')) {
            emit_lexeme(TokenKind::BangEq);
        } else {
            emit_error("'!' is not an operator in HANA; use 'not'");
        }
        return;
    case '<':
        if (match('<')) {
            emit_with_assign(TokenKind::ShlEq, TokenKind::Shl);
        } else {
            emit_with_assign(TokenKind::LtEq, TokenKind::Lt);
        }
        return;
    case '>':
        if (match('>')) {
            emit_with_assign(TokenKind::ShrEq, TokenKind::Shr);
        } else {
            emit_with_assign(TokenKind::GtEq, TokenKind::Gt);
        }
        return;
    default: break;
    }
    lex_unexpected_character();
}

void CodeLexer::lex_unexpected_character()
{
    while (!at_end() && is_utf8_continuation(peek())) {
        advance();
    }
    emit_error("unexpected character '" + std::string(lexeme()) + "'");
}

// ---------------------------------------------------------------------------
// Number parts

bool CodeLexer::consume_radix_digits(int radix)
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

bool CodeLexer::consume_decimal_part()
{
    // "1..2" is a range, so '.' starts a fraction only when a digit follows.
    if (peek() != '.' || !is_digit(peek(1))) {
        return false;
    }
    advance();
    consume_radix_digits(10);
    return true;
}

bool CodeLexer::consume_exponent()
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

// ---------------------------------------------------------------------------
// Whitespace, comments, escapes

void CodeLexer::skip_spaces_and_comments()
{
    for (;;) {
        const char c = peek();
        if (c == ' ' || c == '\t' || (c == '\r' && peek(1) != '\n')) {
            advance();
        } else if (c == '#') {
            while (!at_end() && peek() != '\n') {
                advance();
            }
        } else {
            return;
        }
    }
}

// Escapes: JSON's \" \\ \/ \b \f \n \r \t \uXXXX, plus \0 \' \{ \} \u{X..}.
bool CodeLexer::read_escape_sequence(std::string& out)
{
    if (at_end() || peek() == '\n') {
        return false;
    }
    const char c = advance();
    switch (c) {
    case 'n': out += '\n'; return true;
    case 't': out += '\t'; return true;
    case 'r': out += '\r'; return true;
    case 'b': out += '\b'; return true;
    case 'f': out += '\f'; return true;
    case '0': out += '\0'; return true;
    case '\\': out += '\\'; return true;
    case '/': out += '/'; return true;
    case '"': out += '"'; return true;
    case '\'': out += '\''; return true;
    case '{': out += '{'; return true;
    case '}': out += '}'; return true;
    case 'u': return peek() == '{' ? read_braced_unicode_escape(out) : read_json_unicode_escape(out);
    default: break;
    }
    out += c;
    return false;
}

bool CodeLexer::read_braced_unicode_escape(std::string& out)
{
    advance(); // '{'
    std::uint32_t value = 0;
    int count = 0;
    while (digit_value(peek()) < 16) {
        value = value * 16 + static_cast<std::uint32_t>(digit_value(advance()));
        if (++count > 6) {
            return false;
        }
    }
    if (count == 0 || !match('}')) {
        return false;
    }
    return append_utf8(out, value);
}

// JSON form \uXXXX, including a surrogate pair written as two escapes.
bool CodeLexer::read_json_unicode_escape(std::string& out)
{
    std::uint32_t value = 0;
    if (!read_hex_digits(4, value)) {
        return false;
    }
    if (value >= 0xD800 && value <= 0xDBFF) {
        std::uint32_t low = 0;
        if (peek() != '\\' || peek(1) != 'u') {
            return false;
        }
        advance();
        advance();
        if (!read_hex_digits(4, low) || low < 0xDC00 || low > 0xDFFF) {
            return false;
        }
        value = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
    }
    return append_utf8(out, value);
}

bool CodeLexer::read_hex_digits(int count, std::uint32_t& value)
{
    value = 0;
    for (int i = 0; i < count; ++i) {
        if (digit_value(peek()) >= 16) {
            return false;
        }
        value = value * 16 + static_cast<std::uint32_t>(digit_value(advance()));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Brackets and interpolation

bool CodeLexer::closes_interpolation() const
{
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

void CodeLexer::track_close_bracket(char close)
{
    // Unbalanced brackets are reported by the parser. A closer pops back to
    // its opener, never past the start of the current interpolation.
    const std::size_t floor = interpolation_stack_.empty() ? 0 : interpolation_stack_.back();
    const char open = matching_open_bracket(close);
    for (std::size_t i = bracket_stack_.size(); i > floor; --i) {
        if (bracket_stack_[i - 1] == open) {
            bracket_stack_.resize(i - 1);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Emitting and reading characters

void CodeLexer::emit(TokenKind kind, std::string text)
{
    Token token;
    token.kind = kind;
    token.text = std::move(text);
    token.location = token_start_;
    tokens_.push_back(std::move(token));
}

void CodeLexer::emit_lexeme(TokenKind kind)
{
    emit(kind, std::string(lexeme()));
}

void CodeLexer::emit_with_assign(TokenKind with_eq, TokenKind plain)
{
    emit_lexeme(match('=') ? with_eq : plain);
}

void CodeLexer::emit_error(const std::string& message)
{
    diagnostics_.error(token_start_, message);
    emit_lexeme(TokenKind::Error);
}

std::string_view CodeLexer::lexeme() const
{
    return source_.substr(token_start_.offset, location_.offset - token_start_.offset);
}

bool CodeLexer::at_end() const
{
    return location_.offset >= region_.end;
}

char CodeLexer::peek(std::size_t ahead) const
{
    const std::size_t index = location_.offset + ahead;
    return index < region_.end ? source_[index] : '\0';
}

char CodeLexer::advance()
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

bool CodeLexer::match(char expected)
{
    if (at_end() || peek() != expected) {
        return false;
    }
    advance();
    return true;
}

} // namespace hana::lang
