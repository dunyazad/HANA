// Copies of syntax trees, used to make one copy of a generic function for
// each list of type arguments. Only what the parser wrote is copied; the
// results of semantic analysis are left unset, so the copy is checked
// afresh.

#include "lang/ast_clone.h"

#include <utility>

namespace hana::lang {

namespace {

template <typename T>
std::unique_ptr<T> with_location(const Expr& expr)
{
    return std::make_unique<T>(expr.location);
}

std::vector<ExprPtr> clone_exprs(const std::vector<ExprPtr>& exprs)
{
    std::vector<ExprPtr> copies;
    for (const auto& expr : exprs) {
        copies.push_back(clone_expr(*expr));
    }
    return copies;
}

ExprPtr clone_optional(const ExprPtr& expr)
{
    return expr ? clone_expr(*expr) : nullptr;
}

ExprPtr clone_literal(const Expr& expr)
{
    switch (expr.kind) {
    case ExprKind::IntLiteral: {
        const auto& lit = static_cast<const IntLiteralExpr&>(expr);
        auto copy = with_location<IntLiteralExpr>(expr);
        copy->text = lit.text;
        copy->suffix = lit.suffix;
        copy->value = lit.value;
        copy->overflowed = lit.overflowed;
        return copy;
    }
    case ExprKind::FloatLiteral: {
        const auto& lit = static_cast<const FloatLiteralExpr&>(expr);
        auto copy = with_location<FloatLiteralExpr>(expr);
        copy->text = lit.text;
        copy->suffix = lit.suffix;
        copy->value = lit.value;
        return copy;
    }
    case ExprKind::BoolLiteral:
        return std::make_unique<BoolLiteralExpr>(expr.location,
                                                 static_cast<const BoolLiteralExpr&>(expr).value);
    case ExprKind::CharLiteral:
        return std::make_unique<CharLiteralExpr>(expr.location,
                                                 static_cast<const CharLiteralExpr&>(expr).value);
    case ExprKind::StringLiteral: {
        const auto& lit = static_cast<const StringLiteralExpr&>(expr);
        auto copy = with_location<StringLiteralExpr>(expr);
        copy->is_format = lit.is_format;
        copy->texts = lit.texts;
        copy->parts = clone_exprs(lit.parts);
        return copy;
    }
    case ExprKind::Null: return with_location<NullExpr>(expr);
    default: return with_location<ErrorExpr>(expr);
    }
}

ExprPtr clone_struct_literal(const StructLiteralExpr& lit)
{
    auto copy = std::make_unique<StructLiteralExpr>(lit.location, lit.type_name);
    copy->case_name = lit.case_name;
    copy->case_location = lit.case_location;
    for (const auto& field : lit.fields) {
        copy->fields.push_back({field.name, field.location, clone_expr(*field.value)});
    }
    return copy;
}

ExprPtr clone_match(const MatchExpr& match)
{
    auto copy = std::make_unique<MatchExpr>(match.location);
    copy->subject = clone_expr(*match.subject);
    for (const MatchArm& arm : match.arms) {
        MatchArm& target = copy->arms.emplace_back();
        target.pattern = clone_pattern(*arm.pattern);
        target.guard = clone_optional(arm.guard);
        target.body = clone_block(arm.body);
        target.value = clone_optional(arm.value);
        target.location = arm.location;
    }
    return copy;
}

} // namespace

ExprPtr clone_expr(const Expr& expr)
{
    switch (expr.kind) {
    case ExprKind::Error: return with_location<ErrorExpr>(expr);
    case ExprKind::IntLiteral:
    case ExprKind::FloatLiteral:
    case ExprKind::BoolLiteral:
    case ExprKind::CharLiteral:
    case ExprKind::StringLiteral:
    case ExprKind::Null: return clone_literal(expr);
    case ExprKind::Name:
        return std::make_unique<NameExpr>(expr.location, static_cast<const NameExpr&>(expr).name);
    case ExprKind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expr);
        return std::make_unique<UnaryExpr>(expr.location, unary.op, clone_expr(*unary.operand));
    }
    case ExprKind::Binary: {
        const auto& binary = static_cast<const BinaryExpr&>(expr);
        return std::make_unique<BinaryExpr>(binary.op_location, binary.op, clone_expr(*binary.lhs),
                                            clone_expr(*binary.rhs));
    }
    case ExprKind::Conditional: {
        const auto& cond = static_cast<const ConditionalExpr&>(expr);
        return std::make_unique<ConditionalExpr>(expr.location, clone_expr(*cond.then_value),
                                                 clone_expr(*cond.condition),
                                                 clone_expr(*cond.else_value));
    }
    case ExprKind::Cast: {
        const auto& cast = static_cast<const CastExpr&>(expr);
        return std::make_unique<CastExpr>(expr.location, clone_expr(*cast.operand), cast.target);
    }
    case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        auto copy = std::make_unique<CallExpr>(expr.location, clone_expr(*call.callee));
        copy->args = clone_exprs(call.args);
        return copy;
    }
    case ExprKind::StructLiteral:
        return clone_struct_literal(static_cast<const StructLiteralExpr&>(expr));
    case ExprKind::Tuple: {
        auto copy = with_location<TupleExpr>(expr);
        copy->elements = clone_exprs(static_cast<const TupleExpr&>(expr).elements);
        return copy;
    }
    case ExprKind::List: {
        auto copy = with_location<ListExpr>(expr);
        copy->elements = clone_exprs(static_cast<const ListExpr&>(expr).elements);
        return copy;
    }
    case ExprKind::Match: return clone_match(static_cast<const MatchExpr&>(expr));
    case ExprKind::Member: {
        const auto& member = static_cast<const MemberExpr&>(expr);
        return std::make_unique<MemberExpr>(expr.location, clone_expr(*member.object),
                                            member.member);
    }
    case ExprKind::Index: {
        const auto& index = static_cast<const IndexExpr&>(expr);
        return std::make_unique<IndexExpr>(expr.location, clone_expr(*index.object),
                                           clone_expr(*index.index));
    }
    case ExprKind::Try:
        return std::make_unique<TryExpr>(expr.location,
                                         clone_expr(*static_cast<const TryExpr&>(expr).operand));
    }
    return with_location<ErrorExpr>(expr);
}

