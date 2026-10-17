#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/source.h"

namespace hana::lang {

struct Type;
struct VarSymbol;
struct FunctionSymbol;
struct TypeDecl;

// ---------------------------------------------------------------------------
// Types as written in source

// A type as written: a name; a tuple '(i32, f64)' whose 'elements' are its
// element types; or an Option 'Option<i32>' / 'i32?' whose one element is
// the value type. 'name' is the written text.
struct TypeExpr {
    std::string name;
    SourceLocation location;
    std::vector<TypeExpr> elements{}; // tuple, or the value type of an Option or List
    bool is_option = false;
    bool is_list = false;
};

// ---------------------------------------------------------------------------
// Expressions

enum class ParamConvention {
    Read,  // no marker: read-only, never copied
    Inout, // 'inout': modifies the caller's value
    Sink,  // 'sink': ownership moves into the function
};

const char* param_convention_spelling(ParamConvention convention);

enum class ExprKind {
    Error,
    IntLiteral,
    FloatLiteral,
    BoolLiteral,
    CharLiteral,
    StringLiteral,
    Null,
    Name,
    Unary,
    Binary,
    Conditional,
    Cast,
    Call,
    StructLiteral,
    Tuple,
    List,
    Match,
    Member,
    Index,
    Try,
};

enum class UnaryOp {
    Neg,
    Not,
    BitNot,
    InoutMark, // '&x' marks an inout argument
};

enum class BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
};

const char* binary_op_spelling(BinaryOp op);
const char* unary_op_spelling(UnaryOp op);

struct Expr {
    Expr(ExprKind kind_value, SourceLocation location_value)
        : kind(kind_value)
        , location(location_value)
    {
    }
    virtual ~Expr() = default;

    ExprKind kind;
    SourceLocation location;
    // Set by semantic analysis: the type of the value the expression
    // computes, and the Option type it is wrapped into when it stands where
    // an Option is expected ('let a: i32? = 5'), or null.
    const Type* type = nullptr;
    const Type* converted_to = nullptr;
};

// The type of the value an expression finally produces.
inline const Type* result_type(const Expr& expr)
{
    return expr.converted_to != nullptr ? expr.converted_to : expr.type;
}

using ExprPtr = std::unique_ptr<Expr>;

struct ErrorExpr final : Expr {
    explicit ErrorExpr(SourceLocation location_value)
        : Expr(ExprKind::Error, location_value)
    {
    }
};

struct IntLiteralExpr final : Expr {
    explicit IntLiteralExpr(SourceLocation location_value)
        : Expr(ExprKind::IntLiteral, location_value)
    {
    }
    std::string text;   // as written, without suffix
    std::string suffix; // "u8", "i64", ... or empty
    std::uint64_t value = 0;
    bool overflowed = false; // does not fit in 64 bits
};

struct FloatLiteralExpr final : Expr {
    explicit FloatLiteralExpr(SourceLocation location_value)
        : Expr(ExprKind::FloatLiteral, location_value)
    {
    }
    std::string text;
    std::string suffix;
    double value = 0.0;
};

struct BoolLiteralExpr final : Expr {
    BoolLiteralExpr(SourceLocation location_value, bool value_value)
        : Expr(ExprKind::BoolLiteral, location_value)
        , value(value_value)
    {
    }
    bool value;
};

struct CharLiteralExpr final : Expr {
    CharLiteralExpr(SourceLocation location_value, std::uint32_t value_value)
        : Expr(ExprKind::CharLiteral, location_value)
        , value(value_value)
    {
    }
    std::uint32_t value;
};

// "text" has texts = {"text"} and no parts. f"a{x}b" has texts = {"a", "b"}
// and parts = {x}. texts.size() == parts.size() + 1 always holds.
struct StringLiteralExpr final : Expr {
    explicit StringLiteralExpr(SourceLocation location_value)
        : Expr(ExprKind::StringLiteral, location_value)
    {
    }
    bool is_format = false; // written as f"..."
    std::vector<std::string> texts;
    std::vector<ExprPtr> parts;
};

struct NullExpr final : Expr {
    explicit NullExpr(SourceLocation location_value)
        : Expr(ExprKind::Null, location_value)
    {
    }
};

struct NameExpr final : Expr {
    NameExpr(SourceLocation location_value, std::string name_value)
        : Expr(ExprKind::Name, location_value)
        , name(std::move(name_value))
    {
    }
    std::string name;
    // Set by semantic analysis on success.
    const VarSymbol* var = nullptr;
    const FunctionSymbol* function = nullptr;
};

