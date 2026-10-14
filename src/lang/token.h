#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lang/diagnostic.h"

namespace hana::lang {

enum class TokenKind : std::uint8_t {
    EndOfFile,
    Newline,
    Error,

    Identifier,
    IntLiteral,
    FloatLiteral,
    SizeLiteral,   // 300x200, 1x0.6
    StringLiteral, // "plain text" without interpolation

    // Interpolated string "a{x}b{y}c" is lexed as:
    // InterpStart("a") <tokens of x> InterpMid("b") <tokens of y> InterpEnd("c")
    InterpStart,
    InterpMid,
    InterpEnd,

    KwApp,
    KwComponent,
    KwScene,
    KwState,
    KwAction,
    KwIf,
    KwElse,
    KwFor,
    KwIn,
    KwTrue,
    KwFalse,

    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Colon,
    Semicolon,
    Dot,

    Plus,
    Minus,
    Star,
    Slash,
    Percent,

    Assign,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AndAnd,
    OrOr,
    Bang,
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    // Raw lexeme for identifiers, numbers and operators.
    // Decoded text (escapes resolved) for string segments.
    std::string text;
    SourceLocation location;
};

std::string_view token_kind_name(TokenKind kind);

// One token per line: "line:column  Kind  'text'".
std::string dump_tokens(const std::vector<Token>& tokens);

} // namespace hana::lang
