#include "lang/sema.h"

#include <algorithm>
#include <utility>

namespace hana::lang {

namespace {

bool is_untyped_literal(const Expr& expr)
{
    if (expr.kind == ExprKind::IntLiteral) {
        return static_cast<const IntLiteralExpr&>(expr).suffix.empty();
    }
    if (expr.kind == ExprKind::FloatLiteral) {
        return static_cast<const FloatLiteralExpr&>(expr).suffix.empty();
    }
    if (expr.kind == ExprKind::Unary) {
        const auto& unary = static_cast<const UnaryExpr&>(expr);
        return unary.op == UnaryOp::Neg && is_untyped_literal(*unary.operand);
    }
    return false;
}

bool is_comparison(BinaryOp op)
{
    switch (op) {
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
        return true;
    default:
        return false;
    }
}

bool is_logical(BinaryOp op)
{
    return op == BinaryOp::And || op == BinaryOp::Or;
}

std::string quoted(const Type* type)
{
    return "'" + std::string(type->name) + "'";
}

std::uint64_t max_magnitude(const Type* type, bool negated)
{
    const unsigned bits = checked_bits(type);
    if (!type->is_signed) {
        return bits >= 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
    }
    const std::uint64_t limit = std::uint64_t{1} << (bits - 1);
    return negated ? limit : limit - 1;
}

const UnaryExpr* as_inout_mark(const Expr& expr)
{
    if (expr.kind != ExprKind::Unary) {
        return nullptr;
    }
    const auto& unary = static_cast<const UnaryExpr&>(expr);
    return unary.op == UnaryOp::InoutMark ? &unary : nullptr;
}

} // namespace

Sema::Sema(DiagnosticEngine& diagnostics)
    : diagnostics_(&diagnostics)
{
}

std::unique_ptr<Program> Sema::check(Module& module)
{
    program_ = std::make_unique<Program>();
    declare_intrinsics();
    for (auto& function : module.functions) {
        declare_function(*function);
    }
    for (auto& function : module.functions) {
        check_function_body(*function);
    }
    return std::move(program_);
}

// ---------------------------------------------------------------------------
// Declarations

void Sema::declare_intrinsics()
{
    struct IntrinsicEntry {
        const char* name;
        Intrinsic intrinsic;
    };
    static constexpr IntrinsicEntry kIntrinsics[] = {
        {"print", Intrinsic::Print},
        {"write", Intrinsic::Write},
    };
    for (const IntrinsicEntry& entry : kIntrinsics) {
        FunctionSymbol& symbol = program_->functions.emplace_back();
        symbol.name = entry.name;
        symbol.param_types = {str_type()};
        symbol.conventions = {ParamConvention::Read};
        symbol.return_type = void_type();
        symbol.intrinsic = entry.intrinsic;
        functions_[symbol.name] = &symbol;
    }
}

void Sema::declare_function(FnDecl& function)
{
    if (const FunctionSymbol* existing = lookup_function(function.name)) {
        diagnostics_->error(function.location, "function '" + function.name + "' is already defined");
        if (existing->decl != nullptr) {
            diagnostics_->note(existing->decl->location, "previous definition is here");
        }
        return;
    }
    FunctionSymbol& symbol = program_->functions.emplace_back();
    symbol.name = function.name;
    symbol.decl = &function;
    for (const Param& param : function.params) {
        symbol.param_types.push_back(resolve_type(param.type));
        symbol.conventions.push_back(param.convention);
    }
    symbol.return_type = function.return_type ? resolve_type(*function.return_type) : void_type();
    function.symbol = &symbol;
    functions_[symbol.name] = &symbol;
    if (symbol.name == "main") {
        check_main_signature(symbol);
        program_->main = &symbol;
    }
}

void Sema::check_main_signature(const FunctionSymbol& symbol)
{
    if (!symbol.param_types.empty()) {
        diagnostics_->error(symbol.decl->location, "'main' must not take parameters");
    }
    const Type* ret = symbol.return_type;
    if (!ret->is_void() && ret != i32_type() && !ret->is_error()) {
        diagnostics_->error(symbol.decl->return_type->location,
                           "'main' must return nothing or 'i32', found " + quoted(ret));
    }
}

void Sema::check_function_body(FnDecl& function)
{
    if (function.symbol == nullptr) {
        return; // duplicate definition, already reported
    }
    infer_literal_variables(function);
    check_body_once(function);
}

void Sema::check_body_once(FnDecl& function)
{
    return_type_ = function.symbol->return_type;
    push_scope();
    for (std::size_t i = 0; i < function.params.size(); ++i) {
        Param& param = function.params[i];
        const bool is_inout = param.convention == ParamConvention::Inout;
        param.symbol = declare_var(param.name, function.symbol->param_types[i],
                                   is_inout ? VarKind::InoutParam : VarKind::Param, is_inout,
                                   param.location);
    }
    const bool terminates = check_block(function.body);
    pop_scope();
    if (!terminates && !return_type_->is_void() && !return_type_->is_error()) {
        diagnostics_->error(function.body.end_location,
                           "function '" + function.name + "' must return a value of type " +
                               quoted(return_type_) + " on every path");
    }
}

// A variable initialized with an untyped literal and no declared type takes
// the type of its first typed use in the function: an argument, an operand
// next to a typed value, an assignment, a return. Inference passes run with
// diagnostics discarded and repeat until no new type is found, so chains
// like 'var a = 0; var b = a; f(b)' resolve. Unresolved variables get the
// literal's default type (i32 or f64).
void Sema::infer_literal_variables(FnDecl& function)
{
    pending_.clear();
    let_types_.clear();
    collect_pending_lets(function.body);
    if (pending_.empty()) {
        return;
    }
    DiagnosticEngine scratch;
    DiagnosticEngine* real = diagnostics_;
    diagnostics_ = &scratch;
    inferring_ = true;
    for (std::size_t round = 0; round <= pending_.size(); ++round) {
        bound_new_ = false;
        let_placeholders_.clear();
        placeholder_lets_.clear();
        check_body_once(function);
        if (!bound_new_) {
            break;
        }
    }
    inferring_ = false;
    diagnostics_ = real;
    let_placeholders_.clear();
    placeholder_lets_.clear();
    for (const PendingLet& pending : pending_) {
        let_types_.try_emplace(pending.let, pending.is_float ? f64_type() : i32_type());
    }
}

void Sema::collect_pending_lets(Block& block)
{
    for (auto& statement : block.statements) {
        switch (statement->kind) {
        case StmtKind::Let: {
            const auto& let = static_cast<const LetStmt&>(*statement);
            if (!let.declared_type && is_untyped_literal(*let.init)) {
                const Expr* literal = let.init.get();
                if (literal->kind == ExprKind::Unary) {
                    literal = static_cast<const UnaryExpr&>(*literal).operand.get();
                }
                pending_.push_back(PendingLet{&let, literal->kind == ExprKind::FloatLiteral});
            }
            break;
        }
        case StmtKind::If: {
            auto& stmt = static_cast<IfStmt&>(*statement);
            collect_pending_lets(stmt.then_block);
            if (stmt.else_block) {
                collect_pending_lets(*stmt.else_block);
            }
            break;
        }
        case StmtKind::While: collect_pending_lets(static_cast<WhileStmt&>(*statement).body); break;
        default: break;
        }
    }
}

// The type a pending literal variable gets in the current pass: its found
// type, a placeholder while inferring, or null when the let is not pending.
const Type* Sema::literal_variable_type(const LetStmt& let)
{
    if (auto found = let_types_.find(&let); found != let_types_.end()) {
        return found->second;
    }
    if (!inferring_) {
        return nullptr;
    }
    for (const PendingLet& pending : pending_) {
        if (pending.let != &let) {
            continue;
        }
        if (auto existing = let_placeholders_.find(&let); existing != let_placeholders_.end()) {
            return existing->second;
        }
        Type& placeholder = placeholder_storage_.emplace_back(Type{
            pending.is_float ? TypeKind::Float : TypeKind::Int,
            pending.is_float ? "{float}" : "{integer}", 64, true});
        placeholder.is_literal_var = true;
        let_placeholders_[&let] = &placeholder;
        placeholder_lets_[&placeholder] = &let;
        return &placeholder;
    }
    return nullptr;
}

// Records 'concrete' as the type of the variable behind 'placeholder'.
// Integer literals may become any numeric type, float literals only floats.
bool Sema::bind(const Type* placeholder, const Type* concrete)
{
    auto it = placeholder_lets_.find(placeholder);
    if (it == placeholder_lets_.end() || concrete->is_literal_var || concrete->is_error()) {
        return false;
    }
    const bool compatible = placeholder->is_float() ? concrete->is_float() : concrete->is_numeric();
    if (!compatible) {
        return false;
    }
    if (let_types_.try_emplace(it->second, concrete).second) {
        bound_new_ = true;
    }
    return true;
}

// Returns the common type of a and b, binding a placeholder on the way, or
// null when they do not match.
const Type* Sema::unify(const Type* a, const Type* b)
{
    if (a == b) {
        return a;
    }
    if (a->is_literal_var && bind(a, b)) {
        return b;
    }
    if (b->is_literal_var && bind(b, a)) {
        return a;
    }
    return nullptr;
}

const Type* Sema::resolve_type(const TypeExpr& type)
{
    if (const Type* builtin = find_builtin_type(type.name)) {
        return builtin;
    }
    diagnostics_->error(type.location, "unknown type '" + type.name + "'");
    return error_type();
}

// ---------------------------------------------------------------------------
// Statements

bool Sema::check_block(Block& block)
{
    push_scope();
    bool terminates = false;
    for (auto& statement : block.statements) {
        if (check_statement(*statement)) {
            terminates = true;
        }
    }
    pop_scope();
    return terminates;
}

bool Sema::check_statement(Stmt& stmt)
{
    switch (stmt.kind) {
    case StmtKind::Let: return check_let(static_cast<LetStmt&>(stmt));
    case StmtKind::Assign: return check_assign(static_cast<AssignStmt&>(stmt));
    case StmtKind::Expr: return check_expression_statement(static_cast<ExprStmt&>(stmt));
    case StmtKind::Return: return check_return(static_cast<ReturnStmt&>(stmt));
    case StmtKind::If: return check_if(static_cast<IfStmt&>(stmt));
    case StmtKind::While: return check_while(static_cast<WhileStmt&>(stmt));
    case StmtKind::Break: return check_jump(stmt, "break");
    case StmtKind::Continue: return check_jump(stmt, "continue");
    case StmtKind::Pass: return false;
    }
    return false;
}

bool Sema::check_let(LetStmt& let)
{
    const Type* declared =
        let.declared_type ? resolve_type(*let.declared_type) : literal_variable_type(let);
    const Type* init = check_expr(*let.init, declared);
    const Type* var_type = declared;
    if (declared != nullptr) {
        expect_type(declared, init, let.init->location);
    } else if (init->is_void()) {
        diagnostics_->error(let.init->location, "expression has no value to store");
        var_type = error_type();
    } else {
        var_type = init;
    }
    // Declared after the initializer, so 'let x = x' refers to an outer x.
    let.symbol = declare_var(let.name, var_type, VarKind::Local, let.is_mutable, let.name_location);
    return false;
}

bool Sema::check_assign(AssignStmt& assign)
{
    const VarSymbol* target = check_assignment_target(*assign.target);
    const Type* target_type = target != nullptr ? target->type : error_type();
    if (assign.op && !target_type->is_error()) {
        binary_result_type(*assign.op, target_type, assign.op_location);
    }
    const Type* value = check_expr(*assign.value, target_type);
    expect_type(target_type, value, assign.value->location);
    return false;
}

const VarSymbol* Sema::check_assignment_target(Expr& target)
{
    if (target.kind != ExprKind::Name) {
        if (target.kind != ExprKind::Error) {
            check_expr(target, nullptr);
            diagnostics_->error(target.location, "only variables can be assigned to for now");
        }
        return nullptr;
    }
    auto& name = static_cast<NameExpr&>(target);
    check_name(name);
    const VarSymbol* var = name.var;
    if (var == nullptr) {
        return nullptr;
    }
    if (!var->is_mutable) {
        if (var->kind == VarKind::Param) {
            diagnostics_->error(target.location, "cannot assign to parameter '" + var->name +
                                                    "'; declare it 'inout' to modify the "
                                                    "caller's value");
        } else {
            diagnostics_->error(target.location, "cannot assign to '" + var->name +
                                                    "' because it is declared with 'let'; use "
                                                    "'var' to make it mutable");
        }
        diagnostics_->note(var->location, "'" + var->name + "' is declared here");
    }
    return var;
}

bool Sema::check_expression_statement(ExprStmt& stmt)
{
    check_expr(*stmt.expr, nullptr);
    if (stmt.expr->kind != ExprKind::Call && stmt.expr->kind != ExprKind::Error) {
        diagnostics_->error(stmt.expr->location,
                           "this expression has no effect; only calls can be used as statements");
    }
    return false;
}

bool Sema::check_return(ReturnStmt& ret)
{
    if (ret.value) {
        const Type* value = check_expr(*ret.value, return_type_);
        if (return_type_->is_void()) {
            diagnostics_->error(ret.value->location,
                               "this function does not return a value; remove the value after "
                               "'return'");
        } else {
            expect_type(return_type_, value, ret.value->location);
        }
    } else if (!return_type_->is_void() && !return_type_->is_error()) {
        diagnostics_->error(ret.location, "missing return value of type " + quoted(return_type_));
    }
    return true;
}

bool Sema::check_if(IfStmt& stmt)
{
    check_condition(*stmt.condition);
    const bool then_terminates = check_block(stmt.then_block);
    if (!stmt.else_block) {
        return false;
    }
    const bool else_terminates = check_block(*stmt.else_block);
    return then_terminates && else_terminates;
}

bool Sema::check_while(WhileStmt& stmt)
{
    check_condition(*stmt.condition);
    loops_.push_back(LoopContext{});
    check_block(stmt.body);
    const bool has_break = loops_.back().has_break;
    loops_.pop_back();
    // 'while true:' without 'break' never finishes normally.
    const bool always_true = stmt.condition->kind == ExprKind::BoolLiteral &&
                             static_cast<const BoolLiteralExpr&>(*stmt.condition).value;
    return always_true && !has_break;
}

bool Sema::check_jump(Stmt& stmt, const char* keyword)
{
    if (loops_.empty()) {
        diagnostics_->error(stmt.location, std::string("'") + keyword + "' outside of a loop");
    } else if (stmt.kind == StmtKind::Break) {
        loops_.back().has_break = true;
    }
    return true;
}

void Sema::check_condition(Expr& condition)
{
    const Type* type = check_expr(condition, bool_type());
    expect_type(bool_type(), type, condition.location);
}

// ---------------------------------------------------------------------------
// Expressions

const Type* Sema::check_expr(Expr& expr, const Type* expected)
{
    const Type* type = error_type();
    switch (expr.kind) {
    case ExprKind::Error: break;
    case ExprKind::IntLiteral:
        type = check_int_literal(static_cast<IntLiteralExpr&>(expr), expected, false);
        break;
    case ExprKind::FloatLiteral:
        type = check_float_literal(static_cast<FloatLiteralExpr&>(expr), expected);
        break;
    case ExprKind::BoolLiteral: type = bool_type(); break;
    case ExprKind::CharLiteral: type = char_type(); break;
    case ExprKind::StringLiteral:
        type = check_string_literal(static_cast<StringLiteralExpr&>(expr));
        break;
    case ExprKind::Null:
        diagnostics_->error(expr.location, "'null' needs Option<T>, which is not supported yet");
        break;
    case ExprKind::Name: type = check_name(static_cast<NameExpr&>(expr), expected); break;
    case ExprKind::Conditional:
        type = check_conditional(static_cast<ConditionalExpr&>(expr), expected);
        break;
    case ExprKind::Cast: type = check_cast(static_cast<CastExpr&>(expr)); break;
    case ExprKind::Unary: type = check_unary(static_cast<UnaryExpr&>(expr), expected); break;
    case ExprKind::Binary: type = check_binary(static_cast<BinaryExpr&>(expr), expected); break;
    case ExprKind::Call: type = check_call(static_cast<CallExpr&>(expr)); break;
    case ExprKind::Member: type = unsupported(expr, "member access is"); break;
    case ExprKind::Index: type = unsupported(expr, "indexing is"); break;
    case ExprKind::Try: type = unsupported(expr, "the '?' operator is"); break;
    }
    expr.type = type;
    return type;
}

const Type* Sema::check_int_literal(IntLiteralExpr& lit, const Type* expected, bool negated)
{
    const Type* type = i32_type();
    if (!lit.suffix.empty()) {
        type = find_builtin_type(lit.suffix);
    } else if (expected != nullptr && expected->is_numeric()) {
        type = expected;
    }
    lit.type = type;
    if (lit.overflowed) {
        diagnostics_->error(lit.location, "integer literal is too large");
        return type;
    }
    if (type->is_integer() && !(negated && !type->is_signed) &&
        lit.value > max_magnitude(type, negated)) {
        diagnostics_->error(lit.location, "integer literal " + std::string(negated ? "-" : "") +
                                             lit.text + " does not fit in " + quoted(type));
    }
    return type;
}

const Type* Sema::check_float_literal(FloatLiteralExpr& lit, const Type* expected)
{
    if (!lit.suffix.empty()) {
        return find_builtin_type(lit.suffix);
    }
    if (expected != nullptr && expected->is_float()) {
        return expected;
    }
    return f64_type();
}

const Type* Sema::check_string_literal(StringLiteralExpr& lit)
{
    if (!lit.is_format) {
        return str_type();
    }
    if (!allow_interpolation_) {
        diagnostics_->error(lit.location, "format strings f\"...\" are only supported directly "
                                         "inside print() and write() for now");
    }
    const bool saved = allow_interpolation_;
    allow_interpolation_ = false;
    for (auto& part : lit.parts) {
        const Type* type = check_expr(*part, nullptr);
        if (type->is_void()) {
            diagnostics_->error(part->location, "expression has no value to insert");
        }
    }
    allow_interpolation_ = saved;
    return str_type();
}

const Type* Sema::check_name(NameExpr& name, const Type* expected)
{
    if (const VarSymbol* var = lookup_var(name.name)) {
        name.var = var;
        if (var->type->is_literal_var && expected != nullptr) {
            bind(var->type, expected);
        }
        return var->type;
    }
    if (const FunctionSymbol* function = lookup_function(name.name)) {
        name.function = function;
        diagnostics_->error(name.location, "'" + name.name +
                                              "' is a function; functions cannot be used as "
                                              "values yet, call it with '()'");
        return error_type();
    }
    diagnostics_->error(name.location, "unknown name '" + name.name + "'");
    return error_type();
}

const Type* Sema::check_conditional(ConditionalExpr& cond, const Type* expected)
{
    check_condition(*cond.condition);
    const Type* then_type = nullptr;
    const Type* else_type = nullptr;
    if (is_untyped_literal(*cond.then_value) && !is_untyped_literal(*cond.else_value)) {
        else_type = check_expr(*cond.else_value, expected);
        then_type = check_expr(*cond.then_value, else_type);
    } else {
        then_type = check_expr(*cond.then_value, expected);
        else_type = check_expr(*cond.else_value, then_type);
    }
    if (then_type->is_error() || else_type->is_error()) {
        return error_type();
    }
    if (then_type->is_void()) {
        diagnostics_->error(cond.then_value->location, "this branch has no value");
        return error_type();
    }
    const Type* common = unify(then_type, else_type);
    if (common == nullptr) {
        diagnostics_->error(cond.location, "the two branches of 'if ... else' have different types " +
                                              quoted(then_type) + " and " + quoted(else_type));
        return error_type();
    }
    return common;
}

// Allowed: between numeric types, bool to integer, char to integer.
const Type* Sema::check_cast(CastExpr& cast)
{
    const Type* target = resolve_type(cast.target);
    const Type* source = check_expr(*cast.operand, nullptr);
    if (target->is_error() || source->is_error()) {
        return error_type();
    }
    const bool allowed =
        (source->is_numeric() && target->is_numeric()) ||
        ((source->kind == TypeKind::Bool || source->kind == TypeKind::Char) && target->is_integer()) ||
        source == target;
    if (!allowed) {
        diagnostics_->error(cast.location, "cannot convert " + quoted(source) + " to " +
                                              quoted(target) + " with 'as'");
        return error_type();
    }
    return target;
}

const Type* Sema::check_unary(UnaryExpr& unary, const Type* expected)
{
    switch (unary.op) {
    case UnaryOp::Neg: {
        const Type* type = unary.operand->kind == ExprKind::IntLiteral
                               ? check_int_literal(static_cast<IntLiteralExpr&>(*unary.operand),
                                                   expected, true)
                               : check_expr(*unary.operand, expected);
        unary.operand->type = type;
        if (type->is_error() || type->is_float() || (type->is_integer() && type->is_signed)) {
            return type;
        }
        diagnostics_->error(unary.location, "cannot negate a value of type " + quoted(type));
        return error_type();
    }
    case UnaryOp::Not: {
        const Type* type = check_expr(*unary.operand, bool_type());
        expect_type(bool_type(), type, unary.operand->location);
        return bool_type();
    }
    case UnaryOp::BitNot: {
        const Type* type = check_expr(*unary.operand, expected);
        if (type->is_error() || type->is_integer()) {
            return type;
        }
        diagnostics_->error(unary.location, "'~' needs an integer, found " + quoted(type));
        return error_type();
    }
    case UnaryOp::InoutMark:
        check_expr(*unary.operand, nullptr);
        diagnostics_->error(unary.location, "'&' can only mark an argument for an inout parameter");
        return error_type();
    }
    return error_type();
}

const Type* Sema::check_binary(BinaryExpr& binary, const Type* expected)
{
    const Type* operand = check_binary_operands(binary, expected);
    if (operand->is_error()) {
        return error_type();
    }
    return binary_result_type(binary.op, operand, binary.op_location);
}

const Type* Sema::check_binary_operands(BinaryExpr& binary, const Type* expected)
{
    // Arithmetic results have the operand type, so the expected type flows
    // into the operands. An untyped literal takes the type of the other side.
    const Type* operand_expected = nullptr;
    if (is_logical(binary.op)) {
        operand_expected = bool_type();
    } else if (!is_comparison(binary.op)) {
        operand_expected = expected;
    }
    const Type* lhs = nullptr;
    const Type* rhs = nullptr;
    if (is_untyped_literal(*binary.lhs) && !is_untyped_literal(*binary.rhs)) {
        rhs = check_expr(*binary.rhs, operand_expected);
        lhs = check_expr(*binary.lhs, rhs);
    } else {
        lhs = check_expr(*binary.lhs, operand_expected);
        rhs = check_expr(*binary.rhs, lhs);
    }
    if (lhs->is_error() || rhs->is_error()) {
        return error_type();
    }
    const Type* common = unify(lhs, rhs);
    if (common == nullptr) {
        diagnostics_->error(binary.op_location, std::string("operands of '") +
                                                   binary_op_spelling(binary.op) +
                                                   "' have different types " + quoted(lhs) +
                                                   " and " + quoted(rhs));
        return error_type();
    }
    return common;
}

const Type* Sema::binary_result_type(BinaryOp op, const Type* operand,
                                     const SourceLocation& location)
{
    bool valid = false;
    const Type* result = operand;
    switch (op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Rem:
        valid = operand->is_numeric();
        break;
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
    case BinaryOp::Shl:
    case BinaryOp::Shr:
        valid = operand->is_integer();
        break;
    case BinaryOp::Eq:
    case BinaryOp::Ne:
        if (operand->kind == TypeKind::Str) {
            diagnostics_->error(location, "comparing 'str' values is not supported yet");
            return error_type();
        }
        valid = operand->is_numeric() || operand->kind == TypeKind::Bool ||
                operand->kind == TypeKind::Char;
        result = bool_type();
        break;
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
        valid = operand->is_numeric() || operand->kind == TypeKind::Char;
        result = bool_type();
        break;
    case BinaryOp::And:
    case BinaryOp::Or:
        valid = operand->kind == TypeKind::Bool;
        break;
    }
    if (!valid) {
        diagnostics_->error(location, std::string("operator '") + binary_op_spelling(op) +
                                         "' cannot be applied to " + quoted(operand));
        return error_type();
    }
    return result;
}

const Type* Sema::check_call(CallExpr& call)
{
    if (call.callee->kind != ExprKind::Name) {
        check_expr(*call.callee, nullptr);
        for (auto& arg : call.args) {
            check_expr(*arg, nullptr);
        }
        if (!call.callee->type->is_error()) {
            diagnostics_->error(call.location, "only named functions can be called for now");
        }
        return error_type();
    }
    auto& callee = static_cast<NameExpr&>(*call.callee);
    callee.type = void_type();
    const FunctionSymbol* function = nullptr;
    if (lookup_var(callee.name) != nullptr) {
        diagnostics_->error(callee.location, "'" + callee.name + "' is a variable, not a function");
    } else {
        function = lookup_function(callee.name);
        if (function == nullptr) {
            diagnostics_->error(callee.location, "unknown function '" + callee.name + "'");
        }
    }
    if (function == nullptr) {
        for (auto& arg : call.args) {
            check_expr(*arg, nullptr);
        }
        return error_type();
    }
    callee.function = function;
    if (function->intrinsic != Intrinsic::None) {
        return check_intrinsic_call(call, *function);
    }
    check_call_arguments(call, *function);
    return function->return_type;
}

const Type* Sema::check_intrinsic_call(CallExpr& call, const FunctionSymbol& function)
{
    if (call.args.size() != 1) {
        diagnostics_->error(call.location, "'" + function.name + "' expects 1 argument, found " +
                                              std::to_string(call.args.size()));
        for (auto& arg : call.args) {
            check_expr(*arg, nullptr);
        }
        return void_type();
    }
    allow_interpolation_ = true;
    const Type* type = check_expr(*call.args.front(), nullptr);
    allow_interpolation_ = false;
    if (type->is_void()) {
        diagnostics_->error(call.args.front()->location, "expression has no value to print");
    }
    return void_type();
}

void Sema::check_call_arguments(CallExpr& call, const FunctionSymbol& function)
{
    if (call.args.size() != function.param_types.size()) {
        diagnostics_->error(call.location, "'" + function.name + "' expects " +
                                              std::to_string(function.param_types.size()) +
                                              " argument(s), found " +
                                              std::to_string(call.args.size()));
    }
    std::vector<const VarSymbol*> inout_vars;
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        Expr& arg = *call.args[i];
        if (i >= function.param_types.size()) {
            check_expr(arg, nullptr);
            continue;
        }
        const Type* param_type = function.param_types[i];
        if (function.conventions[i] == ParamConvention::Inout) {
            check_inout_argument(arg, param_type, inout_vars);
            continue;
        }
        if (const UnaryExpr* mark = as_inout_mark(arg)) {
            check_expr(*mark->operand, nullptr);
            diagnostics_->error(arg.location, "'&' marks an inout argument, but parameter " +
                                                 std::to_string(i + 1) + " of '" + function.name +
                                                 "' is not inout");
            continue;
        }
        const Type* type = check_expr(arg, param_type);
        expect_type(param_type, type, arg.location);
    }
    check_inout_exclusivity(call, function, inout_vars);
}

