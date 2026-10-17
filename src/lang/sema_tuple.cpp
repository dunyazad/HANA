// Semantic analysis of tuples: tuple types, values, '.0' access and
// destructuring 'let (a, b) = ...'.

#include <string>
#include <vector>

#include "lang/sema.h"
#include "lang/sema_util.h"

namespace hana::lang {

using sema_util::quoted;

namespace {

bool is_position(const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Types

const Type* Sema::resolve_tuple_type(const TypeExpr& type)
{
    std::vector<const Type*> elements;
    bool failed = false;
    for (const TypeExpr& element : type.elements) {
        const Type* resolved = resolve_type(element);
        failed = failed || resolved->is_error();
        elements.push_back(resolved);
    }
    return failed ? error_type() : tuple_type(elements);
}

// Tuple types are created once per list of element types, so they compare
// by pointer like every other type.
const Type* Sema::tuple_type(const std::vector<const Type*>& elements)
{
    if (auto it = tuple_types_.find(elements); it != tuple_types_.end()) {
        return it->second;
    }
    TupleType& tuple = program_->tuples.emplace_back();
    std::vector<std::string> names;
    for (const Type* element : elements) {
        names.emplace_back(element->name);
    }
    tuple.name = tuple_type_text(names);
    tuple.type.name = tuple.name;
    tuple.type.elements = elements;
    tuple_types_[elements] = &tuple.type;
    return &tuple.type;
}

// ---------------------------------------------------------------------------
// Values

// An expected tuple type of the same length gives each element its
// expected type, so '(1, 2.0)' can become '(u8, f32)'.
const Type* Sema::check_tuple(TupleExpr& tuple, const Type* expected)
{
    const bool use_expected = expected != nullptr && expected->is_tuple() &&
                              expected->elements.size() == tuple.elements.size();
    std::vector<const Type*> types;
    bool failed = false;
    for (std::size_t i = 0; i < tuple.elements.size(); ++i) {
        Expr& element = *tuple.elements[i];
        const Type* type = check_expr(element, use_expected ? expected->elements[i] : nullptr);
        if (type->is_void()) {
            diagnostics_->error(element.location, "this element has no value");
            failed = true;
        }
        failed = failed || type->is_error();
        types.push_back(type);
    }
    return failed ? error_type() : tuple_type(types);
}

// 'pair.0': the elements are numbered from 0.
const Type* Sema::check_tuple_member(MemberExpr& member, const Type& tuple)
{
    if (!is_position(member.member)) {
        diagnostics_->error(member.location, "a tuple has no field '" + member.member +
                                                "'; its elements are '.0', '.1', ...");
        return error_type();
    }
    const std::size_t count = tuple.elements.size();
    if (member.member.size() > 3 || std::stoul(member.member) >= count) {
        diagnostics_->error(member.location, "the tuple has " + std::to_string(count) +
                                                " elements, so '." + member.member +
                                                "' is out of range");
        return error_type();
    }
    member.field_index = static_cast<int>(std::stoul(member.member));
    return tuple.elements[static_cast<std::size_t>(member.field_index)];
}

// ---------------------------------------------------------------------------
// Destructuring

// 'let (q, r) = value' or 'var (a, _) = value': the names are declared after
// the value is checked, like a plain 'let'.
void Sema::check_destructuring_let(LetStmt& let)
{
    const Type* declared = let.declared_type ? resolve_type(*let.declared_type) : nullptr;
    const Type* init = check_expr(*let.init, declared);
    const Type* type = init;
    if (declared != nullptr) {
        expect_type(declared, init, let.init->location);
        type = declared;
    } else if (init->is_void()) {
        diagnostics_->error(let.init->location, "expression has no value to store");
        type = error_type();
    }
    declare_let_pattern(*let.pattern, type, let.is_mutable);
}

// A 'let' pattern must match every value: names, '_' and tuples of them.
void Sema::declare_let_pattern(Pattern& pattern, const Type* type, bool is_mutable)
{
    switch (pattern.kind) {
    case PatternKind::Wildcard: return;
    case PatternKind::Binding:
        pattern.symbol = declare_var(pattern.name, type, VarKind::Local, is_mutable, pattern.location);
        return;
    case PatternKind::Tuple: {
        const std::size_t count = pattern.arguments.size();
        if (!type->is_error() && !type->is_tuple()) {
            diagnostics_->error(pattern.location, "only tuples can be destructured, but the value "
                                                  "has type " + quoted(type));
            type = error_type();
        } else if (type->is_tuple() && type->elements.size() != count) {
            diagnostics_->error(pattern.location, "the tuple has " +
                                                      std::to_string(type->elements.size()) +
                                                      " elements, but the pattern has " +
                                                      std::to_string(count));
            type = error_type();
        }
        for (std::size_t i = 0; i < count; ++i) {
            declare_let_pattern(*pattern.arguments[i],
                                type->is_error() ? error_type() : type->elements[i], is_mutable);
        }
        return;
    }
    case PatternKind::Literal:
    case PatternKind::Range:
    case PatternKind::Case:
        diagnostics_->error(pattern.location, "a 'let' pattern must match every value; use "
                                              "'match' to test values");
        return;
    }
}

// '(p, q)' in 'match': one pattern per element.
void Sema::check_tuple_pattern(Pattern& pattern, const Type* subject)
{
    const std::size_t count = pattern.arguments.size();
    const Type* type = subject;
    if (!subject->is_error() && !subject->is_tuple()) {
        diagnostics_->error(pattern.location, "a tuple pattern cannot match a value of type " +
                                                  quoted(subject));
        type = error_type();
    } else if (subject->is_tuple() && subject->elements.size() != count) {
        diagnostics_->error(pattern.location, "the tuple has " +
                                                  std::to_string(subject->elements.size()) +
                                                  " elements, but the pattern has " +
                                                  std::to_string(count));
        type = error_type();
    }
    for (std::size_t i = 0; i < count; ++i) {
        check_pattern(*pattern.arguments[i], type->is_error() ? error_type() : type->elements[i]);
    }
}

} // namespace hana::lang
