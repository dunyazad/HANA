#pragma once

#include <string_view>

namespace hana::lang {

enum class TypeKind {
    Error, // result of a failed check; suppresses follow-up diagnostics
    Void,
    Bool,
    Char,
    Str,
    Int,
    Float,
};

// Built-in types are unique singletons, so types compare by pointer.
struct Type {
    TypeKind kind;
    std::string_view name;
    unsigned bits = 0;          // for Int and Float
    bool is_signed = false;     // for Int
    bool is_pointer_sized = false; // isize / usize: width comes from the target

    bool is_error() const { return kind == TypeKind::Error; }
    bool is_void() const { return kind == TypeKind::Void; }
    bool is_integer() const { return kind == TypeKind::Int; }
    bool is_float() const { return kind == TypeKind::Float; }
    bool is_numeric() const { return is_integer() || is_float(); }
};

// Looks up a type by its source name ("i32", "bool", ...). Returns null for
// unknown names.
const Type* find_builtin_type(std::string_view name);

const Type* error_type();
const Type* void_type();
const Type* bool_type();
const Type* char_type();
const Type* str_type();
const Type* i32_type();
const Type* f64_type();

// Width used for range checks of isize/usize literals. Code generation uses
// the real target width.
constexpr unsigned kAssumedPointerBits = 64;

unsigned checked_bits(const Type* type);

} // namespace hana::lang
