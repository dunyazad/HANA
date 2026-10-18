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
    Element,    // 'for x in xs' / 'for inout x in xs': an element of the list
};

struct VarSymbol {
    std::string name;
    const Type* type = nullptr;
    VarKind kind = VarKind::Local;
    bool is_mutable = false;
    bool is_loop_variable = false; // the variable of a 'for' loop
    SourceLocation location;
};

enum class Intrinsic {
    None,
    Print, // print(value): writes the value and a newline
    Write, // write(value): writes the value only
    Some,  // Some(value): an Option holding the value
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
    // A generic function: its own type parameters. Code is made only for
    // its instances, which have none.
    std::vector<const Type*> type_params;
    const TraitInfo* trait = nullptr; // a method a trait declares
};

// Result of semantic analysis. Symbols live in deques so the pointers stored
// in the AST stay valid.
struct Program {
    std::deque<VarSymbol> vars;
    std::deque<FunctionSymbol> functions;
    std::deque<UserType> types;
    std::deque<ComposedType> composed_types; // tuples, Options and Lists
    std::deque<TraitInfo> traits;
    std::deque<TypeParamInfo> type_params;
    std::deque<Type> param_types;
    // Copies of generic functions and of the methods of generic types, one
    // per list of type arguments, checked with those types. Code is made
    // for these and for the functions that are not generic.
    std::vector<std::unique_ptr<FnDecl>> instances;
    // The field defaults of instances of generic types, checked with the
    // instance's types.
    std::vector<ExprPtr> instance_defaults;
    const FunctionSymbol* main = nullptr;
};

} // namespace hana::lang
