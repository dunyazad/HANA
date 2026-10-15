#pragma once

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "lang/ast.h"
#include "lang/types.h"
#include "support/source.h"

namespace hana::lang {

enum class VarKind {
    Local,      // let / var
    Param,      // read parameter: may be read only
    SinkParam,  // sink parameter: owned by the function
    InoutParam, // inout parameter: refers to the caller's storage
};

struct VarSymbol {
    std::string name;
    const Type* type = nullptr;
    VarKind kind = VarKind::Local;
    bool is_mutable = false;
    SourceLocation location;
};

enum class Intrinsic {
    None,
    Print, // print(value): writes the value and a newline
    Write, // write(value): writes the value only
};

struct FunctionSymbol {
    std::string name;
    // Methods: the owning struct and how 'self' is passed (none for static
    // functions such as 'Point.new').
    const UserType* owner = nullptr;
    std::optional<ParamConvention> self_convention;
    std::vector<const Type*> param_types;
    std::vector<ParamConvention> conventions;
    const Type* return_type = nullptr;
    const FnDecl* decl = nullptr; // null for intrinsics
    Intrinsic intrinsic = Intrinsic::None;
};

// Result of semantic analysis. Symbols live in deques so the pointers stored
// in the AST stay valid.
struct Program {
    std::deque<VarSymbol> vars;
    std::deque<FunctionSymbol> functions;
    std::deque<UserType> types;
    const FunctionSymbol* main = nullptr;
};

} // namespace hana::lang
