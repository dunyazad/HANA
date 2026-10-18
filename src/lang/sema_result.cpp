// Semantic analysis of 'Result<T, E>' values and the '?' operator. Result
// is a generic enum of the prelude; 'Ok(v)' and 'Err(e)' make its values.

#include <string>

#include "lang/sema.h"
#include "lang/sema_util.h"

namespace hana::lang {

using sema_util::quoted;

bool Sema::is_result(const Type* type) const
{
    return type->is_user() && result_type_ != nullptr && &type->user->origin() == result_type_;
}

// 'Ok(value)' / 'Err(error)': the Result type comes from where the value
// goes, since only one of its two types is in the call.
const Type* Sema::check_result_call(CallExpr& call, bool is_ok, const Type* expected)
{
    const char* name = is_ok ? "Ok" : "Err";
    if (call.args.size() != 1) {
        diagnostics_->error(call.location, std::string("'") + name + "' takes one value, found " +
                                              std::to_string(call.args.size()));
        check_args_without_callee(call);
        return error_type();
    }
    Expr& arg = *call.args.front();
    if (expected == nullptr || !is_result(expected)) {
        check_expr(arg, nullptr);
        diagnostics_->error(call.location,
                           std::string("cannot tell the type of this '") + name +
                               "(...)': use it where a Result is expected, as in 'let r: "
                               "Result<i32, str> = " +
                               name + "(...)' or 'return " + name + "(...)'");
        return error_type();
    }
    const Type* part = expected->user->type_args[is_ok ? 0 : 1];
    const Type* value = check_expr(arg, part);
    if (value->is_void()) {
        diagnostics_->error(arg.location, std::string("this expression has no value to put in '") +
                                              name + "'");
        return error_type();
    }
    expect_type(part, value, arg.location);
    return expected;
}

// 'value?': an 'Err' (or 'null') returns from the function at once, so the
// function must return a Result with the same error type (or an Option).
// The value is the 'Ok' value (or the value inside the Option).
const Type* Sema::check_try(TryExpr& expr, const Type* expected)
{
    const Type* operand_expected = nullptr;
    if (expected != nullptr && is_result(return_type_)) {
        operand_expected = instantiate(*result_type_, {expected, return_type_->user->type_args[1]});
    } else if (expected != nullptr && return_type_->is_option()) {
        operand_expected = option_type(expected);
    }
    const Type* operand = check_expr(*expr.operand, operand_expected);
    if (operand->is_error()) {
        return operand;
    }
    if (operand->is_option()) {
        if (!return_type_->is_option() && !return_type_->is_error()) {
            diagnostics_->error(expr.location, "'?' on an Option returns 'null' early, so the "
                                               "function must return an Option; it returns " +
                                                   quoted(return_type_));
            return error_type();
        }
        return operand->option_value();
    }
    if (!is_result(operand)) {
        diagnostics_->error(expr.location, "'?' works on a Result or an Option, but the value has "
                                           "type " + quoted(operand));
        return error_type();
    }
    const Type* error = operand->user->type_args[1];
    if (!is_result(return_type_)) {
        if (!return_type_->is_error()) {
            diagnostics_->error(expr.location, "'?' returns the error early, so the function must "
                                               "return a Result; it returns " +
                                                   quoted(return_type_));
        }
        return error_type();
    }
    if (return_type_->user->type_args[1] != error) {
        diagnostics_->error(expr.location, "'?' returns an error of type " + quoted(error) +
                                               ", but the function returns errors of type " +
                                               quoted(return_type_->user->type_args[1]));
        return error_type();
    }
    return operand->user->type_args[0];
}

} // namespace hana::lang
