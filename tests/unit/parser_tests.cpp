#include "helpers.h"
#include "test.h"

using namespace hana::test;
using namespace hana::lang;

namespace {

const Stmt& first_statement(const Compiled& r)
{
    return *r.module->functions.front()->body.statements.front();
}

const Expr& let_init(const Compiled& r)
{
    return *static_cast<const LetStmt&>(first_statement(r)).init;
}

} // namespace

TEST(parser_function_signature)
{
    auto r = parse("pub fn add(a: i32, inout b: i64, sink c: str,) -> i32 {\n return a\n}");
    EXPECT(!r->diagnostics.has_errors());
    const FnDecl& f = *r->module->functions.front();
    EXPECT(f.is_pub && f.name == "add" && f.params.size() == 3);
    EXPECT(f.params[1].convention == ParamConvention::Inout && f.params[1].type.name == "i64");
    EXPECT(f.params[2].convention == ParamConvention::Sink);
    EXPECT(f.return_type && f.return_type->name == "i32");
}

TEST(parser_brace_on_next_line)
{
    auto r = parse("fn main()\n{\n}\n");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(parser_precedence)
{
    // a + b * c == d & e  parses as  (a + (b * c)) == (d & e)
    auto r = parse("fn f() {\n let x = a + b * c == d & e\n}");
    EXPECT(!r->diagnostics.has_errors());
    const auto& eq = static_cast<const BinaryExpr&>(let_init(*r));
    EXPECT(eq.op == BinaryOp::Eq);
    EXPECT(static_cast<const BinaryExpr&>(*eq.lhs).op == BinaryOp::Add);
    EXPECT(static_cast<const BinaryExpr&>(*eq.rhs).op == BinaryOp::BitAnd);
}

TEST(parser_chained_comparison_is_an_error)
{
    auto r = parse("fn f() {\n let x = a < b < c\n}");
    EXPECT(r->has_error("cannot be chained"));
}

TEST(parser_compound_assignment)
{
    auto r = parse("fn f() {\n x += 1; y = 2\n}");
    EXPECT(!r->diagnostics.has_errors());
    const auto& a = static_cast<const AssignStmt&>(first_statement(*r));
    EXPECT(a.op && *a.op == BinaryOp::Add);
}

TEST(parser_if_else_chain)
{
    auto r = parse("fn f() {\n if a { } else if b { }\n else { }\n}");
    EXPECT(!r->diagnostics.has_errors());
    const auto& s = static_cast<const IfStmt&>(first_statement(*r));
    EXPECT(s.else_branch && s.else_branch->kind == StmtKind::If);
    const auto& inner = static_cast<const IfStmt&>(*s.else_branch);
    EXPECT(inner.else_branch && inner.else_branch->kind == StmtKind::Block);
}

TEST(parser_interpolated_string)
{
    auto r = parse("fn f() {\n println(\"x = {x + 1}, y = {y}\")\n}");
    EXPECT(!r->diagnostics.has_errors());
    const auto& call = static_cast<const CallExpr&>(
        *static_cast<const ExprStmt&>(first_statement(*r)).expr);
    const auto& lit = static_cast<const StringLiteralExpr&>(*call.args.front());
    EXPECT(lit.parts.size() == 2 && lit.texts.size() == 3);
    EXPECT(lit.texts[0] == "x = " && lit.texts[1] == ", y = " && lit.texts[2].empty());
}

TEST(parser_integer_values)
{
    auto r = parse("fn f() {\n let a = 0xff\n let b = 0b1010\n let c = 18446744073709551616\n}");
    const auto& body = r->module->functions.front()->body.statements;
    const auto value = [&](std::size_t i) {
        return static_cast<const IntLiteralExpr&>(*static_cast<const LetStmt&>(*body[i]).init);
    };
    EXPECT(value(0).value == 255);
    EXPECT(value(1).value == 10);
    EXPECT(value(2).overflowed);
}

TEST(parser_reports_each_broken_statement)
{
    auto r = parse("fn f() {\n let = 1\n let y 2\n let z = 3\n}");
    EXPECT(r->error_count() == 2);
}

TEST(parser_recovers_at_next_function)
{
    auto r = parse("fn f( {\n}\nfn g() {\n let = 1\n}\nfn h() {\n}");
    EXPECT(r->error_count() == 2);
    // f is dropped; g keeps its other statements; h is intact.
    EXPECT(r->module->functions.size() == 2);
}

TEST(parser_unsupported_declarations)
{
    auto r = parse("struct Point {\n x: f32\n}\nfn main() {\n}");
    EXPECT(r->has_error("'struct' declarations are not supported yet"));
    EXPECT(r->module->functions.size() == 1);
}
