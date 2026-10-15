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

bool is_copy_type(const Type* type)
{
    switch (type->kind) {
    case TypeKind::Str: return false;
    case TypeKind::Struct: return type->struct_info->is_copy;
    default: return true;
    }
}

unsigned checked_bits(const Type* type)
{
    return type->is_pointer_sized ? kAssumedPointerBits : type->bits;
}

} // namespace hana::lang
