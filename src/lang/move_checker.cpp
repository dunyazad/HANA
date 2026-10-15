#include "lang/move_checker.h"

#include <utility>

#include "lang/types.h"

namespace hana::lang {

namespace {

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

const Expr& root_of(const Expr& expr)
{
    if (expr.kind == ExprKind::Member) {
        return root_of(*static_cast<const MemberExpr&>(expr).object);
    }
    return expr;
}

// Where 'p.pos.x' starts; a member expression is located at its '.'.
SourceLocation start_location(const Expr& expr)
{
    return &root_of(expr) == &expr ? expr.location : root_of(expr).location;
}

} // namespace

MoveChecker::MoveChecker(DiagnosticEngine& diagnostics)
    : diagnostics_(diagnostics)
{
}

void MoveChecker::check(const Module& module)
{
    for (const auto& function : module.functions) {
        check_function(*function);
    }
    for (const auto& decl : module.types) {
        for (const auto& method : decl->methods) {
            check_function(*method);
        }
    }
}

void MoveChecker::check_function(const FnDecl& function)
{
    State state;
    walk_block(function.body, state);
}

// ---------------------------------------------------------------------------
// Statements

void MoveChecker::walk_block(const Block& block, State& state)
{
    for (const auto& statement : block.statements) {
        walk_statement(*statement, state);
    }
}

void MoveChecker::walk_statement(const Stmt& stmt, State& state)
{
    switch (stmt.kind) {
    case StmtKind::Let: {
        const auto& let = static_cast<const LetStmt&>(stmt);
        consume(*let.init, state);
        // A loop runs the same 'let' again; each run makes a fresh variable.
        state.moved.erase(let.symbol);
        break;
    }
    case StmtKind::Assign: walk_assign(static_cast<const AssignStmt&>(stmt), state); break;
    case StmtKind::Expr: use(*static_cast<const ExprStmt&>(stmt).expr, state); break;
    case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        if (ret.value) {
            consume(*ret.value, state);
        }
        state.reachable = false;
        break;
    }
    case StmtKind::If: walk_if(static_cast<const IfStmt&>(stmt), state); break;
    case StmtKind::While: walk_while(static_cast<const WhileStmt&>(stmt), state); break;
    case StmtKind::Match: walk_match(*static_cast<const MatchStmt&>(stmt).match, false, state); break;
    case StmtKind::Break:
        loops_.back().breaks.push_back(state);
        state.reachable = false;
        break;
    case StmtKind::Continue:
        loops_.back().continues.push_back(state);
        state.reachable = false;
        break;
    case StmtKind::Pass: break;
    }
}

// Assigning a whole variable gives it a new value, so a moved variable can
// be used again afterwards. Assigning a field needs the rest of the value.
void MoveChecker::walk_assign(const AssignStmt& assign, State& state)
{
    if (assign.op) {
        use(*assign.target, state);
        use(*assign.value, state);
        return;
    }
    consume(*assign.value, state);
    if (assign.target->kind == ExprKind::Name) {
        state.moved.erase(static_cast<const NameExpr&>(*assign.target).var);
    } else if (assign.target->kind == ExprKind::Member) {
        use(*static_cast<const MemberExpr&>(*assign.target).object, state);
    }
}

void MoveChecker::walk_if(const IfStmt& stmt, State& state)
{
    use(*stmt.condition, state);
    State then_state = state;
    walk_block(stmt.then_block, then_state);
    State else_state = state;
    if (stmt.else_block) {
        walk_block(*stmt.else_block, else_state);
    }
    state = std::move(then_state);
    merge(state, else_state);
}

// The body is walked again with the moves of the previous iteration until
// nothing new is moved at the start of an iteration.
void MoveChecker::walk_while(const WhileStmt& stmt, State& state)
{
    const bool infinite = stmt.condition->kind == ExprKind::BoolLiteral &&
                          static_cast<const BoolLiteralExpr&>(*stmt.condition).value;
    State head = state;
    for (;;) {
        loops_.emplace_back();
        State body = head;
        use(*stmt.condition, body);
        State exit = body;
        if (infinite) {
            exit.reachable = false;
        }
        walk_block(stmt.body, body);
        Loop loop = std::move(loops_.back());
        loops_.pop_back();

        State next = head;
        merge(next, body);
        for (const State& continued : loop.continues) {
            merge(next, continued);
        }
        if (same_moves(next, head)) {
            for (const State& broken : loop.breaks) {
                merge(exit, broken);
            }
            state = std::move(exit);
            return;
        }
        head = std::move(next);
    }
}

// ---------------------------------------------------------------------------
// Expressions

