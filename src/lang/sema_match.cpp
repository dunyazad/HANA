// Semantic analysis of 'match': patterns, arm types and coverage.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lang/sema.h"
#include "lang/sema_util.h"

namespace hana::lang {

using sema_util::is_untyped_literal;
using sema_util::quoted;

namespace {

// '_', a plain name and a tuple of such patterns match every value.
bool is_catch_all(const Pattern& pattern)
{
    if (pattern.kind == PatternKind::Tuple) {
        for (const auto& element : pattern.arguments) {
            if (!is_catch_all(*element)) {
                return false;
            }
        }
        return true;
    }
    return pattern.kind == PatternKind::Wildcard || pattern.kind == PatternKind::Binding;
}

// The value of a literal pattern as a sign and a magnitude, so literals of
// every integer type and char compare correctly.
struct LiteralValue {
    bool negative = false;
    std::uint64_t magnitude = 0;
};

std::optional<LiteralValue> literal_value(const Expr& expr)
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

bool less_than(const LiteralValue& a, const LiteralValue& b)
{
    if (a.negative != b.negative) {
        return a.negative;
    }
    return a.negative ? a.magnitude > b.magnitude : a.magnitude < b.magnitude;
}

// "'A', 'B'"
std::string quoted_list(const std::vector<std::string>& names)
{
    std::string text;
    for (const std::string& name : names) {
        text += (text.empty() ? "'" : ", '") + name + "'";
    }
    return text;
}

// 'Rect(w, h)' for a case with fields w and h.
std::string case_pattern_example(const EnumCase& entry)
{
    std::string text = entry.name + "(";
    for (std::size_t i = 0; i < entry.fields.size(); ++i) {
        text += (i == 0 ? "" : ", ") + entry.fields[i].name;
    }
    return text + ")";
}

} // namespace

// ---------------------------------------------------------------------------
// Statement and expression forms

// Returns true when every arm ends in return, break or continue.
bool Sema::check_match_statement(MatchStmt& stmt)
{
    MatchExpr& match = *stmt.match;
    match.type = void_type();
    const Type* subject = check_match_subject(match);
    bool terminates = !match.arms.empty();
    for (MatchArm& arm : match.arms) {
        push_scope();
        check_arm_head(arm, subject);
        if (!check_block(arm.body)) {
            terminates = false;
        }
        pop_scope();
    }
    check_match_coverage(match, subject);
    return terminates;
}

// All arms must have values of one type. Arms whose value is an untyped
// literal take the type of the other arms, so they are checked last.
const Type* Sema::check_match_expression(MatchExpr& match, const Type* expected)
{
    const Type* subject = check_match_subject(match);
    std::vector<const Type*> types(match.arms.size(), error_type());
    const Type* target = expected;
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < match.arms.size(); ++i) {
            MatchArm& arm = match.arms[i];
            if (is_untyped_literal(*arm.value) != (pass == 1)) {
                continue;
            }
            types[i] = check_arm_value(arm, subject, target);
            if (target == nullptr && !types[i]->is_error() && !types[i]->is_void()) {
                target = types[i];
            }
        }
    }
    check_match_coverage(match, subject);

    const Type* common = nullptr;
    bool failed = false;
    for (std::size_t i = 0; i < match.arms.size(); ++i) {
        const Type* type = types[i];
        if (type->is_error()) {
            failed = true;
            continue;
        }
        if (type->is_void()) {
            diagnostics_->error(match.arms[i].value->location, "this arm has no value");
            failed = true;
            continue;
        }
        if (common == nullptr) {
            common = type;
            continue;
        }
        const Type* unified = unify(common, type);
        if (unified == nullptr) {
            diagnostics_->error(match.arms[i].value->location,
                               "the arms of 'match' have different types " + quoted(common) +
                                   " and " + quoted(type));
            failed = true;
            continue;
        }
        common = unified;
    }
    return failed || common == nullptr ? error_type() : common;
}

const Type* Sema::check_match_subject(MatchExpr& match)
{
    const Type* subject = check_expr(*match.subject, nullptr);
    if (subject->is_void()) {
        diagnostics_->error(match.subject->location, "this expression has no value to match");
        return error_type();
    }
    return subject;
}

const Type* Sema::check_arm_value(MatchArm& arm, const Type* subject, const Type* expected)
{
    push_scope();
    check_arm_head(arm, subject);
    const Type* type = check_expr(*arm.value, expected);
    pop_scope();
    return type;
}

// The pattern declares its names in the arm's scope, before the guard.
void Sema::check_arm_head(MatchArm& arm, const Type* subject)
{
    check_pattern(*arm.pattern, subject);
    if (arm.guard) {
        check_condition(*arm.guard);
    }
}

// ---------------------------------------------------------------------------
// Patterns

void Sema::check_pattern(Pattern& pattern, const Type* subject)
{
    switch (pattern.kind) {
    case PatternKind::Wildcard: return;
    case PatternKind::Binding:
        // A bare name that is a case of the matched enum is that case.
        if (subject->is_enum() && subject->user->find_case(pattern.name) >= 0) {
            pattern.kind = PatternKind::Case;
            check_case_pattern(pattern, subject);
            return;
        }
        pattern.symbol = declare_var(pattern.name, subject, VarKind::Local, false, pattern.location);
        return;
    case PatternKind::Literal:
    case PatternKind::Range: check_literal_pattern(pattern, subject); return;
    case PatternKind::Case: check_case_pattern(pattern, subject); return;
    case PatternKind::Tuple: check_tuple_pattern(pattern, subject); return;
    }
}

