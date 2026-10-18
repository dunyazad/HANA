#include "lang/access_checker.h"

#include <optional>
#include <utility>

#include "lang/symbols.h"
#include "lang/types.h"

namespace hana::lang {

namespace {

// The place a value comes from. place_of says no for a value wrapped into
// an Option, because the Option is new; the value inside still moves out
// of its place.
std::optional<Place> source_place(const Expr& expr)
{
    if (expr.converted_to == nullptr) {
        return place_of(expr);
    }
    if (expr.kind == ExprKind::Name) {
        const auto& name = static_cast<const NameExpr&>(expr);
        if (name.var == nullptr) {
            return std::nullopt;
        }
        return Place{name.var, {}, &expr};
    }
    if (expr.kind == ExprKind::Member) {
        const auto& member = static_cast<const MemberExpr&>(expr);
        std::optional<Place> place = member.field_index >= 0 ? place_of(*member.object) : std::nullopt;
        if (place) {
            place->path.push_back(member.field_index);
            place->expr = &expr;
        }
        return place;
    }
    if (expr.kind == ExprKind::Index) {
        std::optional<Place> place = place_of(*static_cast<const IndexExpr&>(expr).object);
        if (place) {
            place->path.push_back(kAnyIndex);
            place->expr = &expr;
        }
        return place;
    }
    return std::nullopt;
}

bool is_copy_value(const Expr& expr)
{
    return expr.type == nullptr || is_copy_type(expr.type);
}

std::string quoted_place(const Place& place)
{
    return "'" + place_text(*place.expr) + "'";
}

} // namespace

AccessChecker::AccessChecker(DiagnosticEngine& diagnostics)
    : diagnostics_(diagnostics)
{
}

void AccessChecker::check(const Module& module)
{
    for (const auto& function : module.functions) {
        walk_block(function->body);
    }
    for (const auto& decl : module.types) {
        for (const auto& method : decl->methods) {
            walk_block(method->body);
        }
    }
}

// ---------------------------------------------------------------------------
// Statements

void AccessChecker::walk_block(const Block& block)
{
    for (const auto& statement : block.statements) {
        walk_statement(*statement);
    }
}

void AccessChecker::walk_statement(const Stmt& stmt)
{
    switch (stmt.kind) {
    case StmtKind::Let: walk(*static_cast<const LetStmt&>(stmt).init, true); return;
    case StmtKind::Assign: walk_assign(static_cast<const AssignStmt&>(stmt)); return;
    case StmtKind::Expr: walk(*static_cast<const ExprStmt&>(stmt).expr, false); return;
    case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        if (ret.value) {
            walk(*ret.value, true);
        }
        return;
    }
    case StmtKind::If: {
        const auto& if_stmt = static_cast<const IfStmt&>(stmt);
        walk(*if_stmt.condition, false);
        walk_block(if_stmt.then_block);
        if (if_stmt.else_block) {
            walk_block(*if_stmt.else_block);
        }
        return;
    }
    case StmtKind::While: {
        const auto& loop = static_cast<const WhileStmt&>(stmt);
        walk(*loop.condition, false);
        walk_block(loop.body);
        return;
    }
    case StmtKind::For: walk_for(static_cast<const ForStmt&>(stmt)); return;
    case StmtKind::Match: walk_match(*static_cast<const MatchStmt&>(stmt).match, false); return;
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Pass: return;
    }
}

// The value runs before the target is changed, so the target is not in use
// while the value runs ('xs[0] += grow(&xs)' is fine).
void AccessChecker::walk_assign(const AssignStmt& assign)
{
    walk(*assign.value, !assign.op.has_value());
    walk_place_parts(*assign.target);
    if (std::optional<Place> target = place_of(*assign.target)) {
        check_change(*target, *assign.target, "change", holds_.size());
    }
}

void AccessChecker::walk_for(const ForStmt& stmt)
{
    if (!stmt.iterable) {
        walk(*stmt.start, false);
        walk(*stmt.end, false);
        walk_block(stmt.body);
        return;
    }
    const bool sink = stmt.convention == ParamConvention::Sink;
    walk(*stmt.iterable, sink);
    const std::size_t base = holds_.size();
    if (!sink) {
        if (std::optional<Place> list = place_of(*stmt.iterable)) {
            if (stmt.convention == ParamConvention::Inout) {
                check_change(*list, *stmt.iterable, "change", holds_.size());
            }
            hold(*list, stmt.iterable->location,
                 "the loop over " + quoted_place(*list) + " starts here");
        }
    }
    walk_block(stmt.body);
    holds_.resize(base);
}

// ---------------------------------------------------------------------------
// Expressions

