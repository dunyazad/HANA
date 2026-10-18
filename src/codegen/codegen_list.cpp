// Lists in generated code. A List<T> is { ptr data, i64 length, i64
// capacity } like the runtime's hana_list: 'length' elements of T stored
// one after another. A capacity of 0 means nothing is allocated.
//
// A list owns its elements: dropping the list drops them in order, then
// frees the storage. An element is a place inside the storage; it is never
// moved out except by 'pop', 'remove' and 'for sink'.

#include <cstdint>
#include <functional>
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

// ---------------------------------------------------------------------------
// Parts of a list

llvm::Value* CodeGenerator::list_data(llvm::Value* list_address)
{
    return builder_.CreateLoad(builder_.getPtrTy(),
                               builder_.CreateStructGEP(list_type_, list_address, 0), "data");
}

llvm::Value* CodeGenerator::list_length(llvm::Value* list_address)
{
    return builder_.CreateLoad(builder_.getInt64Ty(),
                               builder_.CreateStructGEP(list_type_, list_address, 1), "length");
}

void CodeGenerator::set_list_length(llvm::Value* list_address, llvm::Value* length)
{
    builder_.CreateStore(length, builder_.CreateStructGEP(list_type_, list_address, 1));
}

llvm::Value* CodeGenerator::element_size(const Type* list)
{
    const llvm::DataLayout& layout = module_.getDataLayout();
    return builder_.getInt64(layout.getTypeAllocSize(llvm_type(list->list_element())));
}

llvm::Value* CodeGenerator::element_address(const Type* list, llvm::Value* data,
                                            llvm::Value* position)
{
    return builder_.CreateGEP(llvm_type(list->list_element()), data, position, "element");
}

void CodeGenerator::reserve_elements(llvm::Value* list_address, const Type* list,
                                     llvm::Value* extra)
{
    builder_.CreateCall(runtime_function("hana_rt_list_reserve"),
                        {list_address, element_size(list), extra});
}

// Runs 'body' for each i64 from 'begin' up to 'end'. The body must not
// leave the loop.
void CodeGenerator::emit_counted_loop(llvm::Value* begin, llvm::Value* end,
                                      const std::function<void(llvm::Value*)>& body)
{
    llvm::AllocaInst* counter = create_entry_alloca(builder_.getInt64Ty(), "i");
    builder_.CreateStore(begin, counter);
    auto* test_block = llvm::BasicBlock::Create(context_, "loop.test", current_function_);
    auto* body_block = llvm::BasicBlock::Create(context_, "loop.body", current_function_);
    auto* end_block = llvm::BasicBlock::Create(context_, "loop.end", current_function_);
    builder_.CreateBr(test_block);
    builder_.SetInsertPoint(test_block);
    llvm::Value* i = builder_.CreateLoad(builder_.getInt64Ty(), counter);
    builder_.CreateCondBr(builder_.CreateICmpULT(i, end), body_block, end_block);
    builder_.SetInsertPoint(body_block);
    body(i);
    builder_.CreateStore(builder_.CreateAdd(i, builder_.getInt64(1)), counter);
    builder_.CreateBr(test_block);
    builder_.SetInsertPoint(end_block);
}

// An index past the end panics with the index, the length and the place in
// the source. 'allow_end' accepts the length itself, for 'insert'.
void CodeGenerator::emit_bounds_check(llvm::Value* position, llvm::Value* length, bool allow_end,
                                      const SourceLocation& location)
{
    llvm::Value* outside = allow_end ? builder_.CreateICmpUGT(position, length)
                                     : builder_.CreateICmpUGE(position, length);
    auto* panic_block = llvm::BasicBlock::Create(context_, "index.panic", current_function_);
    auto* continue_block = llvm::BasicBlock::Create(context_, "index.ok", current_function_);
    builder_.CreateCondBr(outside, panic_block, continue_block);
    builder_.SetInsertPoint(panic_block);
    llvm::Constant* where = emit_string_constant(" at " + source_path_ + ":" +
                                                 std::to_string(location.line) + ":" +
                                                 std::to_string(location.column));
    builder_.CreateCall(runtime_function("hana_rt_index_out_of_bounds"),
                        {position, length, where->getAggregateElement(0u),
                         where->getAggregateElement(1u)});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(continue_block);
}

