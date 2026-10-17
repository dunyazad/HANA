#pragma once

// The code generator class shared by the codegen source files. Internal to
// src/codegen; the public interface is codegen.h.
//
//   codegen.cpp         types, functions, statements, expressions
//   codegen_match.cpp   'match': pattern tests, bindings, arms
//   codegen_memory.cpp  ownership: scopes, temporaries, moves, drop and
//                       clone glue, 'str' operations
//   codegen_list.cpp    List<T>: values, elements, methods, 'for x in xs'

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "codegen/codegen.h"
#include "lang/ast.h"
#include "lang/symbols.h"

namespace hana::codegen {

using namespace hana::lang;

// Symbol names of HANA functions get a prefix that is not a valid C
// identifier, so they never collide with C library symbols.
std::string mangle_function_name(const std::string& name);

// Read parameters of a struct, enum, tuple or Option type that is not Copy
// are passed by pointer, so the value is not copied; inout parameters
// always are. Everything else, Copy values included, is passed by value: a
// Copy argument is read before the call, so it stays correct when it
// overlaps an inout argument.
bool passed_by_pointer(ParamConvention convention, const Type* type);

struct LoopTargets {
    llvm::BasicBlock* break_block;
    llvm::BasicBlock* continue_block;
    std::size_t scope_depth; // scopes open outside the loop body
    std::size_t temp_depth;  // full expressions open outside the loop body
};

// True for a variable or a field path of one, also when the expression is
// wrapped into an Option (place_of says no for those: the Option is new).
bool is_place_expr(const Expr& expr);

// How an expression's value is used. A read leaves ownership where it is
// (a temporary is freed at the end of its full expression); a consume takes
// ownership (a variable it comes from is no longer dropped). The contexts
// match the move checker: 'let', assignment, 'return', 'sink', fields of
// new values, 'Some(...)' and values wrapped into an Option consume.
enum class Use {
    Read,
    Consume,
};

// A value to drop when its scope ends: a variable, or a temporary of a
// full expression. 'flag' is an i1 slot telling whether the value is still
// owned (it may have been moved); null means always owned. 'start', for a
// list taken apart by 'for sink', is an i64 slot holding the first element
// the list still owns.
struct OwnedSlot {
    llvm::Value* address;
    const Type* type;
    llvm::Value* flag;
    llvm::Value* start = nullptr;
};

// One step from a value to a part of it.
struct PathStep {
    enum class Kind {
        Field,       // a struct field or tuple element
        CaseField,   // a field of the active case of an enum
        OptionValue, // the value inside a Some
    };
    Kind kind = Kind::Field;
    unsigned index = 0;
    int case_index = -1;
};

// A name bound by a pattern on a variable: it is the part 'path' of 'root'.
struct AliasInfo {
    const VarSymbol* root;
    std::vector<PathStep> path;
};

class CodeGenerator {
public:
    CodeGenerator(llvm::LLVMContext& context, llvm::Module& module, const Program& program,
                  std::string source_path, const CodegenOptions& options);
    void generate(const lang::Module& ast);

private:
    // Types and declarations
    llvm::Type* llvm_type(const Type* type);
    llvm::StructType* struct_type(const UserType& info);
    llvm::StructType* enum_type(const UserType& info);
    llvm::StructType* case_type(const UserType& info, const EnumCase& entry);
    llvm::StructType* tuple_type(const Type& tuple);
    llvm::StructType* option_type(const Type& option);
    llvm::Type* param_llvm_type(ParamConvention convention, const Type* type);
    void declare_function(const FnDecl& decl);

    // Functions
    void emit_function(const FnDecl& decl);
    void bind_parameters(const FnDecl& decl, llvm::Function& function);
    void bind_parameter(ParamConvention convention, const VarSymbol* symbol, const std::string& name,
                        llvm::Argument& arg);
    void terminate_open_blocks(llvm::Function& function);
    void emit_c_main();
    llvm::AllocaInst* create_entry_alloca(const Type* type, const std::string& name);
    llvm::AllocaInst* create_entry_alloca(llvm::Type* type, const std::string& name);
    void ensure_insertion_point();

    // Statements
    void emit_block(const Block& block);
    void emit_statement(const Stmt& stmt);
    void emit_let(const LetStmt& let);
    void emit_destructuring_let(const LetStmt& let);
    void emit_assign(const AssignStmt& assign);
    void emit_return(const ReturnStmt& ret);
    void emit_jump(const Stmt& stmt);
    void emit_if(const IfStmt& stmt);
    void emit_while(const WhileStmt& stmt);
    void emit_for(const ForStmt& stmt);
    llvm::Value* range_has_values(const Type* type, llvm::Value* start, llvm::Value* end,
                                  bool inclusive);
    void branch_if_open(llvm::BasicBlock* target);

