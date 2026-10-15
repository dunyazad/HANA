#include "helpers.h"
#include "test.h"

using namespace hana::test;
using hana::lang::TokenKind;

TEST(code_lexer_indent_and_dedent)
{
    auto r = lex_block("if a:\n  b\nc\n");
    const std::vector<TokenKind> expected = {
        TokenKind::KwIf,   TokenKind::Identifier, TokenKind::Colon,      TokenKind::Newline,
        TokenKind::Indent, TokenKind::Identifier, TokenKind::Newline,    TokenKind::Dedent,
        TokenKind::Identifier, TokenKind::Newline, TokenKind::EndOfInput,
    };
    EXPECT(kinds_of(r->tokens) == expected);
    EXPECT(!r->diagnostics.has_errors());
}

TEST(code_lexer_dedents_at_end)
{
    auto r = lex_block("while a:\n  if b:\n    c\n");
    std::size_t dedents = 0;
    for (const auto& token : r->tokens) {
        dedents += token.kind == TokenKind::Dedent ? 1 : 0;
    }
    EXPECT(dedents == 2);
}

TEST(code_lexer_first_line_sets_base)
{
    auto r = lex_block("    a\n    b\n");
    EXPECT(!r->diagnostics.has_errors());
    for (const auto& token : r->tokens) {
        EXPECT(token.kind != TokenKind::Indent);
    }
}

