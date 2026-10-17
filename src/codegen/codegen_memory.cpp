// Ownership in generated code: every value that needs dropping has exactly
// one owner, and is dropped once when that owner's scope ends.
//
// Owners are variables (with a flag that is cleared when the value moves
// out) and temporaries (values a full expression only reads, dropped when
// the expression is done). Moving part of a variable drops the rest of it
// right away, since the whole variable counts as moved (decision 47).
// Drop and clone of each type are functions made on first use.

#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include "codegen/generator.h"
#include "lang/sema.h"

namespace hana::codegen {

namespace {

// Keeps function names readable in IR: 'hana.drop.Player'.
std::string glue_name(const char* kind, const Type* type)
{
    return std::string("hana.") + kind + "." + std::string(type->name);
}

} // namespace

// ---------------------------------------------------------------------------
// Scopes and temporaries

void CodeGenerator::push_scope()
{
    scopes_.emplace_back();
}

void CodeGenerator::pop_scope()
{
    scopes_.pop_back();
}

// The variable owns the value at 'address' from here until the end of the
// current scope, unless it moves out. Its flag starts false at function
// entry, so a path that never set the variable never drops it.
void CodeGenerator::own_variable(const VarSymbol* var, llvm::Value* address)
{
    if (!needs_drop(var->type)) {
        return;
    }
    llvm::AllocaInst* flag = create_entry_alloca(bool_type(), var->name + ".owned");
    llvm::IRBuilder<> entry(flag->getParent(), std::next(flag->getIterator()));
    entry.CreateStore(entry.getInt1(false), flag);
    builder_.CreateStore(builder_.getInt1(true), flag);
    drop_flags_[var] = flag;
    scopes_.back().push_back(OwnedSlot{address, var->type, flag});
}

// Drops in reverse order of ownership; a slot with a flag only when the
// flag says the value is still owned.
void CodeGenerator::drop_slots(const std::vector<OwnedSlot>& slots)
{
    for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
        if (it->start != nullptr) {
            emit_drop_list(it->address, it->type,
                           builder_.CreateLoad(builder_.getInt64Ty(), it->start));
            continue;
        }
        if (it->flag == nullptr) {
            emit_drop(it->address, it->type);
            continue;
        }
        auto* drop_block = llvm::BasicBlock::Create(context_, "drop", current_function_);
        auto* next_block = llvm::BasicBlock::Create(context_, "drop.done", current_function_);
        builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), it->flag), drop_block,
                              next_block);
        builder_.SetInsertPoint(drop_block);
        emit_drop(it->address, it->type);
        builder_.CreateBr(next_block);
        builder_.SetInsertPoint(next_block);
    }
}

// Drops the variables of every scope from 'depth' inward, without closing
// the scopes (for 'return', 'break' and 'continue').
void CodeGenerator::drop_scopes_from(std::size_t depth)
{
    for (std::size_t i = scopes_.size(); i-- > depth;) {
        drop_slots(scopes_[i]);
    }
}

void CodeGenerator::begin_full_expression()
{
    temporaries_.emplace_back();
}

void CodeGenerator::end_full_expression()
{
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        drop_slots(temporaries_.back());
    }
    temporaries_.pop_back();
}

// Drops the temporaries of every full expression from 'depth' inward,
// without closing them (for 'return', 'break' and 'continue').
void CodeGenerator::drop_temporaries_from(std::size_t depth)
{
    for (std::size_t i = temporaries_.size(); i-- > depth;) {
        drop_slots(temporaries_[i]);
    }
}

// A value the current full expression only reads is dropped when the
// expression is done. 'owned', when given, is an i1 telling at run time
// whether the value is a temporary at all (a branch of 'a if c else b' may
// be a variable).
llvm::Value* CodeGenerator::keep_temporary(llvm::Value* value, const Type* type,
                                           llvm::Value* owned)
{
    if (!needs_drop(type) || temporaries_.empty()) {
        return value;
    }
    llvm::AllocaInst* slot = create_entry_alloca(type, "tmp");
    builder_.CreateStore(value, slot);
    llvm::Value* flag = nullptr;
    if (owned != nullptr) {
        flag = create_entry_alloca(bool_type(), "tmp.owned");
        builder_.CreateStore(owned, flag);
    }
    temporaries_.back().push_back(OwnedSlot{slot, type, flag});
    return value;
}

