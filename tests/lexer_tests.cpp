#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "lang/lexer.h"

using hana::lang::Lexer;
using hana::lang::Token;
using hana::lang::TokenKind;

namespace {

int g_failures = 0;

void expect(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "%s:%d: expectation failed: %s\n", file, line, expression);
    }
}

#define EXPECT(cond) expect((cond), #cond, __FILE__, __LINE__)

std::vector<TokenKind> kinds_of(const std::vector<Token>& tokens)
{
    std::vector<TokenKind> kinds;
    kinds.reserve(tokens.size());
    for (const Token& token : tokens) {
        kinds.push_back(token.kind);
    }
    return kinds;
}

std::vector<Token> lex(std::string_view source)
{
    Lexer lexer(source);
    return lexer.tokenize();
}

void test_empty_source()
{
    const std::vector<Token> tokens = lex("");
    EXPECT(tokens.size() == 1);
    EXPECT(tokens[0].kind == TokenKind::EndOfFile);
}

void test_keywords_and_identifiers()
{
    const std::vector<Token> tokens =
        lex("app component scene state action true false button on_click _x1");
    const std::vector<TokenKind> expected = {
        TokenKind::KwApp,      TokenKind::KwComponent, TokenKind::KwScene,    TokenKind::KwState,
        TokenKind::KwAction,   TokenKind::KwTrue,     TokenKind::KwFalse,
        TokenKind::Identifier, TokenKind::Identifier, TokenKind::Identifier,
        TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
    EXPECT(tokens[8].text == "on_click");
}

void test_numbers_and_sizes()
{
    const std::vector<Token> tokens = lex("0 42 3.0 300x200 1x0.6 1.foo");
    EXPECT(tokens[0].kind == TokenKind::IntLiteral && tokens[0].text == "0");
    EXPECT(tokens[1].kind == TokenKind::IntLiteral && tokens[1].text == "42");
    EXPECT(tokens[2].kind == TokenKind::FloatLiteral && tokens[2].text == "3.0");
    EXPECT(tokens[3].kind == TokenKind::SizeLiteral && tokens[3].text == "300x200");
    EXPECT(tokens[4].kind == TokenKind::SizeLiteral && tokens[4].text == "1x0.6");
    EXPECT(tokens[5].kind == TokenKind::IntLiteral && tokens[5].text == "1");
    EXPECT(tokens[6].kind == TokenKind::Dot);
    EXPECT(tokens[7].kind == TokenKind::Identifier && tokens[7].text == "foo");
}

void test_invalid_number()
{
    Lexer lexer("12abc 5");
    const std::vector<Token> tokens = lexer.tokenize();
    EXPECT(tokens[0].kind == TokenKind::Error && tokens[0].text == "12abc");
    EXPECT(tokens[1].kind == TokenKind::IntLiteral);
    EXPECT(lexer.diagnostics().size() == 1);
}

void test_operators()
{
    const std::vector<Token> tokens = lex("= == != < <= > >= && || ! + - * / % ( ) [ ] { } , : ; .");
    const std::vector<TokenKind> expected = {
        TokenKind::Assign,  TokenKind::Equal,        TokenKind::NotEqual, TokenKind::Less,
        TokenKind::LessEqual, TokenKind::Greater,    TokenKind::GreaterEqual, TokenKind::AndAnd,
        TokenKind::OrOr,    TokenKind::Bang,         TokenKind::Plus,     TokenKind::Minus,
        TokenKind::Star,    TokenKind::Slash,        TokenKind::Percent,  TokenKind::LParen,
        TokenKind::RParen,  TokenKind::LBracket,     TokenKind::RBracket, TokenKind::LBrace,
        TokenKind::RBrace,  TokenKind::Comma,        TokenKind::Colon,    TokenKind::Semicolon,
        TokenKind::Dot,     TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_plain_string_and_escapes()
{
    const std::vector<Token> tokens = lex(R"("a\"b\n\{c\}")");
    EXPECT(tokens[0].kind == TokenKind::StringLiteral);
    EXPECT(tokens[0].text == "a\"b\n{c}");
}

void test_string_interpolation()
{
    const std::vector<Token> tokens = lex(R"("a{x}b{y + 1}c")");
    const std::vector<TokenKind> expected = {
        TokenKind::InterpStart, TokenKind::Identifier, TokenKind::InterpMid,
        TokenKind::Identifier,  TokenKind::Plus,       TokenKind::IntLiteral,
        TokenKind::InterpEnd,   TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
    EXPECT(tokens[0].text == "a");
    EXPECT(tokens[2].text == "b");
    EXPECT(tokens[6].text == "c");
}

void test_nested_interpolation()
{
    const std::vector<Token> tokens = lex(R"("x{f("in{v}")}y")");
    const std::vector<TokenKind> expected = {
        TokenKind::InterpStart, TokenKind::Identifier, TokenKind::LParen,
        TokenKind::InterpStart, TokenKind::Identifier, TokenKind::InterpEnd,
        TokenKind::RParen,      TokenKind::InterpEnd,  TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
    EXPECT(tokens[7].text == "y");
}

void test_braces_inside_interpolation()
{
    const std::vector<Token> tokens = lex(R"("{ {} }")");
    const std::vector<TokenKind> expected = {
        TokenKind::InterpStart, TokenKind::LBrace,    TokenKind::RBrace,
        TokenKind::InterpEnd,   TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_unterminated_string()
{
    Lexer lexer("\"abc\nx");
    const std::vector<Token> tokens = lexer.tokenize();
    EXPECT(tokens[0].kind == TokenKind::Error);
    EXPECT(tokens[1].kind == TokenKind::Newline);
    EXPECT(tokens[2].kind == TokenKind::Identifier);
    EXPECT(lexer.diagnostics().size() == 1);
}

void test_unterminated_interpolation()
{
    Lexer lexer("\"a{x\ny");
    const std::vector<Token> tokens = lexer.tokenize();
    const std::vector<TokenKind> expected = {
        TokenKind::InterpStart, TokenKind::Identifier, TokenKind::Error,
        TokenKind::Newline,     TokenKind::Identifier, TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
    EXPECT(lexer.diagnostics().size() == 1);
}

void test_comments()
{
    const std::vector<Token> tokens = lex("a // line\n/* block /* nested */ still */ b");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::Newline, TokenKind::Identifier, TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_unterminated_block_comment()
{
    Lexer lexer("a /* never closed");
    const std::vector<Token> tokens = lexer.tokenize();
    EXPECT(tokens.size() == 2);
    EXPECT(lexer.diagnostics().size() == 1);
}

void test_newlines_are_collapsed()
{
    const std::vector<Token> tokens = lex("\n\n a \n\n\r\n b \n");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::Newline, TokenKind::Identifier,
        TokenKind::Newline,    TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_locations()
{
    const std::vector<Token> tokens = lex("ab\n  \"\xED\x95\x9C\" c");
    EXPECT(tokens[0].location.line == 1 && tokens[0].location.column == 1);
    EXPECT(tokens[2].location.line == 2 && tokens[2].location.column == 3);
    // The string holds one 3-byte code point, so c is at column 3 + 3 + 1.
    EXPECT(tokens[3].location.line == 2 && tokens[3].location.column == 7);
}

void test_unexpected_character()
{
    Lexer lexer("a @ \xED\x95\x9C b");
    const std::vector<Token> tokens = lexer.tokenize();
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::Error, TokenKind::Error,
        TokenKind::Identifier, TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
    EXPECT(tokens[2].text == "\xED\x95\x9C");
    EXPECT(lexer.diagnostics().size() == 2);
}

void test_byte_order_mark_is_skipped()
{
    const std::vector<Token> tokens = lex("\xEF\xBB\xBF" "app");
    EXPECT(tokens[0].kind == TokenKind::KwApp);
    EXPECT(tokens[0].location.column == 1);
}

void test_newline_inside_parens_and_brackets()
{
    const std::vector<Token> tokens = lex("(1,\n 2\n)\n[3\n]\nx");
    const std::vector<TokenKind> expected = {
        TokenKind::LParen,     TokenKind::IntLiteral, TokenKind::Comma,
        TokenKind::IntLiteral, TokenKind::RParen,     TokenKind::Newline,
        TokenKind::LBracket,   TokenKind::IntLiteral, TokenKind::RBracket,
        TokenKind::Newline,    TokenKind::Identifier, TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_newline_inside_braces_is_kept()
{
    // The innermost open bracket decides: a block inside parentheses keeps
    // its statement separators.
    const std::vector<Token> tokens = lex("f({\na\nb\n})");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::LParen,     TokenKind::LBrace,
        TokenKind::Newline,    TokenKind::Identifier, TokenKind::Newline,
        TokenKind::Identifier, TokenKind::Newline,    TokenKind::RBrace,
        TokenKind::RParen,     TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_newline_after_operator_continues()
{
    const std::vector<Token> tokens = lex("a = b +\n c &&\n\n d\ntext:\n \"x\"");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::Assign,     TokenKind::Identifier,
        TokenKind::Plus,       TokenKind::Identifier, TokenKind::AndAnd,
        TokenKind::Identifier, TokenKind::Newline,    TokenKind::Identifier,
        TokenKind::Colon,      TokenKind::StringLiteral, TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_newline_after_operand_ends_statement()
{
    const std::vector<Token> tokens = lex("a = b\n+c");
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::Assign, TokenKind::Identifier, TokenKind::Newline,
        TokenKind::Plus,       TokenKind::Identifier, TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_stray_closer_does_not_break_newlines()
{
    const std::vector<Token> tokens = lex("(a ]\n b)\nc");
    const std::vector<TokenKind> expected = {
        TokenKind::LParen,     TokenKind::Identifier, TokenKind::RBracket,
        TokenKind::Identifier, TokenKind::RParen,     TokenKind::Newline,
        TokenKind::Identifier, TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_broken_interpolation_releases_brackets()
{
    Lexer lexer("x \"{f(\ny\nz");
    const std::vector<Token> tokens = lexer.tokenize();
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::InterpStart, TokenKind::Identifier,
        TokenKind::LParen,     TokenKind::Error,       TokenKind::Newline,
        TokenKind::Identifier, TokenKind::Newline,     TokenKind::Identifier,
        TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_unclosed_paren_inside_interpolation()
{
    Lexer lexer("\"{f(}\"\nx");
    const std::vector<Token> tokens = lexer.tokenize();
    const std::vector<TokenKind> expected = {
        TokenKind::InterpStart, TokenKind::Identifier, TokenKind::LParen,
        TokenKind::InterpEnd,   TokenKind::Newline,    TokenKind::Identifier,
        TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
}

void test_block_property_widget()
{
    Lexer lexer("button { text: \"+1\"; on_click: increment }");
    const std::vector<Token> tokens = lexer.tokenize();
    const std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::LBrace,     TokenKind::Identifier,
        TokenKind::Colon,      TokenKind::StringLiteral, TokenKind::Semicolon,
        TokenKind::Identifier, TokenKind::Colon,      TokenKind::Identifier,
        TokenKind::RBrace,     TokenKind::EndOfFile,
    };
    EXPECT(kinds_of(tokens) == expected);
    EXPECT(lexer.diagnostics().empty());
}

} // namespace

int main()
{
    test_empty_source();
    test_keywords_and_identifiers();
    test_numbers_and_sizes();
    test_invalid_number();
    test_operators();
    test_plain_string_and_escapes();
    test_string_interpolation();
    test_nested_interpolation();
    test_braces_inside_interpolation();
    test_unterminated_string();
    test_unterminated_interpolation();
    test_comments();
    test_unterminated_block_comment();
    test_newlines_are_collapsed();
    test_locations();
    test_unexpected_character();
    test_byte_order_mark_is_skipped();
    test_newline_inside_parens_and_brackets();
    test_newline_inside_braces_is_kept();
    test_newline_after_operator_continues();
    test_newline_after_operand_ends_statement();
    test_stray_closer_does_not_break_newlines();
    test_broken_interpolation_releases_brackets();
    test_unclosed_paren_inside_interpolation();
    test_block_property_widget();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d expectation(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all lexer tests passed");
    return 0;
}
