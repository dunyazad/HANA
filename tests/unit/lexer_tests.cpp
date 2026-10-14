#include "helpers.h"
#include "test.h"

using namespace hana::test;
using hana::lang::TokenKind;

TEST(lexer_empty_source)
{
    auto r = lex("");
    EXPECT(r->tokens.size() == 1 && r->tokens[0].kind == TokenKind::EndOfFile);
}

TEST(lexer_keywords_and_identifiers)
{
    auto r = lex("fn let var inout sink unsafe main _x1 iff");
    const std::vector<TokenKind> expected = {
        TokenKind::KwFn,       TokenKind::KwLet,      TokenKind::KwVar,
        TokenKind::KwInout,    TokenKind::KwSink,     TokenKind::KwUnsafe,
        TokenKind::Identifier, TokenKind::Identifier, TokenKind::Identifier,
        TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(lexer_integer_literals)
{
    auto r = lex("42 0xff 0b1010 1_000_000 10u8 7i64 0xffu16");
    EXPECT(r->tokens[0].kind == TokenKind::IntLiteral && r->tokens[0].text == "42");
    EXPECT(r->tokens[1].text == "0xff" && r->tokens[1].suffix.empty());
    EXPECT(r->tokens[2].text == "0b1010");
    EXPECT(r->tokens[3].text == "1_000_000");
    EXPECT(r->tokens[4].text == "10" && r->tokens[4].suffix == "u8");
    EXPECT(r->tokens[5].suffix == "i64");
    EXPECT(r->tokens[6].text == "0xff" && r->tokens[6].suffix == "u16");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(lexer_float_literals)
{
    auto r = lex("3.0 1e-3 2.5f32 2f32 1E+9");
    EXPECT(r->tokens[0].kind == TokenKind::FloatLiteral);
    EXPECT(r->tokens[1].kind == TokenKind::FloatLiteral && r->tokens[1].text == "1e-3");
    EXPECT(r->tokens[2].kind == TokenKind::FloatLiteral && r->tokens[2].suffix == "f32");
    EXPECT(r->tokens[3].kind == TokenKind::FloatLiteral && r->tokens[3].text == "2");
    EXPECT(r->tokens[4].kind == TokenKind::FloatLiteral);
    EXPECT(!r->diagnostics.has_errors());
}

TEST(lexer_range_is_not_a_float)
{
    auto r = lex("1..10 1..=9");
    const std::vector<TokenKind> expected = {
        TokenKind::IntLiteral, TokenKind::DotDot,   TokenKind::IntLiteral,
        TokenKind::IntLiteral, TokenKind::DotDotEq, TokenKind::IntLiteral,
        TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(lexer_invalid_numbers)
{
    auto r = lex("12abc 1.5u8 0x 0b102 0x1f64");
    EXPECT(r->tokens[0].kind == TokenKind::Error);
    EXPECT(r->tokens[1].kind == TokenKind::Error);
    EXPECT(r->tokens[2].kind == TokenKind::Error);
    EXPECT(r->tokens[3].kind == TokenKind::Error);
    // 0x1f64 is the hexadecimal number 0x1F64, not 0x1 with an f64 suffix.
    EXPECT(r->tokens[4].kind == TokenKind::IntLiteral);
    EXPECT(r->diagnostics.diagnostics().size() == 4);
}

TEST(lexer_char_literals)
{
    auto r = lex("'a' '\\n' '\\u{1F600}' '\xED\x95\x9C' '' 'ab'");
    EXPECT(r->tokens[0].kind == TokenKind::CharLiteral && r->tokens[0].text == "a");
    EXPECT(r->tokens[1].text == "\n");
    EXPECT(r->tokens[2].text == "\xF0\x9F\x98\x80");
    EXPECT(r->tokens[3].kind == TokenKind::CharLiteral && r->tokens[3].text == "\xED\x95\x9C");
    EXPECT(r->tokens[4].kind == TokenKind::Error);
    EXPECT(r->tokens[5].kind == TokenKind::Error);
    EXPECT(r->diagnostics.diagnostics().size() == 2);
}

TEST(lexer_operators_longest_match)
{
    auto r = lex("-> => == != <= >= << >> <<= >>= && || += -= *= /= %= &= |= ^= .. ..= ? ~");
    const std::vector<TokenKind> expected = {
        TokenKind::Arrow,  TokenKind::FatArrow, TokenKind::EqEq,    TokenKind::BangEq,
        TokenKind::LtEq,   TokenKind::GtEq,     TokenKind::Shl,     TokenKind::Shr,
        TokenKind::ShlEq,  TokenKind::ShrEq,    TokenKind::AmpAmp,  TokenKind::PipePipe,
        TokenKind::PlusEq, TokenKind::MinusEq,  TokenKind::StarEq,  TokenKind::SlashEq,
        TokenKind::PercentEq, TokenKind::AmpEq, TokenKind::PipeEq,  TokenKind::CaretEq,
        TokenKind::DotDot, TokenKind::DotDotEq, TokenKind::Question, TokenKind::Tilde,
        TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(lexer_string_interpolation)
{
    auto r = lex("\"a{x}b{y + 1}c\"");
    const std::vector<TokenKind> expected = {
        TokenKind::InterpStart, TokenKind::Identifier, TokenKind::InterpMid,
        TokenKind::Identifier,  TokenKind::Plus,       TokenKind::IntLiteral,
        TokenKind::InterpEnd,   TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(r->tokens) == expected);
    EXPECT(r->tokens[0].text == "a" && r->tokens[2].text == "b" && r->tokens[6].text == "c");
}

TEST(lexer_string_escapes)
{
    auto r = lex(R"("a\"b\n\{c\}\u{48}")");
    EXPECT(r->tokens[0].kind == TokenKind::StringLiteral);
    EXPECT(r->tokens[0].text == "a\"b\n{c}H");
}

TEST(lexer_comments_nest)
{
    auto r = lex("a // line\n/* block /* nested */ still */ b");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::Newline, TokenKind::Identifier, TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(lexer_newline_continuation)
{
    auto r = lex("a = b +\n c\nf(1,\n 2)\nx ->\n y\nz");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::Eq,         TokenKind::Identifier, TokenKind::Plus,
        TokenKind::Identifier, TokenKind::Newline,    TokenKind::Identifier, TokenKind::LParen,
        TokenKind::IntLiteral, TokenKind::Comma,      TokenKind::IntLiteral, TokenKind::RParen,
        TokenKind::Newline,    TokenKind::Identifier, TokenKind::Arrow,      TokenKind::Identifier,
        TokenKind::Newline,    TokenKind::Identifier, TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(lexer_question_ends_line)
{
    auto r = lex("let t = read()?\nnext");
    EXPECT(r->tokens[6].kind == TokenKind::Question);
    EXPECT(r->tokens[7].kind == TokenKind::Newline);
}

TEST(lexer_newline_inside_braces_is_kept)
{
    auto r = lex("f({\na\n})");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::LParen,  TokenKind::LBrace, TokenKind::Newline,
        TokenKind::Identifier, TokenKind::Newline, TokenKind::RBrace, TokenKind::RParen,
        TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(lexer_broken_interpolation_recovers)
{
    auto r = lex("x \"{f(\ny\nz");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::InterpStart, TokenKind::Identifier,
        TokenKind::LParen,     TokenKind::Error,       TokenKind::Newline,
        TokenKind::Identifier, TokenKind::Newline,     TokenKind::Identifier,
        TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(lexer_locations_count_code_points)
{
    auto r = lex("ab\n  \"\xED\x95\x9C\" c");
    EXPECT(r->tokens[0].location.line == 1 && r->tokens[0].location.column == 1);
    EXPECT(r->tokens[2].location.line == 2 && r->tokens[2].location.column == 3);
    EXPECT(r->tokens[3].location.column == 7);
}