// ---------------------------------------------------------------------------
// Moves

// Takes the value out of a variable or a field path of one. The variable is
// no longer dropped; a moved field leaves the rest of the variable to be
// dropped now.
llvm::Value* CodeGenerator::emit_move_out(const Expr& place)
{
    llvm::Value* value = builder_.CreateLoad(llvm_type(place.type), emit_address(place));
    release_place(place);
    return value;
}

void CodeGenerator::release_place(const Expr& place)
{
    std::vector<PathStep> path;
    const Expr* current = &place;
    while (current->kind == ExprKind::Member) {
        const auto& member = static_cast<const MemberExpr&>(*current);
        PathStep step;
        step.kind = PathStep::Kind::Field;
        step.index = static_cast<unsigned>(member.field_index);
        path.insert(path.begin(), step);
        current = member.object.get();
    }
    release_variable(static_cast<const NameExpr&>(*current).var, path);
}

// A name bound by a pattern is part of another variable: the move is a
// move of that part of the root variable.
void CodeGenerator::release_variable(const VarSymbol* var, const std::vector<PathStep>& path)
{
    std::vector<PathStep> full = path;
    const VarSymbol* root = var;
    for (auto alias = aliases_.find(root); alias != aliases_.end(); alias = aliases_.find(root)) {
        full.insert(full.begin(), alias->second.path.begin(), alias->second.path.end());
        root = alias->second.root;
    }
    auto flag = drop_flags_.find(root);
    if (flag == drop_flags_.end()) {
        return; // not an owner: the value needs no drop
    }
    drop_except(addresses_.at(root), root->type, full, 0);
    builder_.CreateStore(builder_.getInt1(false), flag->second);
}

// Moving a field out of a temporary keeps that field and drops the rest of
// the temporary.
llvm::Value* CodeGenerator::emit_move_out_of_temporary(const MemberExpr& member)
{
    std::vector<PathStep> path;
    const Expr* current = &member;
    while (current->kind == ExprKind::Member) {
        const auto& step_member = static_cast<const MemberExpr&>(*current);
        PathStep step;
        step.kind = PathStep::Kind::Field;
        step.index = static_cast<unsigned>(step_member.field_index);
        path.insert(path.begin(), step);
        current = step_member.object.get();
    }
    const Type* root_type = result_type(*current);
    llvm::AllocaInst* root = create_entry_alloca(root_type, "moved.from");
    builder_.CreateStore(emit_value(*current, Use::Consume), root);
    llvm::Value* address = root;
    const Type* type = root_type;
    for (const PathStep& step : path) {
        address = builder_.CreateStructGEP(llvm_type(type), address, step.index);
        type = type->is_tuple() ? type->elements[step.index] : type->user->fields[step.index].type;
    }
    llvm::Value* value = builder_.CreateLoad(llvm_type(type), address);
    drop_except(root, root_type, path, 0);
    return value;
}

// Drops everything in the value at 'address' except the part 'path' leads
// to.
void CodeGenerator::drop_except(llvm::Value* address, const Type* type,
                                const std::vector<PathStep>& path, std::size_t from)
{
    if (from == path.size() || !needs_drop(type)) {
        return;
    }
    const PathStep& step = path[from];
    if (step.kind == PathStep::Kind::OptionValue) {
        drop_except(option_value_address(type, address), type->option_value(), path, from + 1);
        return;
    }
    std::vector<const Type*> parts;
    std::vector<llvm::Value*> addresses;
    if (step.kind == PathStep::Kind::CaseField) {
        const EnumCase& entry = type->user->cases[static_cast<std::size_t>(step.case_index)];
        for (std::size_t i = 0; i < entry.fields.size(); ++i) {
            parts.push_back(entry.fields[i].type);
            addresses.push_back(case_field_address(*type->user, entry, address, i));
        }
    } else {
        for (std::size_t i = 0; i < (type->is_tuple() ? type->elements.size()
                                                       : type->user->fields.size());
             ++i) {
            parts.push_back(type->is_tuple() ? type->elements[i] : type->user->fields[i].type);
            addresses.push_back(
                builder_.CreateStructGEP(llvm_type(type), address, static_cast<unsigned>(i)));
        }
    }
    for (std::size_t i = parts.size(); i-- > 0;) {
        if (i == step.index) {
            drop_except(addresses[i], parts[i], path, from + 1);
        } else {
            emit_drop(addresses[i], parts[i]);
        }
    }
}

