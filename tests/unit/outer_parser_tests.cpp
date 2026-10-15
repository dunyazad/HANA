#include "helpers.h"
#include "test.h"

using namespace hana::test;
using namespace hana::lang;

TEST(outer_short_signature)
{
    auto r = parse(R"({
  add: fn<i32(a: i32, inout b: i64, sink c: str,)> a,
  pub scale: fn<(inout p: f32, k: f32)> p = p * k,
  "quoted": fn<void()> pass,
  plain: fn pass,
})");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(r->module->functions.size() == 4);
    const FnDecl& add = *r->module->functions[0];
    EXPECT(add.name == "add" && add.params.size() == 3);
    EXPECT(add.return_type && add.return_type->name == "i32");
    EXPECT(add.params[1].convention == ParamConvention::Inout);
    EXPECT(add.params[2].convention == ParamConvention::Sink);
    EXPECT(r->module->functions[1]->is_pub && !r->module->functions[1]->return_type);
    EXPECT(r->module->functions[2]->name == "quoted" && !r->module->functions[2]->return_type);
    EXPECT(r->module->functions[3]->params.empty());
}

TEST(outer_one_line_body_with_result_returns)
{
    auto r = parse("{ double: fn<i32(x: i32)> x * 2 }");
    EXPECT(!r->diagnostics.has_errors());
    const auto& body = r->module->functions.front()->body;
    EXPECT(body.statements.size() == 1 && body.statements.front()->kind == StmtKind::Return);
}

TEST(outer_one_line_body_stops_at_comma_outside_brackets)
{
    auto r = parse("{ a: fn print(max(1, 2)), b: fn pass }");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(r->module->functions.size() == 2);
}

TEST(outer_object_form)
{
    auto r = parse(R"({
  fib: fn {
    in: { n: i64 },
    out: i64,
    do: '''
      if n < 2: return n
      return fib(n - 1) + fib(n - 2)
    ''',
  },
})");
    EXPECT(!r->diagnostics.has_errors());
    const FnDecl& fib = *r->module->functions.front();
    EXPECT(fib.params.size() == 1 && fib.return_type && fib.body.statements.size() == 2);
}

TEST(outer_one_line_code_block)
{
    auto r = parse("{ reset: fn ''' count = 0 ''' }");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(r->module->functions.front()->body.statements.size() == 1);
}

TEST(outer_comments_and_trailing_commas)
{
    auto r = parse(R"(
// line comment
{
  /* block */ a: fn pass,   // after an entry
  b: fn pass,
}
)");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(r->module->functions.size() == 2);
}

TEST(outer_closing_quotes_must_be_alone)
{
    auto r = parse("{\n  f: fn '''\n    pass '''\n}");
    EXPECT(r->has_error("must be on its own line"));
}

TEST(outer_unterminated_block)
{
    auto r = parse("{\n  f: fn '''\n    pass\n}");
    EXPECT(r->has_error("unterminated ''' block"));
}

TEST(outer_not_an_object)
{
    auto r = parse("[1, 2]");
    EXPECT(r->has_error("a HANA module is an object"));
}

TEST(outer_unsupported_entries_and_recovery)
{
    auto r = parse(R"({
  max_hp: 100,
  Point: struct { x: f32 },
  ok: fn pass,
})");
    EXPECT(r->error_count() == 2);
    EXPECT(r->has_error("'max_hp': only functions"));
    EXPECT(r->module->functions.size() == 1);
}

TEST(outer_use_is_parsed)
{
    auto r = parse("{ use: [], f: fn pass }");
    EXPECT(!r->diagnostics.has_errors());
    auto s = parse("{ use: [\"gui\"], f: fn pass }");
    EXPECT(s->has_error("modules are not supported yet"));
    EXPECT(s->module->functions.size() == 1);
}

TEST(outer_tuple_return_is_reported)
{
    auto r = parse("{ divmod: fn<(i32, i32)(a: i32, b: i32)> a }");
    EXPECT(r->has_error("tuples) are not supported yet"));
}

TEST(outer_missing_comma)
{
    auto r = parse("{ a: fn pass\n b: fn pass }");
    EXPECT(r->has_error("expected ',' between entries"));
}

TEST(outer_standard_json_keys_and_strings)
{
    auto r = parse(R"({ "use": [], "na\u006De": fn pass })");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(r->module->functions.front()->name == "name");
}
