#include "codegen/codegen.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <optional>
#include <unordered_map>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include "lang/sema.h"

namespace hana::codegen {

using namespace hana::lang;

namespace {

// Symbol names of HANA functions get a prefix that is not a valid C
// identifier, so they never collide with C library symbols.
std::string mangle_function_name(const std::string& name)
{
    return "hana." + name;
}

// Read parameters of a struct, enum, tuple or Option type that is not Copy
// are passed by pointer, so the value is not copied; inout parameters
// always are. Everything else, Copy values included, is passed by value: a
// Copy argument is read before the call, so it stays correct when it
// overlaps an inout argument.
bool passed_by_pointer(ParamConvention convention, const Type* type)
{
    return convention == ParamConvention::Inout ||
           (convention == ParamConvention::Read &&
            (type->is_user() || type->is_tuple() || type->is_option()) && !is_copy_type(type));
}

struct LoopTargets {
    llvm::BasicBlock* break_block;
    llvm::BasicBlock* continue_block;
};

class CodeGenerator {
public:
    CodeGenerator(llvm::LLVMContext& context, llvm::Module& module, const Program& program,
                  std::string source_path, const CodegenOptions& options)
        : context_(context)
        , module_(module)
        , builder_(context)
        , program_(program)
        , source_path_(std::move(source_path))
        , options_(options)
    {
        str_type_ = llvm::StructType::create(
            context_, {llvm::PointerType::get(context_, 0), llvm::Type::getInt64Ty(context_)},
            "hana.str");
    }

    void generate(const lang::Module& ast)
    {
        for (const auto& function : ast.functions) {
            declare_function(*function);
        }
        for (const auto& decl : ast.types) {
            for (const auto& method : decl->methods) {
                declare_function(*method);
            }
        }
        for (const auto& function : ast.functions) {
            emit_function(*function);
        }
        for (const auto& decl : ast.types) {
            for (const auto& method : decl->methods) {
                emit_function(*method);
            }
        }
        if (program_.main != nullptr) {
            emit_c_main();
        }
    }

private:
    // -----------------------------------------------------------------------
    // Types and declarations

    llvm::Type* llvm_type(const Type* type)
    {
        switch (type->kind) {
        case TypeKind::Void: return llvm::Type::getVoidTy(context_);
        case TypeKind::Bool: return llvm::Type::getInt1Ty(context_);
        case TypeKind::Char: return llvm::Type::getInt32Ty(context_);
        case TypeKind::Str: return str_type_;
        case TypeKind::Int:
            if (type->is_pointer_sized) {
                return module_.getDataLayout().getIntPtrType(context_);
            }
            return llvm::Type::getIntNTy(context_, type->bits);
        case TypeKind::Float:
            return type->bits == 32 ? llvm::Type::getFloatTy(context_)
                                    : llvm::Type::getDoubleTy(context_);
        case TypeKind::Struct: return struct_type(*type->user);
        case TypeKind::Enum: return enum_type(*type->user);
        case TypeKind::Tuple: return tuple_type(*type);
        case TypeKind::Option: return option_type(*type);
        case TypeKind::Error: break;
        }
        return llvm::Type::getVoidTy(context_);
    }

    // Structs cannot contain themselves (checked by semantic analysis), so
    // the field types are complete when the body is set.
    llvm::StructType* struct_type(const UserType& info)
    {
        if (auto it = user_types_.find(&info); it != user_types_.end()) {
            return it->second;
        }
        std::vector<llvm::Type*> fields;
        for (const FieldInfo& field : info.fields) {
            fields.push_back(llvm_type(field.type));
        }
        llvm::StructType* type = llvm::StructType::create(context_, fields, info.name);
        user_types_[&info] = type;
        return type;
    }

    // An enum value is { i32 tag, [N x i64] payload } where the payload holds
    // the fields of the largest case. No type needs more than 8-byte
    // alignment yet, so i64 elements align every case.
    llvm::StructType* enum_type(const UserType& info)
    {
        if (auto it = user_types_.find(&info); it != user_types_.end()) {
            return it->second;
        }
        std::uint64_t payload_size = 0;
        for (const EnumCase& entry : info.cases) {
            if (!entry.fields.empty()) {
                payload_size = std::max<std::uint64_t>(
                    payload_size, module_.getDataLayout().getTypeAllocSize(case_type(info, entry)));
            }
        }
        std::vector<llvm::Type*> fields{builder_.getInt32Ty()};
        if (payload_size > 0) {
            fields.push_back(llvm::ArrayType::get(builder_.getInt64Ty(), (payload_size + 7) / 8));
        }
        llvm::StructType* type = llvm::StructType::create(context_, fields, info.name);
        user_types_[&info] = type;
        return type;
    }

    // The fields of one case, stored at the start of the payload.
    llvm::StructType* case_type(const UserType& info, const EnumCase& entry)
    {
        if (auto it = case_types_.find(&entry); it != case_types_.end()) {
            return it->second;
        }
        std::vector<llvm::Type*> fields;
        for (const FieldInfo& field : entry.fields) {
            fields.push_back(llvm_type(field.type));
        }
        llvm::StructType* type =
            llvm::StructType::create(context_, fields, info.name + "." + entry.name);
        case_types_[&entry] = type;
        return type;
    }

    // A tuple is an unnamed LLVM struct of its elements.
    llvm::StructType* tuple_type(const Type& tuple)
    {
        std::vector<llvm::Type*> elements;
        for (const Type* element : tuple.elements) {
            elements.push_back(llvm_type(element));
        }
        return llvm::StructType::get(context_, elements);
    }

    // An Option<T> is { i1 has_value, T value }; 'null' is all zeros.
    llvm::StructType* option_type(const Type& option)
    {
        return llvm::StructType::get(context_,
                                     {builder_.getInt1Ty(), llvm_type(option.option_value())});
    }

    llvm::Type* param_llvm_type(ParamConvention convention, const Type* type)
    {
        return passed_by_pointer(convention, type) ? llvm::PointerType::get(context_, 0)
                                                   : llvm_type(type);
    }

    // A method takes 'self' as its first LLVM parameter.
    void declare_function(const FnDecl& decl)
    {
        const FunctionSymbol& symbol = *decl.symbol;
        std::vector<llvm::Type*> params;
        if (symbol.self_convention) {
            params.push_back(param_llvm_type(*symbol.self_convention, decl.owner->type));
        }
        for (std::size_t i = 0; i < symbol.param_types.size(); ++i) {
            params.push_back(param_llvm_type(symbol.conventions[i], symbol.param_types[i]));
        }
        auto* type = llvm::FunctionType::get(llvm_type(symbol.return_type), params, false);
        functions_[&symbol] = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                     mangle_function_name(symbol.name), module_);
    }