llvm::Value* CodeGenerator::emit_position(const Expr& index)
{
    return builder_.CreateZExtOrTrunc(emit_value(index, Use::Read), builder_.getInt64Ty());
}

// ---------------------------------------------------------------------------
// Values and elements

// '[a, b, c]': the elements run in order and move into new storage.
llvm::Value* CodeGenerator::emit_list(const ListExpr& list)
{
    std::vector<const Expr*> parts;
    for (const auto& element : list.elements) {
        parts.push_back(element.get());
    }
    const std::vector<llvm::Value*> values = emit_consumed_parts(parts);
    llvm::AllocaInst* result = create_entry_alloca(list.type, "list");
    builder_.CreateStore(llvm::ConstantAggregateZero::get(list_type_), result);
    if (!values.empty()) {
        reserve_elements(result, list.type, builder_.getInt64(values.size()));
        llvm::Value* data = list_data(result);
        for (std::size_t i = 0; i < values.size(); ++i) {
            builder_.CreateStore(values[i], element_address(list.type, data, builder_.getInt64(i)));
        }
        set_list_length(result, builder_.getInt64(values.size()));
    }
    return builder_.CreateLoad(list_type_, result);
}

// 'xs[i]': the list runs first, then the index. The list is in use while
// the index runs, so its storage is read after the index is known.
llvm::Value* CodeGenerator::emit_element_address(const IndexExpr& index)
{
    const Type* list = result_type(*index.object);
    llvm::Value* list_address = nullptr;
    if (is_place_expr(*index.object)) {
        list_address = emit_address(*index.object);
    } else {
        llvm::AllocaInst* temp = create_entry_alloca(list, "indexed");
        builder_.CreateStore(emit_value(*index.object, Use::Read), temp);
        list_address = temp;
    }
    llvm::Value* position = emit_position(*index.index);
    emit_bounds_check(position, list_length(list_address), false, index.location);
    return element_address(list, list_data(list_address), position);
}

// ---------------------------------------------------------------------------
// Built-in methods

llvm::Value* CodeGenerator::emit_list_method(const CallExpr& call, const MemberExpr& callee)
{
    const Expr& object = *callee.object;
    const Type* list = result_type(object);
    switch (callee.builtin) {
    case BuiltinMethod::ListLen: {
        llvm::Value* length = builder_.CreateExtractValue(emit_value(object, Use::Read), 1);
        return builder_.CreateIntCast(length, llvm_type(call.type), false);
    }
    case BuiltinMethod::ListIsEmpty: {
        llvm::Value* length = builder_.CreateExtractValue(emit_value(object, Use::Read), 1);
        return builder_.CreateICmpEQ(length, builder_.getInt64(0));
    }
    default: break;
    }
    llvm::Value* address = emit_address(object);
    switch (callee.builtin) {
    case BuiltinMethod::ListPush:
        emit_list_push(address, list, emit_value(*call.args[0], Use::Consume));
        return nullptr;
    case BuiltinMethod::ListPop: return emit_list_pop(address, list, call.type);
    case BuiltinMethod::ListInsert: {
        llvm::Value* position = emit_position(*call.args[0]);
        llvm::Value* value = emit_value(*call.args[1], Use::Consume);
        emit_list_insert(address, list, position, value, call.location);
        return nullptr;
    }
    case BuiltinMethod::ListRemove:
        return emit_list_remove(address, list, emit_position(*call.args[0]), call.location);
    case BuiltinMethod::ListClear:
        emit_drop_elements(address, list, builder_.getInt64(0));
        set_list_length(address, builder_.getInt64(0));
        return nullptr;
    default: return nullptr;
    }
}

void CodeGenerator::emit_list_push(llvm::Value* address, const Type* list, llvm::Value* value)
{
    reserve_elements(address, list, builder_.getInt64(1));
    llvm::Value* length = list_length(address);
    builder_.CreateStore(value, element_address(list, list_data(address), length));
    set_list_length(address, builder_.CreateAdd(length, builder_.getInt64(1)));
}