TEST(code_lexer_blank_and_comment_lines_are_ignored)
{
    auto r = lex_block("a\n\n   # comment\n\nb  # trailing\n");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::Newline, TokenKind::Identifier,
        TokenKind::Newline,    TokenKind::EndOfInput,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(code_lexer_brackets_join_lines)
{
    auto r = lex_block("f(1,\n      2)\ng\n");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::LParen,  TokenKind::IntLiteral, TokenKind::Comma,
        TokenKind::IntLiteral, TokenKind::RParen,  TokenKind::Newline,    TokenKind::Identifier,
        TokenKind::Newline,    TokenKind::EndOfInput,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(code_lexer_tabs_are_rejected)
{
    auto r = lex_block("if a:\n\tb\n");
    EXPECT(r->has_error("tabs are not allowed"));
}

TEST(code_lexer_inconsistent_dedent)
{
    auto r = lex_block("if a:\n    b\n  c\n");
    EXPECT(r->has_error("does not match any outer block"));
}

TEST(code_lexer_less_than_closing_indent)
{
    auto result = std::make_unique<Compiled>();
    result->file.text = "    a\n  b\n";
    hana::lang::CodeRegion region;
    region.end = static_cast<std::uint32_t>(result->file.text.size());
    region.multiline = true;
    region.base_indent = 4;
    hana::lang::CodeLexer lexer(result->file.text, region, result->diagnostics);
    lexer.tokenize();
    EXPECT(result->has_error("indented less than the closing"));
}

TEST(code_lexer_numbers)
{
    auto r = lex_line("42 0xff 0b1010 1_000 10u8 2.5f32 1e-3 2f32 1..9");
    EXPECT(r->tokens[0].text == "42");
    EXPECT(r->tokens[1].text == "0xff");
    EXPECT(r->tokens[2].text == "0b1010");
    EXPECT(r->tokens[4].suffix == "u8");
    EXPECT(r->tokens[5].kind == TokenKind::FloatLiteral && r->tokens[5].suffix == "f32");
    EXPECT(r->tokens[6].kind == TokenKind::FloatLiteral);
    EXPECT(r->tokens[7].kind == TokenKind::FloatLiteral);
    EXPECT(r->tokens[8].kind == TokenKind::IntLiteral && r->tokens[9].kind == TokenKind::DotDot);
    EXPECT(!r->diagnostics.has_errors());
}

TEST(code_lexer_invalid_numbers)
{
    auto r = lex_line("12abc 1.5u8 0x");
    EXPECT(r->error_count() == 3);
}

TEST(code_lexer_strings_and_escapes)
{
    auto r = lex_line(R"("a\"b\n\u0041\u{1F600}\/" "{x}")");
    EXPECT(r->tokens[0].kind == TokenKind::StringLiteral);
    EXPECT(r->tokens[0].text == "a\"b\nA\xF0\x9F\x98\x80/");
    // Braces in a plain string are literal.
    EXPECT(r->tokens[1].kind == TokenKind::StringLiteral && r->tokens[1].text == "{x}");
}

TEST(code_lexer_json_surrogate_pair)
{
    auto r = lex_line(R"("\ud83d\ude00")");
    EXPECT(r->tokens[0].text == "\xF0\x9F\x98\x80");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(code_lexer_format_strings)
{
    auto r = lex_line(R"(f"a{x}b{y + 1}c" f"plain")");
    const std::vector<TokenKind> expected = {
        TokenKind::FStringStart, TokenKind::Identifier, TokenKind::FStringMid,
        TokenKind::Identifier,   TokenKind::Plus,       TokenKind::IntLiteral,
        TokenKind::FStringEnd,   TokenKind::FStringWhole, TokenKind::Newline,
        TokenKind::EndOfInput,
    };
    EXPECT(kinds_of(r->tokens) == expected);
    EXPECT(r->tokens[0].text == "a" && r->tokens[6].text == "c");
}

TEST(code_lexer_chars)
{
    auto r = lex_line("'a' '\\n' '\xED\x95\x9C' '' 'ab'");
    EXPECT(r->tokens[0].kind == TokenKind::CharLiteral && r->tokens[0].text == "a");
    EXPECT(r->tokens[1].text == "\n");
    EXPECT(r->tokens[2].text == "\xED\x95\x9C");
    EXPECT(r->tokens[3].kind == TokenKind::Error);
    EXPECT(r->tokens[4].kind == TokenKind::Error);
}

TEST(code_lexer_operators)
{
    auto r = lex_line("== != <= >= << >> += -= *= /= %= &= |= ^= <<= >>= -> ..= ~ ?");
    const std::vector<TokenKind> expected = {
        TokenKind::EqEq,    TokenKind::BangEq,    TokenKind::LtEq,   TokenKind::GtEq,
        TokenKind::Shl,     TokenKind::Shr,       TokenKind::PlusEq, TokenKind::MinusEq,
        TokenKind::StarEq,  TokenKind::SlashEq,   TokenKind::PercentEq, TokenKind::AmpEq,
        TokenKind::PipeEq,  TokenKind::CaretEq,   TokenKind::ShlEq,  TokenKind::ShrEq,
        TokenKind::Arrow,   TokenKind::DotDotEq,  TokenKind::Tilde,  TokenKind::Question,
        TokenKind::Newline, TokenKind::EndOfInput,
    };
    EXPECT(kinds_of(r->tokens) == expected);
}

TEST(code_lexer_bang_is_not_an_operator)
{
    auto r = lex_line("!a");
    EXPECT(r->has_error("use 'not'"));
}

TEST(code_lexer_keywords)
{
    auto r = lex_line("let var elif and or not null pass iff");
    EXPECT(r->tokens[0].kind == TokenKind::KwLet);
    EXPECT(r->tokens[2].kind == TokenKind::KwElif);
    EXPECT(r->tokens[5].kind == TokenKind::KwNot);
    EXPECT(r->tokens[6].kind == TokenKind::KwNull);
    EXPECT(r->tokens[8].kind == TokenKind::Identifier);
}

// Regression: a line indented less than the first code line used to pop the
// base level off the indentation stack and read past its end.
TEST(code_lexer_less_than_first_line)
{
    auto r = lex_block("    a\n  b\n");
    EXPECT(r->has_error("indented less than the first line of its block"));
    auto s = lex_block("    if a:\n      b\n  c\n");
    EXPECT(s->has_error("indented less than the first line of its block"));
}
