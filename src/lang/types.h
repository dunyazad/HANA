#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "support/source.h"

namespace hana::lang {

enum class TypeKind {
    Error, // result of a failed check; suppresses follow-up diagnostics
    Void,
    Bool,
    Char,
    Str,
    Int,
    Float,
    Struct,
};

struct StructInfo;

// Built-in types are unique singletons, so types compare by pointer.
struct Type {
    TypeKind kind;
    std::string_view name;
    unsigned bits = 0;          // for Int and Float
    bool is_signed = false;     // for Int
    bool is_pointer_sized = false; // isize / usize: width comes from the target
    // Placeholder used while inferring the type of a variable initialized
    // with an untyped literal ('var i = 0'). Never survives semantic analysis.
    bool is_literal_var = false;
    const StructInfo* struct_info = nullptr; // for Struct

    bool is_error() const { return kind == TypeKind::Error; }
    bool is_void() const { return kind == TypeKind::Void; }
    bool is_integer() const { return kind == TypeKind::Int; }
    bool is_float() const { return kind == TypeKind::Float; }
    bool is_numeric() const { return is_integer() || is_float(); }
    bool is_struct() const { return kind == TypeKind::Struct; }
};

struct FunctionSymbol;
struct Expr;

struct StructField {
    std::string name;
    const Type* type = nullptr;
    const Expr* default_value = nullptr;
    SourceLocation location;
};

// A user-defined struct. Owned by the Program; 'type.name' views 'name'.
struct StructInfo {
    std::string name;
    SourceLocation location;
    bool is_copy = false;
    std::vector<StructField> fields;
    std::unordered_map<std::string, const FunctionSymbol*> methods;
    Type type{TypeKind::Struct, {}};

    int find_field(std::string_view field_name) const
    {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].name == field_name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

// Copy types are duplicated instead of moved: numbers, bool, char and
// structs declared 'struct copy'.
bool is_copy_type(const Type* type);

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
