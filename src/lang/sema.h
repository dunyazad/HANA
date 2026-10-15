#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lang/ast.h"
#include "lang/symbols.h"
#include "support/diagnostic.h"

namespace hana::lang {

// A storage location named in source: a variable and a path of field
// indices, as in 'p.pos.x'. Used for mutability and inout exclusivity.
struct Place {
    const VarSymbol* root = nullptr;
    std::vector<int> path;
    const Expr* expr = nullptr;
};

// Returns the place an expression names, or nothing for a temporary value.
// The expression must have been checked.
std::optional<Place> place_of(const Expr& expr);

// Semantic analysis: resolves names, checks types and the rules of the
// language (mutability, inout exclusivity, returns on every path, moves,
// ...). Annotates the AST in place and returns the symbols it created.
class Sema {
public:
    explicit Sema(DiagnosticEngine& diagnostics);

    std::unique_ptr<Program> check(Module& module);

private:
    struct LoopContext {
        bool has_break = false;
    };

    enum class MutationKind {
        Assign,        // 'p.x = 1'
        PassInout,     // 'f(&p)'
        InoutReceiver, // 'p.damage(1)' where damage takes 'inout self'
    };

    // Declarations
    void declare_intrinsics();
    void declare_struct(StructDecl& decl);
    void declare_struct_fields(StructDecl& decl);
    void check_struct_containment(const StructDecl& decl);
    bool contains_struct(const StructInfo& outer, const StructInfo& inner,
                         std::vector<const StructInfo*>& visiting) const;
    void check_copy_struct(const StructDecl& decl);
    void check_field_defaults(StructDecl& decl);
    FunctionSymbol& create_function_symbol(FnDecl& function, std::string name);
    void declare_function(FnDecl& function);
    void declare_method(StructDecl& decl, FnDecl& method);
    void check_main_signature(const FunctionSymbol& symbol);
    void check_function_body(FnDecl& function);
    void check_body_once(FnDecl& function);
    void declare_self(FnDecl& function);

    // Literal variable inference ('var i = 0' takes the type of its first
    // typed use, see docs/language.md)
    void infer_literal_variables(FnDecl& function);
    void collect_pending_lets(Block& block);
    const Type* literal_variable_type(const LetStmt& let);
    bool bind(const Type* placeholder, const Type* concrete);
    const Type* unify(const Type* a, const Type* b);
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
    bool check_jump(Stmt& stmt, const char* keyword);
    void check_condition(Expr& condition);
    const Type* check_assignment_target(Expr& target);
    void require_mutable(const Place& place, MutationKind kind, const std::string& method_name);

    // Expressions; each returns the type and also stores it in expr.type.
    const Type* check_expr(Expr& expr, const Type* expected);
    const Type* check_int_literal(IntLiteralExpr& lit, const Type* expected, bool negated);
    const Type* check_float_literal(FloatLiteralExpr& lit, const Type* expected);
    const Type* check_string_literal(StringLiteralExpr& lit);
    const Type* check_name(NameExpr& name, const Type* expected = nullptr);
    const Type* check_conditional(ConditionalExpr& cond, const Type* expected);
    const Type* check_cast(CastExpr& cast);
    const Type* check_unary(UnaryExpr& unary, const Type* expected);
    const Type* check_binary(BinaryExpr& binary, const Type* expected);
    const Type* check_binary_operands(BinaryExpr& binary, const Type* expected);
    const Type* binary_result_type(BinaryOp op, const Type* operand, const SourceLocation& location);
    const Type* check_struct_literal(StructLiteralExpr& lit);
    void report_missing_fields(const StructLiteralExpr& lit, const StructInfo& info);
    const Type* check_member(MemberExpr& member);
    const StructInfo* static_struct_name(const Expr& object) const;
    const Type* check_call(CallExpr& call);
    const Type* check_function_call(CallExpr& call, NameExpr& callee);
    const Type* check_method_call(CallExpr& call, MemberExpr& callee);
    const FunctionSymbol* find_static_function(MemberExpr& callee, const StructInfo& info);
    const FunctionSymbol* find_method(MemberExpr& callee, const Type* receiver);
    std::optional<Place> check_receiver(MemberExpr& callee, const FunctionSymbol& method);
    const Type* check_intrinsic_call(CallExpr& call, const FunctionSymbol& function);
    void check_printable(const Type* type, const SourceLocation& location);
    void check_call_arguments(CallExpr& call, const FunctionSymbol& function,
                              const std::optional<Place>& receiver);
    void check_args_without_callee(CallExpr& call);
    void check_inout_argument(Expr& arg, const Type* param_type, std::vector<Place>& inout_places);
    void check_inout_exclusivity(CallExpr& call, const FunctionSymbol& function,
                                 const std::vector<Place>& inout_places,
                                 const std::optional<Place>& receiver);
    const Type* unsupported(Expr& expr, const std::string& what);
    void expect_type(const Type* expected, const Type* actual, const SourceLocation& location);

    // Scopes
    void push_scope();
    void pop_scope();
    const VarSymbol* declare_var(const std::string& name, const Type* type, VarKind kind,
                                 bool is_mutable, const SourceLocation& location);
    const VarSymbol* lookup_var(const std::string& name) const;
    const FunctionSymbol* lookup_function(const std::string& name) const;
    StructInfo* lookup_struct(const std::string& name) const;

    struct PendingLet {
        const LetStmt* let;
        bool is_float;
    };

    // Points at a scratch engine while inference passes run.
    DiagnosticEngine* diagnostics_;
    std::unique_ptr<Program> program_;
    std::unordered_map<std::string, const FunctionSymbol*> functions_;
    std::unordered_map<std::string, StructInfo*> structs_;
    std::vector<std::unordered_map<std::string, const VarSymbol*>> scopes_;
    std::vector<LoopContext> loops_;
    const Type* return_type_ = nullptr;
    // True only while checking the argument of print/write, the one place
    // where format strings are supported in this stage.
    bool allow_interpolation_ = false;

    std::vector<PendingLet> pending_;
    std::unordered_map<const LetStmt*, const Type*> let_types_;
    std::unordered_map<const LetStmt*, const Type*> let_placeholders_;
    std::unordered_map<const Type*, const LetStmt*> placeholder_lets_;
    std::deque<Type> placeholder_storage_;
    bool inferring_ = false;
    bool bound_new_ = false;
};

} // namespace hana::lang