    // -----------------------------------------------------------------------
    // Functions

    void emit_function(const FnDecl& decl)
    {
        llvm::Function* function = functions_.at(decl.symbol);
        current_function_ = function;
        current_return_type_ = decl.symbol->return_type;
        builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function));
        bind_parameters(decl, *function);
        emit_block(decl.body);
        terminate_open_blocks(*function);
        addresses_.clear();
    }

    void bind_parameters(const FnDecl& decl, llvm::Function& function)
    {
        unsigned index = 0;
        if (decl.self_convention) {
            bind_parameter(*decl.self_convention, decl.self_symbol, "self", *function.getArg(index));
            ++index;
        }
        for (const Param& param : decl.params) {
            bind_parameter(param.convention, param.symbol, param.name, *function.getArg(index));
            ++index;
        }
    }

    void bind_parameter(ParamConvention convention, const VarSymbol* symbol, const std::string& name,
                        llvm::Argument& arg)
    {
        arg.setName(name);
        if (passed_by_pointer(convention, symbol->type)) {
            addresses_[symbol] = &arg;
            return;
        }
        llvm::AllocaInst* slot = create_entry_alloca(symbol->type, name);
        builder_.CreateStore(&arg, slot);
        addresses_[symbol] = slot;
    }

    // Blocks left open are unreachable or the end of a void function.
    void terminate_open_blocks(llvm::Function& function)
    {
        for (llvm::BasicBlock& block : function) {
            if (block.getTerminator() != nullptr) {
                continue;
            }
            builder_.SetInsertPoint(&block);
            if (current_return_type_->is_void()) {
                builder_.CreateRetVoid();
            } else {
                builder_.CreateUnreachable();
            }
        }
    }

    void emit_c_main()
    {
        auto* type = llvm::FunctionType::get(llvm::Type::getInt32Ty(context_), false);
        auto* c_main = llvm::Function::Create(type, llvm::Function::ExternalLinkage, "main", module_);
        builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", c_main));
        llvm::Value* result = builder_.CreateCall(functions_.at(program_.main));
        if (program_.main->return_type->is_void()) {
            builder_.CreateRet(builder_.getInt32(0));
        } else {
            builder_.CreateRet(result);
        }
    }

    llvm::AllocaInst* create_entry_alloca(const Type* type, const std::string& name)
    {
        llvm::IRBuilder<> entry_builder(&current_function_->getEntryBlock(),
                                        current_function_->getEntryBlock().begin());
        return entry_builder.CreateAlloca(llvm_type(type), nullptr, name);
    }

    // Code after return/break/continue is unreachable but still emitted into
    // a fresh block so the builder always has a valid insertion point.
    void ensure_insertion_point()
    {
        if (builder_.GetInsertBlock()->getTerminator() != nullptr) {
            builder_.SetInsertPoint(
                llvm::BasicBlock::Create(context_, "unreachable", current_function_));
        }
    }

    // -----------------------------------------------------------------------
    // Statements

    void emit_block(const Block& block)
    {
        for (const auto& statement : block.statements) {
            ensure_insertion_point();
            emit_statement(*statement);
        }
    }

    void emit_statement(const Stmt& stmt)
    {
        switch (stmt.kind) {
        case StmtKind::Let: emit_let(static_cast<const LetStmt&>(stmt)); break;
        case StmtKind::Assign: emit_assign(static_cast<const AssignStmt&>(stmt)); break;
        case StmtKind::Expr: emit_expr(*static_cast<const ExprStmt&>(stmt).expr); break;
        case StmtKind::Return: emit_return(static_cast<const ReturnStmt&>(stmt)); break;
        case StmtKind::If: emit_if(static_cast<const IfStmt&>(stmt)); break;
        case StmtKind::While: emit_while(static_cast<const WhileStmt&>(stmt)); break;
        case StmtKind::For: emit_for(static_cast<const ForStmt&>(stmt)); break;
        case StmtKind::Match: emit_match(*static_cast<const MatchStmt&>(stmt).match, false); break;
        case StmtKind::Break: builder_.CreateBr(loops_.back().break_block); break;
        case StmtKind::Continue: builder_.CreateBr(loops_.back().continue_block); break;
        case StmtKind::Pass: break;
        }
    }

    void emit_let(const LetStmt& let)
    {
        if (let.pattern) {
            // 'let (a, b) = value': each name gets its part of the value.
            llvm::Value* address = emit_subject_address(*let.init);
            emit_pattern_bindings(*let.pattern, address, let.init->type);
            return;
        }
        llvm::Value* value = emit_expr(*let.init);
        llvm::AllocaInst* slot = create_entry_alloca(let.symbol->type, let.name);
        builder_.CreateStore(value, slot);
        addresses_[let.symbol] = slot;
    }

    void emit_assign(const AssignStmt& assign)
    {
        llvm::Value* address = emit_address(*assign.target);
        const Type* type = assign.target->type;
        llvm::Value* value = emit_expr(*assign.value);
        if (assign.op) {
            llvm::Value* current = builder_.CreateLoad(llvm_type(type), address);
            value = emit_arithmetic(*assign.op, type, current, value, assign.op_location);
        }
        builder_.CreateStore(value, address);
    }

    void emit_return(const ReturnStmt& ret)
    {
        if (ret.value) {
            builder_.CreateRet(emit_expr(*ret.value));
        } else {
            builder_.CreateRetVoid();
        }
    }

    void emit_if(const IfStmt& stmt)
    {
        llvm::Value* condition = emit_expr(*stmt.condition);
        auto* then_block = llvm::BasicBlock::Create(context_, "if.then", current_function_);
        auto* merge_block = llvm::BasicBlock::Create(context_, "if.end");
        llvm::BasicBlock* else_block = merge_block;
        if (stmt.else_block) {
            else_block = llvm::BasicBlock::Create(context_, "if.else", current_function_);
        }
        builder_.CreateCondBr(condition, then_block, else_block);

        builder_.SetInsertPoint(then_block);
        emit_block(stmt.then_block);
        branch_if_open(merge_block);

        if (stmt.else_block) {
            builder_.SetInsertPoint(else_block);
            emit_block(*stmt.else_block);
            branch_if_open(merge_block);
        }
        merge_block->insertInto(current_function_);
        builder_.SetInsertPoint(merge_block);
    }

    void emit_while(const WhileStmt& stmt)
    {
        auto* condition_block = llvm::BasicBlock::Create(context_, "while.cond", current_function_);
        auto* body_block = llvm::BasicBlock::Create(context_, "while.body", current_function_);
        auto* end_block = llvm::BasicBlock::Create(context_, "while.end", current_function_);
        builder_.CreateBr(condition_block);

        builder_.SetInsertPoint(condition_block);
        builder_.CreateCondBr(emit_expr(*stmt.condition), body_block, end_block);

        builder_.SetInsertPoint(body_block);
        loops_.push_back(LoopTargets{end_block, condition_block});
        emit_block(stmt.body);
        loops_.pop_back();
        branch_if_open(condition_block);

        end_block->moveAfter(&current_function_->back());
        builder_.SetInsertPoint(end_block);
    }

    // The loop runs only when the range is not empty, and stops after the
    // body has run with the last value, before stepping past it. So the
    // counter never goes beyond the range and the step cannot overflow,
    // even for '0..=255u8'. 'continue' jumps to the step.
    void emit_for(const ForStmt& stmt)
    {
        const Type* type = stmt.symbol->type;
        llvm::Value* start = emit_expr(*stmt.start);
        llvm::Value* end = emit_expr(*stmt.end);
        llvm::AllocaInst* slot = create_entry_alloca(type, stmt.name);
        builder_.CreateStore(start, slot);
        addresses_[stmt.symbol] = slot;

        auto* body_block = llvm::BasicBlock::Create(context_, "for.body", current_function_);
        auto* step_block = llvm::BasicBlock::Create(context_, "for.step", current_function_);
        auto* next_block = llvm::BasicBlock::Create(context_, "for.next", current_function_);
        auto* end_block = llvm::BasicBlock::Create(context_, "for.end", current_function_);
        // 'end - 1' may wrap for an empty range, but it is used only when the
        // range has values.
        llvm::Value* last =
            stmt.inclusive ? end : builder_.CreateSub(end, llvm::ConstantInt::get(end->getType(), 1));
        builder_.CreateCondBr(range_has_values(type, start, end, stmt.inclusive), body_block,
                              end_block);

        builder_.SetInsertPoint(body_block);
        loops_.push_back(LoopTargets{end_block, step_block});
        emit_block(stmt.body);
        loops_.pop_back();
        branch_if_open(step_block);

        builder_.SetInsertPoint(step_block);
        llvm::Value* current = builder_.CreateLoad(llvm_type(type), slot, stmt.name);
        builder_.CreateCondBr(builder_.CreateICmpEQ(current, last), end_block, next_block);
        builder_.SetInsertPoint(next_block);
        builder_.CreateStore(builder_.CreateAdd(current, llvm::ConstantInt::get(current->getType(), 1)),
                             slot);
        builder_.CreateBr(body_block);

        end_block->moveAfter(&current_function_->back());
        builder_.SetInsertPoint(end_block);
    }

    llvm::Value* range_has_values(const Type* type, llvm::Value* start, llvm::Value* end,
                                  bool inclusive)
    {
        if (type->is_signed) {
            return inclusive ? builder_.CreateICmpSLE(start, end) : builder_.CreateICmpSLT(start, end);
        }
        return inclusive ? builder_.CreateICmpULE(start, end) : builder_.CreateICmpULT(start, end);
    }

    // -----------------------------------------------------------------------
    // match

    // Arms are tried in order: the pattern test, then the bindings, then the
    // guard. Semantic analysis guarantees some arm matches. An expression
    // match stores the arm's value in a slot and returns it.
    llvm::Value* emit_match(const MatchExpr& match, bool is_expression)
    {
        const Type* type = match.subject->type;
        llvm::Value* subject = emit_subject_address(*match.subject);
        llvm::AllocaInst* result = is_expression ? create_entry_alloca(match.type, "match") : nullptr;
        auto* end_block = llvm::BasicBlock::Create(context_, "match.end");
        for (const MatchArm& arm : match.arms) {
            auto* next_block = llvm::BasicBlock::Create(context_, "match.next", current_function_);
            emit_pattern_test(*arm.pattern, subject, type, next_block);
            emit_pattern_bindings(*arm.pattern, subject, type);
            if (arm.guard) {
                continue_if(emit_expr(*arm.guard), next_block);
            }
            if (is_expression) {
                builder_.CreateStore(emit_expr(*arm.value), result);
            } else {
                emit_block(arm.body);
            }
            branch_if_open(end_block);
            builder_.SetInsertPoint(next_block);
        }
        builder_.CreateUnreachable();
        end_block->insertInto(current_function_);
        builder_.SetInsertPoint(end_block);
        return is_expression ? builder_.CreateLoad(llvm_type(match.type), result) : nullptr;
    }

    // A place is matched where it is; a temporary is stored first.
    llvm::Value* emit_subject_address(const Expr& subject)
    {
        if (place_of(subject)) {
            return emit_address(subject);
        }
        llvm::AllocaInst* temp = create_entry_alloca(result_type(subject), "subject");
        builder_.CreateStore(emit_expr(subject), temp);
        return temp;
    }

    // Branches to 'fail' unless the value at 'address' matches the pattern;
    // continues in a new block when it does.
    void emit_pattern_test(const Pattern& pattern, llvm::Value* address, const Type* type,
                           llvm::BasicBlock* fail)
    {
        switch (pattern.kind) {
        case PatternKind::Wildcard:
        case PatternKind::Binding: return;
        case PatternKind::Literal: {
            llvm::Value* value = builder_.CreateLoad(llvm_type(type), address);
            continue_if(builder_.CreateICmpEQ(value, pattern_constant(*pattern.value, type)), fail);
            return;
        }
        case PatternKind::Range: emit_range_test(pattern, address, type, fail); return;
        case PatternKind::Tuple:
            for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
                emit_pattern_test(*pattern.arguments[i], tuple_element_address(type, address, i),
                                  type->elements[i], fail);
            }
            return;
        case PatternKind::Null:
            continue_if(builder_.CreateNot(option_has_value(type, address)), fail);
            return;
        case PatternKind::Case: {
            if (type->is_option()) {
                continue_if(option_has_value(type, address), fail);
                emit_pattern_test(*pattern.arguments.front(), option_value_address(type, address),
                                  type->option_value(), fail);
                return;
            }
            const UserType& info = *type->user;
            llvm::StructType* enum_llvm = enum_type(info);
            llvm::Value* tag = builder_.CreateLoad(builder_.getInt32Ty(),
                                                   builder_.CreateStructGEP(enum_llvm, address, 0));
            continue_if(builder_.CreateICmpEQ(
                            tag, builder_.getInt32(static_cast<std::uint32_t>(pattern.case_index))),
                        fail);
            const EnumCase& entry = info.cases[static_cast<std::size_t>(pattern.case_index)];
            for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
                emit_pattern_test(*pattern.arguments[i], case_field_address(info, entry, address, i),
                                  entry.fields[i].type, fail);
            }
            return;
        }
        }
    }

    void emit_range_test(const Pattern& pattern, llvm::Value* address, const Type* type,
                         llvm::BasicBlock* fail)
    {
        const bool is_signed = type->is_integer() && type->is_signed;
        llvm::Value* value = builder_.CreateLoad(llvm_type(type), address);
        llvm::Value* low = pattern_constant(*pattern.value, type);
        llvm::Value* high = pattern_constant(*pattern.end, type);
        continue_if(is_signed ? builder_.CreateICmpSGE(value, low) : builder_.CreateICmpUGE(value, low),
                    fail);
        llvm::Value* below = nullptr;
        if (pattern.inclusive) {
            below = is_signed ? builder_.CreateICmpSLE(value, high) : builder_.CreateICmpULE(value, high);
        } else {
            below = is_signed ? builder_.CreateICmpSLT(value, high) : builder_.CreateICmpULT(value, high);
        }
        continue_if(below, fail);
    }

    // A name bound by a pattern holds the matched value as it was when the
    // arm was chosen.
    void emit_pattern_bindings(const Pattern& pattern, llvm::Value* address, const Type* type)
    {
        if (pattern.kind == PatternKind::Binding) {
            llvm::AllocaInst* slot = create_entry_alloca(type, pattern.name);
            builder_.CreateStore(builder_.CreateLoad(llvm_type(type), address), slot);
            addresses_[pattern.symbol] = slot;
            return;
        }
        if (pattern.kind == PatternKind::Tuple) {
            for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
                emit_pattern_bindings(*pattern.arguments[i], tuple_element_address(type, address, i),
                                      type->elements[i]);
            }
            return;
        }
        if (pattern.kind != PatternKind::Case) {
            return;
        }
        if (type->is_option()) {
            emit_pattern_bindings(*pattern.arguments.front(), option_value_address(type, address),
                                  type->option_value());
            return;
        }
        const UserType& info = *type->user;
        const EnumCase& entry = info.cases[static_cast<std::size_t>(pattern.case_index)];
        for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
            emit_pattern_bindings(*pattern.arguments[i], case_field_address(info, entry, address, i),
                                  entry.fields[i].type);
        }
    }

    llvm::Value* option_has_value(const Type* option, llvm::Value* address)
    {
        return builder_.CreateLoad(builder_.getInt1Ty(),
                                   builder_.CreateStructGEP(llvm_type(option), address, 0));
    }

    llvm::Value* option_value_address(const Type* option, llvm::Value* address)
    {
        return builder_.CreateStructGEP(llvm_type(option), address, 1);
    }

    llvm::Value* tuple_element_address(const Type* tuple, llvm::Value* address, std::size_t index)
    {
        return builder_.CreateStructGEP(llvm_type(tuple), address, static_cast<unsigned>(index));
    }

    llvm::Value* case_field_address(const UserType& info, const EnumCase& entry,
                                    llvm::Value* address, std::size_t index)
    {
        llvm::Value* payload = builder_.CreateStructGEP(enum_type(info), address, 1);
        return builder_.CreateStructGEP(case_type(info, entry), payload,
                                        static_cast<unsigned>(index));
    }

    // Literal patterns are integers (possibly negated), chars and bools.
    llvm::Constant* pattern_constant(const Expr& literal, const Type* type)
    {
        switch (literal.kind) {
        case ExprKind::BoolLiteral:
            return builder_.getInt1(static_cast<const BoolLiteralExpr&>(literal).value);
        case ExprKind::CharLiteral:
            return builder_.getInt32(static_cast<const CharLiteralExpr&>(literal).value);
        case ExprKind::Unary: {
            const auto& operand =
                static_cast<const IntLiteralExpr&>(*static_cast<const UnaryExpr&>(literal).operand);
            auto* int_type = llvm::cast<llvm::IntegerType>(llvm_type(type));
            llvm::APInt value(int_type->getBitWidth(), operand.value);
            value.negate();
            return llvm::ConstantInt::get(int_type, value);
        }
        default:
            return llvm::ConstantInt::get(llvm_type(type),
                                          static_cast<const IntLiteralExpr&>(literal).value);
        }
    }

    void continue_if(llvm::Value* condition, llvm::BasicBlock* fail)
    {
        auto* next = llvm::BasicBlock::Create(context_, "match.ok", current_function_);
        builder_.CreateCondBr(condition, next, fail);
        builder_.SetInsertPoint(next);
    }

    void branch_if_open(llvm::BasicBlock* target)
    {
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateBr(target);
        }
    }

    // -----------------------------------------------------------------------
    // Expressions

    // The value of the expression, wrapped into an Option where semantic
    // analysis decided so ('let a: i32? = 5').
    llvm::Value* emit_expr(const Expr& expr)
    {
        llvm::Value* value = emit_expr_value(expr);
        return expr.converted_to != nullptr ? emit_some(*expr.converted_to, value) : value;
    }

    llvm::Value* emit_some(const Type& option, llvm::Value* value)
    {
        llvm::Value* result = llvm::PoisonValue::get(llvm_type(&option));
        result = builder_.CreateInsertValue(result, builder_.getInt1(true), 0);
        return builder_.CreateInsertValue(result, value, 1);
    }

    llvm::Value* emit_expr_value(const Expr& expr)
    {
        switch (expr.kind) {
        case ExprKind::IntLiteral: return emit_int_literal(static_cast<const IntLiteralExpr&>(expr));
        case ExprKind::FloatLiteral:
            return llvm::ConstantFP::get(llvm_type(expr.type),
                                         static_cast<const FloatLiteralExpr&>(expr).value);
        case ExprKind::BoolLiteral:
            return builder_.getInt1(static_cast<const BoolLiteralExpr&>(expr).value);
        case ExprKind::CharLiteral:
            return builder_.getInt32(static_cast<const CharLiteralExpr&>(expr).value);
        case ExprKind::StringLiteral:
            return emit_string_constant(static_cast<const StringLiteralExpr&>(expr).texts.front());
        case ExprKind::Name: {
            const auto& name = static_cast<const NameExpr&>(expr);
            return builder_.CreateLoad(llvm_type(expr.type), addresses_.at(name.var), name.name);
        }
        case ExprKind::Unary: return emit_unary(static_cast<const UnaryExpr&>(expr));
        case ExprKind::Binary: return emit_binary(static_cast<const BinaryExpr&>(expr));
        case ExprKind::Conditional:
            return emit_conditional(static_cast<const ConditionalExpr&>(expr));
        case ExprKind::Cast: return emit_cast(static_cast<const CastExpr&>(expr));
        case ExprKind::Call: return emit_call(static_cast<const CallExpr&>(expr));
        case ExprKind::StructLiteral:
            return emit_struct_literal(static_cast<const StructLiteralExpr&>(expr));
        case ExprKind::Member: return emit_member(static_cast<const MemberExpr&>(expr));
        case ExprKind::Match: return emit_match(static_cast<const MatchExpr&>(expr), true);
        case ExprKind::Tuple: return emit_tuple(static_cast<const TupleExpr&>(expr));
        case ExprKind::Null: return llvm::ConstantAggregateZero::get(llvm_type(expr.type));
        case ExprKind::Error:
        case ExprKind::Index:
        case ExprKind::Try: break; // rejected by semantic analysis
        }
        return llvm::UndefValue::get(llvm_type(expr.type));
    }

    // The address of a place: a variable or a field path of one.
    llvm::Value* emit_address(const Expr& place)
    {
        if (place.kind == ExprKind::Name) {
            return addresses_.at(static_cast<const NameExpr&>(place).var);
        }
        const auto& member = static_cast<const MemberExpr&>(place);
        return builder_.CreateStructGEP(llvm_type(member.object->type), emit_address(*member.object),
                                        static_cast<unsigned>(member.field_index), member.member);
    }

    // A field of a place is loaded through its address; a field of a
    // temporary value is extracted from it.
    llvm::Value* emit_member(const MemberExpr& member)
    {
        if (member.case_index >= 0) {
            return emit_enum_value(*member.type->user, member.case_index, {});
        }
        if (place_of(member)) {
            return builder_.CreateLoad(llvm_type(member.type), emit_address(member), member.member);
        }
        return builder_.CreateExtractValue(emit_expr(*member.object),
                                           static_cast<unsigned>(member.field_index));
    }

    llvm::Value* emit_struct_literal(const StructLiteralExpr& lit)
    {
        const UserType& info = *lit.type->user;
        if (lit.case_index >= 0) {
            const EnumCase& entry = info.cases[static_cast<std::size_t>(lit.case_index)];
            return emit_enum_value(info, lit.case_index, emit_literal_fields(lit, entry.fields));
        }
        llvm::Value* value = llvm::PoisonValue::get(llvm_type(lit.type));
        const std::vector<llvm::Value*> fields = emit_literal_fields(lit, info.fields);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            value = builder_.CreateInsertValue(value, fields[i], static_cast<unsigned>(i));
        }
        return value;
    }

    // Written fields run in source order, then the defaults of the fields
    // left out, in declaration order. Returns the values in declaration
    // order.
    std::vector<llvm::Value*> emit_literal_fields(const StructLiteralExpr& lit,
                                                  const std::vector<FieldInfo>& fields)
    {
        std::vector<llvm::Value*> written;
        for (const auto& field : lit.fields) {
            written.push_back(emit_expr(*field.value));
        }
        std::vector<llvm::Value*> values;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const int index = lit.field_order[i];
            values.push_back(index >= 0 ? written[static_cast<std::size_t>(index)]
                                        : emit_expr(*fields[i].default_value));
        }
        return values;
    }

    llvm::Value* emit_enum_value(const UserType& info, int case_index,
                                 const std::vector<llvm::Value*>& fields)
    {
        llvm::StructType* type = enum_type(info);
        llvm::AllocaInst* slot = create_entry_alloca(&info.type, "enum");
        builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(case_index)),
                             builder_.CreateStructGEP(type, slot, 0));
        if (!fields.empty()) {
            const EnumCase& entry = info.cases[static_cast<std::size_t>(case_index)];
            llvm::Value* payload = llvm::PoisonValue::get(case_type(info, entry));
            for (std::size_t i = 0; i < fields.size(); ++i) {
                payload = builder_.CreateInsertValue(payload, fields[i], static_cast<unsigned>(i));
            }
            builder_.CreateStore(payload, builder_.CreateStructGEP(type, slot, 1));
        }
        return builder_.CreateLoad(type, slot);
    }

    llvm::Value* emit_tuple(const TupleExpr& tuple)
    {
        llvm::Value* value = llvm::PoisonValue::get(llvm_type(tuple.type));
        for (std::size_t i = 0; i < tuple.elements.size(); ++i) {
            value = builder_.CreateInsertValue(value, emit_expr(*tuple.elements[i]),
                                               static_cast<unsigned>(i));
        }
        return value;
    }

    // An argument passed by pointer: a place passes its own address, any
    // other value is stored in a temporary first.
    llvm::Value* emit_pointer_argument(const Expr& arg)
    {
        if (place_of(arg)) {
            return emit_address(arg);
        }
        llvm::AllocaInst* temp = create_entry_alloca(result_type(arg), "arg");
        builder_.CreateStore(emit_expr(arg), temp);
        return temp;
    }

    // How the argument is passed follows the parameter's type: the argument
    // may be a value wrapped into the parameter's Option type.
    llvm::Value* emit_argument(ParamConvention convention, const Type* param_type, const Expr& arg)
    {
        if (convention == ParamConvention::Inout) {
            return emit_address(*static_cast<const UnaryExpr&>(arg).operand);
        }
        if (passed_by_pointer(convention, param_type)) {
            return emit_pointer_argument(arg);
        }
        return emit_expr(arg);
    }

    // The value a method is called on; for 'inout self' it is a place
    // written without '&'.
    llvm::Value* emit_receiver(ParamConvention convention, const Expr& object)
    {
        if (convention == ParamConvention::Inout) {
            return emit_address(object);
        }
        if (passed_by_pointer(convention, object.type)) {
            return emit_pointer_argument(object);
        }
        return emit_expr(object);
    }

    llvm::Value* emit_int_literal(const IntLiteralExpr& lit)
    {
        if (lit.type->is_float()) {
            return llvm::ConstantFP::get(llvm_type(lit.type), static_cast<double>(lit.value));
        }
        return llvm::ConstantInt::get(llvm_type(lit.type), lit.value);
    }

    llvm::Constant* emit_string_constant(const std::string& text)
    {
        llvm::Constant* data = builder_.CreateGlobalStringPtr(text, "str", 0, &module_);
        return llvm::ConstantStruct::get(
            str_type_, {data, builder_.getInt64(static_cast<std::uint64_t>(text.size()))});
    }

    llvm::Value* emit_unary(const UnaryExpr& unary)
    {
        llvm::Value* operand = emit_expr(*unary.operand);
        switch (unary.op) {
        case UnaryOp::Neg:
            if (unary.type->is_float()) {
                return builder_.CreateFNeg(operand);
            }
            return emit_overflow_checked(BinaryOp::Sub, unary.type,
                                         llvm::ConstantInt::get(operand->getType(), 0), operand,
                                         unary.location);
        case UnaryOp::Not:
        case UnaryOp::BitNot: return builder_.CreateNot(operand);
        case UnaryOp::InoutMark: break; // rejected by semantic analysis
        }
        return operand;
    }

    llvm::Value* emit_binary(const BinaryExpr& binary)
    {
        if (binary.op == BinaryOp::And || binary.op == BinaryOp::Or) {
            return emit_logical(binary);
        }
        if (result_type(*binary.lhs)->is_option()) {
            return emit_null_comparison(binary);
        }
        llvm::Value* lhs = emit_expr(*binary.lhs);
        llvm::Value* rhs = emit_expr(*binary.rhs);
        const Type* operand_type = binary.lhs->type;
        switch (binary.op) {
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge: return emit_comparison(binary.op, operand_type, lhs, rhs);
        default: return emit_arithmetic(binary.op, operand_type, lhs, rhs, binary.op_location);
        }
    }

    // 'x == null' / 'x != null' test whether the Option has a value. Only
    // the side that is not 'null' is computed.
    llvm::Value* emit_null_comparison(const BinaryExpr& binary)
    {
        const Expr& option = binary.lhs->kind == ExprKind::Null ? *binary.rhs : *binary.lhs;
        llvm::Value* has_value = builder_.CreateExtractValue(emit_expr(option), 0);
        return binary.op == BinaryOp::Eq ? builder_.CreateNot(has_value) : has_value;
    }

    llvm::Value* emit_logical(const BinaryExpr& binary)
    {
        // Short-circuit: the right side runs only when it decides the result.
        const bool is_and = binary.op == BinaryOp::And;
        llvm::Value* lhs = emit_expr(*binary.lhs);
        llvm::BasicBlock* lhs_block = builder_.GetInsertBlock();
        auto* rhs_block = llvm::BasicBlock::Create(context_, is_and ? "and.rhs" : "or.rhs",
                                                   current_function_);
        auto* end_block = llvm::BasicBlock::Create(context_, is_and ? "and.end" : "or.end",
                                                   current_function_);
        if (is_and) {
            builder_.CreateCondBr(lhs, rhs_block, end_block);
        } else {
            builder_.CreateCondBr(lhs, end_block, rhs_block);
        }
        builder_.SetInsertPoint(rhs_block);
        llvm::Value* rhs = emit_expr(*binary.rhs);
        llvm::BasicBlock* rhs_end = builder_.GetInsertBlock();
        builder_.CreateBr(end_block);

        end_block->moveAfter(rhs_end);
        builder_.SetInsertPoint(end_block);
        llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt1Ty(), 2);
        phi->addIncoming(builder_.getInt1(!is_and), lhs_block);
        phi->addIncoming(rhs, rhs_end);
        return phi;
    }

    llvm::Value* emit_comparison(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs)
    {
        if (type->is_float()) {
            static constexpr llvm::CmpInst::Predicate kFloat[] = {
                llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::FCMP_UNE, llvm::CmpInst::FCMP_OLT,
                llvm::CmpInst::FCMP_OLE, llvm::CmpInst::FCMP_OGT, llvm::CmpInst::FCMP_OGE,
            };
            return builder_.CreateFCmp(kFloat[comparison_index(op)], lhs, rhs);
        }
        const bool is_signed = type->is_integer() && type->is_signed;
        static constexpr llvm::CmpInst::Predicate kSigned[] = {
            llvm::CmpInst::ICMP_EQ,  llvm::CmpInst::ICMP_NE,  llvm::CmpInst::ICMP_SLT,
            llvm::CmpInst::ICMP_SLE, llvm::CmpInst::ICMP_SGT, llvm::CmpInst::ICMP_SGE,
        };
        static constexpr llvm::CmpInst::Predicate kUnsigned[] = {
            llvm::CmpInst::ICMP_EQ,  llvm::CmpInst::ICMP_NE,  llvm::CmpInst::ICMP_ULT,
            llvm::CmpInst::ICMP_ULE, llvm::CmpInst::ICMP_UGT, llvm::CmpInst::ICMP_UGE,
        };
        const int index = comparison_index(op);
        return builder_.CreateICmp(is_signed ? kSigned[index] : kUnsigned[index], lhs, rhs);
    }

    static int comparison_index(BinaryOp op)
    {
        switch (op) {
        case BinaryOp::Eq: return 0;
        case BinaryOp::Ne: return 1;
        case BinaryOp::Lt: return 2;
        case BinaryOp::Le: return 3;
        case BinaryOp::Gt: return 4;
        default: return 5;
        }
    }

    llvm::Value* emit_arithmetic(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs,
                                 const SourceLocation& location)
    {
        if (type->is_float()) {
            switch (op) {
            case BinaryOp::Add: return builder_.CreateFAdd(lhs, rhs);
            case BinaryOp::Sub: return builder_.CreateFSub(lhs, rhs);
            case BinaryOp::Mul: return builder_.CreateFMul(lhs, rhs);
            case BinaryOp::Div: return builder_.CreateFDiv(lhs, rhs);
            default: return builder_.CreateFRem(lhs, rhs);
            }
        }
        switch (op) {
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul: return emit_overflow_checked(op, type, lhs, rhs, location);
        case BinaryOp::Div:
        case BinaryOp::Rem: return emit_checked_division(op, type, lhs, rhs, location);
        case BinaryOp::BitAnd: return builder_.CreateAnd(lhs, rhs);
        case BinaryOp::BitOr: return builder_.CreateOr(lhs, rhs);
        case BinaryOp::BitXor: return builder_.CreateXor(lhs, rhs);
        case BinaryOp::Shl:
        case BinaryOp::Shr: return emit_shift(op, type, lhs, rhs);
        default: break;
        }
        return lhs;
    }

    // Debug builds stop with a panic when +, - or * overflow; release builds
    // wrap around (two's complement).
    llvm::Value* emit_overflow_checked(BinaryOp op, const Type* type, llvm::Value* lhs,
                                       llvm::Value* rhs, const SourceLocation& location)
    {
        if (!options_.check_overflow) {
            switch (op) {
            case BinaryOp::Add: return builder_.CreateAdd(lhs, rhs);
            case BinaryOp::Sub: return builder_.CreateSub(lhs, rhs);
            default: return builder_.CreateMul(lhs, rhs);
            }
        }
        llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
        switch (op) {
        case BinaryOp::Add:
            id = type->is_signed ? llvm::Intrinsic::sadd_with_overflow : llvm::Intrinsic::uadd_with_overflow;
            break;
        case BinaryOp::Sub:
            id = type->is_signed ? llvm::Intrinsic::ssub_with_overflow : llvm::Intrinsic::usub_with_overflow;
            break;
        default:
            id = type->is_signed ? llvm::Intrinsic::smul_with_overflow : llvm::Intrinsic::umul_with_overflow;
            break;
        }
        llvm::Value* result = builder_.CreateBinaryIntrinsic(id, lhs, rhs);
        emit_panic_if(builder_.CreateExtractValue(result, 1),
                      std::string("integer overflow in '") + binary_op_spelling(op) + "'", location);
        return builder_.CreateExtractValue(result, 0);
    }

    // 'a if cond else b': only the chosen branch runs.
    llvm::Value* emit_conditional(const ConditionalExpr& cond)
    {
        llvm::Value* condition = emit_expr(*cond.condition);
        auto* then_block = llvm::BasicBlock::Create(context_, "cond.then", current_function_);
        auto* else_block = llvm::BasicBlock::Create(context_, "cond.else", current_function_);
        auto* end_block = llvm::BasicBlock::Create(context_, "cond.end", current_function_);
        builder_.CreateCondBr(condition, then_block, else_block);

        builder_.SetInsertPoint(then_block);
        llvm::Value* then_value = emit_expr(*cond.then_value);
        llvm::BasicBlock* then_end = builder_.GetInsertBlock();
        builder_.CreateBr(end_block);

        builder_.SetInsertPoint(else_block);
        llvm::Value* else_value = emit_expr(*cond.else_value);
        llvm::BasicBlock* else_end = builder_.GetInsertBlock();
        builder_.CreateBr(end_block);

        end_block->moveAfter(else_end);
        builder_.SetInsertPoint(end_block);
        llvm::PHINode* phi = builder_.CreatePHI(llvm_type(cond.type), 2);
        phi->addIncoming(then_value, then_end);
        phi->addIncoming(else_value, else_end);
        return phi;
    }

    // Integer widths change by sign or zero extension or truncation. Float to
    // integer saturates (NaN becomes 0), so no conversion is undefined.
    llvm::Value* emit_cast(const CastExpr& cast)
    {
        llvm::Value* value = emit_expr(*cast.operand);
        const Type* source = cast.operand->type;
        const Type* target = cast.type;
        llvm::Type* target_type = llvm_type(target);
        if (source == target) {
            return value;
        }
        const bool source_signed = source->is_integer() && source->is_signed;
        if (!source->is_float() && target->is_integer()) {
            return builder_.CreateIntCast(value, target_type, source_signed);
        }
        if (!source->is_float() && target->is_float()) {
            return source_signed ? builder_.CreateSIToFP(value, target_type)
                                 : builder_.CreateUIToFP(value, target_type);
        }
        if (source->is_float() && target->is_float()) {
            return builder_.CreateFPCast(value, target_type);
        }
        const llvm::Intrinsic::ID id =
            target->is_signed ? llvm::Intrinsic::fptosi_sat : llvm::Intrinsic::fptoui_sat;
        return builder_.CreateIntrinsic(id, {target_type, value->getType()}, {value});
    }

    // Integer division traps instead of being undefined: division by zero and
    // MIN / -1 stop the program with a panic message.
    llvm::Value* emit_checked_division(BinaryOp op, const Type* type, llvm::Value* lhs,
                                       llvm::Value* rhs, const SourceLocation& location)
    {
        llvm::Value* is_zero = builder_.CreateICmpEQ(rhs, llvm::ConstantInt::get(rhs->getType(), 0));
        emit_panic_if(is_zero, "integer division by zero", location);
        if (type->is_signed) {
            auto* int_type = llvm::cast<llvm::IntegerType>(lhs->getType());
            llvm::Value* is_min = builder_.CreateICmpEQ(
                lhs, llvm::ConstantInt::get(int_type, llvm::APInt::getSignedMinValue(
                                                          int_type->getBitWidth())));
            llvm::Value* is_minus_one =
                builder_.CreateICmpEQ(rhs, llvm::ConstantInt::getSigned(int_type, -1));
            emit_panic_if(builder_.CreateAnd(is_min, is_minus_one), "integer overflow in division",
                          location);
        }
        const bool is_div = op == BinaryOp::Div;
        if (type->is_signed) {
            return is_div ? builder_.CreateSDiv(lhs, rhs) : builder_.CreateSRem(lhs, rhs);
        }
        return is_div ? builder_.CreateUDiv(lhs, rhs) : builder_.CreateURem(lhs, rhs);
    }

    // The shift amount is taken modulo the bit width, so every shift is
    // defined.
    llvm::Value* emit_shift(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs)
    {
        const unsigned bits = lhs->getType()->getIntegerBitWidth();
        llvm::Value* amount = builder_.CreateAnd(rhs, llvm::ConstantInt::get(rhs->getType(), bits - 1));
        if (op == BinaryOp::Shl) {
            return builder_.CreateShl(lhs, amount);
        }
        return type->is_signed ? builder_.CreateAShr(lhs, amount) : builder_.CreateLShr(lhs, amount);
    }

    llvm::Value* emit_call(const CallExpr& call)
    {
        std::vector<llvm::Value*> args;
        const FunctionSymbol* symbol = nullptr;
        if (call.callee->kind == ExprKind::Member) {
            const auto& callee = static_cast<const MemberExpr&>(*call.callee);
            symbol = callee.method;
            if (!callee.is_static_call) {
                args.push_back(emit_receiver(*symbol->self_convention, *callee.object));
            }
        } else {
            symbol = static_cast<const NameExpr&>(*call.callee).function;
            if (symbol->intrinsic == Intrinsic::Some) {
                return emit_some(*call.type, emit_expr(*call.args.front()));
            }
            if (symbol->intrinsic != Intrinsic::None) {
                emit_print(*call.args.front(), symbol->intrinsic == Intrinsic::Print);
                return nullptr;
            }
        }
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            args.push_back(
                emit_argument(symbol->conventions[i], symbol->param_types[i], *call.args[i]));
        }
        return builder_.CreateCall(functions_.at(symbol), args);
    }

    // -----------------------------------------------------------------------
    // print / write

    // Each piece of the output becomes one runtime call; a format string
    // f"a{x}b" writes "a", then x, then "b".
    void emit_print(const Expr& arg, bool newline)
    {
        if (arg.kind == ExprKind::StringLiteral &&
            static_cast<const StringLiteralExpr&>(arg).is_format) {
            const auto& lit = static_cast<const StringLiteralExpr&>(arg);
            for (std::size_t i = 0; i < lit.parts.size(); ++i) {
                emit_print_text(lit.texts[i]);
                emit_print_value(*lit.parts[i]);
            }
            emit_print_text(lit.texts.back());
        } else {
            emit_print_value(arg);
        }
        if (newline) {
            emit_print_text("\n");
        }
    }

    void emit_print_text(const std::string& text)
    {
        if (text.empty()) {
            return;
        }
        llvm::Constant* value = emit_string_constant(text);
        builder_.CreateCall(runtime_function("hana_rt_print_str"),
                            {value->getAggregateElement(0u), value->getAggregateElement(1u)});
    }

    void emit_print_value(const Expr& expr)
    {
        llvm::Value* value = emit_expr(expr);
        const Type* type = expr.type;
        switch (type->kind) {
        case TypeKind::Int:
            if (type->is_signed) {
                builder_.CreateCall(runtime_function("hana_rt_print_i64"),
                                    {builder_.CreateSExt(value, builder_.getInt64Ty())});
            } else {
                builder_.CreateCall(runtime_function("hana_rt_print_u64"),
                                    {builder_.CreateZExt(value, builder_.getInt64Ty())});
            }
            break;
        case TypeKind::Float:
            builder_.CreateCall(runtime_function(type->bits == 32 ? "hana_rt_print_f32"
                                                                  : "hana_rt_print_f64"),
                                {value});
            break;
        case TypeKind::Bool:
            builder_.CreateCall(runtime_function("hana_rt_print_bool"), {value});
            break;
        case TypeKind::Char:
            builder_.CreateCall(runtime_function("hana_rt_print_char"), {value});
            break;
        case TypeKind::Str:
            builder_.CreateCall(runtime_function("hana_rt_print_str"),
                                {builder_.CreateExtractValue(value, 0),
                                 builder_.CreateExtractValue(value, 1)});
            break;
        default: break; // rejected by semantic analysis
        }
    }

    // -----------------------------------------------------------------------
    // Runtime support (runtime/hana_rt.h)

    llvm::FunctionCallee runtime_function(const std::string& name)
    {
        llvm::Type* ptr = llvm::PointerType::get(context_, 0);
        llvm::Type* i64 = builder_.getInt64Ty();
        llvm::Type* void_type = builder_.getVoidTy();
        llvm::FunctionType* type = nullptr;
        if (name == "hana_rt_print_str" || name == "hana_rt_panic") {
            type = llvm::FunctionType::get(void_type, {ptr, i64}, false);
        } else if (name == "hana_rt_print_i64" || name == "hana_rt_print_u64") {
            type = llvm::FunctionType::get(void_type, {i64}, false);
        } else if (name == "hana_rt_print_f64") {
            type = llvm::FunctionType::get(void_type, {builder_.getDoubleTy()}, false);
        } else if (name == "hana_rt_print_f32") {
            type = llvm::FunctionType::get(void_type, {builder_.getFloatTy()}, false);
        } else if (name == "hana_rt_print_bool") {
            type = llvm::FunctionType::get(void_type, {builder_.getInt1Ty()}, false);
        } else {
            type = llvm::FunctionType::get(void_type, {builder_.getInt32Ty()}, false);
        }
        llvm::FunctionCallee callee = module_.getOrInsertFunction(name, type);
        if (name == "hana_rt_panic") {
            llvm::cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
            llvm::cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::Cold);
        }
        return callee;
    }

    void emit_panic_if(llvm::Value* condition, const std::string& message,
                       const SourceLocation& location)
    {
        auto* panic_block = llvm::BasicBlock::Create(context_, "panic", current_function_);
        auto* continue_block = llvm::BasicBlock::Create(context_, "no_panic", current_function_);
        builder_.CreateCondBr(condition, panic_block, continue_block);
        builder_.SetInsertPoint(panic_block);
        const std::string text = message + " at " + source_path_ + ":" +
                                 std::to_string(location.line) + ":" +
                                 std::to_string(location.column);
        llvm::Constant* value = emit_string_constant(text);
        builder_.CreateCall(runtime_function("hana_rt_panic"),
                            {value->getAggregateElement(0u), value->getAggregateElement(1u)});
        builder_.CreateUnreachable();
        builder_.SetInsertPoint(continue_block);
    }

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

} // namespace

