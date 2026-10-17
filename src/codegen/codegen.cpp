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
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>

#include "codegen/generator.h"
#include "lang/sema.h"

namespace hana::codegen {

using namespace hana::lang;

std::string mangle_function_name(const std::string& name)
{
    return "hana." + name;
}

bool is_place_expr(const Expr& expr)
{
    if (expr.kind == ExprKind::Name) {
        return static_cast<const NameExpr&>(expr).var != nullptr;
    }
    if (expr.kind == ExprKind::Member) {
        const auto& member = static_cast<const MemberExpr&>(expr);
        return member.field_index >= 0 && is_place_expr(*member.object);
    }
    return false;
}

bool passed_by_pointer(ParamConvention convention, const Type* type)
{
    return convention == ParamConvention::Inout ||
           (convention == ParamConvention::Read &&
            (type->is_user() || type->is_tuple() || type->is_option()) && !is_copy_type(type));
}

CodeGenerator::CodeGenerator(llvm::LLVMContext& context, llvm::Module& module, const Program& program,
                             std::string source_path, const CodegenOptions& options)
    : context_(context)
    , module_(module)
    , builder_(context)
    , program_(program)
    , source_path_(std::move(source_path))
    , options_(options)
{
    // { data, length, capacity }: see hana_str in runtime/hana_rt.h.
    llvm::Type* i64 = llvm::Type::getInt64Ty(context_);
    str_type_ = llvm::StructType::create(context_, {llvm::PointerType::get(context_, 0), i64, i64},
                                         "hana.str");
}

void CodeGenerator::generate(const lang::Module& ast)
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
    emit_glue_bodies();
}

// ---------------------------------------------------------------------------
// Types and declarations

llvm::Type* CodeGenerator::llvm_type(const Type* type)
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
llvm::StructType* CodeGenerator::struct_type(const UserType& info)
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
llvm::StructType* CodeGenerator::enum_type(const UserType& info)
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
llvm::StructType* CodeGenerator::case_type(const UserType& info, const EnumCase& entry)
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
llvm::StructType* CodeGenerator::tuple_type(const Type& tuple)
{
    std::vector<llvm::Type*> elements;
    for (const Type* element : tuple.elements) {
        elements.push_back(llvm_type(element));
    }
    return llvm::StructType::get(context_, elements);
}

// An Option<T> is { i1 has_value, T value }; 'null' is all zeros.
llvm::StructType* CodeGenerator::option_type(const Type& option)
{
    return llvm::StructType::get(context_,
                                 {builder_.getInt1Ty(), llvm_type(option.option_value())});
}

llvm::Type* CodeGenerator::param_llvm_type(ParamConvention convention, const Type* type)
{
    return passed_by_pointer(convention, type) ? llvm::PointerType::get(context_, 0)
                                               : llvm_type(type);
}

// A method takes 'self' as its first LLVM parameter.
void CodeGenerator::declare_function(const FnDecl& decl)
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

// ---------------------------------------------------------------------------
// Functions

// Parameters form the outermost scope: a 'sink' parameter is owned by the
// function and dropped at its end unless it moves out.
void CodeGenerator::emit_function(const FnDecl& decl)
{
    llvm::Function* function = functions_.at(decl.symbol);
    current_function_ = function;
    current_return_type_ = decl.symbol->return_type;
    drop_flags_.clear();
    aliases_.clear();
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function));
    push_scope();
    bind_parameters(decl, *function);
    emit_block(decl.body);
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        drop_slots(scopes_.back());
    }
    pop_scope();
    terminate_open_blocks(*function);
    addresses_.clear();
}

void CodeGenerator::bind_parameters(const FnDecl& decl, llvm::Function& function)
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

void CodeGenerator::bind_parameter(ParamConvention convention, const VarSymbol* symbol,
                                   const std::string& name, llvm::Argument& arg)
{
    arg.setName(name);
    if (passed_by_pointer(convention, symbol->type)) {
        addresses_[symbol] = &arg;
        return;
    }
    llvm::AllocaInst* slot = create_entry_alloca(symbol->type, name);
    builder_.CreateStore(&arg, slot);
    addresses_[symbol] = slot;
    if (convention == ParamConvention::Sink) {
        own_variable(symbol, slot);
    }
}