void MoveChecker::use(const Expr& expr, State& state)
{
    switch (expr.kind) {
    case ExprKind::Name: {
        const auto& name = static_cast<const NameExpr&>(expr);
        if (name.var == nullptr || !state.reachable) {
            return;
        }
        auto moved = state.moved.find(name.var);
        if (moved != state.moved.end()) {
            const bool same_place = moved->second.offset == name.location.offset;
            error_once(name.location, "use of moved value '" + name.name + "'", &moved->second,
                       "'" + name.name + "' was moved here" +
                           (same_place ? " in the previous iteration of the loop" : ""));
            return;
        }
        // A name bound by a pattern is part of the matched variable.
        for (const VarSymbol* root = alias_of(*name.var); root != nullptr; root = alias_of(*root)) {
            auto root_moved = state.moved.find(root);
            if (root_moved != state.moved.end()) {
                error_once(name.location, "use of '" + name.name + "' after '" + root->name +
                                              "' was moved",
                           &root_moved->second, "'" + root->name + "' was moved here");
                return;
            }
        }
        return;
    }
    case ExprKind::Unary: use(*static_cast<const UnaryExpr&>(expr).operand, state); return;
    case ExprKind::Binary: {
        const auto& binary = static_cast<const BinaryExpr&>(expr);
        use(*binary.lhs, state);
        if (binary.op != BinaryOp::And && binary.op != BinaryOp::Or) {
            use(*binary.rhs, state);
            return;
        }
        // The right side of 'and' / 'or' may not run.
        State rhs = state;
        use(*binary.rhs, rhs);
        merge(state, rhs);
        return;
    }
    case ExprKind::Conditional: {
        const auto& cond = static_cast<const ConditionalExpr&>(expr);
        walk_branches(*cond.condition, *cond.then_value, *cond.else_value, false, state);
        return;
    }
    case ExprKind::Cast: use(*static_cast<const CastExpr&>(expr).operand, state); return;
    case ExprKind::Call: walk_call(static_cast<const CallExpr&>(expr), state); return;
    case ExprKind::Match: walk_match(static_cast<const MatchExpr&>(expr), false, state); return;
    case ExprKind::StructLiteral:
        for (const auto& field : static_cast<const StructLiteralExpr&>(expr).fields) {
            consume(*field.value, state);
        }
        return;
    case ExprKind::Member: use(*static_cast<const MemberExpr&>(expr).object, state); return;
    case ExprKind::StringLiteral:
        for (const auto& part : static_cast<const StringLiteralExpr&>(expr).parts) {
            use(*part, state);
        }
        return;
    case ExprKind::Index: {
        const auto& index = static_cast<const IndexExpr&>(expr);
        use(*index.object, state);
        use(*index.index, state);
        return;
    }
    case ExprKind::Try: use(*static_cast<const TryExpr&>(expr).operand, state); return;
    case ExprKind::Error:
    case ExprKind::IntLiteral:
    case ExprKind::FloatLiteral:
    case ExprKind::BoolLiteral:
    case ExprKind::CharLiteral:
    case ExprKind::Null: return;
    }
}

void MoveChecker::consume(const Expr& expr, State& state)
{
    if (expr.type == nullptr || is_copy_type(expr.type)) {
        use(expr, state);
        return;
    }
    switch (expr.kind) {
    case ExprKind::Name:
    case ExprKind::Member: {
        use(expr, state);
        // Moving a field moves the whole variable it belongs to. A field of
        // a temporary value can be moved freely.
        const Expr& root = root_of(expr);
        if (root.kind == ExprKind::Name && static_cast<const NameExpr&>(root).var != nullptr) {
            move_variable(*static_cast<const NameExpr&>(root).var, expr, state);
        }
        return;
    }
    case ExprKind::Conditional: {
        const auto& cond = static_cast<const ConditionalExpr&>(expr);
        walk_branches(*cond.condition, *cond.then_value, *cond.else_value, true, state);
        return;
    }
    case ExprKind::Match: walk_match(static_cast<const MatchExpr&>(expr), true, state); return;
    default: use(expr, state); return;
    }
}

void MoveChecker::move_variable(const VarSymbol& var, const Expr& moved, State& state)
{
    if (const VarSymbol* root = alias_of(var)) {
        // Moving a name bound by a pattern moves part of the matched variable.
        if (state.reachable) {
            state.moved.emplace(&var, start_location(moved));
        }
        move_variable(*root, moved, state);
        return;
    }
    const std::string text = place_text(moved);
    const bool whole = moved.kind == ExprKind::Name &&
                       static_cast<const NameExpr&>(moved).var == &var;
    const bool is_self = var.name == "self";
    switch (var.kind) {
    case VarKind::Param:
        error_once(start_location(moved),
                   is_self ? "cannot move '" + text + "' out of a method that only reads 'self'; "
                                                      "declare the method with 'sink self' to "
                                                      "take ownership"
                           : "cannot move '" + text + "' because " +
                                 (whole ? std::string("it") : "'" + var.name + "'") +
                                 " is a read-only parameter; declare it 'sink' to take ownership",
                   nullptr, {});
        return;
    case VarKind::InoutParam:
        error_once(start_location(moved),
                   is_self ? "cannot move '" + text + "' out of an 'inout self' method; the "
                                                      "caller still owns the value"
                           : "cannot move '" + text + "' because '" + var.name +
                                 "' is an inout parameter; the caller still owns the value",
                   nullptr, {});
        return;
    case VarKind::Local:
    case VarKind::SinkParam:
        if (state.reachable) {
            state.moved.emplace(&var, start_location(moved));
        }
        return;
    }
}

