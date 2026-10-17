#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "lang/ast.h"
#include "lang/types.h"

// Small helpers shared by the files of semantic analysis.
namespace hana::lang::sema_util {

// The '&' of an inout argument, or null.
inline const UnaryExpr* as_inout_mark(const Expr& expr)
{
    if (expr.kind != ExprKind::Unary) {
        return nullptr;
    }
    const auto& unary = static_cast<const UnaryExpr&>(expr);
    return unary.op == UnaryOp::InoutMark ? &unary : nullptr;
}

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

// The value of a literal pattern as a sign and a magnitude, so literals of
// every integer type and char compare correctly.
struct LiteralValue {
    bool negative = false;
    std::uint64_t magnitude = 0;
};

inline std::optional<LiteralValue> literal_value(const Expr& expr)
{
    switch (expr.kind) {
    case ExprKind::IntLiteral: return LiteralValue{false, static_cast<const IntLiteralExpr&>(expr).value};
    case ExprKind::CharLiteral:
        return LiteralValue{false, static_cast<const CharLiteralExpr&>(expr).value};
    case ExprKind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expr);
        if (unary.op != UnaryOp::Neg || unary.operand->kind != ExprKind::IntLiteral) {
            return std::nullopt;
        }
        const std::uint64_t value = static_cast<const IntLiteralExpr&>(*unary.operand).value;
        return LiteralValue{value != 0, value};
    }
    default: return std::nullopt;
    }
}

inline bool less_than(const LiteralValue& a, const LiteralValue& b)
{
    if (a.negative != b.negative) {
        return a.negative;
    }
    return a.negative ? a.magnitude > b.magnitude : a.magnitude < b.magnitude;
}

// "'i32'" for messages.
inline std::string quoted(const Type* type)
{
    return "'" + std::string(type->name) + "'";
}

} // namespace hana::lang::sema_util