// A value taken apart by a pattern ('let (a, _) = ...', or a temporary
// matched by 'match'): the names own their parts; everything else is
// dropped.
void CodeGenerator::drop_unbound(const Pattern& pattern, llvm::Value* address, const Type* type)
{
    if (!needs_drop(type)) {
        return;
    }
    if (!binds_owned_value(pattern, type)) {
        emit_drop(address, type);
        return;
    }
    switch (pattern.kind) {
    case PatternKind::Tuple:
        for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
            drop_unbound(*pattern.arguments[i], tuple_element_address(type, address, i),
                         type->elements[i]);
        }
        return;
    case PatternKind::Case:
        if (type->is_option()) {
            drop_unbound(*pattern.arguments.front(), option_value_address(type, address),
                         type->option_value());
            return;
        }
        for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
            const EnumCase& entry = type->user->cases[static_cast<std::size_t>(pattern.case_index)];
            drop_unbound(*pattern.arguments[i], case_field_address(*type->user, entry, address, i),
                         entry.fields[i].type);
        }
        return;
    default: return; // a name: it owns this part
    }
}

// True when the pattern binds a name to a part that needs dropping.
bool CodeGenerator::binds_owned_value(const Pattern& pattern, const Type* type)
{
    switch (pattern.kind) {
    case PatternKind::Binding: return needs_drop(type);
    case PatternKind::Tuple:
        for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
            if (binds_owned_value(*pattern.arguments[i], type->elements[i])) {
                return true;
            }
        }
        return false;
    case PatternKind::Case:
        if (type->is_option()) {
            return binds_owned_value(*pattern.arguments.front(), type->option_value());
        }
        for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
            const EnumCase& entry = type->user->cases[static_cast<std::size_t>(pattern.case_index)];
            if (binds_owned_value(*pattern.arguments[i], entry.fields[i].type)) {
                return true;
            }
        }
        return false;
    default: return false;
    }
}

// Assignment drops the value being replaced. A variable that was moved out
// has nothing to drop; a field or an inout parameter always holds a value.
void CodeGenerator::store_with_drop(const Expr& target, llvm::Value* address, llvm::Value* value)
{
    const Type* type = target.type;
    if (!needs_drop(type)) {
        builder_.CreateStore(value, address);
        return;
    }
    if (target.kind == ExprKind::Name) {
        const VarSymbol* var = static_cast<const NameExpr&>(target).var;
        if (auto flag = drop_flags_.find(var); flag != drop_flags_.end()) {
            auto* drop_block = llvm::BasicBlock::Create(context_, "assign.drop", current_function_);
            auto* store_block = llvm::BasicBlock::Create(context_, "assign.store", current_function_);
            builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), flag->second),
                                  drop_block, store_block);
            builder_.SetInsertPoint(drop_block);
            emit_drop(address, type);
            builder_.CreateBr(store_block);
            builder_.SetInsertPoint(store_block);
            builder_.CreateStore(value, address);
            builder_.CreateStore(builder_.getInt1(true), flag->second);
            return;
        }
    }
    emit_drop(address, type);
    builder_.CreateStore(value, address);
}

// ---------------------------------------------------------------------------
// Drop and clone

void CodeGenerator::emit_drop(llvm::Value* address, const Type* type)
{
    if (needs_drop(type)) {
        builder_.CreateCall(drop_function(type), {address});
    }
}

// A Copy value is its own clone; others call the clone function of their
// type, which returns a new value owning its own data.
llvm::Value* CodeGenerator::emit_clone(llvm::Value* address, const Type* type)
{
    if (is_copy_type(type)) {
        return builder_.CreateLoad(llvm_type(type), address);
    }
    return builder_.CreateCall(clone_function(type), {address});
}