void AccessChecker::walk(const Expr& expr, bool consuming)
{
    if (expr.converted_to != nullptr && !is_copy_value(expr)) {
        consuming = true; // the new Option holds the value
    }
    switch (expr.kind) {
    case ExprKind::Name:
    case ExprKind::Member:
    case ExprKind::Index:
        walk_place_parts(expr);
        if (consuming && !is_copy_value(expr)) {
            if (std::optional<Place> place = source_place(expr)) {
                check_change(*place, expr, "move", holds_.size());
            }
        }
        return;
    case ExprKind::Unary: walk(*static_cast<const UnaryExpr&>(expr).operand, false); return;
    case ExprKind::Binary: walk_binary(static_cast<const BinaryExpr&>(expr)); return;
    case ExprKind::Conditional: {
        const auto& cond = static_cast<const ConditionalExpr&>(expr);
        walk(*cond.condition, false);
        walk(*cond.then_value, consuming);
        walk(*cond.else_value, consuming);
        return;
    }
    case ExprKind::Cast: walk(*static_cast<const CastExpr&>(expr).operand, false); return;
    case ExprKind::Call: walk_call(static_cast<const CallExpr&>(expr)); return;
    case ExprKind::Match: walk_match(static_cast<const MatchExpr&>(expr), consuming); return;
    case ExprKind::StructLiteral:
        for (const auto& field : static_cast<const StructLiteralExpr&>(expr).fields) {
            walk(*field.value, true);
        }
        return;
    case ExprKind::Tuple:
        for (const auto& element : static_cast<const TupleExpr&>(expr).elements) {
            walk(*element, true);
        }
        return;
    case ExprKind::List:
        for (const auto& element : static_cast<const ListExpr&>(expr).elements) {
            walk(*element, true);
        }
        return;
    case ExprKind::StringLiteral:
        // Each part is appended to the new string as soon as it runs.
        for (const auto& part : static_cast<const StringLiteralExpr&>(expr).parts) {
            walk(*part, false);
        }
        return;
    case ExprKind::Try: walk(*static_cast<const TryExpr&>(expr).operand, true); return;
    case ExprKind::Error:
    case ExprKind::IntLiteral:
    case ExprKind::FloatLiteral:
    case ExprKind::BoolLiteral:
    case ExprKind::CharLiteral:
    case ExprKind::Null: return;
    }
}

// The parts of a place that run code: the objects of members and the
// indices of elements, as in 'xs[i + 1].name'.
void AccessChecker::walk_place_parts(const Expr& expr)
{
    if (expr.kind == ExprKind::Member) {
        walk_place_parts(*static_cast<const MemberExpr&>(expr).object);
        return;
    }
    if (expr.kind == ExprKind::Index) {
        walk_index(static_cast<const IndexExpr&>(expr));
        return;
    }
    if (expr.kind != ExprKind::Name) {
        walk(expr, false);
    }
}

void AccessChecker::walk_index(const IndexExpr& index)
{
    walk_place_parts(*index.object);
    const std::size_t base = holds_.size();
    if (std::optional<Place> list = place_of(*index.object)) {
        hold(*list, index.location, quoted_place(*list) + " is indexed here");
    }
    walk(*index.index, false);
    holds_.resize(base);
}

void AccessChecker::walk_binary(const BinaryExpr& binary)
{
    walk(*binary.lhs, false);
    const std::size_t base = holds_.size();
    if (!is_copy_value(*binary.lhs)) {
        if (std::optional<Place> lhs = place_of(*binary.lhs)) {
            hold(*lhs, start_location(*binary.lhs),
                 quoted_place(*lhs) + " is the left operand here");
        }
    }
    walk(*binary.rhs, false);
    holds_.resize(base);
}

// Arguments run left to right after the receiver; the places they are
// passed by stay in use until the call runs. Then the call changes its
// inout places, which must not be in use by an enclosing operation.
void AccessChecker::walk_call(const CallExpr& call)
{
    const std::size_t base = holds_.size();
    std::vector<Place> changed;
    if (call.callee->kind == ExprKind::Member) {
        const auto& callee = static_cast<const MemberExpr&>(*call.callee);
        if (callee.builtin != BuiltinMethod::None) {
            const BuiltinSignature signature = builtin_signature(callee.builtin);
            walk_receiver(*callee.object, signature.self, callee.member, changed);
            for (std::size_t i = 0; i < call.args.size() && i < signature.params.size(); ++i) {
                walk_argument(*call.args[i], signature.params[i], callee.member, changed);
            }
        } else {
            const FunctionSymbol& method = *callee.method;
            if (!callee.is_static_call) {
                walk_receiver(*callee.object, *method.self_convention, callee.member, changed);
            }
            for (std::size_t i = 0; i < call.args.size(); ++i) {
                walk_argument(*call.args[i], method.conventions[i], callee.member, changed);
            }
        }
    } else {
        const FunctionSymbol& function = *static_cast<const NameExpr&>(*call.callee).function;
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            ParamConvention convention = ParamConvention::Read;
            if (is_constructor(function.intrinsic)) {
                convention = ParamConvention::Sink;
            } else if (function.intrinsic == Intrinsic::None) {
                convention = function.conventions[i];
            }
            walk_argument(*call.args[i], convention, function.name, changed);
        }
    }
    for (const Place& place : changed) {
        check_change(place, *place.expr, "change", base);
    }
    holds_.resize(base);
}

