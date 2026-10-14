#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "support/source.h"

namespace hana::lang {

enum class TokenKind : std::uint8_t {
    EndOfFile,
    Newline,
    Error,

    Identifier,
    IntLiteral,
    FloatLiteral,
    CharLiteral,
    StringLiteral,

    // "a{x}b{y}c" is lexed as InterpStart("a") <x> InterpMid("b") <y> InterpEnd("c").
    InterpStart,
    InterpMid,
    InterpEnd,

    KwFn,
    KwLet,
    KwVar,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwLoop,
    KwFor,
    KwIn,
    KwMatch,
    KwBreak,
    KwContinue,
    KwStruct,
    KwEnum,
    KwTrait,
    KwImpl,
    KwPub,
    KwImport,
    KwAs,
    KwUnsafe,
    KwExtern,
    KwTrue,
    KwFalse,
    KwInout,
    KwSink,
    KwBind,
    KwYield,
    KwSubscript,

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
    DotDot,
    DotDotEq,
    Arrow,
    FatArrow,
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
    Bang,
    AmpAmp,
    PipePipe,

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
    TokenKind kind = TokenKind::EndOfFile;
    // Raw lexeme for identifiers, numbers and operators. Decoded text
    // (escapes resolved) for string segments and character literals.
    std::string text;
    // Type suffix of a numeric literal ("u8", "f32"), empty if none.
    std::string suffix;
    SourceLocation location;
};

std::string_view token_kind_name(TokenKind kind);

// Text used in diagnostics, for example "'fn'" or "end of file".
std::string describe_token(const Token& token);

// One token per line: "line:column  Kind  'text'".
std::string dump_tokens(const std::vector<Token>& tokens);

} // namespace hana::lang
