#include "lang/ast.h"

#include "lang/symbols.h"
#include "lang/types.h"

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

std::string tuple_type_text(const std::vector<std::string>& element_names)
{
    std::string text = "(";
    for (std::size_t i = 0; i < element_names.size(); ++i) {
        text += (i == 0 ? "" : ", ") + element_names[i];
    }
    return text + ")";
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
        for (const auto& decl : module.types) {
            dump_type_decl(*decl);
        }
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

    void dump_type_decl(const TypeDecl& decl)
    {
        std::string header = std::string(decl.is_pub ? "pub " : "") +
                             (decl.is_enum ? "enum " : "struct ") +
                             (decl.is_copy ? "copy " : "") + decl.name;
        line(header);
        ++depth_;
        dump_fields(decl.fields);
        for (const CaseDecl& entry : decl.cases) {
            line("case " + entry.name);
            ++depth_;
            dump_fields(entry.fields);
            --depth_;
        }
        for (const auto& method : decl.methods) {
            dump_function(*method);
        }
        --depth_;
    }

    void dump_fields(const std::vector<FieldDecl>& fields)
    {
        for (const FieldDecl& field : fields) {
            line("field " + field.name + ": " + field.type.name);
            if (field.default_value) {
                child(*field.default_value);
            }
        }
    }

    void dump_function(const FnDecl& function)
    {
        std::string header = std::string(function.is_pub ? "pub " : "") + "fn " + function.name;
        if (function.return_type) {
            header += " -> " + function.return_type->name;
        }
        line(header);
        ++depth_;
        if (function.self_convention) {
            line(std::string("self ") + param_convention_spelling(*function.self_convention));
        }
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
        case StmtKind::For: {
            const auto& loop = static_cast<const ForStmt&>(stmt);
            std::string header = "for " + loop.name;
            if (loop.symbol != nullptr && loop.symbol->type != nullptr) {
                header += ": " + std::string(loop.symbol->type->name);
            }
            line(header + (loop.inclusive ? " in ..=" : " in .."));
            child(*loop.start);
            child(*loop.end);
            nested(loop.body);
            break;
        }
        case StmtKind::Match: dump_match(*static_cast<const MatchStmt&>(stmt).match); break;
        case StmtKind::Break: line("break"); break;
        case StmtKind::Continue: line("continue"); break;
        case StmtKind::Pass: line("pass"); break;
        }
    }

    void dump_let(const LetStmt& let)
    {
        std::string header = std::string(let.is_mutable ? "var " : "let ") +
                             (let.pattern ? "(destructure)" : let.name);
        if (let.declared_type) {
            header += ": " + let.declared_type->name;
        } else if (let.symbol != nullptr && let.symbol->type != nullptr) {
            header += ": " + std::string(let.symbol->type->name) + " (inferred)";
        }
        line(header);
        if (let.pattern) {
            ++depth_;
            dump_pattern(*let.pattern);
            --depth_;
        }
        child(*let.init);
    }

    void dump_assign(const AssignStmt& assign)
    {
        line(std::string("assign ") + (assign.op ? binary_op_spelling(*assign.op) : "") + "=");
        child(*assign.target);
        child(*assign.value);
    }

    void dump_match(const MatchExpr& match)
    {
        line("match");
        child(*match.subject);
        ++depth_;
        for (const MatchArm& arm : match.arms) {
            line("arm");
            ++depth_;
            dump_pattern(*arm.pattern);
            if (arm.guard) {
                line("guard");
                child(*arm.guard);
            }
            if (arm.value) {
                dump_expr(*arm.value);
            } else {
                dump_block(arm.body);
            }
            --depth_;
        }
        --depth_;
    }

    void dump_pattern(const Pattern& pattern)
    {
        switch (pattern.kind) {
        case PatternKind::Wildcard: line("pattern _"); break;
        case PatternKind::Binding: line("pattern name " + pattern.name); break;
        case PatternKind::Literal:
            line("pattern literal");
            child(*pattern.value);
            break;
        case PatternKind::Range:
            line(pattern.inclusive ? "pattern range ..=" : "pattern range ..");
            child(*pattern.value);
            child(*pattern.end);
            break;
        case PatternKind::Tuple:
            line("pattern tuple");
            ++depth_;
            for (const auto& argument : pattern.arguments) {
                dump_pattern(*argument);
            }
            --depth_;
            break;
        case PatternKind::Case:
            line("pattern case " + pattern.name);
            ++depth_;
            for (const auto& argument : pattern.arguments) {
                dump_pattern(*argument);
            }
            --depth_;
            break;
        }
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
        case ExprKind::StructLiteral: {
            const auto& lit = static_cast<const StructLiteralExpr&>(expr);
            line("struct literal " + lit.type_name +
                 (lit.case_name.empty() ? "" : "." + lit.case_name));
            ++depth_;
            for (const auto& field : lit.fields) {
                line("field " + field.name);
                child(*field.value);
            }
            --depth_;
            break;
        }
        case ExprKind::Match: dump_match(static_cast<const MatchExpr&>(expr)); break;
        case ExprKind::Tuple:
            line("tuple");
            for (const auto& element : static_cast<const TupleExpr&>(expr).elements) {
                child(*element);
            }
            break;
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
