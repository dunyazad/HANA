#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "support/source.h"

namespace hana::lang {

// Tokens of the code language used inside function bodies and other code
// values. The JSON layer around it is read by OuterParser directly.
enum class TokenKind : std::uint8_t {
    EndOfInput,
    Newline,
    Indent,
    Dedent,
    Error,

    Identifier,
    IntLiteral,
    FloatLiteral,
    CharLiteral,
    StringLiteral,

    // f"a{x}b{y}c" is lexed as FStringStart("a") <x> FStringMid("b") <y> FStringEnd("c").
    // A format string without any interpolation, f"plain", is FStringWhole("plain").
    FStringStart,
    FStringMid,
    FStringEnd,
    FStringWhole,

    KwLet,
    KwVar,
    KwIf,
    KwElif,
    KwElse,
    KwWhile,
    KwFor,
    KwIn,
    KwMatch,
    KwBreak,
    KwContinue,
    KwReturn,
    KwPass,
    KwAnd,
    KwOr,
    KwNot,
    KwTrue,
    KwFalse,
    KwNull,
    KwAs,
    KwFn,
    KwInout,
    KwSink,
    KwUnsafe,

    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Comma,
    Colon,
    Dot,
    DotDot,
    DotDotEq,
    Arrow,
    Question,

    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Shl,
    Shr,

    Eq,
    EqEq,
    BangEq,
    Lt,
    LtEq,
    Gt,
    GtEq,

    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,
    AmpEq,
    PipeEq,
    CaretEq,
    ShlEq,
    ShrEq,
};

struct Token {
    TokenKind kind = TokenKind::EndOfInput;
    // Raw lexeme for identifiers, numbers and operators. Decoded text
    // (escapes resolved) for string segments and character literals.
    std::string text;
    // Type suffix of a numeric literal ("u8", "f32"), empty if none.
    std::string suffix;
    SourceLocation location;
};

std::string_view token_kind_name(TokenKind kind);

// Text used in diagnostics, for example "'let'" or "end of line".
std::string describe_token(const Token& token);

// One token per line: "line:column  Kind  'text'".
std::string dump_tokens(const std::vector<Token>& tokens);

} // namespace hana::lang
