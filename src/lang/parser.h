#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "lang/ast.h"
#include "lang/token.h"
#include "support/diagnostic.h"

namespace hana::lang {

// Recursive descent parser producing the AST of one module.
//
// Error handling: after a syntax error the parser reports once, skips to the
// end of the current statement (or declaration) and continues, so one run
// reports every independent error.
class Parser {
public:
    Parser(const std::vector<Token>& tokens, DiagnosticEngine& diagnostics);

    std::unique_ptr<Module> parse_module();

private:
    // Declarations
    void parse_top_level_item(Module& module);
    std::unique_ptr<FnDecl> parse_function(bool is_pub);
    bool parse_params(FnDecl& function);
    std::optional<Param> parse_param();
    std::optional<TypeExpr> parse_type();
    void skip_unsupported_declaration(const Token& keyword);

    // Statements
    bool parse_block(Block& block);
    StmtPtr parse_statement();
    StmtPtr parse_let();
    StmtPtr parse_return();
    StmtPtr parse_if();
    StmtPtr parse_while();
    StmtPtr parse_loop();
    StmtPtr parse_for();
    StmtPtr parse_block_statement();
    StmtPtr parse_expression_statement();
    bool expect_statement_end();

    // Expressions
    ExprPtr parse_expression();
    ExprPtr parse_binary(int min_precedence);
    ExprPtr parse_unary();
    ExprPtr parse_postfix(ExprPtr expr);
    ExprPtr parse_call(ExprPtr callee);
    ExprPtr parse_primary();
    ExprPtr parse_int_literal();
    ExprPtr parse_float_literal();
    ExprPtr parse_char_literal();
    ExprPtr parse_string_literal();
    ExprPtr parse_parenthesized();
    ExprPtr unsupported_expression(const char* what);

    // Token access
    const Token& peek(std::size_t ahead = 0) const;
    const Token& advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    bool expect(TokenKind kind, const char* what);
    bool at_statement_end() const;
    void skip_newlines();
    void skip_separators();
    bool next_significant_is(TokenKind kind) const;

    // Error handling
    void error_at(const Token& token, const std::string& message);
    void error_at(const SourceLocation& location, const std::string& message);
    void synchronize_statement();
    void synchronize_top_level();

    const std::vector<Token>& tokens_;
    DiagnosticEngine& diagnostics_;
    std::size_t position_ = 0;
    // Set after an error until the parser resynchronizes; suppresses
    // follow-up errors caused by the first one.
    bool panic_ = false;
};

} // namespace hana::lang
