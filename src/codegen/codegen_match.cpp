// Code generation for 'match': pattern tests, bindings and the arms.

#include <cstdint>
#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>

#include "codegen/generator.h"
#include "lang/sema.h"

namespace hana::codegen {

// ---------------------------------------------------------------------------
// match

// Arms are tried in order: the pattern test, then the bindings, then the
// guard. Semantic analysis guarantees some arm matches. An expression
// match stores the arm's value in a slot and returns it.
llvm::Value* CodeGenerator::emit_match(const MatchExpr& match, bool is_expression)
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
llvm::Value* CodeGenerator::emit_subject_address(const Expr& subject)
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
void CodeGenerator::emit_pattern_test(const Pattern& pattern, llvm::Value* address, const Type* type,
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

void CodeGenerator::emit_range_test(const Pattern& pattern, llvm::Value* address, const Type* type,
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
void CodeGenerator::emit_pattern_bindings(const Pattern& pattern, llvm::Value* address, const Type* type)
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

llvm::Value* CodeGenerator::option_has_value(const Type* option, llvm::Value* address)
{
    return builder_.CreateLoad(builder_.getInt1Ty(),
                               builder_.CreateStructGEP(llvm_type(option), address, 0));
}

llvm::Value* CodeGenerator::option_value_address(const Type* option, llvm::Value* address)
{
    return builder_.CreateStructGEP(llvm_type(option), address, 1);
}

llvm::Value* CodeGenerator::tuple_element_address(const Type* tuple, llvm::Value* address, std::size_t index)
{
    return builder_.CreateStructGEP(llvm_type(tuple), address, static_cast<unsigned>(index));
}

llvm::Value* CodeGenerator::case_field_address(const UserType& info, const EnumCase& entry,
                                               llvm::Value* address, std::size_t index)
{
    llvm::Value* payload = builder_.CreateStructGEP(enum_type(info), address, 1);
    return builder_.CreateStructGEP(case_type(info, entry), payload,
                                    static_cast<unsigned>(index));
}

// Literal patterns are integers (possibly negated), chars and bools.
llvm::Constant* CodeGenerator::pattern_constant(const Expr& literal, const Type* type)
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

void CodeGenerator::continue_if(llvm::Value* condition, llvm::BasicBlock* fail)
{
    auto* next = llvm::BasicBlock::Create(context_, "match.ok", current_function_);
    builder_.CreateCondBr(condition, next, fail);
    builder_.SetInsertPoint(next);
}

void CodeGenerator::branch_if_open(llvm::BasicBlock* target)
{
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        builder_.CreateBr(target);
    }
}

} // namespace hana::codegen
