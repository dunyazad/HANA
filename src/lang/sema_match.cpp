// Semantic analysis of 'match': patterns and arm types. Coverage is in
// sema_coverage.cpp.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lang/sema.h"
#include "lang/sema_util.h"

namespace hana::lang {

using sema_util::is_untyped_literal;
using sema_util::less_than;
using sema_util::literal_value;
using sema_util::LiteralValue;
using sema_util::quoted;

namespace {

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
        // A bare name that is a case of the matched enum is that case;
        // 'Some' without '(...)' is a malformed Option pattern.
        if ((subject->is_enum() && subject->user->find_case(pattern.name) >= 0) ||
            (subject->is_option() && pattern.name == "Some")) {
            pattern.kind = PatternKind::Case;
            check_case_pattern(pattern, subject);
            return;
        }
        pattern.symbol = declare_var(pattern.name, subject, VarKind::Local, false, pattern.location);
        return;
    case PatternKind::Literal:
    case PatternKind::Range: check_literal_pattern(pattern, subject); return;
    case PatternKind::Case: check_case_pattern(pattern, subject); return;
    case PatternKind::Null:
        if (!subject->is_error() && !subject->is_option()) {
            diagnostics_->error(pattern.location, "'null' only matches an Option, but the value has "
                                                  "type " + quoted(subject));
        }
        return;
    case PatternKind::Tuple: check_tuple_pattern(pattern, subject); return;
    case PatternKind::Struct:
        diagnostics_->error(pattern.location, "struct patterns in 'match' are not supported yet; "
                                              "take the struct apart with 'let " +
                                                  pattern.name + " { ... } = value'");
        return;
    }
}

// 'Case' or 'Case(p, ...)': one pattern per field, in declaration order.
void Sema::check_case_pattern(Pattern& pattern, const Type* subject)
{
    if (subject->is_option()) {
        check_option_case_pattern(pattern, subject);
        return;
    }
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

} // namespace hana::lang
