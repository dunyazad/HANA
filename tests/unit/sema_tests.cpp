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
    EXPECT(!r->has_error("passed as inout and also read")); // 'x' is Copy
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

TEST(sema_format_string_is_a_str_value)
{
    auto r = check("{ f: fn<str(x: i32)> '''\n  let s = f\"{x}\"\n  return s + \"!\"\n''' }");
    EXPECT(!r->diagnostics.has_errors());
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

TEST(sema_struct_program)
{
    auto r = check(R"({
  P: struct {
    x: i32,
    y: i32 = 2,
    sum: fn<i32(self)> self.x + self.y,
    move: fn<(inout self, d: i32)> self.x += d,
    at: fn<P(x: i32)> P { x: x },
  },
  main: fn '''
    var p = P.at(1)
    p.move(2)
    p.y = 5
    print(p.sum())
  ''',
})");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_struct_literal_fields)
{
    auto r = check("{ P: struct { a: i32, b: i32 = 1 }, f: fn '''\n  let p = P { b: 2, c: 3 }\n''' }");
    EXPECT(r->has_error("'P' has no field 'c'"));
    EXPECT(r->has_error("missing field 'a'"));
}

TEST(sema_struct_cannot_contain_itself)
{
    EXPECT(check("{ A: struct { a: A } }")->has_error("contains itself"));
}

TEST(sema_copy_struct_needs_copy_fields)
{
    EXPECT(check("{ A: struct copy { s: str } }")->has_error("is not Copy"));
    EXPECT(!check("{ A: struct copy { v: B }, B: struct copy { x: f32 } }")->diagnostics.has_errors());
}

TEST(sema_inout_receiver_needs_mutable_place)
{
    const char* decl = "P: struct { x: i32, m: fn<(inout self)> pass, at: fn<P()> P { x: 0 } }";
    EXPECT(check(std::string("{ ") + decl + ", f: fn '''\n  let p = P.at()\n  p.m()\n''' }")
               ->has_error("is not mutable"));
    EXPECT(check(std::string("{ ") + decl + ", f: fn P.at().m() }")
               ->has_error("must be called on a variable"));
}

TEST(sema_field_inout_exclusivity)
{
    const char* head = "{ P: struct { x: i32, y: i32 }, two: fn<(inout a: i32, inout b: i32)> pass, "
                       "whole: fn<(inout p: P, inout b: i32)> pass, f: fn '''\n  var p = P { x: 1, y: 2 }\n";
    EXPECT(!check(std::string(head) + "  two(&p.x, &p.y)\n''' }")->diagnostics.has_errors());
    EXPECT(check(std::string(head) + "  whole(&p, &p.x)\n''' }")->has_error("more than once"));
}

TEST(sema_use_after_move)
{
    auto r = check(R"({
  P: struct { s: str },
  eat: fn<(sink p: P)> pass,
  f: fn '''
    let p = P { s: "a" }
    eat(p)
    eat(p)
  ''',
})");
    EXPECT(r->error_count() == 1);
    EXPECT(r->has_error("use of moved value 'p'"));
}

TEST(sema_moves_follow_control_flow)
{
    // Moved on one branch only, then reassigned: no error.
    auto ok = check(R"({
  P: struct { s: str },
  eat: fn<(sink p: P)> pass,
  f: fn<(c: bool)> '''
    var p = P { s: "a" }
    if c:
      eat(p)
      p = P { s: "b" }
    eat(p)
    var i = 0
    while i < 2:
      let q = P { s: "c" }
      eat(q)
      i += 1
  ''',
})");
    EXPECT(!ok->diagnostics.has_errors());
    auto bad = check(R"({
  P: struct { s: str },
  eat: fn<(sink p: P)> pass,
  f: fn<(c: bool)> '''
    let p = P { s: "a" }
    if c:
      eat(p)
    eat(p)
  ''',
})");
    EXPECT(bad->has_error("use of moved value 'p'"));
}

TEST(sema_copy_values_do_not_move)
{
    auto r = check(R"({
  V: struct copy { x: f32 },
  eat: fn<(sink v: V)> pass,
  f: fn '''
    let v = V { x: 1.0 }
    eat(v)
    eat(v)
    let n = 1
    let m = n
    print(n)
  ''',
})");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_moving_a_field_moves_the_variable)
{
    auto r = check(R"({
  P: struct { s: str, n: i32 },
  f: fn '''
    let p = P { s: "a", n: 1 }
    let s = p.s
    print(p.n)
  ''',
})");
    EXPECT(r->has_error("use of moved value 'p'"));
}