// The last element moves out into a Some, or the result is null.
llvm::Value* CodeGenerator::emit_list_pop(llvm::Value* address, const Type* list,
                                          const Type* option)
{
    llvm::AllocaInst* result = create_entry_alloca(option, "popped");
    builder_.CreateStore(llvm::ConstantAggregateZero::get(llvm_type(option)), result);
    llvm::Value* length = list_length(address);
    auto* some_block = llvm::BasicBlock::Create(context_, "pop.some", current_function_);
    auto* end_block = llvm::BasicBlock::Create(context_, "pop.end", current_function_);
    builder_.CreateCondBr(builder_.CreateICmpEQ(length, builder_.getInt64(0)), end_block,
                          some_block);
    builder_.SetInsertPoint(some_block);
    llvm::Value* last = builder_.CreateSub(length, builder_.getInt64(1));
    llvm::Value* value = builder_.CreateLoad(llvm_type(list->list_element()),
                                             element_address(list, list_data(address), last));
    set_list_length(address, last);
    builder_.CreateStore(emit_some(*option, value), result);
    builder_.CreateBr(end_block);
    builder_.SetInsertPoint(end_block);
    return builder_.CreateLoad(llvm_type(option), result);
}

// The elements from 'position' on move one place back to make room.
void CodeGenerator::emit_list_insert(llvm::Value* address, const Type* list, llvm::Value* position,
                                     llvm::Value* value, const SourceLocation& location)
{
    llvm::Value* length = list_length(address);
    emit_bounds_check(position, length, true, location);
    reserve_elements(address, list, builder_.getInt64(1));
    llvm::Value* data = list_data(address);
    llvm::Value* tail = builder_.CreateMul(builder_.CreateSub(length, position), element_size(list));
    llvm::Value* from = element_address(list, data, position);
    llvm::Value* to = element_address(list, data, builder_.CreateAdd(position, builder_.getInt64(1)));
    builder_.CreateMemMove(to, llvm::MaybeAlign(), from, llvm::MaybeAlign(), tail);
    builder_.CreateStore(value, from);
    set_list_length(address, builder_.CreateAdd(length, builder_.getInt64(1)));
}

// The element at 'position' moves out; the ones after it move one place
// forward.
llvm::Value* CodeGenerator::emit_list_remove(llvm::Value* address, const Type* list,
                                             llvm::Value* position, const SourceLocation& location)
{
    llvm::Value* length = list_length(address);
    emit_bounds_check(position, length, false, location);
    llvm::Value* data = list_data(address);
    llvm::Value* removed = element_address(list, data, position);
    llvm::Value* value = builder_.CreateLoad(llvm_type(list->list_element()), removed);
    llvm::Value* next = builder_.CreateAdd(position, builder_.getInt64(1));
    llvm::Value* tail = builder_.CreateMul(builder_.CreateSub(length, next), element_size(list));
    builder_.CreateMemMove(removed, llvm::MaybeAlign(), element_address(list, data, next),
                           llvm::MaybeAlign(), tail);
    set_list_length(address, builder_.CreateSub(length, builder_.getInt64(1)));
    return value;
}

// ---------------------------------------------------------------------------
// Drop and clone

// Drops the elements from 'from' to the end, first to last.
void CodeGenerator::emit_drop_elements(llvm::Value* address, const Type* list, llvm::Value* from)
{
    const Type* element = list->list_element();
    if (!needs_drop(element)) {
        return;
    }
    llvm::Value* data = list_data(address);
    emit_counted_loop(from, list_length(address), [&](llvm::Value* i) {
        emit_drop(element_address(list, data, i), element);
    });
}

// The elements the list still owns, from 'from' on, then the storage.
void CodeGenerator::emit_drop_list(llvm::Value* address, const Type* list, llvm::Value* from)
{
    emit_drop_elements(address, list, from);
    llvm::Value* capacity = builder_.CreateLoad(builder_.getInt64Ty(),
                                                builder_.CreateStructGEP(list_type_, address, 2));
    builder_.CreateCall(runtime_function("hana_rt_list_free"), {list_data(address), capacity});
}