// 'Case' or 'Case(p, ...)': one pattern per field, in declaration order.
void Sema::check_case_pattern(Pattern& pattern, const Type* subject)
{
    const EnumCase* entry = nullptr;
    if (!subject->is_error() && !subject->is_enum()) {
        diagnostics_->error(pattern.location, "'" + pattern.name + "' is an enum case pattern, but "
                                                  "the value has type " + quoted(subject));
    } else if (subject->is_enum()) {
        pattern.case_index = subject->user->find_case(pattern.name);
        if (pattern.case_index < 0) {
            diagnostics_->error(pattern.location, "'" + subject->user->name + "' has no case '" +
                                                      pattern.name + "'");
        } else {
            entry = &subject->user->cases[static_cast<std::size_t>(pattern.case_index)];
        }
    }
    if (entry == nullptr) {
        for (auto& argument : pattern.arguments) {
            check_pattern(*argument, error_type());
        }
        return;
    }
    if (!pattern.has_arguments && !entry->fields.empty()) {
        diagnostics_->error(pattern.location, "case '" + entry->name + "' has fields; match them "
                                                  "with '" + case_pattern_example(*entry) +
                                                  "', using '_' for the ones you do not need");
        return;
    }
    if (pattern.arguments.size() != entry->fields.size()) {
        diagnostics_->error(pattern.location, "case '" + entry->name + "' has " +
                                                  std::to_string(entry->fields.size()) +
                                                  " field(s), but the pattern has " +
                                                  std::to_string(pattern.arguments.size()));
        for (auto& argument : pattern.arguments) {
            check_pattern(*argument, error_type());
        }
        return;
    }
    for (std::size_t i = 0; i < entry->fields.size(); ++i) {
        check_pattern(*pattern.arguments[i], entry->fields[i].type);
    }
}

// Literals match integers, char and bool; ranges match integers and char.
void Sema::check_literal_pattern(Pattern& pattern, const Type* subject)
{
    const bool is_range = pattern.kind == PatternKind::Range;
    if (subject->is_error()) {
        check_expr(*pattern.value, nullptr);
        if (is_range) {
            check_expr(*pattern.end, nullptr);
        }
        return;
    }
    const bool allowed = subject->is_integer() || subject->kind == TypeKind::Char ||
                         (!is_range && subject->kind == TypeKind::Bool);
    if (!allowed) {
        diagnostics_->error(pattern.location, std::string(is_range ? "a range pattern"
                                                                   : "a literal pattern") +
                                                  " cannot match a value of type " +
                                                  quoted(subject));
        return;
    }
    expect_type(subject, check_expr(*pattern.value, subject), pattern.value->location);
    if (!is_range) {
        return;
    }
    expect_type(subject, check_expr(*pattern.end, subject), pattern.end->location);
    const std::optional<LiteralValue> start = literal_value(*pattern.value);
    const std::optional<LiteralValue> end = literal_value(*pattern.end);
    if (start && end) {
        const bool empty = pattern.inclusive ? less_than(*end, *start) : !less_than(*start, *end);
        if (empty) {
            diagnostics_->error(pattern.location, "this range matches no value");
        }
    }
}

// ---------------------------------------------------------------------------
// Coverage

// Every value must reach an arm. Arms with a guard may fail, so they do
// not count. An arm after one that matches everything is an error.
void Sema::check_match_coverage(const MatchExpr& match, const Type* subject)
{
    if (subject->is_error()) {
        return;
    }
    if (match.arms.empty()) {
        diagnostics_->error(match.location, "'match' needs at least one arm");
        return;
    }
    bool matches_all = false;
    std::vector<bool> covered(subject->is_enum() ? subject->user->cases.size() : 0, false);
    bool seen_true = false;
    bool seen_false = false;
    for (const MatchArm& arm : match.arms) {
        if (matches_all) {
            diagnostics_->error(arm.location, "this arm is never reached; an earlier arm matches "
                                              "every value");
            continue;
        }
        if (arm.guard) {
            continue;
        }
        const Pattern& pattern = *arm.pattern;
        if (is_catch_all(pattern)) {
            matches_all = true;
        } else if (pattern.kind == PatternKind::Case && pattern.case_index >= 0) {
            bool whole_case = true;
            for (const auto& argument : pattern.arguments) {
                whole_case = whole_case && is_catch_all(*argument);
            }
            if (whole_case) {
                covered[static_cast<std::size_t>(pattern.case_index)] = true;
            }
        } else if (pattern.kind == PatternKind::Literal &&
                   pattern.value->kind == ExprKind::BoolLiteral) {
            (static_cast<const BoolLiteralExpr&>(*pattern.value).value ? seen_true : seen_false) =
                true;
        }
    }
    if (matches_all) {
        return;
    }
    std::vector<std::string> missing;
    if (subject->is_enum()) {
        for (std::size_t i = 0; i < covered.size(); ++i) {
            if (!covered[i]) {
                missing.push_back(subject->user->cases[i].name);
            }
        }
        if (missing.empty()) {
            return;
        }
    } else if (subject->kind == TypeKind::Bool) {
        if (!seen_true) {
            missing.push_back("true");
        }
        if (!seen_false) {
            missing.push_back("false");
        }
        if (missing.empty()) {
            return;
        }
    } else {
        diagnostics_->error(match.location, "'match' on a value of type " + quoted(subject) +
                                                " needs a final '_' arm (or a name) for the "
                                                "values the other arms do not cover");
        return;
    }
    diagnostics_->error(match.location, "'match' does not cover " + quoted_list(missing) + "; add " +
                                            (missing.size() == 1 ? "an arm for it" : "arms for them") +
                                            " or a final '_' arm");
}

} // namespace hana::lang