    // match (codegen_match.cpp)
    llvm::Value* emit_match(const MatchExpr& match, bool is_expression, Use use);
    void emit_match_arm(const MatchArm& arm, const MatchExpr& match, bool is_expression, Use use,
                        llvm::Value* subject, bool owns_subject, llvm::Value* result,
                        llvm::Value* result_owned, llvm::BasicBlock* end_block);
    void emit_arm_value(const MatchArm& arm, Use use, llvm::Value* result,
                        llvm::Value* result_owned, const std::vector<OwnedSlot>& arm_owned);
    llvm::Value* emit_subject_address(const Expr& subject, bool& owns_subject);
    void record_aliases(const Pattern& pattern, const Expr& subject);
    void collect_alias_paths(const Pattern& pattern, const Type* type, std::vector<PathStep>& path,
                             const AliasInfo& base);
    void emit_pattern_test(const Pattern& pattern, llvm::Value* address, const Type* type,
                           llvm::BasicBlock* fail);
    void emit_range_test(const Pattern& pattern, llvm::Value* address, const Type* type,
                         llvm::BasicBlock* fail);
    void emit_pattern_bindings(const Pattern& pattern, llvm::Value* address, const Type* type);
    void own_pattern_bindings(const Pattern& pattern);
    llvm::Value* option_has_value(const Type* option, llvm::Value* address);
    llvm::Value* option_value_address(const Type* option, llvm::Value* address);
    llvm::Value* tuple_element_address(const Type* tuple, llvm::Value* address, std::size_t index);
    llvm::Value* case_field_address(const UserType& info, const EnumCase& entry,
                                    llvm::Value* address, std::size_t index);
    llvm::Constant* pattern_constant(const Expr& literal, const Type* type);
    void continue_if(llvm::Value* condition, llvm::BasicBlock* fail);

    // Expressions
    llvm::Value* emit_value(const Expr& expr, Use use);
    llvm::Value* emit_owned_value(const Expr& expr, Use use);
    llvm::Value* emit_some(const Type& option, llvm::Value* value);
    llvm::Value* emit_expr_value(const Expr& expr);
    llvm::Value* emit_address(const Expr& place);
    llvm::Value* emit_member(const MemberExpr& member);
    llvm::Value* emit_struct_literal(const StructLiteralExpr& lit);
    std::vector<llvm::Value*> emit_literal_fields(const StructLiteralExpr& lit,
                                                  const std::vector<FieldInfo>& fields);
    llvm::Value* emit_enum_value(const UserType& info, int case_index,
                                 const std::vector<llvm::Value*>& fields);
    llvm::Value* emit_tuple(const TupleExpr& tuple);
    llvm::Value* emit_pointer_argument(const Expr& arg);
    llvm::Value* emit_argument(ParamConvention convention, const Type* param_type, const Expr& arg);
    llvm::Value* emit_receiver(ParamConvention convention, const Expr& object);
    llvm::Value* emit_int_literal(const IntLiteralExpr& lit);
    llvm::Constant* emit_string_constant(const std::string& text);
    llvm::Value* emit_unary(const UnaryExpr& unary);
    llvm::Value* emit_binary(const BinaryExpr& binary);
    llvm::Value* emit_null_comparison(const BinaryExpr& binary);
    llvm::Value* emit_logical(const BinaryExpr& binary);
    llvm::Value* emit_comparison(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs);
    static int comparison_index(BinaryOp op);
    llvm::Value* emit_arithmetic(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs,
                                 const SourceLocation& location);
    llvm::Value* emit_overflow_checked(BinaryOp op, const Type* type, llvm::Value* lhs,
                                       llvm::Value* rhs, const SourceLocation& location);
    llvm::Value* emit_conditional(const ConditionalExpr& cond, Use use);
    llvm::Value* emit_branch_value(const Expr& expr, Use use, llvm::Value*& owned);
    llvm::Value* emit_cast(const CastExpr& cast);
    llvm::Value* emit_checked_division(BinaryOp op, const Type* type, llvm::Value* lhs,
                                       llvm::Value* rhs, const SourceLocation& location);
    llvm::Value* emit_shift(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs);
    llvm::Value* emit_call(const CallExpr& call);
    llvm::Value* emit_builtin_method(const CallExpr& call, const MemberExpr& callee);

    // print / write
    void emit_print(const Expr& arg, bool newline);
    void emit_print_text(const std::string& text);
    void emit_print_value(const Expr& expr);

