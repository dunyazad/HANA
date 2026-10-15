#include "helpers.h"
#include "test.h"

using namespace hana::test;
using namespace hana::lang;

TEST(sema_valid_program)
{
    auto r = check(R"({
  add: fn<i64(a: i64, b: i64)> a + b,
  bump: fn<(inout x: i32)> x += 1,
  main: fn '''
    var n = 0
    bump(&n)
    let total = add(1, 2)
    if total > 2 and n == 1:
      print(f"ok {total} {n}")
  ''',
})");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(r->program && r->program->main != nullptr);
}

TEST(sema_literal_takes_expected_type)
{
    auto r = check("{ f: fn '''\n  let a: u8 = 200\n  let b: i64 = 1 + 2\n  let c: f32 = 1\n''' }");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_literal_range)
{
    auto r = check("{ f: fn '''\n  let a: u8 = 256\n  let b: i8 = -128\n  let c: i8 = -129\n''' }");
    EXPECT(r->error_count() == 2);
    EXPECT(r->has_error("-129 does not fit in 'i8'"));
}

TEST(sema_no_implicit_conversion)
{
    auto r = check("{ f: fn<(a: i32, b: i64)> print(a + b) }");
    EXPECT(r->has_error("different types 'i32' and 'i64'"));
}

TEST(sema_let_is_immutable)
{
    auto r = check("{ f: fn '''\n  let x = 1\n  x = 2\n''' }");
    EXPECT(r->has_error("declared with 'let'"));
}

TEST(sema_assignment_needs_declaration)
{
    auto r = check("{ f: fn '''\n  y = 2\n''' }");
    EXPECT(r->has_error("unknown name 'y'"));
}

TEST(sema_inout_rules)
{
    auto r = check(R"({
  two: fn<(inout a: i32, inout b: i32)> pass,
  one: fn<(inout a: i32, b: i32)> pass,
  f: fn '''
    var x = 1
    let fixed = 2
    two(&x, &x)
    one(&x, x)
    one(x, 1)
    one(&fixed, 1)
  ''',
})");
    EXPECT(r->has_error("passed as inout more than once"));
    EXPECT(r->has_error("passed as inout and also read"));
    EXPECT(r->has_error("must be marked with '&'"));
    EXPECT(r->has_error("not mutable"));
}

TEST(sema_missing_return)
{
    auto r = check("{ f: fn<i32(a: bool)> '''\n  if a: return 1\n''' }");
    EXPECT(r->has_error("must return a value of type 'i32' on every path"));
}

TEST(sema_returns_on_every_path)
{
    auto r = check(R"({
  sign: fn<i32(a: i32)> '''
    if a > 0:
      return 1
    elif a < 0:
      return -1
    else:
      return 0
  ''',
  forever: fn<i32()> '''
    while true:
      pass
  ''',
})");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_while_true_with_break_needs_return)
{
    auto r = check("{ f: fn<i32()> '''\n  while true:\n    break\n''' }");
    EXPECT(r->has_error("on every path"));
}

TEST(sema_conditional_expression)
{
    auto r = check("{ f: fn<i64(a: i64, b: i64)> a if a > b else b }");
    EXPECT(!r->diagnostics.has_errors());
    auto s = check("{ f: fn<(a: i32)> print(a if a > 0 else true) }");
    EXPECT(s->has_error("different types"));
}

TEST(sema_casts)
{
    auto r = check("{ f: fn<(a: i32, c: char)> '''\n  let x = a as f64\n  let y = c as u32\n  let z = true as u8\n''' }");
    EXPECT(!r->diagnostics.has_errors());
    auto s = check("{ f: fn<(a: i32)> print(a as bool) }");
    EXPECT(s->has_error("cannot convert 'i32' to 'bool'"));
}

TEST(sema_format_string_only_in_print)
{
    auto r = check("{ f: fn<(x: i32)> '''\n  let s = f\"{x}\"\n''' }");
    EXPECT(r->has_error("format strings"));
}

TEST(sema_main_signature)
{
    auto r = check("{ main: fn<bool(a: i32)> true }");
    EXPECT(r->has_error("'main' must not take parameters"));
    EXPECT(r->has_error("'main' must return nothing or 'i32'"));
}

TEST(sema_duplicate_function)
{
    auto r = check("{ f: fn pass, f: fn pass }");
    EXPECT(r->has_error("already defined"));
}

TEST(sema_expression_statement_must_be_call)
{
    auto r = check("{ f: fn<(a: i32)> '''\n  a + 1\n''' }");
    EXPECT(r->has_error("has no effect"));
}

namespace {

const Type* let_type(const Compiled& r, const std::string& name)
{
    for (const auto& statement : r.module->functions.back()->body.statements) {
        if (statement->kind == StmtKind::Let) {
            const auto& let = static_cast<const LetStmt&>(*statement);
            if (let.name == name && let.symbol != nullptr) {
                return let.symbol->type;
            }
        }
    }
    return nullptr;
}

} // namespace

TEST(sema_literal_variable_takes_first_typed_use)
{
    auto r = check(R"({
  takes64: fn<(x: i64)> pass,
  half: fn<f32(x: f32)> x / 2.0,
  main: fn '''
    var a = 0
    var b = a
    takes64(b)
    let f = 1.5
    print(half(f))
    var unused = 7
    var i = 0
    while i < 3:
      takes64(i)
      i += 1
  ''',
})");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(let_type(*r, "a") != nullptr && let_type(*r, "a")->name == "i64");
    EXPECT(let_type(*r, "b")->name == "i64");
    EXPECT(let_type(*r, "f")->name == "f32");
    EXPECT(let_type(*r, "unused")->name == "i32");
    EXPECT(let_type(*r, "i")->name == "i64");
}

TEST(sema_literal_variable_conflicting_uses)
{
    auto r = check(R"({
  takes32: fn<(x: i32)> pass,
  takes64: fn<(x: i64)> pass,
  main: fn '''
    var d = 1
    takes32(d)
    takes64(d)
  ''',
})");
    EXPECT(r->has_error("expected 'i64', found 'i32'"));
}

TEST(sema_literal_variable_range_uses_inferred_type)
{
    auto r = check("{ main: fn '''\n  var c = 300\n  let small: u8 = 1\n  print(c + small)\n''' }");
    EXPECT(r->has_error("300 does not fit in 'u8'"));
}

TEST(sema_float_literal_variable_stays_float)
{
    auto r = check("{ takes: fn<(x: i32)> pass, main: fn '''\n  let f = 1.5\n  takes(f)\n''' }");
    EXPECT(r->has_error("expected 'i32', found 'f64'"));
}