// Blocks left open are unreachable or the end of a void function.
void CodeGenerator::terminate_open_blocks(llvm::Function& function)
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

void CodeGenerator::emit_c_main()
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

llvm::AllocaInst* CodeGenerator::create_entry_alloca(const Type* type, const std::string& name)
{
    llvm::IRBuilder<> entry_builder(&current_function_->getEntryBlock(),
                                    current_function_->getEntryBlock().begin());
    return entry_builder.CreateAlloca(llvm_type(type), nullptr, name);
}

// Code after return/break/continue is unreachable but still emitted into
// a fresh block so the builder always has a valid insertion point.
void CodeGenerator::ensure_insertion_point()
{
    if (builder_.GetInsertBlock()->getTerminator() != nullptr) {
        builder_.SetInsertPoint(
            llvm::BasicBlock::Create(context_, "unreachable", current_function_));
    }
}

// ---------------------------------------------------------------------------
// Statements

// A block is a scope: its variables are dropped at its end. Each statement
// is a full expression: the temporaries it reads are dropped after it.
void CodeGenerator::emit_block(const Block& block)
{
    push_scope();
    for (const auto& statement : block.statements) {
        ensure_insertion_point();
        begin_full_expression();
        emit_statement(*statement);
        end_full_expression();
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        drop_slots(scopes_.back());
    }
    pop_scope();
}

void CodeGenerator::emit_statement(const Stmt& stmt)
{
    switch (stmt.kind) {
    case StmtKind::Let: emit_let(static_cast<const LetStmt&>(stmt)); break;
    case StmtKind::Assign: emit_assign(static_cast<const AssignStmt&>(stmt)); break;
    case StmtKind::Expr: emit_value(*static_cast<const ExprStmt&>(stmt).expr, Use::Read); break;
    case StmtKind::Return: emit_return(static_cast<const ReturnStmt&>(stmt)); break;
    case StmtKind::If: emit_if(static_cast<const IfStmt&>(stmt)); break;
    case StmtKind::While: emit_while(static_cast<const WhileStmt&>(stmt)); break;
    case StmtKind::For: emit_for(static_cast<const ForStmt&>(stmt)); break;
    case StmtKind::Match:
        emit_match(*static_cast<const MatchStmt&>(stmt).match, false, Use::Read);
        break;
    case StmtKind::Break:
    case StmtKind::Continue: emit_jump(stmt); break;
    case StmtKind::Pass: break;
    }
}

void CodeGenerator::emit_let(const LetStmt& let)
{
    if (let.pattern) {
        emit_destructuring_let(let);
        return;
    }
    llvm::Value* value = emit_value(*let.init, Use::Consume);
    llvm::AllocaInst* slot = create_entry_alloca(let.symbol->type, let.name);
    builder_.CreateStore(value, slot);
    addresses_[let.symbol] = slot;
    own_variable(let.symbol, slot);
}

// 'let (a, b) = value': the value moves in, each name owns its part, and
// the parts matched by '_' are dropped.
void CodeGenerator::emit_destructuring_let(const LetStmt& let)
{
    const Type* type = result_type(*let.init);
    llvm::AllocaInst* value = create_entry_alloca(type, "destructured");
    builder_.CreateStore(emit_value(*let.init, Use::Consume), value);
    emit_pattern_bindings(*let.pattern, value, type);
    own_pattern_bindings(*let.pattern);
    drop_unbound(*let.pattern, value, type);
}

