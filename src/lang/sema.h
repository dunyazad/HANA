#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "lang/ast.h"
#include "lang/symbols.h"
#include "support/diagnostic.h"

namespace hana::lang {

// Semantic analysis: resolves names, checks types and the rules of the
// language (mutability, inout exclusivity, returns on every path, ...).
// Annotates the AST in place and returns the symbols it created.
class Sema {
public:
    explicit Sema(DiagnosticEngine& diagnostics);

    std::unique_ptr<Program> check(Module& module);

private:
    struct LoopContext {
        bool has_break = false;
    };

    // Declarations
    void declare_intrinsics();
    void declare_function(FnDecl& function);
    void check_main_signature(const FunctionSymbol& symbol);
    void check_function_body(FnDecl& function);
    const Type* resolve_type(const TypeExpr& type);

    // Statements; each returns true when control never reaches the end.
    bool check_block(Block& block);
    bool check_statement(Stmt& stmt);
    bool check_let(LetStmt& let);
    bool check_assign(AssignStmt& assign);
    bool check_expression_statement(ExprStmt& stmt);
    bool check_return(ReturnStmt& ret);
    bool check_if(IfStmt& stmt);
    bool check_while(WhileStmt& stmt);
    bool check_loop(LoopStmt& stmt);
    bool check_jump(Stmt& stmt, const char* keyword);
    void check_condition(Expr& condition);
    const VarSymbol* check_assignment_target(Expr& target);

    // Expressions; each returns the type and also stores it in expr.type.
    const Type* check_expr(Expr& expr, const Type* expected);
    const Type* check_int_literal(IntLiteralExpr& lit, const Type* expected, bool negated);
    const Type* check_float_literal(FloatLiteralExpr& lit, const Type* expected);
    const Type* check_string_literal(StringLiteralExpr& lit);
    const Type* check_name(NameExpr& name);
    const Type* check_unary(UnaryExpr& unary, const Type* expected);
    const Type* check_binary(BinaryExpr& binary, const Type* expected);
    const Type* check_binary_operands(BinaryExpr& binary, const Type* expected);
    const Type* binary_result_type(BinaryOp op, const Type* operand, const SourceLocation& location);
    const Type* check_call(CallExpr& call);
    const Type* check_intrinsic_call(CallExpr& call, const FunctionSymbol& function);
    void check_call_arguments(CallExpr& call, const FunctionSymbol& function);
    void check_inout_argument(Expr& arg, const Type* param_type,
                              std::vector<const VarSymbol*>& inout_vars);
    void check_inout_exclusivity(CallExpr& call, const FunctionSymbol& function,
                                 const std::vector<const VarSymbol*>& inout_vars);
    const Type* unsupported(Expr& expr, const std::string& what);
    void expect_type(const Type* expected, const Type* actual, const SourceLocation& location);

    // Scopes
    void push_scope();
    void pop_scope();
    const VarSymbol* declare_var(const std::string& name, const Type* type, VarKind kind,
                                 bool is_mutable, const SourceLocation& location);
    const VarSymbol* lookup_var(const std::string& name) const;
    const FunctionSymbol* lookup_function(const std::string& name) const;

    DiagnosticEngine& diagnostics_;
    std::unique_ptr<Program> program_;
    std::unordered_map<std::string, const FunctionSymbol*> functions_;
    std::vector<std::unordered_map<std::string, const VarSymbol*>> scopes_;
    std::vector<LoopContext> loops_;
    const Type* return_type_ = nullptr;
    // True only while checking the argument of print/println, the one place
    // where string interpolation is supported in this stage.
    bool allow_interpolation_ = false;
};

} // namespace hana::lang
