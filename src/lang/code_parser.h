#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "lang/ast.h"
#include "lang/token.h"
#include "support/diagnostic.h"

namespace hana::lang {

// Parses the tokens of one code region into statements.
//
// Blocks follow Python: a line ending in ':' opens an indented block, or the
// block body follows the ':' on the same line. After a syntax error the
// parser reports once, skips to the end of the logical line (including any
// block opened on it) and continues.
class CodeParser {
public:
    CodeParser(const std::vector<Token>& tokens, DiagnosticEngine& diagnostics);

    // Parses every statement of the region.
    Block parse_body();

private:
    // Statements
    void parse_statements_until(TokenKind end, Block& block);
    StmtPtr parse_statement();
    StmtPtr parse_simple_statement();
    StmtPtr parse_let();
    StmtPtr parse_return();
    StmtPtr parse_if();
    StmtPtr parse_while();
    StmtPtr parse_expression_statement();
    bool parse_suite(Block& block);
    bool expect_line_end();

    // Expressions, lowest precedence first
    ExprPtr parse_expression();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_not();
    ExprPtr parse_comparison();
    ExprPtr parse_binary(int level);
    ExprPtr parse_cast();
    ExprPtr parse_unary();
    ExprPtr parse_postfix(ExprPtr expr);
    ExprPtr parse_call(ExprPtr callee);
    ExprPtr parse_primary();
    ExprPtr parse_int_literal();
    ExprPtr parse_float_literal();
    ExprPtr parse_char_literal();
    ExprPtr parse_string_literal();
    ExprPtr parse_format_string();
    ExprPtr parse_parenthesized();
    ExprPtr unsupported(const std::string& what);
    std::optional<TypeExpr> parse_type();

    // Token access
    const Token& peek(std::size_t ahead = 0) const;
    const Token& advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    bool expect(TokenKind kind, const char* what);
    bool at_line_end() const;

    // Error handling
    void error_at(const Token& token, const std::string& message);
    void synchronize();

    const std::vector<Token>& tokens_;
    DiagnosticEngine& diagnostics_;
    std::size_t position_ = 0;
    bool panic_ = false;
};

} // namespace hana::lang