    // Ownership (codegen_memory.cpp)
    void push_scope();
    void pop_scope();
    void own_variable(const VarSymbol* var, llvm::Value* address);
    void drop_slots(const std::vector<OwnedSlot>& slots);
    void drop_scopes_from(std::size_t depth);
    void begin_full_expression();
    void end_full_expression();
    void drop_temporaries_from(std::size_t depth);
    llvm::Value* keep_temporary(llvm::Value* value, const Type* type, llvm::Value* owned);
    llvm::Value* emit_move_out(const Expr& place);
    llvm::Value* emit_move_out_of_temporary(const MemberExpr& member);
    void release_place(const Expr& place);
    void release_variable(const VarSymbol* var, const std::vector<PathStep>& path);
    void drop_except(llvm::Value* address, const Type* type, const std::vector<PathStep>& path,
                     std::size_t from);
    void drop_unbound(const Pattern& pattern, llvm::Value* address, const Type* type);
    static bool binds_owned_value(const Pattern& pattern, const Type* type);
    void emit_drop(llvm::Value* address, const Type* type);
    llvm::Value* emit_clone(llvm::Value* address, const Type* type);
    llvm::Function* drop_function(const Type* type);
    llvm::Function* clone_function(const Type* type);
    void emit_glue_bodies();
    void emit_drop_body(const Type* type, llvm::Function& function);
    void emit_drop_enum(const UserType& info, llvm::Value* address);
    void emit_clone_body(const Type* type, llvm::Function& function);
    llvm::Value* emit_clone_enum(const UserType& info, llvm::Value* address);
    void store_with_drop(const Expr& target, llvm::Value* address, llvm::Value* value);

    // 'str' (codegen_memory.cpp)
    llvm::Value* emit_str_concat(llvm::Value* lhs, llvm::Value* rhs);
    llvm::Value* emit_str_compare(BinaryOp op, llvm::Value* lhs, llvm::Value* rhs);
    void emit_str_append(llvm::Value* target, llvm::Value* value);
    llvm::Value* emit_format_string(const StringLiteralExpr& lit);
    void emit_str_push_value(llvm::Value* target, const Expr& part);

    // List<T> (codegen_list.cpp)
    llvm::Value* list_data(llvm::Value* list_address);
    llvm::Value* list_length(llvm::Value* list_address);
    void set_list_length(llvm::Value* list_address, llvm::Value* length);
    llvm::Value* element_size(const Type* list);
    llvm::Value* element_address(const Type* list, llvm::Value* data, llvm::Value* position);
    void reserve_elements(llvm::Value* list_address, const Type* list, llvm::Value* extra);
    void emit_counted_loop(llvm::Value* begin, llvm::Value* end,
                           const std::function<void(llvm::Value*)>& body);
    void emit_bounds_check(llvm::Value* position, llvm::Value* length, bool allow_end,
                           const SourceLocation& location);
    llvm::Value* emit_position(const Expr& index);
    llvm::Value* emit_list(const ListExpr& list);
    llvm::Value* emit_element_address(const IndexExpr& index);
    llvm::Value* emit_list_method(const CallExpr& call, const MemberExpr& callee);
    void emit_list_push(llvm::Value* address, const Type* list, llvm::Value* value);
    llvm::Value* emit_list_pop(llvm::Value* address, const Type* list, const Type* option);
    void emit_list_insert(llvm::Value* address, const Type* list, llvm::Value* position,
                          llvm::Value* value, const SourceLocation& location);
    llvm::Value* emit_list_remove(llvm::Value* address, const Type* list, llvm::Value* position,
                                  const SourceLocation& location);
    void emit_drop_elements(llvm::Value* address, const Type* list, llvm::Value* from);
    void emit_drop_list(llvm::Value* address, const Type* list, llvm::Value* from);
    llvm::Value* emit_clone_list(llvm::Value* address, const Type* list);
    void emit_list_for(const ForStmt& stmt);
    void bind_loop_element(const ForStmt& stmt, llvm::Value* element);

    // Runtime support (runtime/hana_rt.h)
    llvm::FunctionCallee runtime_function(const std::string& name);
    void emit_panic_if(llvm::Value* condition, const std::string& message,
                       const SourceLocation& location);

    llvm::LLVMContext& context_;
    llvm::Module& module_;
    llvm::IRBuilder<> builder_;
    const Program& program_;
    std::string source_path_;
    CodegenOptions options_;
    llvm::StructType* str_type_ = nullptr;
    llvm::StructType* list_type_ = nullptr;
    llvm::Function* current_function_ = nullptr;
    const Type* current_return_type_ = nullptr;
    std::unordered_map<const FunctionSymbol*, llvm::Function*> functions_;
    std::unordered_map<const UserType*, llvm::StructType*> user_types_;
    std::unordered_map<const EnumCase*, llvm::StructType*> case_types_;
    std::unordered_map<const VarSymbol*, llvm::Value*> addresses_;
    std::vector<LoopTargets> loops_;

    // Ownership state of the function being generated.
    std::vector<std::vector<OwnedSlot>> scopes_;
    std::vector<std::vector<OwnedSlot>> temporaries_;
    std::unordered_map<const VarSymbol*, llvm::Value*> drop_flags_;
    std::unordered_map<const VarSymbol*, AliasInfo> aliases_;

    // Drop and clone functions, made on first use; bodies are emitted after
    // the HANA functions.
    std::unordered_map<const Type*, llvm::Function*> drop_functions_;
    std::unordered_map<const Type*, llvm::Function*> clone_functions_;
    std::vector<const Type*> pending_drops_;
    std::vector<const Type*> pending_clones_;
};

} // namespace hana::codegen