PatternPtr clone_pattern(const Pattern& pattern)
{
    auto copy = std::make_unique<Pattern>();
    copy->kind = pattern.kind;
    copy->location = pattern.location;
    copy->name = pattern.name;
    copy->value = clone_optional(pattern.value);
    copy->end = clone_optional(pattern.end);
    copy->inclusive = pattern.inclusive;
    copy->has_arguments = pattern.has_arguments;
    for (const auto& argument : pattern.arguments) {
        copy->arguments.push_back(clone_pattern(*argument));
    }
    return copy;
}

namespace {

StmtPtr clone_let(const LetStmt& let)
{
    auto copy = std::make_unique<LetStmt>(let.location);
    copy->is_mutable = let.is_mutable;
    copy->name = let.name;
    copy->name_location = let.name_location;
    copy->pattern = let.pattern ? clone_pattern(*let.pattern) : nullptr;
    copy->declared_type = let.declared_type;
    copy->init = clone_expr(*let.init);
    return copy;
}

StmtPtr clone_assign(const AssignStmt& assign)
{
    auto copy = std::make_unique<AssignStmt>(assign.location);
    copy->op = assign.op;
    copy->op_location = assign.op_location;
    copy->target = clone_expr(*assign.target);
    copy->value = clone_expr(*assign.value);
    return copy;
}

StmtPtr clone_if(const IfStmt& stmt)
{
    auto copy = std::make_unique<IfStmt>(stmt.location);
    copy->condition = clone_expr(*stmt.condition);
    copy->then_block = clone_block(stmt.then_block);
    if (stmt.else_block) {
        copy->else_block = clone_block(*stmt.else_block);
    }
    return copy;
}

StmtPtr clone_for(const ForStmt& stmt)
{
    auto copy = std::make_unique<ForStmt>(stmt.location);
    copy->name = stmt.name;
    copy->name_location = stmt.name_location;
    copy->start = clone_optional(stmt.start);
    copy->end = clone_optional(stmt.end);
    copy->inclusive = stmt.inclusive;
    copy->iterable = clone_optional(stmt.iterable);
    copy->convention = stmt.convention;
    copy->convention_location = stmt.convention_location;
    copy->body = clone_block(stmt.body);
    return copy;
}

} // namespace

StmtPtr clone_stmt(const Stmt& stmt)
{
    switch (stmt.kind) {
    case StmtKind::Let: return clone_let(static_cast<const LetStmt&>(stmt));
    case StmtKind::Assign: return clone_assign(static_cast<const AssignStmt&>(stmt));
    case StmtKind::Expr:
        return std::make_unique<ExprStmt>(clone_expr(*static_cast<const ExprStmt&>(stmt).expr));
    case StmtKind::Return: {
        auto copy = std::make_unique<ReturnStmt>(stmt.location);
        copy->value = clone_optional(static_cast<const ReturnStmt&>(stmt).value);
        return copy;
    }
    case StmtKind::If: return clone_if(static_cast<const IfStmt&>(stmt));
    case StmtKind::While: {
        const auto& loop = static_cast<const WhileStmt&>(stmt);
        auto copy = std::make_unique<WhileStmt>(stmt.location);
        copy->condition = clone_expr(*loop.condition);
        copy->body = clone_block(loop.body);
        return copy;
    }
    case StmtKind::For: return clone_for(static_cast<const ForStmt&>(stmt));
    case StmtKind::Match: {
        ExprPtr match = clone_expr(*static_cast<const MatchStmt&>(stmt).match);
        return std::make_unique<MatchStmt>(
            std::unique_ptr<MatchExpr>(static_cast<MatchExpr*>(match.release())));
    }
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Pass: return std::make_unique<SimpleStmt>(stmt.kind, stmt.location);
    }
    return std::make_unique<SimpleStmt>(StmtKind::Pass, stmt.location);
}

Block clone_block(const Block& block)
{
    Block copy;
    copy.location = block.location;
    copy.end_location = block.end_location;
    for (const auto& statement : block.statements) {
        copy.statements.push_back(clone_stmt(*statement));
    }
    return copy;
}

std::unique_ptr<FnDecl> clone_function(const FnDecl& function)
{
    auto copy = std::make_unique<FnDecl>();
    copy->name = function.name;
    copy->location = function.location;
    copy->is_pub = function.is_pub;
    copy->type_params = function.type_params;
    copy->has_body = function.has_body;
    copy->owner = function.owner;
    copy->self_convention = function.self_convention;
    copy->self_location = function.self_location;
    for (const Param& param : function.params) {
        Param& target = copy->params.emplace_back();
        target.convention = param.convention;
        target.name = param.name;
        target.location = param.location;
        target.type = param.type;
    }
    copy->return_type = function.return_type;
    copy->body = clone_block(function.body);
    return copy;
}

} // namespace hana::lang