// The new value is computed before the old one is dropped, so 's = s + "!"'
// reads the old string first.
void CodeGenerator::emit_assign(const AssignStmt& assign)
{
    const Type* type = assign.target->type;
    if (assign.op) {
        llvm::Value* value = emit_value(*assign.value, Use::Read);
        llvm::Value* address = emit_address(*assign.target);
        if (type->kind == TypeKind::Str) {
            emit_str_append(address, value);
            return;
        }
        llvm::Value* current = builder_.CreateLoad(llvm_type(type), address);
        builder_.CreateStore(emit_arithmetic(*assign.op, type, current, value, assign.op_location),
                             address);
        return;
    }
    llvm::Value* value = emit_value(*assign.value, Use::Consume);
    store_with_drop(*assign.target, emit_address(*assign.target), value);
}

// The result moves out first; then everything the function still owns is
// dropped.
void CodeGenerator::emit_return(const ReturnStmt& ret)
{
    llvm::Value* value = ret.value ? emit_value(*ret.value, Use::Consume) : nullptr;
    drop_temporaries_from(0);
    drop_scopes_from(0);
    if (value != nullptr) {
        builder_.CreateRet(value);
    } else {
        builder_.CreateRetVoid();
    }
}

// 'break' and 'continue' leave the scopes and full expressions inside the
// loop body.
void CodeGenerator::emit_jump(const Stmt& stmt)
{
    const LoopTargets& loop = loops_.back();
    drop_temporaries_from(loop.temp_depth);
    drop_scopes_from(loop.scope_depth);
    builder_.CreateBr(stmt.kind == StmtKind::Break ? loop.break_block : loop.continue_block);
}

