#include "lang/types.h"

namespace hana::lang {

namespace {

const Type kError{TypeKind::Error, "<error>"};
const Type kVoid{TypeKind::Void, "void"};
const Type kBool{TypeKind::Bool, "bool"};
const Type kChar{TypeKind::Char, "char"};
const Type kStr{TypeKind::Str, "str"};

const Type kI8{TypeKind::Int, "i8", 8, true};
const Type kI16{TypeKind::Int, "i16", 16, true};
const Type kI32{TypeKind::Int, "i32", 32, true};
const Type kI64{TypeKind::Int, "i64", 64, true};
const Type kIsize{TypeKind::Int, "isize", kAssumedPointerBits, true, true};
const Type kU8{TypeKind::Int, "u8", 8, false};
const Type kU16{TypeKind::Int, "u16", 16, false};
const Type kU32{TypeKind::Int, "u32", 32, false};
const Type kU64{TypeKind::Int, "u64", 64, false};
const Type kUsize{TypeKind::Int, "usize", kAssumedPointerBits, false, true};
const Type kF32{TypeKind::Float, "f32", 32};
const Type kF64{TypeKind::Float, "f64", 64};

// "void" is not listed: it cannot be written in source.
const Type* const kNamedTypes[] = {
    &kBool, &kChar, &kStr,  &kI8,  &kI16, &kI32,  &kI64, &kIsize,
    &kU8,   &kU16,  &kU32,  &kU64, &kUsize, &kF32, &kF64,
};

} // namespace

const Type* find_builtin_type(std::string_view name)
{
    for (const Type* type : kNamedTypes) {
        if (type->name == name) {
            return type;
        }
    }
    return nullptr;
}

const Type* error_type() { return &kError; }
const Type* void_type() { return &kVoid; }
const Type* bool_type() { return &kBool; }
const Type* char_type() { return &kChar; }
const Type* str_type() { return &kStr; }
const Type* i32_type() { return &kI32; }
const Type* f64_type() { return &kF64; }

int find_field(const std::vector<FieldInfo>& fields, std::string_view name)
{
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int UserType::find_case(std::string_view case_name) const
{
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (cases[i].name == case_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool is_copy_type(const Type* type)
{
    switch (type->kind) {
    case TypeKind::Str:
    case TypeKind::List: return false;
    case TypeKind::Struct:
    case TypeKind::Enum: return type->user->is_copy;
    case TypeKind::Tuple:
    case TypeKind::Option:
        for (const Type* element : type->elements) {
            if (!is_copy_type(element)) {
                return false;
            }
        }
        return true;
    default: return true;
    }
}

bool needs_drop(const Type* type)
{
    switch (type->kind) {
    case TypeKind::Str:
    case TypeKind::List: return true;
    case TypeKind::Struct:
    case TypeKind::Enum: {
        const UserType& info = *type->user;
        if (info.drop_method != nullptr) {
            return true;
        }
        for (const FieldInfo& field : info.fields) {
            if (needs_drop(field.type)) {
                return true;
            }
        }
        for (const EnumCase& entry : info.cases) {
            for (const FieldInfo& field : entry.fields) {
                if (needs_drop(field.type)) {
                    return true;
                }
            }
        }
        return false;
    }
    case TypeKind::Tuple:
    case TypeKind::Option:
        for (const Type* element : type->elements) {
            if (needs_drop(element)) {
                return true;
            }
        }
        return false;
    default: return false;
    }
}

namespace {

// A struct may hold a list of itself ('children: List<Node>'), so the
// types being checked are tracked to end the recursion; a type found
// again is clonable when the rest of it is.
bool is_clonable_in(const Type* type, std::vector<const Type*>& checking)
{
    if (is_copy_type(type) || type->kind == TypeKind::Str) {
        return true;
    }
    for (const Type* outer : checking) {
        if (outer == type) {
            return true;
        }
    }
    checking.push_back(type);
    bool clonable = true;
    switch (type->kind) {
    case TypeKind::Struct:
    case TypeKind::Enum: {
        const UserType& info = *type->user;
        if (info.clone_method != nullptr) {
            break;
        }
        if (info.drop_method != nullptr) {
            clonable = false;
            break;
        }
        for (const FieldInfo& field : info.fields) {
            clonable = clonable && is_clonable_in(field.type, checking);
        }
        for (const EnumCase& entry : info.cases) {
            for (const FieldInfo& field : entry.fields) {
                clonable = clonable && is_clonable_in(field.type, checking);
            }
        }
        break;
    }
    case TypeKind::Tuple:
    case TypeKind::Option:
    case TypeKind::List:
        for (const Type* element : type->elements) {
            clonable = clonable && is_clonable_in(element, checking);
        }
        break;
    default: clonable = false; break;
    }
    checking.pop_back();
    return clonable;
}

} // namespace

bool is_clonable(const Type* type)
{
    std::vector<const Type*> checking;
    return is_clonable_in(type, checking);
}

unsigned checked_bits(const Type* type)
{
    return type->is_pointer_sized ? kAssumedPointerBits : type->bits;
}

} // namespace hana::lang