void AccessChecker::walk_argument(const Expr& arg, ParamConvention convention,
                                  const std::string& callee, std::vector<Place>& changed)
{
    if (convention == ParamConvention::Inout) {
        const Expr& operand = arg.kind == ExprKind::Unary
                                  ? *static_cast<const UnaryExpr&>(arg).operand
                                  : arg;
        walk_place_parts(operand);
        if (std::optional<Place> place = place_of(operand)) {
            hold(*place, start_location(operand),
                 quoted_place(*place) + " is passed to '" + callee + "' here");
            changed.push_back(*place);
        }
        return;
    }
    walk(arg, convention == ParamConvention::Sink);
    if (convention == ParamConvention::Read && !is_copy_value(arg)) {
        if (std::optional<Place> place = place_of(arg)) {
            hold(*place, start_location(arg),
                 quoted_place(*place) + " is passed to '" + callee + "' here");
        }
    }
}

void AccessChecker::walk_receiver(const Expr& object, ParamConvention convention,
                                  const std::string& callee, std::vector<Place>& changed)
{
    if (convention == ParamConvention::Sink) {
        walk(object, true);
        return;
    }
    walk_place_parts(object);
    std::optional<Place> place = place_of(object);
    if (!place || (convention == ParamConvention::Read && is_copy_value(object))) {
        return;
    }
    hold(*place, start_location(object),
         quoted_place(*place) + " is the receiver of '" + callee + "' here");
    if (convention == ParamConvention::Inout) {
        changed.push_back(*place);
    }
}

// Names bound from a variable that are not Copy share its data, so the
// variable stays in use for the guard and the body of their arm.
void AccessChecker::walk_match(const MatchExpr& match, bool consuming)
{
    walk(*match.subject, false);
    const std::optional<Place> subject = place_of(*match.subject);
    for (const MatchArm& arm : match.arms) {
        const std::size_t base = holds_.size();
        if (subject && binds_shared_part(*arm.pattern)) {
            hold(*subject, arm.pattern->location,
                 "a name in this pattern shares the data of " + quoted_place(*subject));
        }
        if (arm.guard) {
            walk(*arm.guard, false);
        }
        if (arm.value) {
            walk(*arm.value, consuming);
        } else {
            walk_block(arm.body);
        }
        holds_.resize(base);
    }
}

bool AccessChecker::binds_shared_part(const Pattern& pattern)
{
    if (pattern.kind == PatternKind::Binding && pattern.symbol != nullptr &&
        !is_copy_type(pattern.symbol->type)) {
        return true;
    }
    for (const auto& argument : pattern.arguments) {
        if (binds_shared_part(*argument)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helpers

void AccessChecker::hold(const Place& place, const SourceLocation& location, std::string note)
{
    holds_.push_back(Hold{place, location, std::move(note)});
}

// Changing or moving a place conflicts with the places in use below
// 'limit' that overlap it.
void AccessChecker::check_change(const Place& place, const Expr& expr, const char* verb,
                                 std::size_t limit)
{
    for (std::size_t i = 0; i < limit && i < holds_.size(); ++i) {
        const Hold& held = holds_[i];
        if (!places_overlap(place, held.place)) {
            continue;
        }
        const std::string changed = quoted_place(place);
        const std::string in_use = quoted_place(held.place);
        const std::string message =
            changed == in_use ? std::string("cannot ") + verb + " " + changed +
                                    " while it is in use"
                              : std::string("cannot ") + verb + " " + changed + " while " +
                                    in_use + " is in use";
        error_once(start_location(expr), message, held.location, held.note);
        return;
    }
}

void AccessChecker::error_once(const SourceLocation& location, const std::string& message,
                               const SourceLocation& note_location, const std::string& note)
{
    if (!reported_.insert(location.offset).second) {
        return;
    }
    diagnostics_.error(location, message);
    diagnostics_.note(note_location, note);
}

} // namespace hana::lang
