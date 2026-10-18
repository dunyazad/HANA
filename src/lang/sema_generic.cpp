// Semantic analysis of generics and traits.
//
// A generic function or type is checked once with its type parameters as
// Param types: the body may use only what their bounds allow. Every use
// with concrete types makes an instance: a generic type becomes a separate
// UserType ('Pair<i32>'), and a generic function or a method of a generic
// type becomes a copy of its syntax tree checked with the concrete types.
// Code generation sees only the instances.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "lang/ast_clone.h"
#include "lang/sema.h"
#include "lang/sema_util.h"

namespace hana::lang {

using sema_util::as_inout_mark;
using sema_util::is_untyped_literal;
using sema_util::quoted;

namespace {

// Instances of instances deeper than this mean a generic function calls
// itself with ever larger types ('f<List<T>>' inside 'f<T>').
constexpr int kMaxInstanceDepth = 24;

int type_depth(const Type* type)
{
    int depth = 0;
    for (const Type* element : type->elements) {
        depth = std::max(depth, type_depth(element));
    }
    if (type->is_user()) {
        for (const Type* argument : type->user->arguments()) {
            depth = std::max(depth, type_depth(argument));
        }
    }
    return depth + 1;
}

std::string type_list_text(const std::vector<const Type*>& types)
{
    std::string text;
    for (std::size_t i = 0; i < types.size(); ++i) {
        text += (i == 0 ? "" : ", ") + std::string(types[i]->name);
    }
    return text;
}

bool includes_type(const std::vector<const Type*>& types, const Type* type)
{
    return std::find(types.begin(), types.end(), type) != types.end();
}

// 'null' and '[]' have no type without an expected one.
bool needs_expected_type(const Expr& arg)
{
    return arg.kind == ExprKind::Null ||
           (arg.kind == ExprKind::List && static_cast<const ListExpr&>(arg).elements.empty());
}

// An argument whose type should come from the parameter: untyped literals,
// 'null', '[]' and names of variables still inferring their literal type.
bool takes_type_from_parameter(const Expr& arg)
{
    if (is_untyped_literal(arg) || arg.kind == ExprKind::Null) {
        return true;
    }
    if (arg.kind == ExprKind::List) {
        return static_cast<const ListExpr&>(arg).elements.empty();
    }
    if (arg.kind == ExprKind::Name) {
        const VarSymbol* var = static_cast<const NameExpr&>(arg).var;
        return var != nullptr && var->type->is_literal_var;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Traits

void Sema::declare_builtin_traits()
{
    struct Entry {
        const char* name;
        BuiltinTrait trait;
    };
    static constexpr Entry kTraits[] = {
        {"Copy", BuiltinTrait::Copy},
        {"Clone", BuiltinTrait::Clone},
        {"Eq", BuiltinTrait::Eq},
        {"Ord", BuiltinTrait::Ord},
    };
    for (const Entry& entry : kTraits) {
        TraitInfo& info = program_->traits.emplace_back();
        info.name = entry.name;
        info.builtin = entry.trait;
        traits_[info.name] = &info;
    }
}

void Sema::declare_trait(TraitDecl& decl)
{
    if (traits_.count(decl.name) != 0 || lookup_type(decl.name) != nullptr ||
        find_builtin_type(decl.name) != nullptr) {
        diagnostics_->error(decl.location, "'" + decl.name + "' is already defined");
        return;
    }
    TraitInfo& info = program_->traits.emplace_back();
    info.name = decl.name;
    info.location = decl.location;
    TypeParamInfo& self = program_->type_params.emplace_back();
    self.name = "Self";
    self.location = decl.location;
    self.bounds = {&info};
    Type& self_type = program_->param_types.emplace_back(Type{TypeKind::Param, {}});
    self_type.name = self.name;
    self_type.param = &self;
    self.type = &self_type;
    info.self_type = &self_type;
    decl.trait = &info;
    traits_[info.name] = &info;
}

// The signatures of a trait's methods, with 'Self' for the type that
// implements the trait.
void Sema::declare_trait_methods(TraitDecl& decl)
{
    if (decl.trait == nullptr) {
        return;
    }
    auto& info = const_cast<TraitInfo&>(*decl.trait);
    type_scopes_.push_back({{"Self", info.self_type}});
    for (auto& method : decl.methods) {
        if (info.methods.count(method->name) != 0) {
            diagnostics_->error(method->location, "'" + method->name + "' is already declared in '" +
                                                      decl.name + "'");
            continue;
        }
        if (!method->type_params.empty()) {
            diagnostics_->error(method->location, "generic methods in traits are not supported yet");
            continue;
        }
        if (!method->self_convention) {
            diagnostics_->error(method->location, "a trait method takes 'self'; functions without "
                                                  "'self' in traits are not supported yet");
            continue;
        }
        FunctionSymbol& symbol = create_function_symbol(*method, decl.name + "." + method->name);
        symbol.trait = &info;
        info.methods[method->name] = &symbol;
        info.method_order.push_back(method->name);
    }
    type_scopes_.pop_back();
}

std::vector<const TraitInfo*> Sema::resolve_bounds(const std::vector<NameRef>& names)
{
    std::vector<const TraitInfo*> traits;
    for (const NameRef& name : names) {
        auto it = traits_.find(name.name);
        if (it == traits_.end()) {
            diagnostics_->error(name.location, "unknown trait '" + name.name + "'");
            continue;
        }
        traits.push_back(it->second);
    }
    return traits;
}

// 'is: [Shape]': built-in traits are never listed, a type has them by the
// rules of the language.
std::vector<const TraitInfo*> Sema::resolve_declared_traits(const TypeDecl& decl)
{
    std::vector<const TraitInfo*> traits;
    for (const TraitInfo* trait : resolve_bounds(decl.traits)) {
        if (trait->builtin != BuiltinTrait::None) {
            diagnostics_->error(decl.location, "'" + trait->name + "' is built in: a type has it "
                                                                   "when the language says so, so "
                                                                   "it is not listed in 'is'");
            continue;
        }
        traits.push_back(trait);
    }
    return traits;
}

std::string Sema::signature_text(const FunctionSymbol& function)
{
    std::string text = "fn<";
    if (!function.return_type->is_void()) {
        text += std::string(function.return_type->name);
    }
    text += "(";
    bool first = true;
    if (function.self_convention) {
        text += *function.self_convention == ParamConvention::Read
                    ? std::string("self")
                    : std::string(param_convention_spelling(*function.self_convention)) + " self";
        first = false;
    }
    for (std::size_t i = 0; i < function.param_types.size(); ++i) {
        text += first ? "" : ", ";
        first = false;
        if (function.conventions[i] != ParamConvention::Read) {
            text += std::string(param_convention_spelling(function.conventions[i])) + " ";
        }
        text += std::string(function.param_types[i]->name);
    }
    return text + ")>";
}

// Every method of every trait in 'is' must be defined with the trait's
// signature, 'Self' being the type itself.
void Sema::check_trait_conformance(const TypeDecl& decl)
{
    if (decl.type == nullptr) {
        return;
    }
    const UserType& info = *decl.type->user;
    for (const TraitInfo* trait : info.traits) {
        const TypeMap self{{trait->self_type, &info.type}};
        for (const std::string& name : trait->method_order) {
            const FunctionSymbol& required = *trait->methods.at(name);
            FunctionSymbol expected = required;
            expected.param_types.clear();
            for (const Type* param : required.param_types) {
                expected.param_types.push_back(substitute(param, self));
            }
            expected.return_type = substitute(required.return_type, self);
            auto found = info.methods.find(name);
            if (found == info.methods.end()) {
                diagnostics_->error(decl.location, "'" + decl.name + "' says it is '" + trait->name +
                                                       "', but has no method '" + name + "'");
                diagnostics_->note(required.decl->location, "'" + name + "' is declared here as '" +
                                                                signature_text(expected) + "'");
                continue;
            }
            const FunctionSymbol& actual = *found->second;
            const bool same = actual.self_convention == expected.self_convention &&
                              actual.param_types == expected.param_types &&
                              actual.conventions == expected.conventions &&
                              actual.return_type == expected.return_type &&
                              actual.type_params.empty();
            if (!same) {
                diagnostics_->error(actual.decl->location,
                                   "'" + name + "' does not match the trait '" + trait->name +
                                       "': it must be '" + signature_text(expected) + "'");
            }
        }
    }
}

// 'x.area()' on a type parameter: a method of one of its traits.
const FunctionSymbol* Sema::find_trait_method(MemberExpr& callee, const Type* receiver)
{
    if (callee.member == "clone") {
        diagnostics_->error(callee.location, "'clone' needs 'Clone'; write '" +
                                                std::string(receiver->name) + ": Clone'");
        return nullptr;
    }
    for (const TraitInfo* trait : receiver->param->bounds) {
        auto it = trait->methods.find(callee.member);
        if (it != trait->methods.end()) {
            return it->second;
        }
    }
    diagnostics_->error(callee.location, "'" + std::string(receiver->name) + "' has no method '" +
                                            callee.member + "'; give '" +
                                            std::string(receiver->name) +
                                            "' a trait that has it, as in '" +
                                            std::string(receiver->name) + ": Shape'");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Type parameters

std::vector<const Type*> Sema::declare_type_params(const std::vector<TypeParamDecl>& decls)
{
    std::vector<const Type*> params;
    for (std::size_t i = 0; i < decls.size(); ++i) {
        const TypeParamDecl& decl = decls[i];
        for (std::size_t j = 0; j < i; ++j) {
            if (decls[j].name == decl.name) {
                diagnostics_->error(decl.location, "type parameter '" + decl.name +
                                                       "' is declared twice");
            }
        }
        if (find_builtin_type(decl.name) != nullptr) {
            diagnostics_->error(decl.location, "'" + decl.name + "' is a built-in type and cannot "
                                                                 "name a type parameter");
        }
        TypeParamInfo& info = program_->type_params.emplace_back();
        info.name = decl.name;
        info.location = decl.location;
        info.bounds = resolve_bounds(decl.bounds);
        Type& type = program_->param_types.emplace_back(Type{TypeKind::Param, {}});
        type.name = info.name;
        type.param = &info;
        info.type = &type;
        params.push_back(&type);
    }
    return params;
}

// Makes the names of type parameters visible: 'T' is the Param type in a
// generic body and the concrete type in an instance.
void Sema::push_type_names(const std::vector<TypeParamDecl>& decls,
                           const std::vector<const Type*>& types)
{
    std::unordered_map<std::string, const Type*> names;
    for (std::size_t i = 0; i < decls.size() && i < types.size(); ++i) {
        names[decls[i].name] = types[i];
    }
    type_scopes_.push_back(std::move(names));
}

void Sema::push_type_bindings(const std::vector<std::pair<std::string, const Type*>>& bindings)
{
    std::unordered_map<std::string, const Type*> names;
    for (const auto& [name, type] : bindings) {
        names[name] = type;
    }
    type_scopes_.push_back(std::move(names));
}

const Type* Sema::lookup_type_name(const std::string& name) const
{
    for (auto it = type_scopes_.rbegin(); it != type_scopes_.rend(); ++it) {
        if (auto found = it->find(name); found != it->end()) {
            return found->second;
        }
    }
    return nullptr;
}

// The type names a function's signature and body see: those of its type
// (for a method of a generic type), and its own type parameters; or, for an
// instance, the concrete types they stand for. Returns how many scopes were
// pushed.
int Sema::push_function_type_names(const FnDecl& function)
{
    if (!function.type_bindings.empty()) {
        push_type_bindings(function.type_bindings);
        return 1;
    }
    int pushed = 0;
    if (function.owner != nullptr && !function.owner->type_params.empty() &&
        function.owner->type != nullptr) {
        push_type_names(function.owner->type_params, function.owner->type->user->type_params);
        ++pushed;
    }
    if (!function.type_params.empty() && function.symbol != nullptr) {
        push_type_names(function.type_params, function.symbol->type_params);
        ++pushed;
    }
    return pushed;
}

void Sema::pop_type_names(int count)
{
    for (int i = 0; i < count; ++i) {
        type_scopes_.pop_back();
    }
}

// ---------------------------------------------------------------------------
// Generic types

// 'Pair<i32>' as written in a type.
const Type* Sema::resolve_generic_type(const TypeExpr& type)
{
    const std::string& name = type.generic_name;
    if (name == "Option" || name == "List") {
        diagnostics_->error(type.location, "'" + name + "' takes one type argument");
        return error_type();
    }
    const UserType* generic = lookup_type(name);
    if (generic == nullptr) {
        diagnostics_->error(type.location, "unknown type '" + name + "'");
        return error_type();
    }
    if (generic->type_params.size() != type.elements.size()) {
        diagnostics_->error(type.location,
                           generic->type_params.empty()
                               ? "'" + name + "' is not generic; write it without '<...>'"
                               : "'" + name + "' takes " +
                                     std::to_string(generic->type_params.size()) +
                                     " type argument(s), found " +
                                     std::to_string(type.elements.size()));
        return error_type();
    }
    std::vector<const Type*> args;
    for (const TypeExpr& element : type.elements) {
        const Type* arg = resolve_type(element);
        if (arg->is_error()) {
            return arg;
        }
        args.push_back(arg);
    }
    if (!check_bounds(generic->type_params, args, "'" + name + "'", type.location)) {
        return error_type();
    }
    return instantiate(*generic, args);
}

// Each type argument must have the traits its parameter is bound by.
bool Sema::check_bounds(const std::vector<const Type*>& params, const std::vector<const Type*>& args,
                        const std::string& what, const SourceLocation& location)
{
    bool ok = true;
    for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) {
        for (const TraitInfo* bound : params[i]->param->bounds) {
            if (implements(args[i], *bound)) {
                continue;
            }
            diagnostics_->error(location, what + " needs '" + std::string(params[i]->name) + ": " +
                                              bound->name + "', but " + quoted(args[i]) +
                                              " is not '" + bound->name + "'");
            ok = false;
        }
    }
    return ok;
}

// The instance of a generic type for the type arguments, made once. Its
// fields and methods are the generic type's with the arguments put in.
// Methods of an instance with concrete types are copied and checked later
// so code can be made for them.
const Type* Sema::instantiate(const UserType& generic, const std::vector<const Type*>& args)
{
    if (args == generic.type_params) {
        return &generic.type;
    }
    const auto key = std::make_pair(&generic, args);
    if (auto it = type_instances_.find(key); it != type_instances_.end()) {
        return it->second;
    }
    UserType& instance = program_->types.emplace_back();
    instance.name = generic.decl->name + "<" + type_list_text(args) + ">";
    instance.location = generic.location;
    instance.is_copy = generic.is_copy;
    instance.type.kind = generic.type.kind;
    instance.type.name = instance.name;
    instance.type.user = &instance;
    instance.generic = &generic;
    instance.type_args = args;
    instance.decl = generic.decl;
    instance.traits = generic.traits;
    type_instances_[key] = &instance.type;

    const bool concrete = std::all_of(args.begin(), args.end(), is_concrete);
    // The generic declaration was checked already; its errors are not
    // reported again for each instance.
    DiagnosticEngine scratch;
    DiagnosticEngine* real = diagnostics_;
    diagnostics_ = &scratch;
    push_type_names(generic.decl->type_params, args);
    auto& decl = const_cast<TypeDecl&>(*generic.decl);
    declare_fields(decl.fields, instance.fields, instance.name);
    for (CaseDecl& entry : decl.cases) {
        EnumCase& target = instance.cases.emplace_back();
        target.name = entry.name;
        target.location = entry.location;
        declare_fields(entry.fields, target.fields, instance.name);
    }
    pop_type_names(1);
    diagnostics_ = real;
    if (concrete) {
        copy_instance_defaults(instance);
    }
    if (methods_declared_) {
        declare_instance_methods(instance);
    } else {
        waiting_for_methods_.push_back(&instance);
    }
    return &instance.type;
}

// An instance's methods are the generic type's with the arguments put in.
// They need the generic type's methods to be declared first: a generic
// method keeps the type parameters it was declared with, so 'map<U>'
// returning 'Stack<U>' makes the instance 'Stack<U>' only once.
void Sema::declare_instance_methods(UserType& instance)
{
    const bool concrete = std::all_of(instance.type_args.begin(), instance.type_args.end(),
                                      is_concrete);
    DiagnosticEngine scratch;
    DiagnosticEngine* real = diagnostics_;
    diagnostics_ = &scratch;
    push_type_names(instance.decl->type_params, instance.type_args);
    for (auto& method : const_cast<TypeDecl&>(*instance.decl).methods) {
        declare_instance_method(instance, *method, concrete);
    }
    pop_type_names(1);
    diagnostics_ = real;
}

void Sema::declare_waiting_instance_methods()
{
    methods_declared_ = true;
    // Declaring methods can make more instances; those declare their own.
    for (std::size_t i = 0; i < waiting_for_methods_.size(); ++i) {
        declare_instance_methods(*waiting_for_methods_[i]);
    }
    waiting_for_methods_.clear();
}

// A concrete instance has its own copy of each field default, checked with
// its field types, because the default's type depends on the arguments
// ('items: List<T> = []').
void Sema::copy_instance_defaults(UserType& instance)
{
    auto copy_defaults = [&](std::vector<FieldInfo>& fields) {
        for (FieldInfo& field : fields) {
            if (field.default_value != nullptr) {
                program_->instance_defaults.push_back(clone_expr(*field.default_value));
                field.default_value = program_->instance_defaults.back().get();
                pending_defaults_.push_back(PendingDefault{&instance, &field});
            }
        }
    };
    copy_defaults(instance.fields);
    for (EnumCase& entry : instance.cases) {
        copy_defaults(entry.fields);
    }
}

void Sema::declare_instance_method(UserType& instance, FnDecl& method, bool concrete)
{
    if (instance.methods.count(method.name) != 0 || instance.find_field(method.name) >= 0) {
        return; // reported for the generic type
    }
    FnDecl* decl = &method;
    if (concrete) {
        std::unique_ptr<FnDecl> copy = clone_function(method);
        copy->self_type = &instance.type;
        for (std::size_t i = 0; i < instance.generic->type_params.size(); ++i) {
            copy->type_bindings.emplace_back(instance.decl->type_params[i].name,
                                             instance.type_args[i]);
        }
        decl = copy.get();
        program_->instances.push_back(std::move(copy));
    }
    // A generic method keeps its own type parameters; the names of the
    // type's parameters are already bound by the caller.
    const std::vector<const Type*>* own_params =
        method.symbol != nullptr && !method.type_params.empty() ? &method.symbol->type_params
                                                                : nullptr;
    FunctionSymbol& symbol = create_function_symbol(*decl, instance.name + "." + method.name,
                                                    concrete, own_params);
    symbol.owner = &instance;
    instance.methods[method.name] = &symbol;
    if (method.name == "drop") {
        instance.drop_method = &symbol;
    } else if (method.name == "clone") {
        instance.clone_method = &symbol;
    }
    if (concrete && symbol.type_params.empty()) {
        pending_bodies_.push_back(decl);
    } else if (concrete) {
        // Instances of the method are made per call; this copy is only
        // their template.
        program_->instances.back()->has_body = false;
    }
}

// ---------------------------------------------------------------------------
// Substitution and inference

const Type* Sema::substitute(const Type* type, const TypeMap& map)
{
    if (map.empty()) {
        return type;
    }
    switch (type->kind) {
    case TypeKind::Param: {
        auto it = map.find(type);
        return it != map.end() ? it->second : type;
    }
    case TypeKind::List: return list_type(substitute(type->list_element(), map));
    case TypeKind::Option: return option_type(substitute(type->option_value(), map));
    case TypeKind::Tuple: {
        std::vector<const Type*> elements;
        for (const Type* element : type->elements) {
            elements.push_back(substitute(element, map));
        }
        return tuple_type(elements);
    }
    case TypeKind::Struct:
    case TypeKind::Enum: {
        const std::vector<const Type*>& args = type->user->arguments();
        if (args.empty()) {
            return type;
        }
        std::vector<const Type*> substituted;
        for (const Type* arg : args) {
            substituted.push_back(substitute(arg, map));
        }
        return instantiate(type->user->origin(), substituted);
    }
    default: return type;
    }
}

// Binds the parameters in 'params' that 'pattern' places where 'actual'
// has a type. A value where an Option is expected is wrapped, so 'T?'
// matches a plain value too. Mismatches are reported by the caller.
void Sema::unify_params(const Type* pattern, const Type* actual,
                        const std::vector<const Type*>& params, TypeMap& map)
{
    if (actual->is_error() || actual->is_void()) {
        return;
    }
    if (pattern->is_param() && includes_type(params, pattern)) {
        map.try_emplace(pattern, actual);
        return;
    }
    if (pattern->is_option() && !actual->is_option()) {
        unify_params(pattern->option_value(), actual, params, map);
        return;
    }
    if (pattern->kind != actual->kind) {
        return;
    }
    if (pattern->is_list() || pattern->is_option() || pattern->is_tuple()) {
        for (std::size_t i = 0; i < pattern->elements.size() && i < actual->elements.size(); ++i) {
            unify_params(pattern->elements[i], actual->elements[i], params, map);
        }
        return;
    }
    if (pattern->is_user() && &pattern->user->origin() == &actual->user->origin()) {
        const auto& expected_args = pattern->user->arguments();
        const auto& actual_args = actual->user->arguments();
        for (std::size_t i = 0; i < expected_args.size() && i < actual_args.size(); ++i) {
            unify_params(expected_args[i], actual_args[i], params, map);
        }
    }
}

bool Sema::mentions(const Type* type, const std::vector<const Type*>& params, const TypeMap& map)
{
    if (type->is_param()) {
        return includes_type(params, type) && map.count(type) == 0;
    }
    for (const Type* element : type->elements) {
        if (mentions(element, params, map)) {
            return true;
        }
    }
    if (type->is_user()) {
        for (const Type* arg : type->user->arguments()) {
            if (mentions(arg, params, map)) {
                return true;
            }
        }
    }
    return false;
}

// The types of 'params' in a call, from the arguments and the expected
// result: arguments with a type of their own go first, then literals and
// 'null', which take the type the parameter has by then.
bool Sema::infer_call_types(CallExpr& call, const FunctionSymbol& function,
                            const std::vector<const Type*>& params, TypeMap& map,
                            const Type* expected, const std::string& what)
{
    std::vector<std::size_t> deferred;
    for (std::size_t i = 0; i < call.args.size() && i < function.param_types.size(); ++i) {
        Expr& arg = *call.args[i];
        Expr& value = as_inout_mark(arg) != nullptr ? *static_cast<UnaryExpr&>(arg).operand : arg;
        if (takes_type_from_parameter(value)) {
            deferred.push_back(i);
            continue;
        }
        const Type* param = function.param_types[i];
        const Type* type = check_expr(value, mentions(param, params, map) ? nullptr : param);
        unify_params(param, type, params, map);
    }
    if (expected != nullptr) {
        unify_params(function.return_type, expected, params, map);
    }
    for (std::size_t i : deferred) {
        Expr& arg = *call.args[i];
        Expr& value = as_inout_mark(arg) != nullptr ? *static_cast<UnaryExpr&>(arg).operand : arg;
        const Type* param = function.param_types[i];
        const Type* hint = mentions(param, params, map) ? nullptr : substitute(param, map);
        if (hint == nullptr && needs_expected_type(value)) {
            continue; // nothing to learn; reported below if the type stays unknown
        }
        unify_params(param, check_expr(value, hint), params, map);
    }
    for (const Type* param : params) {
        if (map.count(param) == 0) {
            diagnostics_->error(call.location, "cannot tell what '" + std::string(param->name) +
                                                  "' is in this call to " + what +
                                                  "; give the arguments or the result a type");
            return false;
        }
    }
    std::vector<const Type*> args;
    for (const Type* param : params) {
        args.push_back(map.at(param));
    }
    return check_bounds(params, args, what, call.location);
}

// A view of a generic function with the inferred types put in, for
// checking a call's arguments.
FunctionSymbol Sema::substituted_signature(const FunctionSymbol& function, const TypeMap& map)
{
    FunctionSymbol view = function;
    for (const Type*& param : view.param_types) {
        param = substitute(param, map);
    }
    view.return_type = substitute(function.return_type, map);
    return view;
}

// The instance of a generic function (or method) for concrete type
// arguments: a copy of its declaration checked with those types. Returns
// null while some argument is not concrete, as in a call inside another
// generic body; that body's own instances make the call concrete.
const FunctionSymbol* Sema::instance_function(const FunctionSymbol& generic,
                                              const std::vector<const Type*>& args,
                                              const SourceLocation& location)
{
    if (!std::all_of(args.begin(), args.end(), is_concrete)) {
        return nullptr;
    }
    const auto key = std::make_pair(&generic, args);
    if (auto it = function_instances_.find(key); it != function_instances_.end()) {
        return it->second;
    }
    for (const Type* arg : args) {
        if (type_depth(arg) > kMaxInstanceDepth) {
            diagnostics_->error(location, "'" + generic.name + "' is used with ever larger types; "
                                              "a generic function cannot call itself with a type "
                                              "built from its own type parameter, such as "
                                              "'List<T>' inside it");
            return nullptr;
        }
    }
    const FnDecl& decl = *generic.decl;
    std::unique_ptr<FnDecl> copy = clone_function(decl);
    copy->self_type = decl.self_type;
    copy->type_bindings = decl.type_bindings;
    if (copy->type_bindings.empty() && decl.owner != nullptr && decl.owner->type != nullptr) {
        // A generic method of a type that is not generic.
        copy->self_type = decl.owner->type;
    }
    for (std::size_t i = 0; i < decl.type_params.size() && i < args.size(); ++i) {
        copy->type_bindings.emplace_back(decl.type_params[i].name, args[i]);
    }
    copy->type_params.clear();
    copy->has_body = true;
    FnDecl* instance = copy.get();
    program_->instances.push_back(std::move(copy));
    FunctionSymbol& symbol =
        create_function_symbol(*instance, generic.name + "<" + type_list_text(args) + ">", true);
    symbol.owner = generic.owner;
    function_instances_[key] = &symbol;
    pending_bodies_.push_back(instance);
    return &symbol;
}

// Bodies of instances, and defaults of instances' fields, found while
// checking: each may find more.
void Sema::check_pending_instances()
{
    while (!pending_bodies_.empty() || !pending_defaults_.empty()) {
        if (!pending_defaults_.empty()) {
            const PendingDefault pending = pending_defaults_.front();
            pending_defaults_.pop_front();
            push_type_names(pending.instance->decl->type_params, pending.instance->type_args);
            return_type_ = void_type();
            push_scope();
            Expr& value = const_cast<Expr&>(*pending.field->default_value);
            const Type* type = check_expr(value, pending.field->type);
            pop_scope();
            expect_type(pending.field->type, type, value.location);
            pop_type_names(1);
            continue;
        }
        FnDecl* body = pending_bodies_.front();
        pending_bodies_.pop_front();
        check_function_body(*body);
    }
}

// ---------------------------------------------------------------------------
// Calls and values of generic declarations

// A call to a generic function: its type parameters are inferred, then the
// arguments are checked against the signature with the types put in.
const Type* Sema::check_generic_function_call(CallExpr& call, NameExpr& callee,
                                              const FunctionSymbol& function,
                                              const Type* expected)
{
    TypeMap map;
    if (!infer_call_types(call, function, function.type_params, map, expected,
                          "'" + function.name + "'")) {
        return error_type(); // the arguments were checked while inferring
    }
    std::vector<const Type*> args;
    for (const Type* param : function.type_params) {
        args.push_back(map.at(param));
    }
    if (const FunctionSymbol* instance = instance_function(function, args, call.location)) {
        callee.function = instance;
    }
    const FunctionSymbol view = substituted_signature(function, map);
    check_call_arguments(call, view, std::nullopt);
    return view.return_type;
}

// A method whose signature mentions type parameters still to be inferred:
// its own (a generic method), those of a generic type called as
// 'Pair.new(...)', or 'Self' of a trait method called on a type parameter.
const Type* Sema::check_generic_method_call(CallExpr& call, MemberExpr& callee,
                                            const FunctionSymbol& method,
                                            const std::vector<const Type*>& params, TypeMap map,
                                            const std::optional<Place>& receiver,
                                            const Type* expected)
{
    if (!params.empty() &&
        !infer_call_types(call, method, params, map, expected, "'" + callee.member + "'")) {
        return error_type(); // the arguments were checked while inferring
    }
    callee.method = &method;
    if (method.owner != nullptr && !method.owner->type_params.empty() && callee.is_static_call) {
        // 'Pair.new(...)': the method of the instance for the inferred types.
        std::vector<const Type*> type_args;
        for (const Type* param : method.owner->type_params) {
            type_args.push_back(map.at(param));
        }
        const Type* instance = instantiate(*method.owner, type_args);
        auto found = instance->user->methods.find(callee.member);
        if (found != instance->user->methods.end()) {
            callee.method = found->second;
        }
    }
    if (!callee.method->type_params.empty()) {
        std::vector<const Type*> own;
        for (const Type* param : method.type_params) {
            own.push_back(map.at(param));
        }
        if (const FunctionSymbol* instance = instance_function(*callee.method, own, call.location)) {
            callee.method = instance;
        }
    }
    const FunctionSymbol view = substituted_signature(method, map);
    check_call_arguments(call, view, receiver);
    return view.return_type;
}

// 'Pair { first: 1, second: 2 }' for a generic 'Pair<T>': the instance
// comes from the expected type, or from the field values.
const UserType* Sema::infer_literal_type(StructLiteralExpr& lit, const UserType& generic,
                                         const Type* expected)
{
    if (expected != nullptr && expected->is_option()) {
        expected = expected->option_value();
    }
    if (expected != nullptr && expected->is_user() && &expected->user->origin() == &generic) {
        return expected->user;
    }
    if (expected != nullptr && expected->is_error()) {
        return nullptr; // the written type was wrong, already reported
    }
    const std::vector<FieldInfo>* fields = nullptr;
    if (!generic.is_enum()) {
        fields = &generic.fields;
    } else if (const int index = generic.find_case(lit.case_name); index >= 0) {
        fields = &generic.cases[static_cast<std::size_t>(index)].fields;
    }
    if (fields == nullptr) {
        return &generic; // reported by the caller
    }
    TypeMap map;
    std::vector<const StructLiteralExpr::FieldInit*> deferred;
    for (const auto& field : lit.fields) {
        const int index = find_field(*fields, field.name);
        if (index < 0) {
            continue;
        }
        if (takes_type_from_parameter(*field.value)) {
            deferred.push_back(&field);
            continue;
        }
        const Type* type = (*fields)[static_cast<std::size_t>(index)].type;
        unify_params(type, check_expr(*field.value, mentions(type, generic.type_params, map) ? nullptr : type),
                     generic.type_params, map);
    }
    for (const StructLiteralExpr::FieldInit* field : deferred) {
        const Type* type = (*fields)[static_cast<std::size_t>(find_field(*fields, field->name))].type;
        const Type* hint = mentions(type, generic.type_params, map) ? nullptr : substitute(type, map);
        if (hint == nullptr && needs_expected_type(*field->value)) {
            continue;
        }
        unify_params(type, check_expr(*field->value, hint), generic.type_params, map);
    }
    std::vector<const Type*> args;
    for (const Type* param : generic.type_params) {
        auto it = map.find(param);
        if (it == map.end()) {
            diagnostics_->error(lit.location, "cannot tell what '" + std::string(param->name) +
                                                  "' is in this '" + generic.decl->name +
                                                  "' value; write its type, as in 'let v: " +
                                                  generic.decl->name + "<i32> = ...'");
            return nullptr;
        }
        args.push_back(it->second);
    }
    if (!check_bounds(generic.type_params, args, "'" + generic.decl->name + "'", lit.location)) {
        return nullptr;
    }
    return instantiate(generic, args)->user;
}

} // namespace hana::lang