TEST(sema_cannot_move_out_of_read_or_inout_params)
{
    EXPECT(check("{ P: struct { s: str }, f: fn<P(p: P)> p }")->has_error("read-only parameter"));
    EXPECT(check("{ P: struct { s: str }, f: fn<(inout p: P)> '''\n  let q = p\n''' }")
               ->has_error("inout parameter"));
    EXPECT(!check("{ P: struct { s: str }, f: fn<P(sink p: P)> p }")->diagnostics.has_errors());
}

TEST(sema_copy_read_may_overlap_inout)
{
    auto r = check(R"({
  V: struct copy { x: i32, m: fn<(inout self, n: i32)> self.x += n, w: fn<(self, inout v: V)> pass },
  P: struct { s: str, n: i32, hit: fn<(inout self, n: i32)> self.n -= n },
  look: fn<(inout p: P, q: P)> pass,
  f: fn '''
    var v = V { x: 1 }
    v.m(v.x)
    v.w(&v)
    var p = P { s: "a", n: 1 }
    p.hit(p.n)
    look(&p, p)
  ''',
})");
    EXPECT(r->error_count() == 1);
    EXPECT(r->has_error("'p' is passed as inout and also read"));
}

TEST(sema_enum_program)
{
    auto r = check(R"({
  Shape: enum {
    Circle: { r: f64 },
    Empty: {},
    area: fn<f64(self)> '''
      return match self:
        Circle(r): r * r
        Empty: 0.0
    ''',
  },
  f: fn<f64()> '''
    let s = Shape.Circle { r: 2.0 }
    let e = Shape.Empty
    return s.area() + e.area()
  ''',
})");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_match_coverage)
{
    const char* head = "{ E: enum { A: {}, B: { x: i32 } }, f: fn<(e: E)> '''\n  match e:\n";
    EXPECT(check(std::string(head) + "    A: pass\n''' }")->has_error("does not cover 'B(_)'"));
    EXPECT(!check(std::string(head) + "    A: pass\n    B(_): pass\n''' }")->diagnostics.has_errors());
    // A guarded arm or a refutable field pattern does not cover the case.
    EXPECT(check(std::string(head) + "    A: pass\n    B(x) if x > 0: pass\n''' }")
               ->has_error("does not cover 'B(_)'"));
    EXPECT(check(std::string(head) + "    A: pass\n    B(1): pass\n''' }")
               ->has_error("does not cover 'B(_)'"));
    EXPECT(check("{ f: fn<(n: i32)> '''\n  match n:\n    1: pass\n''' }")->has_error("needs a final '_'"));
}

TEST(sema_match_expression_types)
{
    auto r = check("{ f: fn<i64(n: i32)> '''\n  return match n:\n    0: 1\n    _: 2\n''' }");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(check("{ f: fn<(n: i32)> '''\n  let x = match n:\n    0: 1\n    _: true\n''' }")
               ->has_error("different types"));
}

TEST(sema_match_statement_terminates)
{
    EXPECT(!check("{ f: fn<i32(b: bool)> '''\n  match b:\n    true: return 1\n    false: return 2\n''' }")
                ->diagnostics.has_errors());
}

TEST(sema_match_binding_is_part_of_subject)
{
    const char* decl = "M: enum { T: { s: str }, Q: {} }, eat: fn<(sink s: str)> pass, "
                       "eat_m: fn<(sink m: M)> pass";
    EXPECT(check(std::string("{ ") + decl + ", f: fn<(m: M)> '''\n  match m:\n    T(s): eat(s)\n    Q: pass\n''' }")
               ->has_error("read-only parameter"));
    EXPECT(!check(std::string("{ ") + decl + ", f: fn<(m: M)> '''\n  match m:\n    T(s): print(s)\n    Q: pass\n''' }")
                ->diagnostics.has_errors());
    EXPECT(check(std::string("{ ") + decl +
                 ", f: fn<(sink m: M)> '''\n  match m:\n    T(s):\n      eat_m(m)\n      print(s)\n    Q: pass\n''' }")
               ->has_error("use of 's' after 'm' was moved"));
}

TEST(sema_for_range_types)
{
    EXPECT(!check("{ f: fn<(n: u64)> '''\n  for i in 0..n:\n    pass\n''' }")->diagnostics.has_errors());
    EXPECT(check("{ f: fn<(a: i64, b: i32)> '''\n  for i in a..b:\n    pass\n''' }")
               ->has_error("different types"));
    EXPECT(check("{ f: fn '''\n  for i in 0.0..1.0:\n    pass\n''' }")->has_error("integer bounds"));
    EXPECT(check("{ f: fn '''\n  for i in 0..3:\n    i = 1\n''' }")->has_error("'for' loop cannot be changed"));
}

