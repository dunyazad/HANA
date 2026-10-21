#pragma once

#include <deque>
#include <map>
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
// indices, as in 'p.pos.x', where a list element is kAnyIndex. Used for
// mutability and inout exclusivity.
struct Place {
    const VarSymbol* root = nullptr;
    std::vector<int> path;
    const Expr* expr = nullptr;
};

// The path step of a list element 'xs[i]'. The index is not known before
// the program runs, so it stands for every element.
inline constexpr int kAnyIndex = -1;

// Returns the place an expression names, or nothing for a temporary value.
// The expression must have been checked.
std::optional<Place> place_of(const Expr& expr);

// Two places overlap when one contains the other: 'p' and 'p.x' do,
// 'p.x' and 'p.y' do not. 'xs[i]' and 'xs[j]' are taken to overlap.
bool places_overlap(const Place& a, const Place& b);

// 'p.pos.x' as written; 'xs[...]' for an element.
std::string place_text(const Expr& expr);

// Where 'p.pos.x' starts; a member expression is located at its '.'.
SourceLocation start_location(const Expr& expr);

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
        InoutLoop,     // 'for inout x in xs'
    };

    // Instances of generic types and functions, made once per list of type
    // arguments.
    using TypeMap = std::unordered_map<const Type*, const Type*>;
    struct PendingDefault {
        UserType* instance;
        FieldInfo* field;
    };

    // Declarations
    void declare_module(Module& module);
    void check_module(Module& module);
    void declare_intrinsics();
    void declare_type(TypeDecl& decl);
    void declare_type_fields(TypeDecl& decl);
    void declare_fields(std::vector<FieldDecl>& decls, std::vector<FieldInfo>& fields,
                        const std::string& owner);
    static std::vector<const FieldInfo*> value_fields(const UserType& info);
    void check_type_containment(const TypeDecl& decl);
    bool contains_type(const UserType& outer, const UserType& inner,
                       std::vector<const UserType*>& visiting) const;
    bool type_holds(const Type* type, const UserType& inner,
                    std::vector<const UserType*>& visiting) const;
    void check_copy_type(const TypeDecl& decl);
    void check_field_defaults(TypeDecl& decl);
    void check_defaults(std::vector<FieldDecl>& decls, const std::vector<FieldInfo>& fields);
    FunctionSymbol& create_function_symbol(FnDecl& function, std::string name, bool attach = true,
                                           const std::vector<const Type*>* own_params = nullptr);
    void declare_function(FnDecl& function);
    void declare_method(TypeDecl& decl, FnDecl& method);
    void check_drop_method(const TypeDecl& decl, const FnDecl& method,
                           const FunctionSymbol& symbol);
    void check_clone_method(const FnDecl& method, const FunctionSymbol& symbol,
                            const UserType& info);
    void check_main_signature(const FunctionSymbol& symbol);
    void check_function_body(FnDecl& function);
    void check_body_once(FnDecl& function);
    void declare_self(FnDecl& function);

    // Literal variable inference ('var i = 0' takes the type of its first
    // typed use, see docs/language.md)
    void infer_literal_variables(FnDecl& function);
    void collect_pending_vars(Block& block);
    const Type* literal_variable_type(const Stmt& stmt);
    bool bind(const Type* placeholder, const Type* concrete);
    const Type* unify(const Type* a, const Type* b);
    const Type* unify_tuples(const Type* a, const Type* b);
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
    bool check_for(ForStmt& stmt);
    const Type* check_range_bounds(ForStmt& stmt);

    // Option (sema_option.cpp)
    const Type* option_type(const Type* value);
    static const Type* value_expectation(const Expr& expr, const Type* expected);
    const Type* wrap_in_option(Expr& expr, const Type* type, const Type* expected);
    const Type* check_null(Expr& expr, const Type* expected);
    const Type* check_some_call(CallExpr& call, const Type* expected);
    const Type* check_option_comparison(BinaryExpr& binary);
    void check_option_case_pattern(Pattern& pattern, const Type* subject);

    // Generics and traits (sema_generic.cpp)
    void declare_builtin_traits();
    void declare_trait(TraitDecl& decl);
    void declare_trait_methods(TraitDecl& decl);
    std::vector<const TraitInfo*> resolve_bounds(const std::vector<NameRef>& names);
    std::vector<const TraitInfo*> resolve_declared_traits(const TypeDecl& decl);
    static std::string signature_text(const FunctionSymbol& function);
    void check_trait_conformance(const TypeDecl& decl);
    const FunctionSymbol* find_trait_method(MemberExpr& callee, const Type* receiver);
    std::vector<const Type*> declare_type_params(const std::vector<TypeParamDecl>& decls);
    void push_type_names(const std::vector<TypeParamDecl>& decls,
                         const std::vector<const Type*>& types);
    void push_type_bindings(const std::vector<std::pair<std::string, const Type*>>& bindings);
    const Type* lookup_type_name(const std::string& name) const;
    int push_function_type_names(const FnDecl& function);
    void pop_type_names(int count);
    const Type* resolve_generic_type(const TypeExpr& type);
    bool check_bounds(const std::vector<const Type*>& params, const std::vector<const Type*>& args,
                      const std::string& what, const SourceLocation& location);
    const Type* instantiate(const UserType& generic, const std::vector<const Type*>& args);
    void copy_instance_defaults(UserType& instance);
    void declare_instance_methods(UserType& instance);
    void declare_instance_method(UserType& instance, FnDecl& method, bool concrete);
    void declare_waiting_instance_methods();
    const Type* substitute(const Type* type, const TypeMap& map);
    static void unify_params(const Type* pattern, const Type* actual,
                             const std::vector<const Type*>& params, TypeMap& map);
    static bool mentions(const Type* type, const std::vector<const Type*>& params,
                         const TypeMap& map);
    bool infer_call_types(CallExpr& call, const FunctionSymbol& function,
                          const std::vector<const Type*>& params, TypeMap& map,
                          const Type* expected, const std::string& what);
    FunctionSymbol substituted_signature(const FunctionSymbol& function, const TypeMap& map);
    const FunctionSymbol* instance_function(const FunctionSymbol& generic,
                                            const std::vector<const Type*>& args,
                                            const SourceLocation& location);
    void check_pending_instances();
    const Type* check_generic_function_call(CallExpr& call, NameExpr& callee,
                                            const FunctionSymbol& function, const Type* expected);
    const Type* check_generic_method_call(CallExpr& call, MemberExpr& callee,
                                          const FunctionSymbol& method,
                                          const std::vector<const Type*>& params, TypeMap map,
                                          const std::optional<Place>& receiver,
                                          const Type* expected);
    const UserType* infer_literal_type(StructLiteralExpr& lit, const UserType& generic,
                                       const Type* expected);

    // Result and '?' (sema_result.cpp)
    bool is_result(const Type* type) const;
    const Type* check_result_call(CallExpr& call, bool is_ok, const Type* expected);
    const Type* check_try(TryExpr& expr, const Type* expected);

    // Lists (sema_list.cpp)
    const Type* list_type(const Type* element);
    const Type* check_list(ListExpr& list, const Type* expected);
    const Type* check_index(IndexExpr& index);
    std::vector<const Type*> builtin_param_types(BuiltinMethod builtin, const Type* receiver);
    const Type* builtin_result_type(BuiltinMethod builtin, const Type* receiver);
    bool check_list_for(ForStmt& stmt);

    // Tuples (sema_tuple.cpp)
    const Type* resolve_tuple_type(const TypeExpr& type);
    const Type* tuple_type(const std::vector<const Type*>& elements);
    const Type* check_tuple(TupleExpr& tuple, const Type* expected);
    const Type* check_tuple_member(MemberExpr& member, const Type& tuple);
    void check_destructuring_let(LetStmt& let);
    void declare_let_pattern(Pattern& pattern, const Type* type, bool is_mutable);
    void declare_struct_pattern(Pattern& pattern, const Type* type, bool is_mutable);
    const UserType* struct_pattern_type(const Pattern& pattern, const Type* type);
    void check_tuple_pattern(Pattern& pattern, const Type* subject);

    // match (sema_match.cpp)
    bool check_match_statement(MatchStmt& stmt);
    const Type* check_match_expression(MatchExpr& match, const Type* expected);
    const Type* check_match_subject(MatchExpr& match);
    const Type* check_arm_value(MatchArm& arm, const Type* subject, const Type* expected);
    void check_arm_head(MatchArm& arm, const Type* subject);
    void check_pattern(Pattern& pattern, const Type* subject);
    void check_case_pattern(Pattern& pattern, const Type* subject);
    void check_literal_pattern(Pattern& pattern, const Type* subject);
    void check_match_coverage(const MatchExpr& match, const Type* subject);
    bool check_jump(Stmt& stmt, const char* keyword);
    void check_condition(Expr& condition);
    const Type* check_assignment_target(Expr& target);
    void require_mutable(const Place& place, MutationKind kind, const std::string& method_name);

    // Expressions; each returns the type and also stores it in expr.type.
    const Type* check_expr(Expr& expr, const Type* expected);
    const Type* check_expr_value(Expr& expr, const Type* expected);
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
    const Type* check_struct_literal(StructLiteralExpr& lit, const Type* expected);
    const std::vector<FieldInfo>* literal_fields(StructLiteralExpr& lit, const UserType& type);
    void report_missing_fields(const StructLiteralExpr& lit, const std::vector<FieldInfo>& fields,
                               const std::string& written);
    static std::string type_name_hint(const UserType& info);
    const Type* check_member(MemberExpr& member, const Type* expected);
    const Type* check_static_member(MemberExpr& member, const UserType& generic,
                                    const Type* expected);
    const UserType* static_type_name(const Expr& object) const;
    const Type* check_call(CallExpr& call, const Type* expected);
    const Type* check_function_call(CallExpr& call, NameExpr& callee, const Type* expected);
    const Type* check_method_call(CallExpr& call, MemberExpr& callee, const Type* expected);
    const FunctionSymbol* find_static_function(MemberExpr& callee, const UserType& info);
    const FunctionSymbol* find_method(MemberExpr& callee, const Type* receiver);
    static BuiltinMethod builtin_method(const Type* receiver, const std::string& name);
    const Type* check_builtin_method_call(CallExpr& call, MemberExpr& callee,
                                          BuiltinMethod builtin, const Type* receiver);
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
    VarSymbol* declare_var(const std::string& name, const Type* type, VarKind kind,
                           bool is_mutable, const SourceLocation& location);
    const VarSymbol* lookup_var(const std::string& name) const;
    const FunctionSymbol* lookup_function(const std::string& name) const;
    UserType* lookup_type(const std::string& name) const;

    // A 'let' or 'for' whose variable takes its type from its first use.
    struct PendingVar {
        const Stmt* stmt;
        bool is_float;
    };

    // Points at a scratch engine while inference passes run.
    DiagnosticEngine* diagnostics_;
    std::unique_ptr<Program> program_;
    std::unordered_map<std::string, const FunctionSymbol*> functions_;
    std::unordered_map<std::string, UserType*> types_;
    std::map<std::vector<const Type*>, const Type*> tuple_types_;
    std::unordered_map<const Type*, const Type*> option_types_;
    std::unordered_map<const Type*, const Type*> list_types_;
    std::unordered_map<std::string, const TraitInfo*> traits_;
    const UserType* result_type_ = nullptr; // 'Result<T, E>' of the prelude
    std::vector<std::unordered_map<std::string, const Type*>> type_scopes_;
    std::map<std::pair<const UserType*, std::vector<const Type*>>, const Type*> type_instances_;
    std::map<std::pair<const FunctionSymbol*, std::vector<const Type*>>, const FunctionSymbol*>
        function_instances_;
    std::deque<FnDecl*> pending_bodies_;
    // Instances made before the methods of generic types were declared get
    // their methods once those are.
    bool methods_declared_ = false;
    std::vector<UserType*> waiting_for_methods_;
    std::deque<PendingDefault> pending_defaults_;
    std::vector<std::unordered_map<std::string, const VarSymbol*>> scopes_;
    std::vector<LoopContext> loops_;
    const Type* return_type_ = nullptr;

    std::vector<PendingVar> pending_;
    std::unordered_map<const Stmt*, const Type*> var_types_;
    std::unordered_map<const Stmt*, const Type*> var_placeholders_;
    std::unordered_map<const Type*, const Stmt*> placeholder_vars_;
    std::deque<Type> placeholder_storage_;
    bool inferring_ = false;
    bool bound_new_ = false;
};

} // namespace hana::lang