struct UnaryExpr final : Expr {
    UnaryExpr(SourceLocation location_value, UnaryOp op_value, ExprPtr operand_value)
        : Expr(ExprKind::Unary, location_value)
        , op(op_value)
        , operand(std::move(operand_value))
    {
    }
    UnaryOp op;
    ExprPtr operand;
};

struct BinaryExpr final : Expr {
    BinaryExpr(SourceLocation op_location_value, BinaryOp op_value, ExprPtr lhs_value,
               ExprPtr rhs_value)
        : Expr(ExprKind::Binary, lhs_value->location)
        , op(op_value)
        , op_location(op_location_value)
        , lhs(std::move(lhs_value))
        , rhs(std::move(rhs_value))
    {
    }
    BinaryOp op;
    SourceLocation op_location;
    ExprPtr lhs;
    ExprPtr rhs;
};

// 'then_value if condition else else_value'
struct ConditionalExpr final : Expr {
    ConditionalExpr(SourceLocation location_value, ExprPtr then_value_in, ExprPtr condition_in,
                    ExprPtr else_value_in)
        : Expr(ExprKind::Conditional, location_value)
        , then_value(std::move(then_value_in))
        , condition(std::move(condition_in))
        , else_value(std::move(else_value_in))
    {
    }
    ExprPtr then_value;
    ExprPtr condition;
    ExprPtr else_value;
};

// 'operand as Type'
struct CastExpr final : Expr {
    CastExpr(SourceLocation location_value, ExprPtr operand_value, TypeExpr target_value)
        : Expr(ExprKind::Cast, location_value)
        , operand(std::move(operand_value))
        , target(std::move(target_value))
    {
    }
    ExprPtr operand;
    TypeExpr target;
};

struct CallExpr final : Expr {
    CallExpr(SourceLocation location_value, ExprPtr callee_value)
        : Expr(ExprKind::Call, location_value)
        , callee(std::move(callee_value))
    {
    }
    ExprPtr callee;
    std::vector<ExprPtr> args;
};

// 'Point { x: 1.0, y: 2.0 }', or an enum value 'Shape.Circle { radius: 1.0 }'
struct StructLiteralExpr final : Expr {
    StructLiteralExpr(SourceLocation location_value, std::string type_name_value)
        : Expr(ExprKind::StructLiteral, location_value)
        , type_name(std::move(type_name_value))
    {
    }
    struct FieldInit {
        std::string name;
        SourceLocation location;
        ExprPtr value;
    };
    std::string type_name;
    std::string case_name; // empty for a struct value
    SourceLocation case_location;
    std::vector<FieldInit> fields;
    // Set by semantic analysis: for each field in declaration order, the
    // index into 'fields', or -1 when the field's default is used.
    std::vector<int> field_order;
    int case_index = -1; // for an enum value
};

// Methods every value of a type has without declaring them.
enum class BuiltinMethod {
    None,
    StrLen,      // 's.len()': the length in bytes
    Clone,       // 'v.clone()': a copy that owns its own data
    ListLen,     // 'xs.len()'
    ListIsEmpty, // 'xs.is_empty()'
    ListPush,    // 'xs.push(v)'
    ListPop,     // 'xs.pop()': the last element, or null
    ListInsert,  // 'xs.insert(i, v)'
    ListRemove,  // 'xs.remove(i)': the element at i
    ListClear,   // 'xs.clear()'
};

// 'object.member': a field, or the callee of a method call.
// How a built-in method takes its receiver and its arguments.
struct BuiltinSignature {
    ParamConvention self = ParamConvention::Read;
    std::vector<ParamConvention> params;
};

BuiltinSignature builtin_signature(BuiltinMethod method);

struct MemberExpr final : Expr {
    MemberExpr(SourceLocation location_value, ExprPtr object_value, std::string member_value)
        : Expr(ExprKind::Member, location_value)
        , object(std::move(object_value))
        , member(std::move(member_value))
    {
    }
    ExprPtr object;
    std::string member;
    // Set by semantic analysis.
    int field_index = -1;                     // a field access
    int case_index = -1;                      // an enum value without fields: 'Color.Red'
    const FunctionSymbol* method = nullptr;   // the callee of a method call
    bool is_static_call = false;              // 'Type.function(...)', no receiver
    BuiltinMethod builtin = BuiltinMethod::None; // the callee is a built-in method
};

