// Semantic analysis of Option<T>: the type, 'null', 'Some(value)', values
// wrapped where an Option is expected, comparison with 'null' and the
// 'Some(p)' pattern.

#include <string>

#include "lang/sema.h"
#include "lang/sema_util.h"

namespace hana::lang {

using sema_util::quoted;

// Option types are created once per value type, so they compare by pointer.
const Type* Sema::option_type(const Type* value)
{
    if (auto it = option_types_.find(value); it != option_types_.end()) {
        return it->second;
    }
    ComposedType& option = program_->composed_types.emplace_back();
    option.name = "Option<" + std::string(value->name) + ">";
    option.type.kind = TypeKind::Option;
    option.type.name = option.name;
    option.type.elements = {value};
    option_types_[value] = &option.type;
    return &option.type;
}

// Where an Option<T> is expected, most expressions are checked as a T and
// then wrapped, so 'let a: u8? = 300' types the literal as u8. 'null', the
// conditional expression and 'match' keep the Option: each of their
// branches may be 'null' or a value.
const Type* Sema::value_expectation(const Expr& expr, const Type* expected)
{
    if (expected == nullptr || !expected->is_option()) {
        return expected;
    }
    switch (expr.kind) {
    case ExprKind::Null:
    case ExprKind::Conditional:
    case ExprKind::Match: return expected;
    default: return expected->option_value();
    }
}

// A T value where an Option<T> is expected becomes 'Some(value)'. An
// Option value is never wrapped again, so a T? where a T?? is expected
// needs 'Some(...)'.
const Type* Sema::wrap_in_option(Expr& expr, const Type* type, const Type* expected)
{
    if (expected == nullptr || !expected->is_option() || type->is_option() || type->is_error() ||
        type->is_void()) {
        return type;
    }
    if (unify(expected->option_value(), type) == nullptr) {
        return type;
    }
    expr.converted_to = expected;
    return expected;
}

// 'null' has the Option type the context expects.
const Type* Sema::check_null(Expr& expr, const Type* expected)
{
    if (expected != nullptr && expected->is_option()) {
        return expected;
    }
    if (expected != nullptr && !expected->is_error()) {
        diagnostics_->error(expr.location, "a value of type " + quoted(expected) +
                                               " cannot be 'null'; use '" +
                                               std::string(expected->name) + "?' to allow it");
    } else if (expected == nullptr) {
        diagnostics_->error(expr.location, "'null' needs to know its Option type here; write the "
                                           "type, as in 'let x: i32? = null'");
    }
    return error_type();
}

// 'Some(value)'. 'expected' is already the expected value type (see
// value_expectation).
const Type* Sema::check_some_call(CallExpr& call, const Type* expected)
{
    if (call.args.size() != 1) {
        diagnostics_->error(call.location, "'Some' takes one value, found " +
                                              std::to_string(call.args.size()));
        check_args_without_callee(call);
        return error_type();
    }
    Expr& arg = *call.args.front();
    const Type* value = check_expr(arg, expected);
    if (value->is_void()) {
        diagnostics_->error(arg.location, "this expression has no value to put in 'Some'");
        return error_type();
    }
    return value->is_error() ? value : option_type(value);
}

// Options compare only with 'null': 'x == null', 'x != null'.
const Type* Sema::check_option_comparison(BinaryExpr& binary)
{
    const bool with_null =
        binary.lhs->kind == ExprKind::Null || binary.rhs->kind == ExprKind::Null;
    if ((binary.op == BinaryOp::Eq || binary.op == BinaryOp::Ne) && with_null) {
        return bool_type();
    }
    if (binary.op == BinaryOp::Eq || binary.op == BinaryOp::Ne) {
        diagnostics_->error(binary.op_location, "an Option can only be compared with 'null'; use "
                                                "'match' to read its value");
    } else {
        diagnostics_->error(binary.op_location, std::string("operator '") +
                                                    binary_op_spelling(binary.op) +
                                                    "' cannot be applied to an Option; use "
                                                    "'match' to read its value");
    }
    return error_type();
}

// 'Some(p)' matches an Option holding a value that matches p.
void Sema::check_option_case_pattern(Pattern& pattern, const Type* subject)
{
    if (pattern.name != "Some") {
        diagnostics_->error(pattern.location, "an Option is matched with 'Some(...)' or 'null', "
                                              "not '" + pattern.name + "'");
    } else if (!pattern.has_arguments || pattern.arguments.size() != 1) {
        diagnostics_->error(pattern.location, "'Some' holds one value; match it with 'Some(name)' "
                                              "or 'Some(_)'");
    } else {
        pattern.case_index = 1;
        check_pattern(*pattern.arguments.front(), subject->option_value());
        return;
    }
    for (auto& argument : pattern.arguments) {
        check_pattern(*argument, error_type());
    }
}

} // namespace hana::lang
