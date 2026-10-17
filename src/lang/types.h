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
    Enum,
    Tuple,
    Option, // Option<T>: 'elements' holds T
    List,   // List<T>: 'elements' holds T
};

struct UserType;

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
    const UserType* user = nullptr; // for Struct and Enum
    std::vector<const Type*> elements{}; // for Tuple, and T of Option<T> and List<T>

    bool is_error() const { return kind == TypeKind::Error; }
    bool is_void() const { return kind == TypeKind::Void; }
    bool is_integer() const { return kind == TypeKind::Int; }
    bool is_float() const { return kind == TypeKind::Float; }
    bool is_numeric() const { return is_integer() || is_float(); }
    bool is_struct() const { return kind == TypeKind::Struct; }
    bool is_enum() const { return kind == TypeKind::Enum; }
    bool is_user() const { return is_struct() || is_enum(); }
    bool is_tuple() const { return kind == TypeKind::Tuple; }
    bool is_option() const { return kind == TypeKind::Option; }
    bool is_list() const { return kind == TypeKind::List; }
    // T of Option<T>.
    const Type* option_value() const { return elements.front(); }
    // T of List<T>.
    const Type* list_element() const { return elements.front(); }
};

struct FunctionSymbol;
struct Expr;

struct FieldInfo {
    std::string name;
    const Type* type = nullptr;
    const Expr* default_value = nullptr;
    SourceLocation location;
};

int find_field(const std::vector<FieldInfo>& fields, std::string_view name);

// One case of an enum, with the fields its value carries.
struct EnumCase {
    std::string name;
    SourceLocation location;
    std::vector<FieldInfo> fields;
};

// A user-defined struct or enum. Owned by the Program; 'type.name' views
// 'name'. A struct has 'fields', an enum has 'cases'.
struct UserType {
    std::string name;
    SourceLocation location;
    bool is_copy = false;
    std::vector<FieldInfo> fields;
    std::vector<EnumCase> cases;
    std::unordered_map<std::string, const FunctionSymbol*> methods;
    const FunctionSymbol* drop_method = nullptr;  // 'drop: fn<(inout self)>'
    const FunctionSymbol* clone_method = nullptr; // a 'clone' the user wrote
    Type type{TypeKind::Struct, {}};

    bool is_enum() const { return type.kind == TypeKind::Enum; }
    // "struct" or "enum", for messages.
    const char* kind_word() const { return is_enum() ? "enum" : "struct"; }
    int find_field(std::string_view field_name) const { return lang::find_field(fields, field_name); }
    int find_case(std::string_view case_name) const;
};

// A tuple, Option or List type, created once per element types. Owned by
// the Program; 'type.name' views 'name'.
struct ComposedType {
    std::string name; // "(i32, f64)", "Option<i32>", "List<i32>"
    Type type{TypeKind::Tuple, {}};
};

// Copy types are duplicated instead of moved: numbers, bool, char and
// structs and enums declared with 'copy', and tuples and Options of Copy
// elements.
bool is_copy_type(const Type* type);

// True when a value of the type owns something to free when it goes out of
// scope: a 'str', a 'List', a type with 'drop', or a value holding one of
// these.
bool needs_drop(const Type* type);

// True when '.clone()' works on the type: Copy types, 'str', types with a
// 'clone' the user wrote, and types without 'drop' whose fields all clone,
// and lists of clonable elements.
bool is_clonable(const Type* type);

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