llvm::Function* CodeGenerator::drop_function(const Type* type)
{
    if (auto it = drop_functions_.find(type); it != drop_functions_.end()) {
        return it->second;
    }
    auto* signature =
        llvm::FunctionType::get(builder_.getVoidTy(), {llvm::PointerType::get(context_, 0)}, false);
    llvm::Function* function = llvm::Function::Create(
        signature, llvm::Function::InternalLinkage, glue_name("drop", type), module_);
    drop_functions_[type] = function;
    pending_drops_.push_back(type);
    return function;
}

llvm::Function* CodeGenerator::clone_function(const Type* type)
{
    if (auto it = clone_functions_.find(type); it != clone_functions_.end()) {
        return it->second;
    }
    auto* signature =
        llvm::FunctionType::get(llvm_type(type), {llvm::PointerType::get(context_, 0)}, false);
    llvm::Function* function = llvm::Function::Create(
        signature, llvm::Function::InternalLinkage, glue_name("clone", type), module_);
    clone_functions_[type] = function;
    pending_clones_.push_back(type);
    return function;
}

// Bodies may ask for more drop and clone functions, so this runs until no
// function is left without a body.
void CodeGenerator::emit_glue_bodies()
{
    while (!pending_drops_.empty() || !pending_clones_.empty()) {
        if (!pending_drops_.empty()) {
            const Type* type = pending_drops_.back();
            pending_drops_.pop_back();
            emit_drop_body(type, *drop_functions_.at(type));
        } else {
            const Type* type = pending_clones_.back();
            pending_clones_.pop_back();
            emit_clone_body(type, *clone_functions_.at(type));
        }
    }
}

// A user 'drop' runs first, then the parts are dropped in reverse order.
void CodeGenerator::emit_drop_body(const Type* type, llvm::Function& function)
{
    current_function_ = &function;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", &function));
    llvm::Value* address = function.getArg(0);
    switch (type->kind) {
    case TypeKind::Str: {
        llvm::Value* data =
            builder_.CreateLoad(builder_.getPtrTy(), builder_.CreateStructGEP(str_type_, address, 0));
        llvm::Value* capacity = builder_.CreateLoad(builder_.getInt64Ty(),
                                                    builder_.CreateStructGEP(str_type_, address, 2));
        builder_.CreateCall(runtime_function("hana_rt_str_free"), {data, capacity});
        break;
    }
    case TypeKind::Struct:
        if (type->user->drop_method != nullptr) {
            builder_.CreateCall(functions_.at(type->user->drop_method), {address});
        }
        for (std::size_t i = type->user->fields.size(); i-- > 0;) {
            emit_drop(builder_.CreateStructGEP(llvm_type(type), address, static_cast<unsigned>(i)),
                      type->user->fields[i].type);
        }
        break;
    case TypeKind::Enum:
        if (type->user->drop_method != nullptr) {
            builder_.CreateCall(functions_.at(type->user->drop_method), {address});
        }
        emit_drop_enum(*type->user, address);
        break;
    case TypeKind::Tuple:
        for (std::size_t i = type->elements.size(); i-- > 0;) {
            emit_drop(tuple_element_address(type, address, i), type->elements[i]);
        }
        break;
    case TypeKind::List: emit_drop_list(address, type, builder_.getInt64(0)); break;
    case TypeKind::Option: {
        auto* drop_block = llvm::BasicBlock::Create(context_, "some", &function);
        auto* end_block = llvm::BasicBlock::Create(context_, "end", &function);
        builder_.CreateCondBr(option_has_value(type, address), drop_block, end_block);
        builder_.SetInsertPoint(drop_block);
        emit_drop(option_value_address(type, address), type->option_value());
        builder_.CreateBr(end_block);
        builder_.SetInsertPoint(end_block);
        break;
    }
    default: break;
    }
    builder_.CreateRetVoid();
}