void CodeGenerator::emit_if(const IfStmt& stmt)
{
    llvm::Value* condition = emit_value(*stmt.condition, Use::Read);
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

// The condition is its own full expression: it runs again on every
// iteration, and its temporaries are dropped each time before branching.
void CodeGenerator::emit_while(const WhileStmt& stmt)
{
    auto* condition_block = llvm::BasicBlock::Create(context_, "while.cond", current_function_);
    auto* body_block = llvm::BasicBlock::Create(context_, "while.body", current_function_);
    auto* end_block = llvm::BasicBlock::Create(context_, "while.end", current_function_);
    builder_.CreateBr(condition_block);

    builder_.SetInsertPoint(condition_block);
    begin_full_expression();
    llvm::Value* condition = emit_value(*stmt.condition, Use::Read);
    end_full_expression();
    builder_.CreateCondBr(condition, body_block, end_block);

    builder_.SetInsertPoint(body_block);
    loops_.push_back(LoopTargets{end_block, condition_block, scopes_.size(), temporaries_.size()});
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
void CodeGenerator::emit_for(const ForStmt& stmt)
{
    const Type* type = stmt.symbol->type;
    llvm::Value* start = emit_value(*stmt.start, Use::Read);
    llvm::Value* end = emit_value(*stmt.end, Use::Read);
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
    loops_.push_back(LoopTargets{end_block, step_block, scopes_.size(), temporaries_.size()});
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

llvm::Value* CodeGenerator::range_has_values(const Type* type, llvm::Value* start, llvm::Value* end,
                                             bool inclusive)
{
    if (type->is_signed) {
        return inclusive ? builder_.CreateICmpSLE(start, end) : builder_.CreateICmpSLT(start, end);
    }
    return inclusive ? builder_.CreateICmpULE(start, end) : builder_.CreateICmpULT(start, end);
}

// ---------------------------------------------------------------------------
// Expressions

// The value of an expression used as 'use' says. A value wrapped into an
// Option where one is expected becomes a new Option holding it, so a value
// that is not Copy is taken, like 'Some(value)'.
llvm::Value* CodeGenerator::emit_value(const Expr& expr, Use use)
{
    if (expr.converted_to == nullptr) {
        return emit_owned_value(expr, use);
    }
    const Use inner = is_copy_type(expr.type) ? Use::Read : Use::Consume;
    llvm::Value* wrapped = emit_some(*expr.converted_to, emit_owned_value(expr, inner));
    return use == Use::Read ? keep_temporary(wrapped, expr.converted_to, nullptr) : wrapped;
}

// The value itself, before any wrapping. Reading a variable or a field of
// one borrows its bits; consuming moves out of it. A new value that is
// only read becomes a temporary of the full expression.
llvm::Value* CodeGenerator::emit_owned_value(const Expr& expr, Use use)
{
    if (expr.kind == ExprKind::Conditional) {
        return emit_conditional(static_cast<const ConditionalExpr&>(expr), use);
    }
    if (expr.kind == ExprKind::Match) {
        return emit_match(static_cast<const MatchExpr&>(expr), true, use);
    }
    if (!needs_drop(expr.type)) {
        return emit_expr_value(expr);
    }
    if (is_place_expr(expr)) {
        return use == Use::Consume ? emit_move_out(expr) : emit_expr_value(expr);
    }
    if (expr.kind == ExprKind::Member && static_cast<const MemberExpr&>(expr).field_index >= 0) {
        // A field of a temporary.
        const auto& member = static_cast<const MemberExpr&>(expr);
        return use == Use::Consume ? emit_move_out_of_temporary(member) : emit_member(member);
    }
    llvm::Value* value = emit_expr_value(expr);
    return use == Use::Read ? keep_temporary(value, expr.type, nullptr) : value;
}

llvm::Value* CodeGenerator::emit_some(const Type& option, llvm::Value* value)
{
    llvm::Value* result = llvm::PoisonValue::get(llvm_type(&option));
    result = builder_.CreateInsertValue(result, builder_.getInt1(true), 0);
    return builder_.CreateInsertValue(result, value, 1);
}

llvm::Value* CodeGenerator::emit_expr_value(const Expr& expr)
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
    case ExprKind::StringLiteral: {
        const auto& lit = static_cast<const StringLiteralExpr&>(expr);
        return lit.is_format ? emit_format_string(lit) : emit_string_constant(lit.texts.front());
    }
    case ExprKind::Name: {
        const auto& name = static_cast<const NameExpr&>(expr);
        return builder_.CreateLoad(llvm_type(expr.type), addresses_.at(name.var), name.name);
    }
    case ExprKind::Unary: return emit_unary(static_cast<const UnaryExpr&>(expr));
    case ExprKind::Binary: return emit_binary(static_cast<const BinaryExpr&>(expr));
    case ExprKind::Conditional:
        return emit_conditional(static_cast<const ConditionalExpr&>(expr), Use::Read);
    case ExprKind::Cast: return emit_cast(static_cast<const CastExpr&>(expr));
    case ExprKind::Call: return emit_call(static_cast<const CallExpr&>(expr));
    case ExprKind::StructLiteral:
        return emit_struct_literal(static_cast<const StructLiteralExpr&>(expr));
    case ExprKind::Member: return emit_member(static_cast<const MemberExpr&>(expr));
    case ExprKind::Match: return emit_match(static_cast<const MatchExpr&>(expr), true, Use::Read);
    case ExprKind::Tuple: return emit_tuple(static_cast<const TupleExpr&>(expr));
    case ExprKind::Null: return llvm::ConstantAggregateZero::get(llvm_type(expr.type));
    case ExprKind::Error:
    case ExprKind::Index:
    case ExprKind::Try: break; // rejected by semantic analysis
    }
    return llvm::UndefValue::get(llvm_type(expr.type));
}

// The address of a place: a variable or a field path of one.
llvm::Value* CodeGenerator::emit_address(const Expr& place)
{
    if (place.kind == ExprKind::Name) {
        return addresses_.at(static_cast<const NameExpr&>(place).var);
    }
    const auto& member = static_cast<const MemberExpr&>(place);
    return builder_.CreateStructGEP(llvm_type(member.object->type), emit_address(*member.object),
                                    static_cast<unsigned>(member.field_index), member.member);
}

// A field of a place is loaded through its address; a field of a
// temporary value is extracted from it, and the temporary is dropped at
// the end of the full expression.
llvm::Value* CodeGenerator::emit_member(const MemberExpr& member)
{
    if (member.case_index >= 0) {
        return emit_enum_value(*member.type->user, member.case_index, {});
    }
    if (is_place_expr(member)) {
        return builder_.CreateLoad(llvm_type(member.type), emit_address(member), member.member);
    }
    return builder_.CreateExtractValue(emit_value(*member.object, Use::Read),
                                       static_cast<unsigned>(member.field_index));
}

llvm::Value* CodeGenerator::emit_struct_literal(const StructLiteralExpr& lit)
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
// left out, in declaration order. The new value owns them all. Returns the
// values in declaration order.
std::vector<llvm::Value*> CodeGenerator::emit_literal_fields(const StructLiteralExpr& lit,
                                                             const std::vector<FieldInfo>& fields)
{
    std::vector<llvm::Value*> written;
    for (const auto& field : lit.fields) {
        written.push_back(emit_value(*field.value, Use::Consume));
    }
    std::vector<llvm::Value*> values;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const int index = lit.field_order[i];
        values.push_back(index >= 0 ? written[static_cast<std::size_t>(index)]
                                    : emit_value(*fields[i].default_value, Use::Consume));
    }
    return values;
}

