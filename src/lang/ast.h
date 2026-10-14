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

// ---------------------------------------------------------------------------
// Types as written in source

struct TypeExpr {
    std::string name;
    SourceLocation location;
};

// ---------------------------------------------------------------------------
// Expressions

enum class ExprKind {
    Error,
    IntLiteral,
    FloatLiteral,
    BoolLiteral,
    CharLiteral,
    StringLiteral,
    Name,
    Unary,
    Binary,
    Call,
    Member,
    Index,
    Try,
};

enum class UnaryOp {
    Neg,
    Not,
    BitNot,
    InoutMark, // '&x' marks an inout argument
    Deref,     // '*p'
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
    // Set by semantic analysis.
    const Type* type = nullptr;
};

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
    std::string suffix; // "u8", "f32", ... or empty
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

// "a{x}b" is stored as texts = {"a", "b"}, parts = {x}.
// texts.size() == parts.size() + 1 always holds.
struct StringLiteralExpr final : Expr {
    explicit StringLiteralExpr(SourceLocation location_value)
        : Expr(ExprKind::StringLiteral, location_value)
    {
    }
    std::vector<std::string> texts;
    std::vector<ExprPtr> parts;
};

struct NameExpr final : Expr {
    NameExpr(SourceLocation location_value, std::string name_value)
        : Expr(ExprKind::Name, location_value)
        , name(std::move(name_value))
    {
    }
    std::string name;
    // Exactly one of these is set by semantic analysis on success.
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

struct CallExpr final : Expr {
    CallExpr(SourceLocation location_value, ExprPtr callee_value)
        : Expr(ExprKind::Call, location_value)
        , callee(std::move(callee_value))
    {
    }
    ExprPtr callee;
    std::vector<ExprPtr> args;
};

struct MemberExpr final : Expr {
    MemberExpr(SourceLocation location_value, ExprPtr object_value, std::string member_value)
        : Expr(ExprKind::Member, location_value)
        , object(std::move(object_value))
        , member(std::move(member_value))
    {
    }
    ExprPtr object;
    std::string member;
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

struct TryExpr final : Expr {
    TryExpr(SourceLocation location_value, ExprPtr operand_value)
        : Expr(ExprKind::Try, location_value)
        , operand(std::move(operand_value))
    {
    }
    ExprPtr operand;
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
    Loop,
    For,
    Break,
    Continue,
    Block,
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
    SourceLocation end_location; // position of the closing '}'
    std::vector<StmtPtr> statements;
};

struct LetStmt final : Stmt {
    explicit LetStmt(SourceLocation location_value)
        : Stmt(StmtKind::Let, location_value)
    {
    }
    bool is_mutable = false; // 'var' instead of 'let'
    std::string name;
    SourceLocation name_location;
    std::optional<TypeExpr> declared_type;
    ExprPtr init;
    const VarSymbol* symbol = nullptr;
};

struct AssignStmt final : Stmt {
    explicit AssignStmt(SourceLocation location_value)
        : Stmt(StmtKind::Assign, location_value)
    {
    }
    // Set for compound assignment such as '+='.
    std::optional<BinaryOp> op;
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

struct IfStmt final : Stmt {
    explicit IfStmt(SourceLocation location_value)
        : Stmt(StmtKind::If, location_value)
    {
    }
    ExprPtr condition;
    Block then_block;
    // Null, an IfStmt ('else if') or a BlockStmt ('else').
    StmtPtr else_branch;
};

struct WhileStmt final : Stmt {
    explicit WhileStmt(SourceLocation location_value)
        : Stmt(StmtKind::While, location_value)
    {
    }
    ExprPtr condition;
    Block body;
};

struct LoopStmt final : Stmt {
    explicit LoopStmt(SourceLocation location_value)
        : Stmt(StmtKind::Loop, location_value)
    {
    }
    Block body;
};

struct ForStmt final : Stmt {
    explicit ForStmt(SourceLocation location_value)
        : Stmt(StmtKind::For, location_value)
    {
    }
    std::string name;
    ExprPtr iterable;
    Block body;
};

struct BreakStmt final : Stmt {
    explicit BreakStmt(SourceLocation location_value)
        : Stmt(StmtKind::Break, location_value)
    {
    }
};

struct ContinueStmt final : Stmt {
    explicit ContinueStmt(SourceLocation location_value)
        : Stmt(StmtKind::Continue, location_value)
    {
    }
};

struct BlockStmt final : Stmt {
    explicit BlockStmt(SourceLocation location_value)
        : Stmt(StmtKind::Block, location_value)
    {
    }
    Block block;
};

// ---------------------------------------------------------------------------
// Declarations

enum class ParamConvention {
    Read,  // no marker: read-only, never copied
    Inout, // 'inout': modifies the caller's value
    Sink,  // 'sink': ownership moves into the function
};

const char* param_convention_spelling(ParamConvention convention);

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
    std::vector<Param> params;
    std::optional<TypeExpr> return_type;
    Block body;
    const FunctionSymbol* symbol = nullptr;
};

struct Module {
    std::vector<std::unique_ptr<FnDecl>> functions;
};

// Indented tree, one node per line.
std::string dump_module(const Module& module);

} // namespace hana::lang
