// Semantic analysis of List<T>: the type, '[a, b]' values, 'xs[i]'
// elements, the built-in methods and 'for x in xs' loops.

#include <string>
#include <vector>

#include "lang/sema.h"
#include "lang/sema_util.h"

namespace hana::lang {

using sema_util::as_inout_mark;
using sema_util::is_untyped_literal;
using sema_util::quoted;

namespace {

const Type* usize_type()
{
    return find_builtin_type("usize");
}

} // namespace

// ---------------------------------------------------------------------------
// Types

// List types are created once per element type, so they compare by pointer.
const Type* Sema::list_type(const Type* element)
{
    if (auto it = list_types_.find(element); it != list_types_.end()) {
        return it->second;
    }
    ComposedType& list = program_->composed_types.emplace_back();
    list.name = "List<" + std::string(element->name) + ">";
    list.type.kind = TypeKind::List;
    list.type.name = list.name;
    list.type.elements = {element};
    list_types_[element] = &list.type;
    return &list.type;
}

// ---------------------------------------------------------------------------
// Values

// The element type comes from the expected list type, else from the first
// element that is not an untyped literal, so '[1, x]' takes the type of x.
// An empty list needs an expected type.
const Type* Sema::check_list(ListExpr& list, const Type* expected)
{
    const Type* element_type = nullptr;
    if (expected != nullptr && expected->is_list()) {
        element_type = expected->list_element();
    }
    if (element_type == nullptr && list.elements.empty()) {
        diagnostics_->error(list.location, "the element type of an empty list is not known; "
                                           "write the type, as in 'let xs: List<i32> = []'");
        return error_type();
    }
    if (element_type == nullptr) {
        Expr* first = list.elements.front().get();
        for (auto& element : list.elements) {
            if (!is_untyped_literal(*element)) {
                first = element.get();
                break;
            }
        }
        element_type = check_expr(*first, nullptr);
    }
    if (element_type->is_void()) {
        diagnostics_->error(list.location, "the elements of this list have no value");
        return error_type();
    }
    bool failed = element_type->is_error();
    for (auto& element : list.elements) {
        const Type* type = check_expr(*element, element_type);
        if (!failed && !type->is_error()) {
            expect_type(element_type, type, element->location);
        }
        failed = failed || type->is_error();
    }
    return failed ? error_type() : list_type(element_type);
}

// 'xs[i]' with a 'usize' index. The element is a place when the list is.
const Type* Sema::check_index(IndexExpr& index)
{
    const Type* object = check_expr(*index.object, nullptr);
    const Type* position = check_expr(*index.index, usize_type());
    if (!position->is_error() && position != usize_type()) {
        diagnostics_->error(index.index->location,
                           "a list index has type 'usize', found " + quoted(position) +
                               "; convert it with 'as usize'");
    }
    if (object->is_error()) {
        return object;
    }
    if (!object->is_list()) {
        diagnostics_->error(index.location, "a value of type " + quoted(object) +
                                               " cannot be indexed; only lists can");
        return error_type();
    }
    return object->list_element();
}

// ---------------------------------------------------------------------------
// Built-in methods

BuiltinMethod Sema::builtin_method(const Type* receiver, const std::string& name)
{
    if (receiver->is_error() ||
        (receiver->is_user() && receiver->user->methods.count(name) != 0)) {
        return BuiltinMethod::None;
    }
    if (name == "len" && receiver->kind == TypeKind::Str) {
        return BuiltinMethod::StrLen;
    }
    if (name == "clone" && is_clonable(receiver)) {
        return BuiltinMethod::Clone;
    }
    if (!receiver->is_list()) {
        return BuiltinMethod::None;
    }
    if (name == "len") {
        return BuiltinMethod::ListLen;
    }
    if (name == "is_empty") {
        return BuiltinMethod::ListIsEmpty;
    }
    if (name == "push") {
        return BuiltinMethod::ListPush;
    }
    if (name == "pop") {
        return BuiltinMethod::ListPop;
    }
    if (name == "insert") {
        return BuiltinMethod::ListInsert;
    }
    if (name == "remove") {
        return BuiltinMethod::ListRemove;
    }
    if (name == "clear") {
        return BuiltinMethod::ListClear;
    }
    return BuiltinMethod::None;
}

// The parameter types of a built-in method; an element is T of List<T>.
std::vector<const Type*> Sema::builtin_param_types(BuiltinMethod builtin, const Type* receiver)
{
    switch (builtin) {
    case BuiltinMethod::ListPush: return {receiver->list_element()};
    case BuiltinMethod::ListInsert: return {usize_type(), receiver->list_element()};
    case BuiltinMethod::ListRemove: return {usize_type()};
    default: return {};
    }
}

const Type* Sema::builtin_result_type(BuiltinMethod builtin, const Type* receiver)
{
    switch (builtin) {
    case BuiltinMethod::StrLen:
    case BuiltinMethod::ListLen: return usize_type();
    case BuiltinMethod::ListIsEmpty: return bool_type();
    case BuiltinMethod::Clone: return receiver;
    case BuiltinMethod::ListPop: return option_type(receiver->list_element());
    case BuiltinMethod::ListRemove: return receiver->list_element();
    case BuiltinMethod::ListPush:
    case BuiltinMethod::ListInsert:
    case BuiltinMethod::ListClear:
    case BuiltinMethod::None: return void_type();
    }
    return void_type();
}

// A built-in method takes its receiver and arguments like a function does:
// 'push' modifies the list, so it needs a mutable variable or field.
const Type* Sema::check_builtin_method_call(CallExpr& call, MemberExpr& callee,
                                            BuiltinMethod builtin, const Type* receiver)
{
    callee.type = void_type();
    callee.builtin = builtin;
    const BuiltinSignature signature = builtin_signature(builtin);
    if (signature.self == ParamConvention::Inout) {
        if (std::optional<Place> place = place_of(*callee.object)) {
            require_mutable(*place, MutationKind::InoutReceiver, callee.member);
        } else {
            diagnostics_->error(callee.object->location,
                               "'" + callee.member + "' modifies the list, so it must be called "
                                                    "on a variable or a field of one");
        }
    }
    const std::vector<const Type*> params = builtin_param_types(builtin, receiver);
    if (call.args.size() != params.size()) {
        diagnostics_->error(call.location, "'" + callee.member + "' expects " +
                                              std::to_string(params.size()) +
                                              " argument(s), found " +
                                              std::to_string(call.args.size()));
        check_args_without_callee(call);
        return builtin_result_type(builtin, receiver);
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        Expr& arg = *call.args[i];
        if (as_inout_mark(arg) != nullptr) {
            check_args_without_callee(call);
            diagnostics_->error(arg.location, "'&' marks an inout argument, but argument " +
                                                 std::to_string(i + 1) + " of '" + callee.member +
                                                 "' is not inout");
            return builtin_result_type(builtin, receiver);
        }
        const Type* type = check_expr(arg, params[i]);
        expect_type(params[i], type, arg.location);
    }
    return builtin_result_type(builtin, receiver);
}

// ---------------------------------------------------------------------------
// 'for x in xs'

// The element is passed to the body like an argument: read, 'inout' (the
// list must be mutable) or 'sink' (the list moves into the loop).
bool Sema::check_list_for(ForStmt& stmt)
{
    const Type* list = check_expr(*stmt.iterable, nullptr);
    const Type* element = error_type();
    if (!list->is_error() && !list->is_list()) {
        diagnostics_->error(stmt.iterable->location,
                           "'for' loops over a list or a range 'a..b', but the value has type " +
                               quoted(list));
    } else if (list->is_list()) {
        element = list->list_element();
    }
    if (stmt.convention == ParamConvention::Inout && !list->is_error()) {
        if (std::optional<Place> place = place_of(*stmt.iterable)) {
            require_mutable(*place, MutationKind::InoutLoop, {});
        } else {
            diagnostics_->error(stmt.iterable->location,
                               "'for inout' modifies the elements, so it needs a variable or a "
                               "field of one");
        }
    }
    loops_.push_back(LoopContext{});
    push_scope();
    VarSymbol* symbol = nullptr;
    if (stmt.convention == ParamConvention::Sink) {
        symbol = declare_var(stmt.name, element, VarKind::Local, false, stmt.name_location);
        symbol->is_loop_variable = true;
    } else {
        symbol = declare_var(stmt.name, element, VarKind::Element,
                             stmt.convention == ParamConvention::Inout, stmt.name_location);
    }
    stmt.symbol = symbol;
    check_block(stmt.body);
    pop_scope();
    loops_.pop_back();
    return false;
}

} // namespace hana::lang
