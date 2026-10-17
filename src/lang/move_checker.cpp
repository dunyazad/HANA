#include "lang/move_checker.h"

#include <utility>

#include "lang/sema.h"
#include "lang/types.h"

namespace hana::lang {

namespace {

// The variable or temporary value that 'p.pos.x' or 'xs[i].name' is part
// of.
const Expr& root_of(const Expr& expr)
{
    if (expr.kind == ExprKind::Member) {
        return root_of(*static_cast<const MemberExpr&>(expr).object);
    }
    if (expr.kind == ExprKind::Index) {
        return root_of(*static_cast<const IndexExpr&>(expr).object);
    }
    return expr;
}

// True for a list element or a part of one, as in 'xs[i].name'.
bool is_in_element(const Expr& expr)
{
    if (expr.kind == ExprKind::Index) {
        return true;
    }
    if (expr.kind == ExprKind::Member) {
        return is_in_element(*static_cast<const MemberExpr&>(expr).object);
    }
    return false;
}

const char* const kElementRemedy = "use '.clone()', 'pop()' or 'remove(i)'";

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
        // A loop runs the same 'let' again; each run makes fresh variables.
        // Names from 'let (a, b) = ...' own their parts of the moved value.
        state.moved.erase(let.symbol);
        if (let.pattern) {
            std::vector<const VarSymbol*> bindings;
            collect_bindings(*let.pattern, bindings);
            for (const VarSymbol* binding : bindings) {
                state.moved.erase(binding);
                aliases_.erase(binding);
            }
        }
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
    case StmtKind::For: walk_for(static_cast<const ForStmt&>(stmt), state); break;
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
    } else if (assign.target->kind == ExprKind::Index) {
        use(*assign.target, state);
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

void MoveChecker::walk_while(const WhileStmt& stmt, State& state)
{
    const bool infinite = stmt.condition->kind == ExprKind::BoolLiteral &&
                          static_cast<const BoolLiteralExpr&>(*stmt.condition).value;
    walk_loop(stmt.condition.get(), infinite, stmt.body, state, nullptr);
}

// The bounds or the list run once before the loop; the body may run zero
// times. 'for sink' moves the list into the loop, and each iteration's
// variable owns a new element.
void MoveChecker::walk_for(const ForStmt& stmt, State& state)
{
    if (!stmt.iterable) {
        use(*stmt.start, state);
        use(*stmt.end, state);
    } else if (stmt.convention == ParamConvention::Sink) {
        consume(*stmt.iterable, state);
    } else {
        use(*stmt.iterable, state);
    }
    walk_loop(nullptr, false, stmt.body, state, stmt.symbol);
}

// The body is walked again with the moves of the previous iteration until
// nothing new is moved at the start of an iteration. The condition, when
// there is one, runs before each iteration and exits the loop when false.
void MoveChecker::walk_loop(const Expr* condition, bool infinite, const Block& body, State& state,
                            const VarSymbol* fresh)
{
    State head = state;
    for (;;) {
        loops_.emplace_back();
        State iteration = head;
        if (fresh != nullptr) {
            iteration.moved.erase(fresh);
        }
        if (condition != nullptr) {
            use(*condition, iteration);
        }
        State exit = iteration;
        if (infinite) {
            exit.reachable = false;
        }
        walk_block(body, iteration);
        Loop loop = std::move(loops_.back());
        loops_.pop_back();

        State next = head;
        merge(next, iteration);
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

// A value wrapped into an Option where one is expected is held by the new
// Option, like 'Some(value)', so a value that is not Copy moves even when
// it is only read, as in an argument for a read parameter of type 'P?'.
void MoveChecker::use(const Expr& expr, State& state)
{
    if (expr.converted_to != nullptr && expr.type != nullptr && !is_copy_type(expr.type)) {
        consume(expr, state);
        return;
    }
    use_value(expr, state);
}

void MoveChecker::use_value(const Expr& expr, State& state)
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
    case ExprKind::Tuple:
        for (const auto& element : static_cast<const TupleExpr&>(expr).elements) {
            consume(*element, state);
        }
        return;
    case ExprKind::List:
        for (const auto& element : static_cast<const ListExpr&>(expr).elements) {
            consume(*element, state);
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
        use_value(expr, state);
        return;
    }
    switch (expr.kind) {
    case ExprKind::Name:
    case ExprKind::Member:
    case ExprKind::Index: {
        use_value(expr, state);
        // An element stays in its list; taking it out is 'remove(i)'.
        if (is_in_element(expr)) {
            error_once(start_location(expr), "cannot move '" + place_text(expr) +
                                                 "' out of the list; " + kElementRemedy,
                       nullptr, {});
            return;
        }
        // Moving a field moves the whole variable it belongs to. A field of
        // a temporary value is taken out of it, and the rest is dropped.
        const Expr& root = root_of(expr);
        if (root.kind == ExprKind::Name && static_cast<const NameExpr&>(root).var != nullptr) {
            move_variable(*static_cast<const NameExpr&>(root).var, expr, state);
        } else if (expr.kind == ExprKind::Member) {
            check_temporary_member_move(static_cast<const MemberExpr&>(expr));
        }
        return;
    }
    case ExprKind::Conditional: {
        const auto& cond = static_cast<const ConditionalExpr&>(expr);
        walk_branches(*cond.condition, *cond.then_value, *cond.else_value, true, state);
        return;
    }
    case ExprKind::Match: walk_match(static_cast<const MatchExpr&>(expr), true, state); return;
    default: use_value(expr, state); return;
    }
}

void MoveChecker::move_variable(const VarSymbol& var, const Expr& moved, State& state)
{
    if (auto alias = aliases_.find(&var); alias != aliases_.end()) {
        if (alias->second.in_element) {
            error_once(start_location(moved), "cannot move '" + place_text(moved) +
                                                  "', because it is part of a list element; " +
                                                  kElementRemedy,
                       nullptr, {});
            return;
        }
        // Moving a name bound by a pattern moves part of the matched variable.
        std::vector<const Type*> containers = alias->second.containers;
        const std::vector<const Type*> inner = member_containers(moved);
        containers.insert(containers.end(), inner.begin(), inner.end());
        if (check_partial_move(containers, moved)) {
            return;
        }
        if (state.reachable) {
            state.moved.emplace(&var, start_location(moved));
        }
        move_variable(*alias->second.root, moved, state);
        return;
    }
    if (moved.kind == ExprKind::Member && check_partial_move(member_containers(moved), moved)) {
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
    case VarKind::Element:
        error_once(start_location(moved),
                   "cannot move '" + text + "' out of the list; loop with 'for sink " + var.name +
                       " in ...' to take the elements, or use '.clone()'",
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
    const bool in_element = is_in_element(*match.subject);
    State fallthrough = state;
    State after;
    after.reachable = false;
    for (const MatchArm& arm : match.arms) {
        State arm_state = fallthrough;
        std::vector<BoundName> bindings;
        std::vector<const Type*> containers = member_containers(*match.subject);
        collect_bound_names(*arm.pattern, result_type(*match.subject), containers, bindings);
        for (const BoundName& bound : bindings) {
            arm_state.moved.erase(bound.symbol);
            if (root != nullptr) {
                aliases_[bound.symbol] = Alias{root, bound.containers, in_element};
            } else {
                aliases_.erase(bound.symbol);
                check_owned_binding(bound);
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

// Each name a pattern binds, with the types of the values it lies inside,
// outermost first.
void MoveChecker::collect_bound_names(const Pattern& pattern, const Type* type,
                                      std::vector<const Type*>& containers,
                                      std::vector<BoundName>& out)
{
    switch (pattern.kind) {
    case PatternKind::Binding:
        if (pattern.symbol != nullptr) {
            out.push_back(BoundName{pattern.symbol, containers, &pattern});
        }
        return;
    case PatternKind::Case:
    case PatternKind::Tuple: {
        std::vector<const Type*> element_types;
        if (type->is_option()) {
            element_types.push_back(type->option_value());
        } else if (type->is_tuple()) {
            element_types = type->elements;
        } else if (type->is_enum() && pattern.case_index >= 0) {
            for (const FieldInfo& field : type->user->cases[static_cast<std::size_t>(pattern.case_index)].fields) {
                element_types.push_back(field.type);
            }
        }
        if (element_types.size() != pattern.arguments.size()) {
            return;
        }
        containers.push_back(type);
        for (std::size_t i = 0; i < element_types.size(); ++i) {
            collect_bound_names(*pattern.arguments[i], element_types[i], containers, out);
        }
        containers.pop_back();
        return;
    }
    default: return;
    }
}

// A name bound from a temporary owns its part of the value, which is taken
// apart; that is not possible when the value has 'drop'.
void MoveChecker::check_owned_binding(const BoundName& bound)
{
    if (is_copy_type(bound.symbol->type)) {
        return;
    }
    for (const Type* container : bound.containers) {
        if (container->is_user() && container->user->drop_method != nullptr) {
            error_once(bound.pattern->location,
                       "cannot take '" + bound.symbol->name + "' out of a temporary value of type '" +
                           std::string(container->name) + "', because it has 'drop'; store the "
                           "value in a variable first",
                       nullptr, {});
            return;
        }
    }
}

// The types of the values a member path lies inside: [p, p.a] for 'p.a.b'.
std::vector<const Type*> MoveChecker::member_containers(const Expr& expr)
{
    std::vector<const Type*> containers;
    const Expr* current = &expr;
    while (current->kind == ExprKind::Member) {
        current = static_cast<const MemberExpr&>(*current).object.get();
        containers.insert(containers.begin(), current->type);
    }
    return containers;
}

// Moving part of a value is not possible when a value around it has
// 'drop': the drop needs the whole value.
bool MoveChecker::check_partial_move(const std::vector<const Type*>& containers, const Expr& moved)
{
    for (const Type* container : containers) {
        if (container->is_user() && container->user->drop_method != nullptr) {
            error_once(start_location(moved),
                       "cannot move '" + place_text(moved) + "' out of a value of type '" +
                           std::string(container->name) + "', because it has 'drop'; move the "
                           "whole value instead",
                       nullptr, {});
            return true;
        }
    }
    return false;
}

// A field taken out of a temporary leaves the rest of it to be dropped
// separately; that is not possible when a value around the field has 'drop'.
void MoveChecker::check_temporary_member_move(const MemberExpr& moved)
{
    if (moved.case_index >= 0) {
        return;
    }
    for (const Type* container : member_containers(moved)) {
        if (container != nullptr && container->is_user() && container->user->drop_method != nullptr) {
            error_once(start_location(moved),
                       "cannot take '" + moved.member + "' out of a temporary value of type '" +
                           std::string(container->name) + "', because it has 'drop'; store the "
                           "value in a variable first",
                       nullptr, {});
            return;
        }
    }
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
    return it != aliases_.end() ? it->second.root : nullptr;
}

// Arguments run left to right. The receiver of a method runs first and is
// passed like an argument for 'self'.
void MoveChecker::walk_call(const CallExpr& call, State& state)
{
    if (call.callee->kind == ExprKind::Member) {
        const auto& callee = static_cast<const MemberExpr&>(*call.callee);
        if (callee.builtin != BuiltinMethod::None) {
            use(*callee.object, state); // a list method reads or modifies its receiver
            const BuiltinSignature signature = builtin_signature(callee.builtin);
            for (std::size_t i = 0; i < call.args.size() && i < signature.params.size(); ++i) {
                if (signature.params[i] == ParamConvention::Sink) {
                    consume(*call.args[i], state);
                } else {
                    use(*call.args[i], state);
                }
            }
            return;
        }
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
    if (function.intrinsic == Intrinsic::Some) {
        for (const auto& arg : call.args) {
            consume(*arg, state); // the Option holds the value
        }
        return;
    }
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