llvm::Value* CodeGenerator::emit_enum_value(const UserType& info, int case_index,
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

llvm::Value* CodeGenerator::emit_tuple(const TupleExpr& tuple)
{
    llvm::Value* value = llvm::PoisonValue::get(llvm_type(tuple.type));
    for (std::size_t i = 0; i < tuple.elements.size(); ++i) {
        value = builder_.CreateInsertValue(value, emit_value(*tuple.elements[i], Use::Consume),
                                           static_cast<unsigned>(i));
    }
    return value;
}

// An argument passed by pointer: a place passes its own address, any
// other value is stored in a slot first.
llvm::Value* CodeGenerator::emit_pointer_argument(const Expr& arg)
{
    if (place_of(arg)) {
        return emit_address(arg);
    }
    llvm::AllocaInst* temp = create_entry_alloca(result_type(arg), "arg");
    builder_.CreateStore(emit_value(arg, Use::Read), temp);
    return temp;
}

// How the argument is passed follows the parameter's type: the argument
// may be a value wrapped into the parameter's Option type. A 'sink'
// argument moves into the call.
llvm::Value* CodeGenerator::emit_argument(ParamConvention convention, const Type* param_type,
                                          const Expr& arg)
{
    if (convention == ParamConvention::Inout) {
        return emit_address(*static_cast<const UnaryExpr&>(arg).operand);
    }
    if (passed_by_pointer(convention, param_type)) {
        return emit_pointer_argument(arg);
    }
    return emit_value(arg, convention == ParamConvention::Sink ? Use::Consume : Use::Read);
}

// The value a method is called on; for 'inout self' it is a place
// written without '&'.
llvm::Value* CodeGenerator::emit_receiver(ParamConvention convention, const Expr& object)
{
    if (convention == ParamConvention::Inout) {
        return emit_address(object);
    }
    if (passed_by_pointer(convention, object.type)) {
        return emit_pointer_argument(object);
    }
    return emit_value(object, convention == ParamConvention::Sink ? Use::Consume : Use::Read);
}

llvm::Value* CodeGenerator::emit_int_literal(const IntLiteralExpr& lit)
{
    if (lit.type->is_float()) {
        return llvm::ConstantFP::get(llvm_type(lit.type), static_cast<double>(lit.value));
    }
    return llvm::ConstantInt::get(llvm_type(lit.type), lit.value);
}

// A literal's bytes are not owned: its capacity is 0.
llvm::Constant* CodeGenerator::emit_string_constant(const std::string& text)
{
    llvm::Constant* data = builder_.CreateGlobalStringPtr(text, "str", 0, &module_);
    return llvm::ConstantStruct::get(
        str_type_, {data, builder_.getInt64(static_cast<std::uint64_t>(text.size())),
                    builder_.getInt64(0)});
}

llvm::Value* CodeGenerator::emit_unary(const UnaryExpr& unary)
{
    llvm::Value* operand = emit_value(*unary.operand, Use::Read);
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

llvm::Value* CodeGenerator::emit_binary(const BinaryExpr& binary)
{
    if (binary.op == BinaryOp::And || binary.op == BinaryOp::Or) {
        return emit_logical(binary);
    }
    if (result_type(*binary.lhs)->is_option()) {
        return emit_null_comparison(binary);
    }
    llvm::Value* lhs = emit_value(*binary.lhs, Use::Read);
    llvm::Value* rhs = emit_value(*binary.rhs, Use::Read);
    const Type* operand_type = binary.lhs->type;
    const bool is_str = operand_type->kind == TypeKind::Str;
    switch (binary.op) {
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
        return is_str ? emit_str_compare(binary.op, lhs, rhs)
                      : emit_comparison(binary.op, operand_type, lhs, rhs);
    default:
        if (is_str) {
            return emit_str_concat(lhs, rhs);
        }
        return emit_arithmetic(binary.op, operand_type, lhs, rhs, binary.op_location);
    }
}

// 'x == null' / 'x != null' test whether the Option has a value. Only
// the side that is not 'null' is computed.
llvm::Value* CodeGenerator::emit_null_comparison(const BinaryExpr& binary)
{
    const Expr& option = binary.lhs->kind == ExprKind::Null ? *binary.rhs : *binary.lhs;
    llvm::Value* has_value = builder_.CreateExtractValue(emit_value(option, Use::Read), 0);
    return binary.op == BinaryOp::Eq ? builder_.CreateNot(has_value) : has_value;
}

// Short-circuit: the right side runs only when it decides the result, so
// it is a full expression of its own.
llvm::Value* CodeGenerator::emit_logical(const BinaryExpr& binary)
{
    const bool is_and = binary.op == BinaryOp::And;
    llvm::Value* lhs = emit_value(*binary.lhs, Use::Read);
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
    begin_full_expression();
    llvm::Value* rhs = emit_value(*binary.rhs, Use::Read);
    end_full_expression();
    llvm::BasicBlock* rhs_end = builder_.GetInsertBlock();
    builder_.CreateBr(end_block);

    end_block->moveAfter(rhs_end);
    builder_.SetInsertPoint(end_block);
    llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt1Ty(), 2);
    phi->addIncoming(builder_.getInt1(!is_and), lhs_block);
    phi->addIncoming(rhs, rhs_end);
    return phi;
}

llvm::Value* CodeGenerator::emit_comparison(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs)
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

int CodeGenerator::comparison_index(BinaryOp op)
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

llvm::Value* CodeGenerator::emit_arithmetic(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs,
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
llvm::Value* CodeGenerator::emit_overflow_checked(BinaryOp op, const Type* type, llvm::Value* lhs,
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

// 'a if cond else b': only the chosen branch runs, as a full expression of
// its own. When the result is only read, it is a temporary on the branches
// that made a new value and borrowed on the branches that read a place.
llvm::Value* CodeGenerator::emit_conditional(const ConditionalExpr& cond, Use use)
{
    llvm::Value* condition = emit_value(*cond.condition, Use::Read);
    auto* then_block = llvm::BasicBlock::Create(context_, "cond.then", current_function_);
    auto* else_block = llvm::BasicBlock::Create(context_, "cond.else", current_function_);
    auto* end_block = llvm::BasicBlock::Create(context_, "cond.end", current_function_);
    builder_.CreateCondBr(condition, then_block, else_block);

    builder_.SetInsertPoint(then_block);
    llvm::Value* then_owned = nullptr;
    llvm::Value* then_value = emit_branch_value(*cond.then_value, use, then_owned);
    llvm::BasicBlock* then_end = builder_.GetInsertBlock();
    builder_.CreateBr(end_block);

    builder_.SetInsertPoint(else_block);
    llvm::Value* else_owned = nullptr;
    llvm::Value* else_value = emit_branch_value(*cond.else_value, use, else_owned);
    llvm::BasicBlock* else_end = builder_.GetInsertBlock();
    builder_.CreateBr(end_block);

    end_block->moveAfter(else_end);
    builder_.SetInsertPoint(end_block);
    llvm::PHINode* phi = builder_.CreatePHI(llvm_type(cond.type), 2);
    phi->addIncoming(then_value, then_end);
    phi->addIncoming(else_value, else_end);
    if (use == Use::Consume || !needs_drop(cond.type)) {
        return phi;
    }
    llvm::PHINode* owned = builder_.CreatePHI(builder_.getInt1Ty(), 2);
    owned->addIncoming(then_owned, then_end);
    owned->addIncoming(else_owned, else_end);
    return keep_temporary(phi, cond.type, owned);
}

// One branch of a conditional value. When the value is only read, a place
// is borrowed and anything else is a new value the branch hands over;
// 'owned' tells which.
llvm::Value* CodeGenerator::emit_branch_value(const Expr& expr, Use use, llvm::Value*& owned)
{
    begin_full_expression();
    llvm::Value* value = nullptr;
    const bool borrowed = use == Use::Read && place_of(expr).has_value();
    if (borrowed || !needs_drop(result_type(expr))) {
        value = emit_value(expr, Use::Read);
    } else {
        value = emit_value(expr, Use::Consume);
    }
    owned = builder_.getInt1(!borrowed);
    end_full_expression();
    return value;
}

llvm::Value* CodeGenerator::emit_cast(const CastExpr& cast)
{
    llvm::Value* value = emit_value(*cast.operand, Use::Read);
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
llvm::Value* CodeGenerator::emit_checked_division(BinaryOp op, const Type* type, llvm::Value* lhs,
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
llvm::Value* CodeGenerator::emit_shift(BinaryOp op, const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    const unsigned bits = lhs->getType()->getIntegerBitWidth();
    llvm::Value* amount = builder_.CreateAnd(rhs, llvm::ConstantInt::get(rhs->getType(), bits - 1));
    if (op == BinaryOp::Shl) {
        return builder_.CreateShl(lhs, amount);
    }
    return type->is_signed ? builder_.CreateAShr(lhs, amount) : builder_.CreateLShr(lhs, amount);
}

llvm::Value* CodeGenerator::emit_call(const CallExpr& call)
{
    std::vector<llvm::Value*> args;
    const FunctionSymbol* symbol = nullptr;
    if (call.callee->kind == ExprKind::Member) {
        const auto& callee = static_cast<const MemberExpr&>(*call.callee);
        if (callee.builtin != BuiltinMethod::None) {
            return emit_builtin_method(call, callee);
        }
        symbol = callee.method;
        if (!callee.is_static_call) {
            args.push_back(emit_receiver(*symbol->self_convention, *callee.object));
        }
    } else {
        symbol = static_cast<const NameExpr&>(*call.callee).function;
        if (symbol->intrinsic == Intrinsic::Some) {
            return emit_some(*call.type, emit_value(*call.args.front(), Use::Consume));
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

// 's.len()' and 'v.clone()' read their receiver.
llvm::Value* CodeGenerator::emit_builtin_method(const CallExpr& call, const MemberExpr& callee)
{
    const Expr& object = *callee.object;
    if (callee.builtin == BuiltinMethod::StrLen) {
        llvm::Value* length = builder_.CreateExtractValue(emit_value(object, Use::Read), 1);
        return builder_.CreateIntCast(length, llvm_type(call.type), false);
    }
    llvm::Value* address = place_of(object) ? emit_address(object) : emit_pointer_argument(object);
    return emit_clone(address, result_type(object));
}

// ---------------------------------------------------------------------------
// print / write

// Each piece of the output becomes one runtime call; a format string
// f"a{x}b" writes "a", then x, then "b".
void CodeGenerator::emit_print(const Expr& arg, bool newline)
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

void CodeGenerator::emit_print_text(const std::string& text)
{
    if (text.empty()) {
        return;
    }
    llvm::Constant* value = emit_string_constant(text);
    builder_.CreateCall(runtime_function("hana_rt_print_str"),
                        {value->getAggregateElement(0u), value->getAggregateElement(1u)});
}

void CodeGenerator::emit_print_value(const Expr& expr)
{
    llvm::Value* value = emit_value(expr, Use::Read);
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

// ---------------------------------------------------------------------------
// Runtime support (runtime/hana_rt.h)

// Declares a function of runtime/hana_rt.h with the signature the code
// generator uses for it.
llvm::FunctionCallee CodeGenerator::runtime_function(const std::string& name)
{
    llvm::Type* ptr = llvm::PointerType::get(context_, 0);
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Type* void_type = builder_.getVoidTy();
    struct Signature {
        const char* name;
        llvm::Type* result;
        std::vector<llvm::Type*> params;
    };
    const Signature signatures[] = {
        {"hana_rt_print_str", void_type, {ptr, i64}},
        {"hana_rt_panic", void_type, {ptr, i64}},
        {"hana_rt_print_i64", void_type, {i64}},
        {"hana_rt_print_u64", void_type, {i64}},
        {"hana_rt_print_f64", void_type, {builder_.getDoubleTy()}},
        {"hana_rt_print_f32", void_type, {builder_.getFloatTy()}},
        {"hana_rt_print_bool", void_type, {builder_.getInt1Ty()}},
        {"hana_rt_print_char", void_type, {i32}},
        {"hana_rt_str_push", void_type, {ptr, ptr, i64}},
        {"hana_rt_str_push_i64", void_type, {ptr, i64}},
        {"hana_rt_str_push_u64", void_type, {ptr, i64}},
        {"hana_rt_str_push_f64", void_type, {ptr, builder_.getDoubleTy()}},
        {"hana_rt_str_push_f32", void_type, {ptr, builder_.getFloatTy()}},
        {"hana_rt_str_push_bool", void_type, {ptr, builder_.getInt1Ty()}},
        {"hana_rt_str_push_char", void_type, {ptr, i32}},
        {"hana_rt_str_concat", void_type, {ptr, ptr, i64, ptr, i64}},
        {"hana_rt_str_clone", void_type, {ptr, ptr, i64}},
        {"hana_rt_str_compare", i32, {ptr, i64, ptr, i64}},
        {"hana_rt_str_free", void_type, {ptr, i64}},
    };
    llvm::FunctionType* type = nullptr;
    for (const Signature& signature : signatures) {
        if (name == signature.name) {
            type = llvm::FunctionType::get(signature.result, signature.params, false);
        }
    }
    llvm::FunctionCallee callee = module_.getOrInsertFunction(name, type);
    if (name == "hana_rt_panic") {
        llvm::cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
        llvm::cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::Cold);
    }
    return callee;
}

void CodeGenerator::emit_panic_if(llvm::Value* condition, const std::string& message,
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

namespace {

// Runs the module passes 'make_passes' builds, with the analyses they need.
template <typename MakePasses>
void run_module_passes(llvm::Module& module, llvm::TargetMachine& target, MakePasses make_passes)
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
    llvm::ModulePassManager passes = make_passes(builder);
    passes.run(module, module_analyses);
}

} // namespace

void optimize_module(llvm::Module& module, llvm::TargetMachine& target)
{
    run_module_passes(module, target, [](llvm::PassBuilder& builder) {
        return builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    });
}

void instrument_address_sanitizer(llvm::Module& module, llvm::TargetMachine& target)
{
    for (llvm::Function& function : module) {
        if (!function.isDeclaration()) {
            function.addFnAttr(llvm::Attribute::SanitizeAddress);
        }
    }
    run_module_passes(module, target, [](llvm::PassBuilder&) {
        llvm::ModulePassManager passes;
        passes.addPass(llvm::AddressSanitizerPass(llvm::AddressSanitizerOptions{}));
        return passes;
    });
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