void Sema::check_inout_argument(Expr& arg, const Type* param_type,
                                std::vector<const VarSymbol*>& inout_vars)
{
    const UnaryExpr* mark = as_inout_mark(arg);
    if (mark == nullptr) {
        check_expr(arg, param_type);
        diagnostics_->error(arg.location,
                           "an argument for an inout parameter must be marked with '&', as in "
                           "'&x'");
        return;
    }
    if (mark->operand->kind != ExprKind::Name) {
        check_expr(*mark->operand, nullptr);
        diagnostics_->error(mark->operand->location, "only a variable can be passed as inout");
        return;
    }
    auto& name = static_cast<NameExpr&>(*mark->operand);
    const Type* type = check_name(name);
    arg.type = type;
    if (name.var == nullptr) {
        return;
    }
    if (!name.var->is_mutable) {
        diagnostics_->error(name.location, "cannot pass '" + name.name +
                                              "' as inout because it is not mutable");
        diagnostics_->note(name.var->location, "'" + name.name + "' is declared here");
    }
    expect_type(param_type, type, name.location);
    inout_vars.push_back(name.var);
}

void Sema::check_inout_exclusivity(CallExpr& call, const FunctionSymbol& function,
                                   const std::vector<const VarSymbol*>& inout_vars)
{
    for (std::size_t i = 0; i < inout_vars.size(); ++i) {
        for (std::size_t j = i + 1; j < inout_vars.size(); ++j) {
            if (inout_vars[i] == inout_vars[j]) {
                diagnostics_->error(call.location, "'" + inout_vars[i]->name +
                                                      "' is passed as inout more than once in "
                                                      "this call");
            }
        }
    }
    for (std::size_t i = 0; i < call.args.size() && i < function.conventions.size(); ++i) {
        if (function.conventions[i] == ParamConvention::Inout ||
            call.args[i]->kind != ExprKind::Name) {
            continue;
        }
        const VarSymbol* read = static_cast<const NameExpr&>(*call.args[i]).var;
        if (read != nullptr && std::find(inout_vars.begin(), inout_vars.end(), read) !=
                                   inout_vars.end()) {
            diagnostics_->error(call.args[i]->location, "'" + read->name +
                                                           "' is passed as inout and also read "
                                                           "in the same call");
        }
    }
}