// Drops the fields of the active case.
void CodeGenerator::emit_drop_enum(const UserType& info, llvm::Value* address)
{
    llvm::StructType* type = enum_type(info);
    llvm::Value* tag =
        builder_.CreateLoad(builder_.getInt32Ty(), builder_.CreateStructGEP(type, address, 0));
    auto* end_block = llvm::BasicBlock::Create(context_, "end", current_function_);
    llvm::SwitchInst* cases = builder_.CreateSwitch(tag, end_block);
    for (std::size_t c = 0; c < info.cases.size(); ++c) {
        const EnumCase& entry = info.cases[c];
        bool any = false;
        for (const FieldInfo& field : entry.fields) {
            any = any || needs_drop(field.type);
        }
        if (!any) {
            continue;
        }
        auto* case_block = llvm::BasicBlock::Create(context_, entry.name, current_function_);
        cases->addCase(builder_.getInt32(static_cast<std::uint32_t>(c)), case_block);
        builder_.SetInsertPoint(case_block);
        for (std::size_t i = entry.fields.size(); i-- > 0;) {
            emit_drop(case_field_address(info, entry, address, i), entry.fields[i].type);
        }
        builder_.CreateBr(end_block);
    }
    builder_.SetInsertPoint(end_block);
}

// A 'clone' the user wrote is used as is; otherwise every part is cloned.
void CodeGenerator::emit_clone_body(const Type* type, llvm::Function& function)
{
    current_function_ = &function;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", &function));
    llvm::Value* address = function.getArg(0);
    if (type->is_user() && type->user->clone_method != nullptr) {
        builder_.CreateRet(builder_.CreateCall(functions_.at(type->user->clone_method), {address}));
        return;
    }
    switch (type->kind) {
    case TypeKind::Str: {
        llvm::AllocaInst* result = create_entry_alloca(type, "clone");
        llvm::Value* data =
            builder_.CreateLoad(builder_.getPtrTy(), builder_.CreateStructGEP(str_type_, address, 0));
        llvm::Value* length = builder_.CreateLoad(builder_.getInt64Ty(),
                                                  builder_.CreateStructGEP(str_type_, address, 1));
        builder_.CreateCall(runtime_function("hana_rt_str_clone"), {result, data, length});
        builder_.CreateRet(builder_.CreateLoad(str_type_, result));
        return;
    }
    case TypeKind::Enum: builder_.CreateRet(emit_clone_enum(*type->user, address)); return;
    case TypeKind::List: builder_.CreateRet(emit_clone_list(address, type)); return;
    case TypeKind::Option: {
        llvm::AllocaInst* result = create_entry_alloca(type, "clone");
        builder_.CreateStore(llvm::ConstantAggregateZero::get(llvm_type(type)), result);
        auto* some_block = llvm::BasicBlock::Create(context_, "some", &function);
        auto* end_block = llvm::BasicBlock::Create(context_, "end", &function);
        builder_.CreateCondBr(option_has_value(type, address), some_block, end_block);
        builder_.SetInsertPoint(some_block);
        llvm::Value* value = emit_clone(option_value_address(type, address), type->option_value());
        builder_.CreateStore(emit_some(*type, value), result);
        builder_.CreateBr(end_block);
        builder_.SetInsertPoint(end_block);
        builder_.CreateRet(builder_.CreateLoad(llvm_type(type), result));
        return;
    }
    default: break;
    }
    // A struct or tuple: clone each part.
    const std::size_t count = type->is_tuple() ? type->elements.size() : type->user->fields.size();
    llvm::Value* value = llvm::PoisonValue::get(llvm_type(type));
    for (std::size_t i = 0; i < count; ++i) {
        const Type* part = type->is_tuple() ? type->elements[i] : type->user->fields[i].type;
        llvm::Value* field = builder_.CreateStructGEP(llvm_type(type), address, static_cast<unsigned>(i));
        value = builder_.CreateInsertValue(value, emit_clone(field, part), static_cast<unsigned>(i));
    }
    builder_.CreateRet(value);
}

// Copies the tag, then clones the fields of the active case.
llvm::Value* CodeGenerator::emit_clone_enum(const UserType& info, llvm::Value* address)
{
    llvm::StructType* type = enum_type(info);
    llvm::AllocaInst* result = create_entry_alloca(&info.type, "clone");
    llvm::Value* tag =
        builder_.CreateLoad(builder_.getInt32Ty(), builder_.CreateStructGEP(type, address, 0));
    builder_.CreateStore(tag, builder_.CreateStructGEP(type, result, 0));
    auto* end_block = llvm::BasicBlock::Create(context_, "end", current_function_);
    llvm::SwitchInst* cases = builder_.CreateSwitch(tag, end_block);
    for (std::size_t c = 0; c < info.cases.size(); ++c) {
        const EnumCase& entry = info.cases[c];
        if (entry.fields.empty()) {
            continue;
        }
        auto* case_block = llvm::BasicBlock::Create(context_, entry.name, current_function_);
        cases->addCase(builder_.getInt32(static_cast<std::uint32_t>(c)), case_block);
        builder_.SetInsertPoint(case_block);
        for (std::size_t i = 0; i < entry.fields.size(); ++i) {
            llvm::Value* copy = emit_clone(case_field_address(info, entry, address, i),
                                           entry.fields[i].type);
            builder_.CreateStore(copy, case_field_address(info, entry, result, i));
        }
        builder_.CreateBr(end_block);
    }
    builder_.SetInsertPoint(end_block);
    return builder_.CreateLoad(type, result);
}

