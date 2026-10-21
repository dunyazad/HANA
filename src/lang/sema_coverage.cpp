// Coverage of 'match': every value must reach an arm, and every arm must be
// reachable. This is the usefulness check of pattern matrices (Maranget,
// "Warnings for pattern matching"): an arm is reachable when it matches some
// value the arms before it do not, and the match covers every value when a
// final '_' would not be reachable.
//
// Enums, Options, bools and tuples have a known, closed set of
// constructors, so arms can cover them one constructor at a time, also in
// nested patterns. Integers, chars and other types are open: literals and
// ranges never cover them, and a '_' or a name is needed.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "lang/sema.h"
#include "lang/sema_util.h"

namespace hana::lang {

using sema_util::literal_value;
using sema_util::quoted;

namespace {

// A pattern as the coverage check sees it: '_', or a constructor with one
// pattern per argument. 'key' tells constructors apart: "c<n>" for enum
// case n, "null" and "some" for Options, "true" and "false", "tuple", and
// a unique key for each literal or range of an open type.
struct Pat {
    bool wildcard = true;
    std::string key;
    std::vector<Pat> args;
};

using Row = std::vector<Pat>;

// A constructor of a closed type: its key, how it is written, and the
// types of its arguments.
struct Constructor {
    std::string key;
    std::string name;
    std::vector<const Type*> args;
};

// Every constructor of a closed type; empty for an open type.
std::vector<Constructor> constructors_of(const Type* type)
{
    std::vector<Constructor> result;
    if (type->is_enum()) {
        const auto& cases = type->user->cases;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            Constructor& entry = result.emplace_back();
            entry.key = "c" + std::to_string(i);
            entry.name = cases[i].name;
            for (const FieldInfo& field : cases[i].fields) {
                entry.args.push_back(field.type);
            }
        }
    } else if (type->is_option()) {
        result.push_back({"null", "null", {}});
        result.push_back({"some", "Some", {type->option_value()}});
    } else if (type->kind == TypeKind::Bool) {
        result.push_back({"false", "false", {}});
        result.push_back({"true", "true", {}});
    } else if (type->is_tuple()) {
        result.push_back({"tuple", "", type->elements});
    }
    return result;
}

// The argument types of a constructor; none for literals and ranges.
std::vector<const Type*> argument_types(const Type* type, const std::string& key)
{
    for (const Constructor& entry : constructors_of(type)) {
        if (entry.key == key) {
            return entry.args;
        }
    }
    return {};
}

// How a constructor applied to argument examples is written, as in
// 'Rect(_, 1)', 'Some(null)' or '(_, false)'.
std::string constructor_text(const Type* type, const std::string& key,
                             const std::vector<std::string>& args)
{
    std::string name;
    for (const Constructor& entry : constructors_of(type)) {
        if (entry.key == key) {
            name = entry.name;
        }
    }
    if (args.empty() && !type->is_tuple()) {
        return name;
    }
    std::string text = name + "(";
    for (std::size_t i = 0; i < args.size(); ++i) {
        text += (i == 0 ? "" : ", ") + args[i];
    }
    return text + ")";
}

std::optional<Pat> constructor_pat(const Pattern& pattern, const Type* type);

// Converts a checked pattern matched against a value of 'type'. Returns
// nothing when the pattern does not fit the type (it had an error, already
// reported), so coverage is not reported on top of it.
std::optional<Pat> to_pat(const Pattern& pattern, const Type* type)
{
    Pat result;
    switch (pattern.kind) {
    case PatternKind::Struct: return std::nullopt; // rejected in 'match'
    case PatternKind::Wildcard:
    case PatternKind::Binding: return result;
    case PatternKind::Null:
        if (!type->is_option()) {
            return std::nullopt;
        }
        result.wildcard = false;
        result.key = "null";
        return result;
    case PatternKind::Literal: {
        result.wildcard = false;
        if (pattern.value->kind == ExprKind::BoolLiteral) {
            result.key = static_cast<const BoolLiteralExpr&>(*pattern.value).value ? "true" : "false";
            return result;
        }
        const std::optional<sema_util::LiteralValue> value = literal_value(*pattern.value);
        if (!value) {
            return std::nullopt;
        }
        result.key = std::string("v") + (value->negative ? "-" : "") +
                     std::to_string(value->magnitude);
        return result;
    }
    case PatternKind::Range:
        // Ranges are never compared with each other: each one is its own
        // constructor of an open type.
        result.wildcard = false;
        result.key = "r" + std::to_string(pattern.location.offset);
        return result;
    case PatternKind::Case:
    case PatternKind::Tuple: return constructor_pat(pattern, type);
    }
    return std::nullopt;
}

// A case, 'Some(p)' or tuple pattern, with its argument patterns.
std::optional<Pat> constructor_pat(const Pattern& pattern, const Type* type)
{
    Pat result;
    result.wildcard = false;
    if (pattern.kind == PatternKind::Tuple && type->is_tuple()) {
        result.key = "tuple";
    } else if (pattern.kind == PatternKind::Case && type->is_option() && pattern.case_index == 1) {
        result.key = "some";
    } else if (pattern.kind == PatternKind::Case && type->is_enum() && pattern.case_index >= 0 &&
               static_cast<std::size_t>(pattern.case_index) < type->user->cases.size()) {
        result.key = "c" + std::to_string(pattern.case_index);
    } else {
        return std::nullopt;
    }
    const std::vector<const Type*> types = argument_types(type, result.key);
    if (types.size() != pattern.arguments.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < types.size(); ++i) {
        std::optional<Pat> converted = to_pat(*pattern.arguments[i], types[i]);
        if (!converted) {
            return std::nullopt;
        }
        result.args.push_back(std::move(*converted));
    }
    return result;
}

class Coverage {
public:
    // An example of the values matched by 'q' that no row matches, one
    // written pattern per column, or nothing when the rows match all of
    // them.
    std::optional<std::vector<std::string>> useful(const std::vector<Row>& rows, const Row& q,
                                                   const std::vector<const Type*>& types) const
    {
        if (q.empty()) {
            return rows.empty() ? std::optional(std::vector<std::string>{}) : std::nullopt;
        }
        const Type* type = types.front();
        const std::vector<const Type*> rest_types(types.begin() + 1, types.end());
        const Pat& head = q.front();
        if (!head.wildcard) {
            return useful_constructor(rows, q, type, rest_types, head.key, head.args);
        }
        const std::vector<Constructor> all = constructors_of(type);
        if (!all.empty() && covers_all(rows, all)) {
            for (const Constructor& entry : all) {
                const std::vector<Pat> wildcards(entry.args.size());
                if (auto example = useful_constructor(rows, q, type, rest_types, entry.key,
                                                      wildcards)) {
                    return example;
                }
            }
            return std::nullopt;
        }
        std::optional<std::vector<std::string>> example =
            useful(default_rows(rows), Row(q.begin() + 1, q.end()), rest_types);
        if (!example) {
            return std::nullopt;
        }
        example->insert(example->begin(), missing_constructor(rows, type, all));
        return example;
    }

private:
    // 'q' starts with the constructor 'key' applied to 'args'.
    std::optional<std::vector<std::string>> useful_constructor(
        const std::vector<Row>& rows, const Row& q, const Type* type,
        const std::vector<const Type*>& rest_types, const std::string& key,
        const std::vector<Pat>& args) const
    {
        std::vector<const Type*> types = argument_types(type, key);
        types.insert(types.end(), rest_types.begin(), rest_types.end());
        Row next(args.begin(), args.end());
        next.insert(next.end(), q.begin() + 1, q.end());
        std::optional<std::vector<std::string>> example =
            useful(specialize(rows, key, args.size()), next, types);
        if (!example) {
            return std::nullopt;
        }
        const std::vector<std::string> arg_texts(example->begin(),
                                                 example->begin() + static_cast<std::ptrdiff_t>(args.size()));
        std::vector<std::string> result{constructor_text(type, key, arg_texts)};
        result.insert(result.end(), example->begin() + static_cast<std::ptrdiff_t>(args.size()),
                      example->end());
        return result;
    }