// The subject is read once. Each arm starts from the state in which the
// earlier arms did not match: their guards ran, their bodies did not. A
// name bound by a pattern refers to part of the subject, so moving it
// moves the subject's variable; binding alone moves nothing.
void MoveChecker::walk_match(const MatchExpr& match, bool consuming, State& state)
{
    use(*match.subject, state);
    const VarSymbol* root = root_variable(*match.subject);
    State fallthrough = state;
    State after;
    after.reachable = false;
    for (const MatchArm& arm : match.arms) {
        State arm_state = fallthrough;
        std::vector<const VarSymbol*> bindings;
        collect_bindings(*arm.pattern, bindings);
        for (const VarSymbol* binding : bindings) {
            arm_state.moved.erase(binding);
            if (root != nullptr) {
                aliases_[binding] = root;
            } else {
                aliases_.erase(binding);
            }
        }
        if (arm.guard) {
            use(*arm.guard, arm_state);
            merge(fallthrough, arm_state);
        }
        if (arm.value) {
            if (consuming) {
                consume(*arm.value, arm_state);
            } else {
                use(*arm.value, arm_state);
            }
        } else {
            walk_block(arm.body, arm_state);
        }
        merge(after, arm_state);
    }
    state = std::move(after);
}

void MoveChecker::collect_bindings(const Pattern& pattern, std::vector<const VarSymbol*>& out)
{
    if (pattern.kind == PatternKind::Binding && pattern.symbol != nullptr) {
        out.push_back(pattern.symbol);
    }
    for (const auto& argument : pattern.arguments) {
        collect_bindings(*argument, out);
    }
}

// The variable an expression names or is a field of, or null for a
// temporary value.
const VarSymbol* MoveChecker::root_variable(const Expr& expr) const
{
    const Expr& root = root_of(expr);
    if (root.kind != ExprKind::Name) {
        return nullptr;
    }
    return static_cast<const NameExpr&>(root).var;
}

const VarSymbol* MoveChecker::alias_of(const VarSymbol& var) const
{
    auto it = aliases_.find(&var);
    return it != aliases_.end() ? it->second : nullptr;
}

// Arguments run left to right. The receiver of a method runs first and is
// passed like an argument for 'self'.
void MoveChecker::walk_call(const CallExpr& call, State& state)
{
    if (call.callee->kind == ExprKind::Member) {
        const auto& callee = static_cast<const MemberExpr&>(*call.callee);
        const FunctionSymbol& method = *callee.method;
        if (!callee.is_static_call) {
            if (method.self_convention == ParamConvention::Sink) {
                consume(*callee.object, state);
            } else {
                use(*callee.object, state);
            }
        }
        walk_arguments(call, method, state);
        return;
    }
    const FunctionSymbol& function = *static_cast<const NameExpr&>(*call.callee).function;
    if (function.intrinsic != Intrinsic::None) {
        for (const auto& arg : call.args) {
            use(*arg, state);
        }
        return;
    }
    walk_arguments(call, function, state);
}

void MoveChecker::walk_arguments(const CallExpr& call, const FunctionSymbol& function,
                                 State& state)
{
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        if (function.conventions[i] == ParamConvention::Sink) {
            consume(*call.args[i], state);
        } else {
            use(*call.args[i], state);
        }
    }
}

void MoveChecker::walk_branches(const Expr& condition, const Expr& then_value,
                                const Expr& else_value, bool consuming, State& state)
{
    use(condition, state);
    State then_state = state;
    State else_state = state;
    if (consuming) {
        consume(then_value, then_state);
        consume(else_value, else_state);
    } else {
        use(then_value, then_state);
        use(else_value, else_state);
    }
    state = std::move(then_state);
    merge(state, else_state);
}

// ---------------------------------------------------------------------------
// Helpers

void MoveChecker::merge(State& into, const State& other)
{
    if (!other.reachable) {
        return;
    }
    if (!into.reachable) {
        into = other;
        return;
    }
    for (const auto& [var, location] : other.moved) {
        into.moved.emplace(var, location);
    }
}

bool MoveChecker::same_moves(const State& a, const State& b)
{
    if (a.reachable != b.reachable || a.moved.size() != b.moved.size()) {
        return false;
    }
    for (const auto& entry : a.moved) {
        if (b.moved.count(entry.first) == 0) {
            return false;
        }
    }
    return true;
}

void MoveChecker::error_once(const SourceLocation& location, const std::string& message,
                             const SourceLocation* note_location, const std::string& note)
{
    if (!reported_.insert(location.offset).second) {
        return;
    }
    diagnostics_.error(location, message);
    if (note_location != nullptr) {
        diagnostics_.note(*note_location, note);
    }
}

} // namespace hana::lang