// ---------------------------------------------------------------------------
// 'str'

llvm::Value* CodeGenerator::emit_str_concat(llvm::Value* lhs, llvm::Value* rhs)
{
    llvm::AllocaInst* result = create_entry_alloca(str_type(), "concat");
    builder_.CreateCall(runtime_function("hana_rt_str_concat"),
                        {result, builder_.CreateExtractValue(lhs, 0),
                         builder_.CreateExtractValue(lhs, 1), builder_.CreateExtractValue(rhs, 0),
                         builder_.CreateExtractValue(rhs, 1)});
    return builder_.CreateLoad(str_type_, result);
}

// Strings compare by their bytes.
llvm::Value* CodeGenerator::emit_str_compare(BinaryOp op, llvm::Value* lhs, llvm::Value* rhs)
{
    llvm::Value* order = builder_.CreateCall(
        runtime_function("hana_rt_str_compare"),
        {builder_.CreateExtractValue(lhs, 0), builder_.CreateExtractValue(lhs, 1),
         builder_.CreateExtractValue(rhs, 0), builder_.CreateExtractValue(rhs, 1)});
    return emit_comparison(op, find_builtin_type("i32"), order, builder_.getInt32(0));
}

// 's += value' grows the string in place.
void CodeGenerator::emit_str_append(llvm::Value* target, llvm::Value* value)
{
    builder_.CreateCall(runtime_function("hana_rt_str_push"),
                        {target, builder_.CreateExtractValue(value, 0),
                         builder_.CreateExtractValue(value, 1)});
}

// f"a{x}b" as a value: a new string with each piece appended.
llvm::Value* CodeGenerator::emit_format_string(const StringLiteralExpr& lit)
{
    llvm::AllocaInst* result = create_entry_alloca(str_type(), "format");
    builder_.CreateStore(llvm::ConstantAggregateZero::get(str_type_), result);
    for (std::size_t i = 0; i < lit.texts.size(); ++i) {
        if (!lit.texts[i].empty()) {
            emit_str_append(result, emit_string_constant(lit.texts[i]));
        }
        if (i < lit.parts.size()) {
            emit_str_push_value(result, *lit.parts[i]);
        }
    }
    return builder_.CreateLoad(str_type_, result);
}

void CodeGenerator::emit_str_push_value(llvm::Value* target, const Expr& part)
{
    llvm::Value* value = emit_value(part, Use::Read);
    const Type* type = part.type;
    switch (type->kind) {
    case TypeKind::Int:
        if (type->is_signed) {
            builder_.CreateCall(runtime_function("hana_rt_str_push_i64"),
                                {target, builder_.CreateSExt(value, builder_.getInt64Ty())});
        } else {
            builder_.CreateCall(runtime_function("hana_rt_str_push_u64"),
                                {target, builder_.CreateZExt(value, builder_.getInt64Ty())});
        }
        break;
    case TypeKind::Float:
        builder_.CreateCall(runtime_function(type->bits == 32 ? "hana_rt_str_push_f32"
                                                              : "hana_rt_str_push_f64"),
                            {target, value});
        break;
    case TypeKind::Bool:
        builder_.CreateCall(runtime_function("hana_rt_str_push_bool"), {target, value});
        break;
    case TypeKind::Char:
        builder_.CreateCall(runtime_function("hana_rt_str_push_char"), {target, value});
        break;
    case TypeKind::Str: emit_str_append(target, value); break;
    default: break; // rejected by semantic analysis
    }
}

} // namespace hana::codegen