// '[a, b, c]'
struct ListExpr final : Expr {
    explicit ListExpr(SourceLocation location_value)
        : Expr(ExprKind::List, location_value)
    {
    }
    std::vector<ExprPtr> elements;
};

struct IndexExpr final : Expr {
    IndexExpr(SourceLocation location_value, ExprPtr object_value, ExprPtr index_value)
        : Expr(ExprKind::Index, location_value)
        , object(std::move(object_value))
        , index(std::move(index_value))
    {
    }
    ExprPtr object;
    ExprPtr index;
};

// '(a, b)': two or more elements.
struct TupleExpr final : Expr {
    explicit TupleExpr(SourceLocation location_value)
        : Expr(ExprKind::Tuple, location_value)
    {
    }
    std::vector<ExprPtr> elements;
};

struct TryExpr final : Expr {
    TryExpr(SourceLocation location_value, ExprPtr operand_value)
        : Expr(ExprKind::Try, location_value)
        , operand(std::move(operand_value))
    {
    }
    ExprPtr operand;
};

// ---------------------------------------------------------------------------
// Patterns of 'match' and of 'let (a, b) = ...'

enum class PatternKind {
    Wildcard, // '_'
    Binding,  // a name: matches anything and binds it
    Literal,  // 0, -1, 'a', true
    Range,    // 1..=9 or 1..10
    Case,     // an enum case: Circle(r), Empty
    Tuple,    // (a, _, 0): one pattern per element, in 'arguments'
    Null,     // null: an Option without a value
};

struct Pattern;
using PatternPtr = std::unique_ptr<Pattern>;

struct Pattern {
    PatternKind kind = PatternKind::Wildcard;
    SourceLocation location;
    std::string name;          // Binding and Case
    ExprPtr value;             // Literal, and the start of a Range
    ExprPtr end;               // the end of a Range
    bool inclusive = false;    // '..=' rather than '..'
    bool has_arguments = false; // Case written with '(...)'
    std::vector<PatternPtr> arguments; // Case: one per field; Tuple: one per element
    // Set by semantic analysis.
    const VarSymbol* symbol = nullptr; // Binding
    int case_index = -1;               // Case
};

// ---------------------------------------------------------------------------
// Statements

enum class StmtKind {
    Let,
    Assign,
    Expr,
    Return,
    If,
    While,
    For,
    Match,
    Break,
    Continue,
    Pass,
};

struct Stmt {
    Stmt(StmtKind kind_value, SourceLocation location_value)
        : kind(kind_value)
        , location(location_value)
    {
    }
    virtual ~Stmt() = default;

    StmtKind kind;
    SourceLocation location;
};

using StmtPtr = std::unique_ptr<Stmt>;

struct Block {
    SourceLocation location;
    // Where control falls off the end; used for "missing return" errors.
    SourceLocation end_location;
    std::vector<StmtPtr> statements;
};

struct LetStmt final : Stmt {
    explicit LetStmt(SourceLocation location_value)
        : Stmt(StmtKind::Let, location_value)
    {
    }
    bool is_mutable = false; // 'var' instead of 'let'
    std::string name;        // empty when 'pattern' is set
    SourceLocation name_location;
    PatternPtr pattern;      // 'let (q, r) = ...'
    std::optional<TypeExpr> declared_type;
    ExprPtr init;
    const VarSymbol* symbol = nullptr;
};

struct AssignStmt final : Stmt {
    explicit AssignStmt(SourceLocation location_value)
        : Stmt(StmtKind::Assign, location_value)
    {
    }
    std::optional<BinaryOp> op; // set for compound assignment such as '+='
    SourceLocation op_location;
    ExprPtr target;
    ExprPtr value;
};

struct ExprStmt final : Stmt {
    explicit ExprStmt(ExprPtr expr_value)
        : Stmt(StmtKind::Expr, expr_value->location)
        , expr(std::move(expr_value))
    {
    }
    ExprPtr expr;
};

struct ReturnStmt final : Stmt {
    explicit ReturnStmt(SourceLocation location_value)
        : Stmt(StmtKind::Return, location_value)
    {
    }
    ExprPtr value; // null for a bare 'return'
};

// 'elif' is stored as an IfStmt inside else_block.
struct IfStmt final : Stmt {
    explicit IfStmt(SourceLocation location_value)
        : Stmt(StmtKind::If, location_value)
    {
    }
    ExprPtr condition;
    Block then_block;
    std::optional<Block> else_block;
};

