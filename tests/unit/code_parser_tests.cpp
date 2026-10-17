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

TEST(code_parser_struct_literal)
{
    auto r = parse_main("    let p = Point { x: 1, y: 2, }\n    let e = Empty {}\n");
    EXPECT(!r->diagnostics.has_errors());
    const auto& lit = static_cast<const StructLiteralExpr&>(first_let_init(*r));
    EXPECT(lit.kind == ExprKind::StructLiteral);
    EXPECT(lit.type_name == "Point");
    EXPECT(lit.fields.size() == 2);
    EXPECT(lit.fields[1].name == "y");
}

TEST(code_parser_struct_literal_spans_lines)
{
    auto r = parse_main("    let p = Point {\n      x: 1,\n      y: 2,\n    }\n    pass\n");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(main_body(*r).statements.size() == 2);
}

TEST(code_parser_enum_value)
{
    auto r = parse_main("    let s = Shape.Circle { radius: 1.0 }\n");
    EXPECT(!r->diagnostics.has_errors());
    const auto& lit = static_cast<const StructLiteralExpr&>(first_let_init(*r));
    EXPECT(lit.type_name == "Shape");
    EXPECT(lit.case_name == "Circle");
    EXPECT(lit.fields.size() == 1);
}

TEST(code_parser_match_statement_and_patterns)
{
    auto r = parse_main("    match v:\n"
                        "      Circle(r): pass\n"
                        "      Rect(_, h) if h > 1:\n"
                        "        pass\n"
                        "      -3..=9: pass\n"
                        "      'a'..'z': pass\n"
                        "      n: pass\n"
                        "    pass\n");
    EXPECT(!r->diagnostics.has_errors());
    const Block& body = main_body(*r);
    EXPECT(body.statements.size() == 2);
    const auto& match = *static_cast<const MatchStmt&>(*body.statements[0]).match;
    EXPECT(match.arms.size() == 5);
    EXPECT(match.arms[0].pattern->kind == PatternKind::Case);
    EXPECT(match.arms[1].pattern->arguments[0]->kind == PatternKind::Wildcard);
    EXPECT(match.arms[1].guard != nullptr);
    EXPECT(match.arms[2].pattern->kind == PatternKind::Range);
    EXPECT(match.arms[2].pattern->inclusive);
    EXPECT(!match.arms[3].pattern->inclusive);
    EXPECT(match.arms[4].pattern->kind == PatternKind::Binding);
}

TEST(code_parser_match_expression_positions)
{
    auto r = parse_main("    let x = match v:\n      0: 1\n      _: 2\n    print(x)\n");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(main_body(*r).statements.size() == 2);
    EXPECT(first_let_init(*r).kind == ExprKind::Match);
    EXPECT(parse_main("    f(match v:\n      _: 1\n    )\n")->has_error("can only be the value"));
}

TEST(code_parser_for_range)
{
    auto r = parse_main("    for i in 0..n + 1:\n      pass\n    for j in 1..=3: pass\n");
    EXPECT(!r->diagnostics.has_errors());
    const auto& first = static_cast<const ForStmt&>(*main_body(*r).statements[0]);
    EXPECT(first.name == "i");
    EXPECT(!first.inclusive);
    EXPECT(first.end->kind == ExprKind::Binary);
    EXPECT(static_cast<const ForStmt&>(*main_body(*r).statements[1]).inclusive);
    EXPECT(parse_main("    for inout i in 0..3:\n      pass\n")
               ->has_error("'inout' is for looping over a list"));
}

TEST(code_parser_list_values_and_loops)
{
    auto r = parse_main("    let xs = [1, 2,\n      3,]\n    let e: List<i32> = []\n"
                        "    for x in xs: pass\n    for inout y in xs[0..1]: pass\n");
    EXPECT(r->has_error("expected ']'"));
    auto ok = parse_main("    let xs = [1, 2,\n      3,]\n    let e: List<i32> = []\n"
                         "    for sink x in xs: pass\n    for inout y in e:\n      y = xs[0]\n");
    EXPECT(!ok->diagnostics.has_errors());
    const auto& list = static_cast<const ListExpr&>(first_let_init(*ok));
    EXPECT(list.elements.size() == 3);
    const auto& sink = static_cast<const ForStmt&>(*main_body(*ok).statements[2]);
    EXPECT(sink.iterable && sink.convention == ParamConvention::Sink && !sink.start);
    const auto& inout = static_cast<const ForStmt&>(*main_body(*ok).statements[3]);
    EXPECT(inout.convention == ParamConvention::Inout);
}

TEST(code_parser_tuple_values_and_destructuring)
{
    auto r = parse_main("    let t = (1, (2, 3),)\n    let (a, (b, _)) = t\n    let g = (1)\n");
    EXPECT(!r->diagnostics.has_errors());
    const auto& tuple = static_cast<const TupleExpr&>(first_let_init(*r));
    EXPECT(tuple.kind == ExprKind::Tuple);
    EXPECT(tuple.elements.size() == 2);
    EXPECT(tuple.elements[1]->kind == ExprKind::Tuple);
    const auto& let = static_cast<const LetStmt&>(*main_body(*r).statements[1]);
    EXPECT(let.pattern && let.pattern->kind == PatternKind::Tuple);
    EXPECT(let.pattern->arguments[1]->arguments[1]->kind == PatternKind::Wildcard);
    const auto& grouped = static_cast<const LetStmt&>(*main_body(*r).statements[2]);
    EXPECT(grouped.init->kind == ExprKind::IntLiteral);
    EXPECT(parse_main("    let x = (1,)\n")->has_error("at least two elements"));
}

TEST(code_parser_option_types_and_patterns)
{
    auto r = parse_main("    let a: Option<Option<i32>> = null\n    let b: i32?? = null\n"
                        "    match a:\n      Some(null): pass\n      null: pass\n      _: pass\n");
    EXPECT(!r->diagnostics.has_errors());
    const auto& first = static_cast<const LetStmt&>(*main_body(*r).statements[0]);
    EXPECT(first.declared_type->name == "Option<Option<i32>>");
    const auto& second = static_cast<const LetStmt&>(*main_body(*r).statements[1]);
    EXPECT(second.declared_type->name == "Option<Option<i32>>");
    const auto& match = *static_cast<const MatchStmt&>(*main_body(*r).statements[2]).match;
    EXPECT(match.arms[0].pattern->arguments[0]->kind == PatternKind::Null);
    EXPECT(match.arms[1].pattern->kind == PatternKind::Null);
}
