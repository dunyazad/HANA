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
// match stores the arm's value in a slot and returns it; when it is only
// read, a second slot tells whether the value is a temporary.
llvm::Value* CodeGenerator::emit_match(const MatchExpr& match, bool is_expression, Use use)
{
    bool owns_subject = false;
    llvm::Value* subject = emit_subject_address(*match.subject, owns_subject);
    llvm::AllocaInst* result = is_expression ? create_entry_alloca(match.type, "match") : nullptr;
    llvm::AllocaInst* result_owned = nullptr;
    if (is_expression && use == Use::Read && needs_drop(match.type)) {
        result_owned = create_entry_alloca(bool_type(), "match.owned");
    }
    auto* end_block = llvm::BasicBlock::Create(context_, "match.end");
    for (const MatchArm& arm : match.arms) {
        emit_match_arm(arm, match, is_expression, use, subject, owns_subject, result,
                       result_owned, end_block);
    }
    builder_.CreateUnreachable();
    end_block->insertInto(current_function_);
    builder_.SetInsertPoint(end_block);
    if (!is_expression) {
        return nullptr;
    }
    llvm::Value* value = builder_.CreateLoad(llvm_type(match.type), result);
    if (result_owned == nullptr) {
        return value;
    }
    return keep_temporary(value, match.type,
                          builder_.CreateLoad(builder_.getInt1Ty(), result_owned));
}

// One arm. Names bound from a variable are parts of it (aliases); names
// bound from a temporary own their parts, and the rest of the temporary is
// dropped when the arm is entered. The arm is a scope for the names it
// owns.
void CodeGenerator::emit_match_arm(const MatchArm& arm, const MatchExpr& match, bool is_expression,
                                   Use use, llvm::Value* subject, bool owns_subject,
                                   llvm::Value* result, llvm::Value* result_owned,
                                   llvm::BasicBlock* end_block)
{
    const Type* type = result_type(*match.subject);
    auto* next_block = llvm::BasicBlock::Create(context_, "match.next", current_function_);
    emit_pattern_test(*arm.pattern, subject, type, next_block);
    emit_pattern_bindings(*arm.pattern, subject, type);
    if (place_of(*match.subject)) {
        record_aliases(*arm.pattern, *match.subject);
    }
    if (arm.guard) {
        begin_full_expression();
        llvm::Value* condition = emit_value(*arm.guard, Use::Read);
        end_full_expression();
        continue_if(condition, next_block);
    }
    push_scope();
    if (owns_subject) {
        own_pattern_bindings(*arm.pattern);
        drop_unbound(*arm.pattern, subject, type);
    }
    if (is_expression) {
        begin_full_expression();
        emit_arm_value(arm, use, result, result_owned, scopes_.back());
        end_full_expression();
    } else {
        emit_block(arm.body);
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        drop_slots(scopes_.back());
    }
    pop_scope();
    branch_if_open(end_block);
    builder_.SetInsertPoint(next_block);
}

// The value of an expression arm. When the match is only read, a place is
// borrowed, except a name the arm owns: the arm ends here, so its value is
// handed over instead.
void CodeGenerator::emit_arm_value(const MatchArm& arm, Use use, llvm::Value* result,
                                   llvm::Value* result_owned,
                                   const std::vector<OwnedSlot>& arm_owned)
{
    const Expr& value = *arm.value;
    if (result_owned == nullptr) {
        builder_.CreateStore(emit_value(value, use), result);
        return;
    }
    bool owned_by_arm = false;
    if (is_place_expr(value) && value.converted_to == nullptr) {
        const Expr* root = &value;
        while (root->kind == ExprKind::Member || root->kind == ExprKind::Index) {
            root = root->kind == ExprKind::Member
                       ? static_cast<const MemberExpr&>(*root).object.get()
                       : static_cast<const IndexExpr&>(*root).object.get();
        }
        llvm::Value* address = addresses_.at(static_cast<const NameExpr&>(*root).var);
        for (const OwnedSlot& slot : arm_owned) {
            owned_by_arm = owned_by_arm || slot.address == address;
        }
    }
    const bool borrowed = place_of(value).has_value() && !owned_by_arm;
    builder_.CreateStore(emit_value(value, borrowed ? Use::Read : Use::Consume), result);
    builder_.CreateStore(builder_.getInt1(!borrowed), result_owned);
}

// A variable or a field of one is matched where it is. Any other value is
// stored first; the match owns it when it needs dropping.
llvm::Value* CodeGenerator::emit_subject_address(const Expr& subject, bool& owns_subject)
{
    if (place_of(subject)) {
        owns_subject = false;
        return emit_address(subject);
    }
    const Type* type = result_type(subject);
    owns_subject = needs_drop(type);
    llvm::AllocaInst* temp = create_entry_alloca(type, "subject");
    builder_.CreateStore(emit_value(subject, owns_subject ? Use::Consume : Use::Read), temp);
    return temp;
}

// Names bound from a matched variable are parts of it. A name bound from a
// list element is never moved (checked by the move checker), so it needs
// no alias.
void CodeGenerator::record_aliases(const Pattern& pattern, const Expr& subject)
{
    std::vector<PathStep> path;
    const Expr* current = &subject;
    while (current->kind == ExprKind::Member || current->kind == ExprKind::Index) {
        if (current->kind == ExprKind::Index) {
            return;
        }
        const auto& member = static_cast<const MemberExpr&>(*current);
        PathStep step;
        step.kind = PathStep::Kind::Field;
        step.index = static_cast<unsigned>(member.field_index);
        path.insert(path.begin(), step);
        current = member.object.get();
    }
    AliasInfo base{static_cast<const NameExpr&>(*current).var, path};
    std::vector<PathStep> inner;
    collect_alias_paths(pattern, result_type(subject), inner, base);
}

void CodeGenerator::collect_alias_paths(const Pattern& pattern, const Type* type,
                                        std::vector<PathStep>& path, const AliasInfo& base)
{
    switch (pattern.kind) {
    case PatternKind::Binding: {
        AliasInfo alias = base;
        alias.path.insert(alias.path.end(), path.begin(), path.end());
        aliases_[pattern.symbol] = alias;
        return;
    }
    case PatternKind::Tuple:
        for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
            PathStep step;
            step.kind = PathStep::Kind::Field;
            step.index = static_cast<unsigned>(i);
            path.push_back(step);
            collect_alias_paths(*pattern.arguments[i], type->elements[i], path, base);
            path.pop_back();
        }
        return;
    case PatternKind::Case:
        if (type->is_option()) {
            PathStep step;
            step.kind = PathStep::Kind::OptionValue;
            path.push_back(step);
            collect_alias_paths(*pattern.arguments.front(), type->option_value(), path, base);
            path.pop_back();
            return;
        }
        for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
            const EnumCase& entry = type->user->cases[static_cast<std::size_t>(pattern.case_index)];
            PathStep step;
            step.kind = PathStep::Kind::CaseField;
            step.index = static_cast<unsigned>(i);
            step.case_index = pattern.case_index;
            path.push_back(step);
            collect_alias_paths(*pattern.arguments[i], entry.fields[i].type, path, base);
            path.pop_back();
        }
        return;
    default: return;
    }
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

// The names of a pattern own the parts they are bound to.
void CodeGenerator::own_pattern_bindings(const Pattern& pattern)
{
    if (pattern.kind == PatternKind::Binding) {
        own_variable(pattern.symbol, addresses_.at(pattern.symbol));
        return;
    }
    for (const auto& argument : pattern.arguments) {
        own_pattern_bindings(*argument);
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