struct WhileStmt final : Stmt {
    explicit WhileStmt(SourceLocation location_value)
        : Stmt(StmtKind::While, location_value)
    {
    }
    ExprPtr condition;
    Block body;
};

struct MatchArm {
    PatternPtr pattern;
    ExprPtr guard;             // 'n if n > 100', or null
    Block body;                // statement form
    ExprPtr value;             // expression form
    SourceLocation location;
};

// 'match subject:' followed by indented arms. As a statement each arm has
// a block; as an expression each arm has a value.
struct MatchExpr final : Expr {
    MatchExpr(SourceLocation location_value)
        : Expr(ExprKind::Match, location_value)
    {
    }
    ExprPtr subject;
    std::vector<MatchArm> arms;
};

struct MatchStmt final : Stmt {
    explicit MatchStmt(std::unique_ptr<MatchExpr> match_value)
        : Stmt(StmtKind::Match, match_value->location)
        , match(std::move(match_value))
    {
    }
    std::unique_ptr<MatchExpr> match;
};

// 'for name in start..end:' or 'start..=end', or 'for name in list:'
// with the element passed like an argument: 'for inout x in xs',
// 'for sink x in xs'. The bounds or the list are computed once before the
// loop.
struct ForStmt final : Stmt {
    explicit ForStmt(SourceLocation location_value)
        : Stmt(StmtKind::For, location_value)
    {
    }
    std::string name;
    SourceLocation name_location;
    ExprPtr start;
    ExprPtr end;
    bool inclusive = false; // '..=' rather than '..'
    ExprPtr iterable;       // a list; null for a range
    ParamConvention convention = ParamConvention::Read;
    SourceLocation convention_location;
    Block body;
    const VarSymbol* symbol = nullptr;
};

struct SimpleStmt final : Stmt {
    SimpleStmt(StmtKind kind_value, SourceLocation location_value)
        : Stmt(kind_value, location_value)
    {
    }
};

// ---------------------------------------------------------------------------
// Declarations

struct Param {
    ParamConvention convention = ParamConvention::Read;
    std::string name;
    SourceLocation location;
    TypeExpr type;
    const VarSymbol* symbol = nullptr;
};

struct FnDecl {
    std::string name;
    SourceLocation location;
    bool is_pub = false;
    // Methods: the struct that owns the function and, when it takes 'self',
    // how 'self' is passed. 'self' is not part of 'params'.
    TypeDecl* owner = nullptr;
    std::optional<ParamConvention> self_convention;
    SourceLocation self_location;
    std::vector<Param> params;
    std::optional<TypeExpr> return_type;
    Block body;
    const FunctionSymbol* symbol = nullptr;
    const VarSymbol* self_symbol = nullptr;
};

struct FieldDecl {
    std::string name;
    SourceLocation location;
    TypeExpr type;
    ExprPtr default_value; // 'hp: i32 = 100', or null
};

struct CaseDecl {
    std::string name;
    SourceLocation location;
    std::vector<FieldDecl> fields;
};

// 'Point: struct { x: f32, y: f32, length: fn<f32(self)> ... }' or
// 'Shape: enum { Circle: { radius: f32 }, Empty: {}, area: fn... }'
struct TypeDecl {
    std::string name;
    SourceLocation location;
    bool is_pub = false;
    bool is_enum = false;
    bool is_copy = false; // 'struct copy { ... }', 'enum copy { ... }'
    std::vector<FieldDecl> fields; // struct
    std::vector<CaseDecl> cases;   // enum
    std::vector<std::unique_ptr<FnDecl>> methods;
    const Type* type = nullptr;
};

struct Module {
    std::vector<std::unique_ptr<TypeDecl>> types; // structs and enums
    std::vector<std::unique_ptr<FnDecl>> functions;
};

// 'Option<T>' for the written value type T ('T?' and 'Option<T>' alike).
TypeExpr option_type_expr(TypeExpr value);
// 'List<T>' for the element type T.
TypeExpr list_type_expr(TypeExpr element);
// 'Option<T>' or 'List<T>' for a generic type name; false for other names.
bool make_generic_type_expr(const std::string& name, TypeExpr argument, TypeExpr& out);

// "(i32, f64)" for the element types of a tuple.
std::string tuple_type_text(const std::vector<std::string>& element_names);

// Indented tree, one node per line.
std::string dump_module(const Module& module);

} // namespace hana::lang