TEST(sema_for_variable_is_inferred_from_use)
{
    auto r = check("{ take: fn<(x: i64)> pass, f: fn '''\n  for i in 0..3:\n    take(i)\n''' }");
    EXPECT(!r->diagnostics.has_errors());
    const auto& loop = static_cast<const ForStmt&>(*r->module->functions[1]->body.statements[0]);
    EXPECT(loop.symbol->type->name == "i64");
}

TEST(sema_for_moves_repeat)
{
    EXPECT(check("{ P: struct { s: str }, eat: fn<(sink p: P)> pass, f: fn '''\n"
                 "  let p = P { s: \"a\" }\n  for i in 0..2:\n    eat(p)\n''' }")
               ->has_error("use of moved value 'p'"));
}

TEST(sema_tuple_program)
{
    auto r = check(R"({
  divmod: fn<(i32, i32)(a: i32, b: i32)> (a / b, a % b),
  f: fn<i32()> '''
    let (q, r) = divmod(7, 2)
    var t = divmod(9, 4)
    t.1 += q
    return t.0 + t.1 + r
  ''',
})");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_tuple_types_are_shared)
{
    auto r = check("{ f: fn<(i32, i64)()> (1, 2), g: fn<(i32, i64)()> f() }");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(r->program->composed_types.size() == 1);
}

TEST(sema_tuple_errors)
{
    EXPECT(check("{ f: fn '''\n  let t = (1, 2)\n  print(t.2)\n''' }")->has_error("out of range"));
    EXPECT(check("{ f: fn '''\n  let (a, b, c) = (1, 2)\n''' }")->has_error("the pattern has 3"));
    EXPECT(check("{ f: fn '''\n  let (a, 1) = (1, 2)\n''' }")->has_error("must match every value"));
    EXPECT(check("{ A: struct { t: (A, i32) } }")->has_error("contains itself"));
    EXPECT(check("{ A: struct copy { t: (i32, str) } }")->has_error("is not Copy"));
}

TEST(sema_tuple_copy_and_moves)
{
    // A tuple of Copy values is Copy; one with a str moves.
    EXPECT(!check("{ eat: fn<(sink t: (i32, f64))> pass, f: fn '''\n  let t = (1, 2.0)\n  eat(t)\n  eat(t)\n''' }")
                ->diagnostics.has_errors());
    EXPECT(check("{ eat: fn<(sink t: (i32, str))> pass, f: fn '''\n  let t = (1, \"a\")\n  eat(t)\n  eat(t)\n''' }")
               ->has_error("use of moved value 't'"));
}

TEST(sema_tuple_inference_through_elements)
{
    auto r = check("{ f: fn '''\n  var i = 0\n  let t: (i64, i32) = (i, 1)\n''' }");
    EXPECT(!r->diagnostics.has_errors());
    const auto& let = static_cast<const LetStmt&>(*r->module->functions[0]->body.statements[0]);
    EXPECT(let.symbol->type->name == "i64");
}

TEST(sema_option_values)
{
    auto r = check(R"({
  find: fn<i32?(n: i32)> n if n > 0 else null,
  f: fn<bool()> '''
    let a: u8? = 200
    let b: Option<u8> = Some(1)
    let c: i64?? = Some(null)
    var d: i32? = null
    d = 5
    return a != null and null == d and find(1) != null
  ''',
})");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(check("{ f: fn '''\n  let a = null\n''' }")->has_error("needs to know its Option type"));
    EXPECT(check("{ f: fn '''\n  let a: i32 = null\n''' }")->has_error("cannot be 'null'"));
    EXPECT(check("{ f: fn<(a: i32?)> print(a == 5) }")->has_error("only be compared with 'null'"));
    EXPECT(check("{ g: fn<(inout x: i32?)> pass, f: fn '''\n  var n = 1\n  g(&n)\n''' }")
               ->has_error("expected 'Option<i32>', found 'i32'"));
}

TEST(sema_option_types_are_shared)
{
    auto r = check("{ f: fn<i32?()> null, g: fn<Option<i32>()> f() }");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_coverage_is_nested)
{
    // Some(Some(_)), Some(null) and null cover Option<Option<T>>.
    EXPECT(!check("{ f: fn<(x: Option<i32?>)> '''\n  match x:\n    Some(Some(v)): pass\n    Some(null): pass\n"
                  "    null: pass\n''' }")
                ->diagnostics.has_errors());
    EXPECT(check("{ f: fn<(x: Option<i32?>)> '''\n  match x:\n    Some(Some(v)): pass\n    null: pass\n''' }")
               ->has_error("does not cover 'Some(null)'"));
    // Tuples of bools are covered constructor by constructor.
    EXPECT(!check("{ f: fn<(x: (bool, bool))> '''\n  match x:\n    (true, _): pass\n    (false, true): pass\n"
                  "    (false, false): pass\n''' }")
                ->diagnostics.has_errors());
    EXPECT(check("{ f: fn<(x: (bool, bool))> '''\n  match x:\n    (true, _): pass\n    (_, true): pass\n''' }")
               ->has_error("does not cover '(false, false)'"));
}

