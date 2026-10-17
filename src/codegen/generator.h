#pragma once

// The code generator class shared by the codegen source files. Internal to
// src/codegen; the public interface is codegen.h.

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
};

class CodeGenerator {
public:
    CodeGenerator(llvm::LLVMContext& context, llvm::Module& module, const Program& program,
                  std::string source_path, const CodegenOptions& options);
    void generate(const lang::Module& ast);
private:
    llvm::Type* llvm_type(const Type* type);
    llvm::StructType* struct_type(const UserType& info);
    llvm::StructType* enum_type(const UserType& info);
    llvm::StructType* case_type(const UserType& info, const EnumCase& entry);
    llvm::StructType* tuple_type(const Type& tuple);
    llvm::StructType* option_type(const Type& option);
    llvm::Type* param_llvm_type(ParamConvention convention, const Type* type);
    void declare_function(const FnDecl& decl);
    void emit_function(const FnDecl& decl);
    void bind_parameters(const FnDecl& decl, llvm::Function& function);
    void bind_parameter(ParamConvention convention, const VarSymbol* symbol, const std::string& name,
                        llvm::Argument& arg);
    void terminate_open_blocks(llvm::Function& function);
    void emit_c_main();
    llvm::AllocaInst* create_entry_alloca(const Type* type, const std::string& name);
    void ensure_insertion_point();
    void emit_block(const Block& block);
    void emit_statement(const Stmt& stmt);
    void emit_let(const LetStmt& let);
    void emit_assign(const AssignStmt& assign);
    void emit_return(const ReturnStmt& ret);
    void emit_if(const IfStmt& stmt);
    void emit_while(const WhileStmt& stmt);
    void emit_for(const ForStmt& stmt);
    llvm::Value* range_has_values(const Type* type, llvm::Value* start, llvm::Value* end,
                                  bool inclusive);
    llvm::Value* emit_match(const MatchExpr& match, bool is_expression);
    llvm::Value* emit_subject_address(const Expr& subject);
    void emit_pattern_test(const Pattern& pattern, llvm::Value* address, const Type* type,
                           llvm::BasicBlock* fail);
    void emit_range_test(const Pattern& pattern, llvm::Value* address, const Type* type,
                         llvm::BasicBlock* fail);
    void emit_pattern_bindings(const Pattern& pattern, llvm::Value* address, const Type* type);
    llvm::Value* option_has_value(const Type* option, llvm::Value* address);
    llvm::Value* option_value_address(const Type* option, llvm::Value* address);
    llvm::Value* tuple_element_address(const Type* tuple, llvm::Value* address, std::size_t index);
    llvm::Value* case_field_address(const UserType& info, const EnumCase& entry,
                                    llvm::Value* address, std::size_t index);
    llvm::Constant* pattern_constant(const Expr& literal, const Type* type);
    void continue_if(llvm::Value* condition, llvm::BasicBlock* fail);
    void branch_if_open(llvm::BasicBlock* target);
    llvm::Value* emit_expr(const Expr& expr);
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
    llvm::Value* emit_conditional(const ConditionalExpr& cond);
    llvm::Value* emit_cast(const CastExpr& cast);
    llvm::Value* emit_checked_division(BinaryOp op, const Type* type, llvm::Value* lhs,
                                       llvm::Value* rhs, const SourceLocation& location);
    llvm::Value* emit_shift(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs);
    llvm::Value* emit_call(const CallExpr& call);
    void emit_print(const Expr& arg, bool newline);
    void emit_print_text(const std::string& text);
    void emit_print_value(const Expr& expr);
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
    llvm::Function* current_function_ = nullptr;
    const Type* current_return_type_ = nullptr;
    std::unordered_map<const FunctionSymbol*, llvm::Function*> functions_;
    std::unordered_map<const UserType*, llvm::StructType*> user_types_;
    std::unordered_map<const EnumCase*, llvm::StructType*> case_types_;
    std::unordered_map<const VarSymbol*, llvm::Value*> addresses_;
    std::vector<LoopTargets> loops_;
};

} // namespace hana::codegen