const Type* Sema::unsupported(Expr& expr, const std::string& what)
{
    diagnostics_->error(expr.location, what + " not supported yet");
    return error_type();
}

void Sema::expect_type(const Type* expected, const Type* actual, const SourceLocation& location)
{
    if (expected->is_error() || actual->is_error() || unify(expected, actual) != nullptr) {
        return;
    }
    if (actual->is_void()) {
        diagnostics_->error(location, "expected a value of type " + quoted(expected) +
                                         ", but this expression has no value");
        return;
    }
    diagnostics_->error(location, "expected " + quoted(expected) + ", found " + quoted(actual));
}

// ---------------------------------------------------------------------------
// Scopes

void Sema::push_scope()
{
    scopes_.emplace_back();
}

void Sema::pop_scope()
{
    scopes_.pop_back();
}

const VarSymbol* Sema::declare_var(const std::string& name, const Type* type, VarKind kind,
                                   bool is_mutable, const SourceLocation& location)
{
    auto& scope = scopes_.back();
    if (auto it = scope.find(name); it != scope.end()) {
        diagnostics_->error(location, "'" + name + "' is already declared in this scope");
        diagnostics_->note(it->second->location, "previous declaration is here");
    }
    VarSymbol& symbol = program_->vars.emplace_back();
    symbol.name = name;
    symbol.type = type;
    symbol.kind = kind;
    symbol.is_mutable = is_mutable;
    symbol.location = location;
    scope[name] = &symbol;
    return &symbol;
}

const VarSymbol* Sema::lookup_var(const std::string& name) const
{
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (auto found = it->find(name); found != it->end()) {
            return found->second;
        }
    }
    return nullptr;
}

const FunctionSymbol* Sema::lookup_function(const std::string& name) const
{
    auto it = functions_.find(name);
    return it != functions_.end() ? it->second : nullptr;
}

} // namespace hana::lang