llvm::Value* CodeGenerator::emit_clone_list(llvm::Value* address, const Type* list)
{
    llvm::AllocaInst* result = create_entry_alloca(list, "clone");
    builder_.CreateStore(llvm::ConstantAggregateZero::get(list_type_), result);
    llvm::Value* length = list_length(address);
    reserve_elements(result, list, length);
    llvm::Value* source = list_data(address);
    llvm::Value* target = list_data(result);
    const Type* element = list->list_element();
    emit_counted_loop(builder_.getInt64(0), length, [&](llvm::Value* i) {
        builder_.CreateStore(emit_clone(element_address(list, source, i), element),
                             element_address(list, target, i));
    });
    set_list_length(result, length);
    return builder_.CreateLoad(list_type_, result);
}

// ---------------------------------------------------------------------------
// 'for x in xs'

// The list is in use for the whole loop, so its storage and length do not
// change and are read once. A read or inout element is the element itself.
// 'for sink' moves the list into the loop: each iteration's variable owns
// its element, and the elements the loop did not reach are dropped with
// the storage when the loop ends, also by 'break' or 'return'.
void CodeGenerator::emit_list_for(const ForStmt& stmt)
{
    const Type* list = result_type(*stmt.iterable);
    const bool sink = stmt.convention == ParamConvention::Sink;
    push_scope();
    llvm::AllocaInst* counter = create_entry_alloca(builder_.getInt64Ty(), "for.index");
    builder_.CreateStore(builder_.getInt64(0), counter);
    llvm::Value* list_address = nullptr;
    if (!sink && is_place_expr(*stmt.iterable)) {
        list_address = emit_address(*stmt.iterable);
    } else {
        // A temporary list read by the loop is dropped with the 'for'
        // statement's other temporaries.
        llvm::AllocaInst* slot = create_entry_alloca(list, "for.list");
        builder_.CreateStore(emit_value(*stmt.iterable, sink ? Use::Consume : Use::Read), slot);
        if (sink) {
            scopes_.back().push_back(OwnedSlot{slot, list, nullptr, counter});
        }
        list_address = slot;
    }
    llvm::Value* data = list_data(list_address);
    llvm::Value* length = list_length(list_address);

    auto* test_block = llvm::BasicBlock::Create(context_, "for.test", current_function_);
    auto* body_block = llvm::BasicBlock::Create(context_, "for.body", current_function_);
    auto* end_block = llvm::BasicBlock::Create(context_, "for.end", current_function_);
    builder_.CreateBr(test_block);
    builder_.SetInsertPoint(test_block);
    llvm::Value* i = builder_.CreateLoad(builder_.getInt64Ty(), counter);
    builder_.CreateCondBr(builder_.CreateICmpULT(i, length), body_block, end_block);

    builder_.SetInsertPoint(body_block);
    llvm::Value* element = element_address(list, data, i);
    builder_.CreateStore(builder_.CreateAdd(i, builder_.getInt64(1)), counter);
    loops_.push_back(LoopTargets{end_block, test_block, scopes_.size(), temporaries_.size()});
    push_scope();
    bind_loop_element(stmt, element);
    emit_block(stmt.body);
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        drop_slots(scopes_.back());
    }
    pop_scope();
    loops_.pop_back();
    branch_if_open(test_block);

    end_block->moveAfter(&current_function_->back());
    builder_.SetInsertPoint(end_block);
    drop_slots(scopes_.back());
    pop_scope();
}

// A 'for sink' variable owns a copy of the element's bits; the list gives
// the element up when the counter passes it.
void CodeGenerator::bind_loop_element(const ForStmt& stmt, llvm::Value* element)
{
    if (stmt.convention != ParamConvention::Sink) {
        addresses_[stmt.symbol] = element;
        return;
    }
    const Type* type = stmt.symbol->type;
    llvm::AllocaInst* slot = create_entry_alloca(type, stmt.name);
    builder_.CreateStore(builder_.CreateLoad(llvm_type(type), element), slot);
    addresses_[stmt.symbol] = slot;
    own_variable(stmt.symbol, slot);
}

} // namespace hana::codegen
