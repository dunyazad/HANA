#pragma once

#include <string>

#include "lang/ast.h"
#include "lang/types.h"

// Small helpers shared by the files of semantic analysis.
namespace hana::lang::sema_util {

// An integer or float literal without a suffix, possibly negated.
inline bool is_untyped_literal(const Expr& expr)
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

// "'i32'" for messages.
inline std::string quoted(const Type* type)
{
    return "'" + std::string(type->name) + "'";
}

} // namespace hana::lang::sema_util