TEST(sema_coverage_finds_unreachable_arms)
{
    EXPECT(check("{ E: enum { A: {}, B: {} }, f: fn<(e: E)> '''\n  match e:\n    A: pass\n    B: pass\n"
                 "    A: pass\n''' }")
               ->has_error("never reached"));
    // A guarded arm covers nothing, so an arm after it is reachable.
    EXPECT(!check("{ f: fn<(x: i32?)> '''\n  match x:\n    Some(v) if v > 0: pass\n    Some(_): pass\n"
                  "    null: pass\n''' }")
                ->diagnostics.has_errors());
}

TEST(sema_wrapped_values_move)
{
    EXPECT(check("{ P: struct { s: str }, keep: fn<(o: P?)> pass, f: fn '''\n  let q = P { s: \"a\" }\n"
                 "  keep(q)\n  keep(q)\n''' }")
               ->has_error("use of moved value 'q'"));
}

TEST(sema_user_clone_is_called)
{
    auto r = check("{ H: struct { id: i32, clone: fn<H(self)> H { id: self.id + 1 },\n"
                   "  drop: fn<(inout self)> pass },\n"
                   "  f: fn '''\n  let a = H { id: 1 }\n  let b = a.clone().clone()\n''' }");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_no_part_of_temporary_with_drop)
{
    auto r = check("{ P: struct { s: str, drop: fn<(inout self)> pass },\n"
                   "  make: fn<P()> P { s: \"a\" }, take: fn<(sink s: str)> pass,\n"
                   "  f: fn '''\n  take(make().s)\n  let n = make().s.len()\n''' }");
    EXPECT(r->has_error("cannot take 's' out of a temporary value of type 'P', because it has "
                        "'drop'"));
    EXPECT(r->diagnostics.diagnostics().size() == 1);
}

TEST(sema_list_types_are_shared)
{
    auto r = check("{ f: fn<List<i32>(sink a: List<i32>)> '''\n  let b = [1, 2]\n  let c: List<i32> = []\n"
                   "  return a\n''' }");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(r->program->composed_types.size() == 1);
}

TEST(sema_list_element_type_and_index_inference)
{
    auto r = check("{ f: fn '''\n  let x: i64 = 5\n  let xs = [1, x, 2]\n  var i = 0\n"
                   "  let y: i64 = xs[i]\n''' }");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(check("{ f: fn '''\n  let xs = [1]\n  let i: i32 = 0\n  print(xs[i])\n''' }")
               ->has_error("a list index has type 'usize'"));
    EXPECT(check("{ f: fn '''\n  let xs = []\n''' }")->has_error("empty list is not known"));
}

TEST(sema_list_methods_and_loops)
{
    auto r = check("{ P: struct { hp: i32 }, f: fn '''\n  var ps = [P { hp: 1 }]\n"
                   "  ps.push(P { hp: 2 })\n  ps.insert(0, P { hp: 0 })\n  let p: P = ps.remove(0)\n"
                   "  let q: P? = ps.pop()\n  for inout x in ps:\n    x.hp += 1\n"
                   "  for sink x in ps:\n    print(x.hp)\n''' }");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(check("{ f: fn '''\n  let xs = [1]\n  for inout x in xs:\n    pass\n''' }")
               ->has_error("cannot loop with 'for inout' over 'xs'"));
    EXPECT(check("{ f: fn '''\n  var xs = [1]\n  for x in xs:\n    x = 2\n''' }")
               ->has_error("write 'for inout x in ...'"));
}

TEST(sema_list_elements_stay_in_the_list)
{
    EXPECT(check("{ P: struct { s: str }, f: fn '''\n  var ps = [P { s: \"a\" }]\n  let p = ps[0]\n''' }")
               ->has_error("cannot move 'ps[...]' out of the list"));
    EXPECT(check("{ P: struct { s: str }, f: fn '''\n  var ps = [P { s: \"a\" }]\n  for p in ps:\n"
                 "    ps.push(P { s: \"b\" })\n''' }")
               ->has_error("cannot change 'ps' while it is in use"));
}
