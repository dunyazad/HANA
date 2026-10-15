#include "helpers.h"
#include "test.h"

using namespace hana::test;
using namespace hana::lang;

namespace {

std::unique_ptr<Compiled> parse_main(const std::string& body)
{
    return parse("{\n  main: fn '''\n" + body + "  ''',\n}\n");
}

const Block& main_body(const Compiled& r)
{
    return r.module->functions.front()->body;
}

const Expr& first_let_init(const Compiled& r)
{
    return *static_cast<const LetStmt&>(*main_body(r).statements.front()).init;
}

} // namespace

TEST(code_parser_if_elif_else)
{
    auto r = parse_main("    if a:\n      pass\n    elif b: pass\n    else:\n      pass\n");
    EXPECT(!r->diagnostics.has_errors());
    const auto& stmt = static_cast<const IfStmt&>(*main_body(*r).statements.front());
    EXPECT(stmt.else_block.has_value());
    EXPECT(stmt.else_block->statements.front()->kind == StmtKind::If);
    const auto& inner = static_cast<const IfStmt&>(*stmt.else_block->statements.front());
    EXPECT(inner.else_block.has_value());
}

TEST(code_parser_python_precedence)
{
    // not a == b or c and d  parses as  (not (a == b)) or (c and d)
    auto r = parse_main("    let x = not a == b or c and d\n");
    EXPECT(!r->diagnostics.has_errors());
    const auto& top = static_cast<const BinaryExpr&>(first_let_init(*r));
    EXPECT(top.op == BinaryOp::Or);
    const auto& lhs = static_cast<const UnaryExpr&>(*top.lhs);
    EXPECT(lhs.op == UnaryOp::Not && lhs.operand->kind == ExprKind::Binary);
    EXPECT(static_cast<const BinaryExpr&>(*top.rhs).op == BinaryOp::And);
}

TEST(code_parser_bitwise_binds_tighter_than_comparison)
{
    auto r = parse_main("    let x = a & mask == 0\n");
    const auto& eq = static_cast<const BinaryExpr&>(first_let_init(*r));
    EXPECT(eq.op == BinaryOp::Eq);
    EXPECT(static_cast<const BinaryExpr&>(*eq.lhs).op == BinaryOp::BitAnd);
}

TEST(code_parser_conditional_expression)
{
    auto r = parse_main("    let x = a if a > b else b\n");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(first_let_init(*r).kind == ExprKind::Conditional);
}

TEST(code_parser_cast_binds_tighter_than_multiplication)
{
    auto r = parse_main("    let x = -a as f64 * b as f64\n");
    const auto& mul = static_cast<const BinaryExpr&>(first_let_init(*r));
    EXPECT(mul.op == BinaryOp::Mul);
    const auto& lhs = static_cast<const CastExpr&>(*mul.lhs);
    EXPECT(lhs.operand->kind == ExprKind::Unary);
}

TEST(code_parser_chained_comparison)
{
    auto r = parse_main("    let x = a < b < c\n");
    EXPECT(r->has_error("cannot be chained"));
}

TEST(code_parser_format_string)
{
    auto r = parse_main("    print(f\"x = {x + 1}, y = {y}\")\n");
    EXPECT(!r->diagnostics.has_errors());
    const auto& call = static_cast<const CallExpr&>(
        *static_cast<const ExprStmt&>(*main_body(*r).statements.front()).expr);
    const auto& lit = static_cast<const StringLiteralExpr&>(*call.args.front());
    EXPECT(lit.is_format && lit.parts.size() == 2 && lit.texts.size() == 3);
}

TEST(code_parser_compound_assignment)
{
    auto r = parse_main("    x += 1\n    y = 2\n");
    EXPECT(!r->diagnostics.has_errors());
    const auto& a = static_cast<const AssignStmt&>(*main_body(*r).statements.front());
    EXPECT(a.op && *a.op == BinaryOp::Add);
}

TEST(code_parser_reports_each_broken_line)
{
    auto r = parse_main("    let = 1\n    let y 2\n    let z = 3\n");
    EXPECT(r->error_count() == 2);
}

TEST(code_parser_skips_block_of_broken_header)
{
    auto r = parse_main("    if a b:\n      let = 1\n    let ok = 2\n");
    EXPECT(r->error_count() == 1);
    EXPECT(main_body(*r).statements.size() == 1);
}

TEST(code_parser_missing_block)
{
    auto r = parse_main("    if a:\n    pass\n");
    EXPECT(r->has_error("expected an indented block"));
}

TEST(code_parser_unexpected_indent)
{
    auto r = parse_main("    a()\n      b()\n");
    EXPECT(r->has_error("unexpected indentation"));
}

TEST(code_parser_else_without_if)
{
    auto r = parse_main("    else: pass\n");
    EXPECT(r->has_error("without a matching 'if'"));
}
