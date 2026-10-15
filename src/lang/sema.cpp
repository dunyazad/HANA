#include "lang/sema.h"

#include <algorithm>
#include <utility>

#include "lang/move_checker.h"
#include "lang/sema_util.h"

namespace hana::lang {

using sema_util::is_untyped_literal;
using sema_util::quoted;

namespace {

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

// 'p.pos.x' as written.
std::string place_text(const Expr& expr)
{
    if (expr.kind == ExprKind::Member) {
        const auto& member = static_cast<const MemberExpr&>(expr);
        return place_text(*member.object) + "." + member.member;
    }
    if (expr.kind == ExprKind::Name) {
        return static_cast<const NameExpr&>(expr).name;
    }
    return "<value>";
}

// Where 'p.pos.x' starts; a member expression is located at its '.'.
SourceLocation start_location(const Expr& expr)
{
    if (expr.kind == ExprKind::Member) {
        return start_location(*static_cast<const MemberExpr&>(expr).object);
    }
    return expr.location;
}

// Two places overlap when one contains the other: 'p' and 'p.x' do,
// 'p.x' and 'p.y' do not.
bool places_overlap(const Place& a, const Place& b)
{
    if (a.root != b.root) {
        return false;
    }
    const std::size_t common = std::min(a.path.size(), b.path.size());
    return std::equal(a.path.begin(), a.path.begin() + static_cast<std::ptrdiff_t>(common),
                      b.path.begin());
}

} // namespace

std::optional<Place> place_of(const Expr& expr)
{
    if (expr.kind == ExprKind::Name) {
        const auto& name = static_cast<const NameExpr&>(expr);
        if (name.var == nullptr) {
            return std::nullopt;
        }
        return Place{name.var, {}, &expr};
    }
    if (expr.kind == ExprKind::Member) {
        const auto& member = static_cast<const MemberExpr&>(expr);
        if (member.field_index < 0) {
            return std::nullopt;
        }
        std::optional<Place> place = place_of(*member.object);
        if (place) {
            place->path.push_back(member.field_index);
            place->expr = &expr;
        }
        return place;
    }
    return std::nullopt;
}

Sema::Sema(DiagnosticEngine& diagnostics)
    : diagnostics_(&diagnostics)
{
}

std::unique_ptr<Program> Sema::check(Module& module)
{
    program_ = std::make_unique<Program>();
    declare_intrinsics();
    for (auto& decl : module.types) {
        declare_type(*decl);
    }
    for (auto& decl : module.types) {
        declare_type_fields(*decl);
    }
    for (const auto& decl : module.types) {
        check_type_containment(*decl);
        check_copy_type(*decl);
    }
    for (auto& function : module.functions) {
        declare_function(*function);
    }
    for (auto& decl : module.types) {
        for (auto& method : decl->methods) {
            declare_method(*decl, *method);
        }
    }
    for (auto& decl : module.types) {
        check_field_defaults(*decl);
    }
    for (auto& function : module.functions) {
        check_function_body(*function);
    }
    for (auto& decl : module.types) {
        for (auto& method : decl->methods) {
            check_function_body(*method);
        }
    }
    // Moves are checked only on a well-typed module, so every expression
    // has its type and every name its symbol.
    if (!diagnostics_->has_errors()) {
        MoveChecker(*diagnostics_).check(module);
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

// Type names are declared before any field or function so types can refer
// to structs and enums declared later in the file.
void Sema::declare_type(TypeDecl& decl)
{
    if (find_builtin_type(decl.name) != nullptr) {
        diagnostics_->error(decl.location, "'" + decl.name + "' is a built-in type and cannot be "
                                                            "redefined");
        return;
    }
    if (UserType* existing = lookup_type(decl.name)) {
        diagnostics_->error(decl.location, "'" + decl.name + "' is already defined as " +
                                               (existing->is_enum() ? "an enum" : "a struct"));
        diagnostics_->note(existing->location, "previous definition is here");
        return;
    }
    UserType& info = program_->types.emplace_back();
    info.name = decl.name;
    info.location = decl.location;
    info.is_copy = decl.is_copy;
    info.type.kind = decl.is_enum ? TypeKind::Enum : TypeKind::Struct;
    info.type.name = info.name;
    info.type.user = &info;
    decl.type = &info.type;
    types_[info.name] = &info;
}

void Sema::declare_type_fields(TypeDecl& decl)
{
    if (decl.type == nullptr) {
        return; // duplicate definition, already reported
    }
    UserType& info = *lookup_type(decl.name);
    declare_fields(decl.fields, info.fields, "'" + decl.name + "'");
    for (CaseDecl& entry : decl.cases) {
        if (info.find_case(entry.name) >= 0) {
            diagnostics_->error(entry.location, "case '" + entry.name + "' is already declared in '" +
                                                    decl.name + "'");
            continue;
        }
        EnumCase& target = info.cases.emplace_back();
        target.name = entry.name;
        target.location = entry.location;
        declare_fields(entry.fields, target.fields, "case '" + decl.name + "." + entry.name + "'");
    }
}

void Sema::declare_fields(std::vector<FieldDecl>& decls, std::vector<FieldInfo>& fields,
                          const std::string& owner)
{
    for (FieldDecl& field : decls) {
        if (find_field(fields, field.name) >= 0) {
            diagnostics_->error(field.location, "field '" + field.name + "' is already declared in " +
                                                    owner);
            continue;
        }
        FieldInfo& entry = fields.emplace_back();
        entry.name = field.name;
        entry.type = resolve_type(field.type);
        entry.default_value = field.default_value.get();
        entry.location = field.location;
    }
}

// The fields a value of the type holds directly: a struct's fields, or the
// fields of every case of an enum.
std::vector<const FieldInfo*> Sema::value_fields(const UserType& info)
{
    std::vector<const FieldInfo*> result;
    for (const FieldInfo& field : info.fields) {
        result.push_back(&field);
    }
    for (const EnumCase& entry : info.cases) {
        for (const FieldInfo& field : entry.fields) {
            result.push_back(&field);
        }
    }
    return result;
}

// A struct or enum cannot hold itself by value: its size would be infinite.
void Sema::check_type_containment(const TypeDecl& decl)
{
    if (decl.type == nullptr) {
        return;
    }
    const UserType& info = *decl.type->user;
    for (const FieldInfo* field : value_fields(info)) {
        if (!field->type->is_user()) {
            continue;
        }
        std::vector<const UserType*> visiting;
        if (contains_type(*field->type->user, info, visiting)) {
            diagnostics_->error(field->location, std::string(info.kind_word()) + " '" + info.name +
                                                     "' contains itself through field '" +
                                                     field->name + "'; " +
                                                     (info.is_enum() ? "an enum" : "a struct") +
                                                     " cannot hold itself by value");
        }
    }
}

bool Sema::contains_type(const UserType& outer, const UserType& inner,
                         std::vector<const UserType*>& visiting) const
{
    if (&outer == &inner) {
        return true;
    }
    if (std::find(visiting.begin(), visiting.end(), &outer) != visiting.end()) {
        return false; // a cycle that does not pass through 'inner', reported elsewhere
    }
    visiting.push_back(&outer);
    for (const FieldInfo* field : value_fields(outer)) {
        if (field->type->is_user() && contains_type(*field->type->user, inner, visiting)) {
            return true;
        }
    }
    return false;
}

void Sema::check_copy_type(const TypeDecl& decl)
{
    if (decl.type == nullptr || !decl.is_copy) {
        return;
    }
    const UserType& info = *decl.type->user;
    for (const FieldInfo* field : value_fields(info)) {
        if (!field->type->is_error() && !is_copy_type(field->type)) {
            diagnostics_->error(field->location, "'" + decl.name + "' is declared '" +
                                                     info.kind_word() + " copy', but field '" +
                                                     field->name + "' of type " +
                                                     quoted(field->type) + " is not Copy");
        }
    }
}

// Defaults are checked without any variables in scope; they are evaluated
// again for every value that leaves the field out.
void Sema::check_field_defaults(TypeDecl& decl)
{
    if (decl.type == nullptr) {
        return;
    }
    const UserType& info = *decl.type->user;
    check_defaults(decl.fields, info.fields);
    for (CaseDecl& entry : decl.cases) {
        const int index = info.find_case(entry.name);
        if (index >= 0) {
            check_defaults(entry.fields, info.cases[static_cast<std::size_t>(index)].fields);
        }
    }
}

void Sema::check_defaults(std::vector<FieldDecl>& decls, const std::vector<FieldInfo>& fields)
{
    for (FieldDecl& field : decls) {
        if (!field.default_value) {
            continue;
        }
        const int index = find_field(fields, field.name);
        const Type* type = index >= 0 ? fields[static_cast<std::size_t>(index)].type : error_type();
        return_type_ = void_type();
        push_scope();
        const Type* value = check_expr(*field.default_value, type);
        pop_scope();
        expect_type(type, value, field.default_value->location);
    }
}

FunctionSymbol& Sema::create_function_symbol(FnDecl& function, std::string name)
{
    FunctionSymbol& symbol = program_->functions.emplace_back();
    symbol.name = std::move(name);
    symbol.decl = &function;
    symbol.self_convention = function.self_convention;
    for (const Param& param : function.params) {
        symbol.param_types.push_back(resolve_type(param.type));
        symbol.conventions.push_back(param.convention);
    }
    symbol.return_type = function.return_type ? resolve_type(*function.return_type) : void_type();
    function.symbol = &symbol;
    return symbol;
}

void Sema::declare_method(TypeDecl& decl, FnDecl& method)
{
    if (decl.type == nullptr) {
        return;
    }
    UserType& info = *lookup_type(decl.name);
    if (info.find_field(method.name) >= 0 || info.find_case(method.name) >= 0) {
        diagnostics_->error(method.location, "'" + method.name + "' is already declared as a " +
                                                 (info.is_enum() ? "case" : "field") + " of '" +
                                                 decl.name + "'");
        return;
    }
    if (auto existing = info.methods.find(method.name); existing != info.methods.end()) {
        diagnostics_->error(method.location, "function '" + method.name +
                                                 "' is already defined in '" + decl.name + "'");
        diagnostics_->note(existing->second->decl->location, "previous definition is here");
        return;
    }
    FunctionSymbol& symbol = create_function_symbol(method, decl.name + "." + method.name);
    symbol.owner = &info;
    info.methods[method.name] = &symbol;
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
    if (const UserType* existing = lookup_type(function.name)) {
        diagnostics_->error(function.location, "'" + function.name + "' is already defined as " +
                                                   (existing->is_enum() ? "an enum" : "a struct"));
        diagnostics_->note(existing->location, std::string("the ") + existing->kind_word() +
                                                   " is defined here");
        return;
    }
    FunctionSymbol& symbol = create_function_symbol(function, function.name);
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
    declare_self(function);
    for (std::size_t i = 0; i < function.params.size(); ++i) {
        Param& param = function.params[i];
        const bool is_inout = param.convention == ParamConvention::Inout;
        VarKind kind = VarKind::Param;
        if (is_inout) {
            kind = VarKind::InoutParam;
        } else if (param.convention == ParamConvention::Sink) {
            kind = VarKind::SinkParam;
        }
        param.symbol = declare_var(param.name, function.symbol->param_types[i], kind, is_inout,
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

void Sema::declare_self(FnDecl& function)
{
    if (!function.self_convention || function.owner == nullptr || function.owner->type == nullptr) {
        return;
    }
    VarKind kind = VarKind::Param;
    if (*function.self_convention == ParamConvention::Inout) {
        kind = VarKind::InoutParam;
    } else if (*function.self_convention == ParamConvention::Sink) {
        kind = VarKind::SinkParam;
    }
    function.self_symbol = declare_var("self", function.owner->type, kind,
                                       kind == VarKind::InoutParam, function.self_location);
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
        case StmtKind::Match:
            for (MatchArm& arm : static_cast<MatchStmt&>(*statement).match->arms) {
                collect_pending_lets(arm.body);
            }
            break;
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
    if (const UserType* info = lookup_type(type.name)) {
        return &info->type;
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
    case StmtKind::Match: return check_match_statement(static_cast<MatchStmt&>(stmt));
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
    const Type* target_type = check_assignment_target(*assign.target);
    if (assign.op && !target_type->is_error()) {
        binary_result_type(*assign.op, target_type, assign.op_location);
    }
    const Type* value = check_expr(*assign.value, target_type);
    expect_type(target_type, value, assign.value->location);
    return false;
}

// A target is a variable or a field path of one: 'x', 'p.pos.x'.
const Type* Sema::check_assignment_target(Expr& target)
{
    if (target.kind != ExprKind::Name && target.kind != ExprKind::Member) {
        if (target.kind != ExprKind::Error) {
            check_expr(target, nullptr);
            diagnostics_->error(target.location,
                               "only variables and their fields can be assigned to for now");
        }
        return error_type();
    }
    const Type* type = check_expr(target, nullptr);
    if (type->is_error()) {
        return type;
    }
    std::optional<Place> place = place_of(target);
    if (!place) {
        diagnostics_->error(target.location, "cannot assign to a field of a temporary value");
        return type;
    }
    require_mutable(*place, MutationKind::Assign, {});
    return type;
}

// Reports an error when the root variable of the place cannot be modified.
void Sema::require_mutable(const Place& place, MutationKind kind, const std::string& method_name)
{
    const VarSymbol& var = *place.root;
    if (var.is_mutable) {
        return;
    }
    const std::string text = place_text(*place.expr);
    const bool whole = place.path.empty();
    const bool is_param = var.kind == VarKind::Param || var.kind == VarKind::SinkParam;
    std::string remedy;
    if (is_param && var.name == "self") {
        remedy = "declare the method with 'inout self' to modify the value it is called on";
    } else if (is_param) {
        remedy = "declare it 'inout' to modify the caller's value";
    } else {
        remedy = "use 'var' to make it mutable";
    }
    std::string message;
    switch (kind) {
    case MutationKind::Assign:
        if (whole && is_param) {
            message = "cannot assign to parameter '" + var.name + "'; " + remedy;
        } else if (whole) {
            message = "cannot assign to '" + var.name + "' because it is declared with 'let'; " +
                      remedy;
        } else {
            message = "cannot assign to '" + text + "' because '" + var.name + "' is " +
                      (is_param ? "a read-only parameter; " : "declared with 'let'; ") + remedy;
        }
        break;
    case MutationKind::PassInout:
        message = "cannot pass '" + text + "' as inout because " +
                  (whole ? std::string("it is not mutable")
                         : "'" + var.name + "' is not mutable");
        break;
    case MutationKind::InoutReceiver:
        message = "cannot call '" + method_name + "' on '" + text +
                  "' because it modifies its value ('inout self') and '" + var.name +
                  "' is not mutable; " + remedy;
        break;
    }
    diagnostics_->error(start_location(*place.expr), message);
    diagnostics_->note(var.location, "'" + var.name + "' is declared here");
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
    case ExprKind::StructLiteral:
        type = check_struct_literal(static_cast<StructLiteralExpr&>(expr));
        break;
    case ExprKind::Member: type = check_member(static_cast<MemberExpr&>(expr)); break;
    case ExprKind::Match:
        type = check_match_expression(static_cast<MatchExpr&>(expr), expected);
        break;
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
        } else if (type->is_user()) {
            check_printable(type, part->location);
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
    if (const UserType* info = lookup_type(name.name)) {
        diagnostics_->error(name.location, type_name_hint(*info));
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

// 'Name { field: value, ... }' or 'Enum.Case { field: value, ... }': every
// field without a default must be given, each at most once.
const Type* Sema::check_struct_literal(StructLiteralExpr& lit)
{
    const std::vector<FieldInfo>* fields = literal_fields(lit);
    if (fields == nullptr) {
        for (auto& field : lit.fields) {
            check_expr(*field.value, nullptr);
        }
        return error_type();
    }
    const UserType& info = *lookup_type(lit.type_name);
    const std::string written =
        lit.case_name.empty() ? info.name : info.name + "." + lit.case_name;
    lit.field_order.assign(fields->size(), -1);
    for (std::size_t i = 0; i < lit.fields.size(); ++i) {
        StructLiteralExpr::FieldInit& field = lit.fields[i];
        const int index = find_field(*fields, field.name);
        if (index < 0) {
            diagnostics_->error(field.location, "'" + written + "' has no field '" + field.name + "'");
            check_expr(*field.value, nullptr);
            continue;
        }
        int& slot = lit.field_order[static_cast<std::size_t>(index)];
        if (slot >= 0) {
            diagnostics_->error(field.location, "field '" + field.name + "' is given more than once");
            check_expr(*field.value, nullptr);
            continue;
        }
        slot = static_cast<int>(i);
        const Type* field_type = (*fields)[static_cast<std::size_t>(index)].type;
        const Type* value = check_expr(*field.value, field_type);
        expect_type(field_type, value, field.value->location);
    }
    report_missing_fields(lit, *fields, written);
    return &info.type;
}

// The fields a struct value or an enum case takes, or null after an error.
const std::vector<FieldInfo>* Sema::literal_fields(StructLiteralExpr& lit)
{
    const UserType* info = lookup_type(lit.type_name);
    if (info == nullptr) {
        diagnostics_->error(lit.location, "unknown type '" + lit.type_name + "'");
        return nullptr;
    }
    if (!info->is_enum()) {
        if (!lit.case_name.empty()) {
            diagnostics_->error(lit.case_location, "'" + info->name + "' is a struct; create a value "
                                                       "with '" + info->name + " { ... }'");
            return nullptr;
        }
        return &info->fields;
    }
    if (lit.case_name.empty()) {
        diagnostics_->error(lit.location, type_name_hint(*info));
        return nullptr;
    }
    lit.case_index = info->find_case(lit.case_name);
    if (lit.case_index < 0) {
        diagnostics_->error(lit.case_location, "'" + info->name + "' has no case '" + lit.case_name +
                                                   "'");
        return nullptr;
    }
    return &info->cases[static_cast<std::size_t>(lit.case_index)].fields;
}

void Sema::report_missing_fields(const StructLiteralExpr& lit, const std::vector<FieldInfo>& fields,
                                 const std::string& written)
{
    std::string missing;
    std::size_t count = 0;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (lit.field_order[i] >= 0 || fields[i].default_value != nullptr) {
            continue;
        }
        missing += (count == 0 ? "'" : ", '") + fields[i].name + "'";
        ++count;
    }
    if (count > 0) {
        diagnostics_->error(lit.location, std::string(count == 1 ? "missing field " : "missing fields ") +
                                             missing + " in '" + written + " { ... }'");
    }
}

// Explains how to make a value when a type name is used like a value.
std::string Sema::type_name_hint(const UserType& info)
{
    if (!info.is_enum()) {
        return "'" + info.name + "' is a struct; create a value with '" + info.name + " { ... }'";
    }
    std::string example = info.name + ".Case { ... }";
    if (!info.cases.empty()) {
        const EnumCase& first = info.cases.front();
        example = info.name + "." + first.name + (first.fields.empty() ? "" : " { ... }");
    }
    return "'" + info.name + "' is an enum; create a value of one of its cases, as in '" + example +
           "'";
}

// A field read 'value.field', or an enum case without fields 'Color.Red'.
// Calls of methods are handled by check_call.
const Type* Sema::check_member(MemberExpr& member)
{
    if (const UserType* info = static_type_name(*member.object)) {
        return check_static_member(member, *info);
    }
    const Type* object = check_expr(*member.object, nullptr);
    if (object->is_error()) {
        return error_type();
    }
    if (!object->is_user()) {
        diagnostics_->error(member.location, "a value of type " + quoted(object) + " has no field '" +
                                                member.member + "'");
        return error_type();
    }
    const UserType& info = *object->user;
    const int index = info.find_field(member.member);
    if (index >= 0) {
        member.field_index = index;
        return info.fields[static_cast<std::size_t>(index)].type;
    }
    if (info.methods.count(member.member) != 0) {
        diagnostics_->error(member.location, "methods cannot be used as values yet; call '" +
                                                member.member + "' with '()'");
    } else if (info.is_enum()) {
        diagnostics_->error(member.location, "a value of the enum '" + info.name +
                                                "' has no field '" + member.member +
                                                "'; use 'match' to read the fields of its case");
    } else {
        diagnostics_->error(member.location, "'" + info.name + "' has no field '" + member.member +
                                                "'");
    }
    return error_type();
}

// 'Type.member' outside a call: only an enum case without fields is a value.
const Type* Sema::check_static_member(MemberExpr& member, const UserType& info)
{
    member.object->type = &info.type;
    const int index = info.find_case(member.member);
    if (index < 0) {
        if (info.methods.count(member.member) != 0) {
            diagnostics_->error(member.location, "'" + info.name + "." + member.member +
                                                    "' is not a value; call the function with '()'");
        } else {
            diagnostics_->error(member.location, "'" + info.name + "' has no " +
                                                    (info.is_enum() ? "case" : "function") + " '" +
                                                    member.member + "'");
        }
        return error_type();
    }
    const EnumCase& entry = info.cases[static_cast<std::size_t>(index)];
    if (!entry.fields.empty()) {
        diagnostics_->error(member.location, "'" + info.name + "." + entry.name + "' has fields; "
                                                "create it with '" + info.name + "." + entry.name +
                                                " { " + entry.fields.front().name + ": ... }'");
        return error_type();
    }
    member.case_index = index;
    return &info.type;
}

// The type named by 'Point' in 'Point.new(...)' or 'Color.Red', unless a
// variable of that name hides it.
const UserType* Sema::static_type_name(const Expr& object) const
{
    if (object.kind != ExprKind::Name) {
        return nullptr;
    }
    const std::string& name = static_cast<const NameExpr&>(object).name;
    return lookup_var(name) == nullptr ? lookup_type(name) : nullptr;
}

const Type* Sema::check_call(CallExpr& call)
{
    if (call.callee->kind == ExprKind::Name) {
        return check_function_call(call, static_cast<NameExpr&>(*call.callee));
    }
    if (call.callee->kind == ExprKind::Member) {
        return check_method_call(call, static_cast<MemberExpr&>(*call.callee));
    }
    check_expr(*call.callee, nullptr);
    check_args_without_callee(call);
    if (!call.callee->type->is_error()) {
        diagnostics_->error(call.location, "only named functions can be called for now");
    }
    return error_type();
}

const Type* Sema::check_function_call(CallExpr& call, NameExpr& callee)
{
    callee.type = void_type();
    const FunctionSymbol* function = nullptr;
    if (lookup_var(callee.name) != nullptr) {
        diagnostics_->error(callee.location, "'" + callee.name + "' is a variable, not a function");
    } else if (const UserType* info = lookup_type(callee.name)) {
        diagnostics_->error(callee.location, type_name_hint(*info));
    } else {
        function = lookup_function(callee.name);
        if (function == nullptr) {
            diagnostics_->error(callee.location, "unknown function '" + callee.name + "'");
        }
    }
    if (function == nullptr) {
        check_args_without_callee(call);
        return error_type();
    }
    callee.function = function;
    if (function->intrinsic != Intrinsic::None) {
        return check_intrinsic_call(call, *function);
    }
    check_call_arguments(call, *function, std::nullopt);
    return function->return_type;
}

// 'Point.new(...)' calls a function of the struct without 'self';
// 'p.area()' calls a method on the value p.
const Type* Sema::check_method_call(CallExpr& call, MemberExpr& callee)
{
    callee.type = void_type();
    const FunctionSymbol* function = nullptr;
    std::optional<Place> receiver;
    if (const UserType* info = static_type_name(*callee.object)) {
        callee.object->type = &info->type;
        function = find_static_function(callee, *info);
    } else {
        const Type* object = check_expr(*callee.object, nullptr);
        function = find_method(callee, object);
        if (function != nullptr) {
            receiver = check_receiver(callee, *function);
        }
    }
    if (function == nullptr) {
        check_args_without_callee(call);
        return error_type();
    }
    callee.method = function;
    check_call_arguments(call, *function, receiver);
    return function->return_type;
}

const FunctionSymbol* Sema::find_static_function(MemberExpr& callee, const UserType& info)
{
    auto it = info.methods.find(callee.member);
    if (it == info.methods.end()) {
        if (info.find_case(callee.member) >= 0) {
            diagnostics_->error(callee.location, "enum values are created with '" + info.name + "." +
                                                    callee.member + " { field: value }', not with "
                                                                    "'()'");
        } else {
            diagnostics_->error(callee.location, "'" + info.name + "' has no function '" +
                                                    callee.member + "'");
        }
        return nullptr;
    }
    if (it->second->self_convention) {
        diagnostics_->error(callee.location, "'" + it->second->name + "' takes 'self'; call it on a "
                                                                     "value, as in 'value." +
                                                                     callee.member + "()'");
        return nullptr;
    }
    callee.is_static_call = true;
    return it->second;
}

const FunctionSymbol* Sema::find_method(MemberExpr& callee, const Type* receiver)
{
    if (receiver->is_error()) {
        return nullptr;
    }
    if (!receiver->is_user()) {
        diagnostics_->error(callee.location, "a value of type " + quoted(receiver) +
                                                " has no method '" + callee.member + "'");
        return nullptr;
    }
    const UserType& info = *receiver->user;
    auto it = info.methods.find(callee.member);
    if (it == info.methods.end()) {
        if (info.find_field(callee.member) >= 0) {
            diagnostics_->error(callee.location, "'" + callee.member + "' is a field of '" +
                                                    info.name + "', not a function");
        } else {
            diagnostics_->error(callee.location, "'" + info.name + "' has no method '" +
                                                    callee.member + "'");
        }
        return nullptr;
    }
    if (!it->second->self_convention) {
        diagnostics_->error(callee.location, "'" + it->second->name + "' does not take 'self'; "
                                                                     "call it as '" +
                                                                     it->second->name + "(...)'");
        return nullptr;
    }
    return it->second;
}

// The value a method is called on is passed like an argument for 'self':
// an 'inout self' method needs a mutable variable or field of one.
std::optional<Place> Sema::check_receiver(MemberExpr& callee, const FunctionSymbol& method)
{
    std::optional<Place> place = place_of(*callee.object);
    if (*method.self_convention != ParamConvention::Inout) {
        return place;
    }
    if (!place) {
        diagnostics_->error(callee.object->location,
                           "'" + method.name + "' modifies its value ('inout self'), so it must "
                                               "be called on a variable or a field of one");
        return std::nullopt;
    }
    require_mutable(*place, MutationKind::InoutReceiver, method.name);
    return place;
}

void Sema::check_args_without_callee(CallExpr& call)
{
    for (auto& arg : call.args) {
        if (const UnaryExpr* mark = as_inout_mark(*arg)) {
            check_expr(*mark->operand, nullptr);
        } else {
            check_expr(*arg, nullptr);
        }
    }
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
    check_printable(type, call.args.front()->location);
    return void_type();
}

void Sema::check_printable(const Type* type, const SourceLocation& location)
{
    if (type->is_void()) {
        diagnostics_->error(location, "expression has no value to print");
    } else if (type->is_struct()) {
        diagnostics_->error(location, "a value of type " + quoted(type) +
                                          " cannot be printed yet; print its fields");
    } else if (type->is_enum()) {
        diagnostics_->error(location, "a value of type " + quoted(type) +
                                          " cannot be printed yet; use 'match' to print each case");
    }
}

void Sema::check_call_arguments(CallExpr& call, const FunctionSymbol& function,
                                const std::optional<Place>& receiver)
{
    if (call.args.size() != function.param_types.size()) {
        diagnostics_->error(call.location, "'" + function.name + "' expects " +
                                              std::to_string(function.param_types.size()) +
                                              " argument(s), found " +
                                              std::to_string(call.args.size()));
    }
    std::vector<Place> inout_places;
    if (receiver && function.self_convention == ParamConvention::Inout) {
        inout_places.push_back(*receiver);
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        Expr& arg = *call.args[i];
        if (i >= function.param_types.size()) {
            const UnaryExpr* mark = as_inout_mark(arg);
            check_expr(mark != nullptr ? *mark->operand : arg, nullptr);
            continue;
        }
        const Type* param_type = function.param_types[i];
        if (function.conventions[i] == ParamConvention::Inout) {
            check_inout_argument(arg, param_type, inout_places);
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
    check_inout_exclusivity(call, function, inout_places, receiver);
}

void Sema::check_inout_argument(Expr& arg, const Type* param_type, std::vector<Place>& inout_places)
{
    const UnaryExpr* mark = as_inout_mark(arg);
    if (mark == nullptr) {
        check_expr(arg, param_type);
        diagnostics_->error(arg.location,
                           "an argument for an inout parameter must be marked with '&', as in "
                           "'&x'");
        return;
    }
    Expr& operand = *mark->operand;
    const Type* type = check_expr(operand, nullptr);
    arg.type = type;
    if (type->is_error()) {
        return;
    }
    std::optional<Place> place = place_of(operand);
    if (!place) {
        diagnostics_->error(operand.location,
                           "only a variable or a field of one can be passed as inout");
        return;
    }
    require_mutable(*place, MutationKind::PassInout, {});
    expect_type(param_type, type, operand.location);
    inout_places.push_back(*place);
}

// While a call runs, a place passed as inout must not be reachable through
// any other argument or the receiver. A Copy value read by the call is
// copied before the call starts, so it may overlap an inout place.
void Sema::check_inout_exclusivity(CallExpr& call, const FunctionSymbol& function,
                                   const std::vector<Place>& inout_places,
                                   const std::optional<Place>& receiver)
{
    for (std::size_t i = 0; i < inout_places.size(); ++i) {
        for (std::size_t j = i + 1; j < inout_places.size(); ++j) {
            if (places_overlap(inout_places[i], inout_places[j])) {
                diagnostics_->error(call.location, "'" + place_text(*inout_places[j].expr) +
                                                      "' is passed as inout more than once in "
                                                      "this call");
            }
        }
    }
    std::vector<Place> reads;
    if (receiver && function.self_convention != ParamConvention::Inout) {
        reads.push_back(*receiver);
    }
    for (std::size_t i = 0; i < call.args.size() && i < function.conventions.size(); ++i) {
        if (function.conventions[i] == ParamConvention::Inout) {
            continue;
        }
        if (std::optional<Place> read = place_of(*call.args[i])) {
            reads.push_back(*read);
        }
    }
    for (const Place& read : reads) {
        if (is_copy_type(read.expr->type)) {
            continue;
        }
        for (const Place& written : inout_places) {
            if (places_overlap(read, written)) {
                diagnostics_->error(start_location(*read.expr), "'" + place_text(*read.expr) +
                                                            "' is passed as inout and also read "
                                                            "in the same call");
                break;
            }
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

UserType* Sema::lookup_type(const std::string& name) const
{
    auto it = types_.find(name);
    return it != types_.end() ? it->second : nullptr;
}

} // namespace hana::lang
