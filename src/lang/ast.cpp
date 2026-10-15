#include "lang/ast.h"

#include <string>
#include <utility>

namespace hana::lang {

const char* binary_op_spelling(BinaryOp op)
{
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Rem: return "%";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";
    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Ge: return ">=";
    case BinaryOp::And: return "and";
    case BinaryOp::Or: return "or";
    }
    return "?";
}

const char* unary_op_spelling(UnaryOp op)
{
    switch (op) {
    case UnaryOp::Neg: return "-";
    case UnaryOp::Not: return "not";
    case UnaryOp::BitNot: return "~";
    case UnaryOp::InoutMark: return "&";
    }
    return "?";
}

const char* param_convention_spelling(ParamConvention convention)
{
    switch (convention) {
    case ParamConvention::Read: return "read";
    case ParamConvention::Inout: return "inout";
    case ParamConvention::Sink: return "sink";
    }
    return "?";
}

namespace {

std::string escape(const std::string& text)
{
    std::string out;
    for (char c : text) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default: out += c; break;
        }
    }
    return out;
}

class AstDumper {
public:
    std::string dump(const Module& module)
    {
        for (const auto& function : module.functions) {
            dump_function(*function);
        }
        return std::move(out_);
    }

private:
    void line(const std::string& text)
    {
        out_.append(static_cast<std::size_t>(depth_) * 2, ' ');
        out_ += text;
        out_ += '\n';
    }

    void nested(const Block& block)
    {
        ++depth_;
        dump_block(block);
        --depth_;
    }

    void child(const Expr& expr)
    {
        ++depth_;
        dump_expr(expr);
        --depth_;
    }

    void dump_function(const FnDecl& function)
    {
        std::string header = std::string(function.is_pub ? "pub " : "") + "fn " + function.name;
        if (function.return_type) {
            header += " -> " + function.return_type->name;
        }
        line(header);
        ++depth_;
        for (const Param& param : function.params) {
            line(std::string("param ") + param_convention_spelling(param.convention) + " " +
                 param.name + ": " + param.type.name);
        }
        dump_block(function.body);
        --depth_;
    }

    void dump_block(const Block& block)
    {
        line("block");
        ++depth_;
        for (const auto& statement : block.statements) {
            dump_stmt(*statement);
        }
        --depth_;
    }

    void dump_stmt(const Stmt& stmt)
    {
        switch (stmt.kind) {
        case StmtKind::Let: dump_let(static_cast<const LetStmt&>(stmt)); break;
        case StmtKind::Assign: dump_assign(static_cast<const AssignStmt&>(stmt)); break;
        case StmtKind::Expr:
            line("expr");
            child(*static_cast<const ExprStmt&>(stmt).expr);
            break;
        case StmtKind::Return: {
            const auto& ret = static_cast<const ReturnStmt&>(stmt);
            line("return");
            if (ret.value) {
                child(*ret.value);
            }
            break;
        }
        case StmtKind::If: dump_if(static_cast<const IfStmt&>(stmt)); break;
        case StmtKind::While: {
            const auto& loop = static_cast<const WhileStmt&>(stmt);
            line("while");
            child(*loop.condition);
            nested(loop.body);
            break;
        }
        case StmtKind::Break: line("break"); break;
        case StmtKind::Continue: line("continue"); break;
        case StmtKind::Pass: line("pass"); break;
        }
    }

    void dump_let(const LetStmt& let)
    {
        std::string header = std::string(let.is_mutable ? "var " : "let ") + let.name;
        if (let.declared_type) {
            header += ": " + let.declared_type->name;
        }
        line(header);
        child(*let.init);
    }

    void dump_assign(const AssignStmt& assign)
    {
        line(std::string("assign ") + (assign.op ? binary_op_spelling(*assign.op) : "") + "=");
        child(*assign.target);
        child(*assign.value);
    }

    void dump_if(const IfStmt& stmt)
    {
        line("if");
        child(*stmt.condition);
        nested(stmt.then_block);
        if (stmt.else_block) {
            line("else");
            nested(*stmt.else_block);
        }
    }

    void dump_expr(const Expr& expr)
    {
        switch (expr.kind) {
        case ExprKind::Error: line("<error>"); break;
        case ExprKind::IntLiteral: {
            const auto& lit = static_cast<const IntLiteralExpr&>(expr);
            line("int " + lit.text + lit.suffix);
            break;
        }
        case ExprKind::FloatLiteral: {
            const auto& lit = static_cast<const FloatLiteralExpr&>(expr);
            line("float " + lit.text + lit.suffix);
            break;
        }
        case ExprKind::BoolLiteral:
            line(static_cast<const BoolLiteralExpr&>(expr).value ? "bool true" : "bool false");
            break;
        case ExprKind::CharLiteral:
            line("char U+" + std::to_string(static_cast<const CharLiteralExpr&>(expr).value));
            break;
        case ExprKind::StringLiteral: dump_string(static_cast<const StringLiteralExpr&>(expr)); break;
        case ExprKind::Null: line("null"); break;
        case ExprKind::Name: line("name " + static_cast<const NameExpr&>(expr).name); break;
        case ExprKind::Unary: {
            const auto& unary = static_cast<const UnaryExpr&>(expr);
            line(std::string("unary ") + unary_op_spelling(unary.op));
            child(*unary.operand);
            break;
        }
        case ExprKind::Binary: {
            const auto& binary = static_cast<const BinaryExpr&>(expr);
            line(std::string("binary ") + binary_op_spelling(binary.op));
            child(*binary.lhs);
            child(*binary.rhs);
            break;
        }
        case ExprKind::Conditional: {
            const auto& cond = static_cast<const ConditionalExpr&>(expr);
            line("conditional");
            child(*cond.then_value);
            child(*cond.condition);
            child(*cond.else_value);
            break;
        }
        case ExprKind::Cast: {
            const auto& cast = static_cast<const CastExpr&>(expr);
            line("cast as " + cast.target.name);
            child(*cast.operand);
            break;
        }
        case ExprKind::Call: {
            const auto& call = static_cast<const CallExpr&>(expr);
            line("call");
            child(*call.callee);
            for (const auto& arg : call.args) {
                child(*arg);
            }
            break;
        }
        case ExprKind::Member: {
            const auto& member = static_cast<const MemberExpr&>(expr);
            line("member ." + member.member);
            child(*member.object);
            break;
        }
        case ExprKind::Index: {
            const auto& index = static_cast<const IndexExpr&>(expr);
            line("index");
            child(*index.object);
            child(*index.index);
            break;
        }
        case ExprKind::Try:
            line("try ?");
            child(*static_cast<const TryExpr&>(expr).operand);
            break;
        }
    }

    void dump_string(const StringLiteralExpr& lit)
    {
        if (!lit.is_format) {
            line("string \"" + escape(lit.texts.front()) + "\"");
            return;
        }
        line("format string");
        ++depth_;
        for (std::size_t i = 0; i < lit.parts.size(); ++i) {
            line("text \"" + escape(lit.texts[i]) + "\"");
            dump_expr(*lit.parts[i]);
        }
        line("text \"" + escape(lit.texts.back()) + "\"");
        --depth_;
    }

    std::string out_;
    int depth_ = 0;
};

} // namespace

std::string dump_module(const Module& module)
{
    AstDumper dumper;
    return dumper.dump(module);
}

} // namespace hana::lang