    // The rows that can start with constructor 'key', with that constructor
    // replaced by its arguments.
    static std::vector<Row> specialize(const std::vector<Row>& rows, const std::string& key,
                                       std::size_t arity)
    {
        std::vector<Row> result;
        for (const Row& row : rows) {
            const Pat& head = row.front();
            Row next;
            if (head.wildcard) {
                next.assign(arity, Pat{});
            } else if (head.key == key) {
                next = head.args;
            } else {
                continue;
            }
            next.insert(next.end(), row.begin() + 1, row.end());
            result.push_back(std::move(next));
        }
        return result;
    }

    // The rows starting with '_', without it.
    static std::vector<Row> default_rows(const std::vector<Row>& rows)
    {
        std::vector<Row> result;
        for (const Row& row : rows) {
            if (row.front().wildcard) {
                result.emplace_back(row.begin() + 1, row.end());
            }
        }
        return result;
    }

    static bool covers_all(const std::vector<Row>& rows, const std::vector<Constructor>& all)
    {
        for (const Constructor& entry : all) {
            bool found = false;
            for (const Row& row : rows) {
                found = found || (!row.front().wildcard && row.front().key == entry.key);
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }

    // A constructor no row starts with, written with '_' arguments, or '_'
    // for an open type.
    static std::string missing_constructor(const std::vector<Row>& rows, const Type* type,
                                           const std::vector<Constructor>& all)
    {
        for (const Constructor& entry : all) {
            bool found = false;
            for (const Row& row : rows) {
                found = found || (!row.front().wildcard && row.front().key == entry.key);
            }
            if (!found) {
                return constructor_text(type, entry.key,
                                        std::vector<std::string>(entry.args.size(), "_"));
            }
        }
        return "_";
    }
};

// "'A', 'B'"
std::string quoted_list(const std::vector<std::string>& names)
{
    std::string text;
    for (const std::string& name : names) {
        text += (text.empty() ? "'" : ", '") + name + "'";
    }
    return text;
}

} // namespace

// Arms with a guard may fail, so they cover nothing; they can still be
// unreachable. Missing values are listed per top-level constructor, as in
// "does not cover 'Rect(_, _)', 'Empty'".
void Sema::check_match_coverage(const MatchExpr& match, const Type* subject)
{
    if (subject->is_error()) {
        return;
    }
    if (match.arms.empty()) {
        diagnostics_->error(match.location, "'match' needs at least one arm");
        return;
    }
    const Coverage coverage;
    const std::vector<const Type*> types{subject};
    std::vector<Row> rows;
    for (const MatchArm& arm : match.arms) {
        std::optional<Pat> pattern = to_pat(*arm.pattern, subject);
        if (!pattern) {
            return; // the pattern has an error, already reported
        }
        if (!coverage.useful(rows, Row{*pattern}, types)) {
            diagnostics_->error(arm.location, "this arm is never reached; the arms before it "
                                              "already match all of its values");
        }
        if (!arm.guard) {
            rows.push_back(Row{std::move(*pattern)});
        }
    }
    std::vector<std::string> missing;
    const std::vector<Constructor> all = constructors_of(subject);
    if (!all.empty() && !subject->is_tuple()) {
        for (const Constructor& entry : all) {
            Pat q;
            q.wildcard = false;
            q.key = entry.key;
            q.args.resize(entry.args.size());
            if (auto example = coverage.useful(rows, Row{q}, types)) {
                missing.push_back(example->front());
            }
        }
    } else if (auto example = coverage.useful(rows, Row{Pat{}}, types)) {
        missing.push_back(example->front());
    }
    if (missing.empty()) {
        return;
    }
    if (all.empty()) {
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