void initialize_native_target()
{
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
    });
}

std::unique_ptr<llvm::TargetMachine> create_host_target_machine(bool optimize, std::string& error)
{
    initialize_native_target();
    const std::string triple = llvm::sys::getDefaultTargetTriple();
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (target == nullptr) {
        return nullptr;
    }
    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt,
        optimize ? llvm::CodeGenOptLevel::Default : llvm::CodeGenOptLevel::None));
    if (!machine) {
        error = "cannot create a target machine for '" + triple + "'";
    }
    return machine;
}

std::unique_ptr<llvm::Module> generate_module(llvm::LLVMContext& context,
                                              llvm::TargetMachine& target,
                                              const lang::Module& module,
                                              const lang::Program& program,
                                              const std::string& source_path,
                                              const CodegenOptions& options)
{
    auto llvm_module = std::make_unique<llvm::Module>(source_path, context);
    llvm_module->setTargetTriple(target.getTargetTriple().getTriple());
    llvm_module->setDataLayout(target.createDataLayout());
    CodeGenerator generator(context, *llvm_module, program, source_path, options);
    generator.generate(module);
    return llvm_module;
}

void optimize_module(llvm::Module& module, llvm::TargetMachine& target)
{
    llvm::LoopAnalysisManager loop_analyses;
    llvm::FunctionAnalysisManager function_analyses;
    llvm::CGSCCAnalysisManager cgscc_analyses;
    llvm::ModuleAnalysisManager module_analyses;
    llvm::PassBuilder builder(&target);
    builder.registerModuleAnalyses(module_analyses);
    builder.registerCGSCCAnalyses(cgscc_analyses);
    builder.registerFunctionAnalyses(function_analyses);
    builder.registerLoopAnalyses(loop_analyses);
    builder.crossRegisterProxies(loop_analyses, function_analyses, cgscc_analyses,
                                 module_analyses);
    llvm::ModulePassManager passes =
        builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    passes.run(module, module_analyses);
}

std::string module_to_ir_text(const llvm::Module& module)
{
    std::string text;
    llvm::raw_string_ostream stream(text);
    module.print(stream, nullptr);
    return text;
}

bool emit_object_file(llvm::Module& module, llvm::TargetMachine& target, const std::string& path,
                      std::string& error)
{
    std::error_code code;
    llvm::raw_fd_ostream out(path, code, llvm::sys::fs::OF_None);
    if (code) {
        error = "cannot open '" + path + "': " + code.message();
        return false;
    }
    llvm::legacy::PassManager passes;
    if (target.addPassesToEmitFile(passes, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        error = "the target cannot emit object files";
        return false;
    }
    passes.run(module);
    out.flush();
    return true;
}

} // namespace hana::codegen
