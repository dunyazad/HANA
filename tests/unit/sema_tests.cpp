#include "helpers.h"
#include "test.h"

using namespace hana::test;
using namespace hana::lang;

TEST(sema_valid_program)
{
    auto r = check(R"(
fn add(a: i64, b: i64) -> i64 {
    return a + b
}

fn bump(inout x: i32) {
    x += 1
}

fn main() {
    var n = 0
    bump(&n)
    let total = add(1, 2)
    if total > 2 && n == 1 {
        println("ok {total} {n}")
    }
}
)");
    EXPECT(!r->diagnostics.has_errors());
    EXPECT(r->program->main != nullptr);
}

TEST(sema_literal_takes_expected_type)
{
    auto r = check("fn f() {\n let a: u8 = 200\n let b: i64 = 1 + 2\n let c: f32 = 1\n}");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_literal_range)
{
    auto r = check("fn f() {\n let a: u8 = 256\n let b: i8 = -128\n let c: i8 = -129\n}");
    EXPECT(r->error_count() == 2);
    EXPECT(r->has_error("does not fit in 'u8'"));
    EXPECT(r->has_error("-129 does not fit in 'i8'"));
}

TEST(sema_no_implicit_conversion)
{
    auto r = check("fn f(a: i32, b: i64) {\n let c = a + b\n}");
    EXPECT(r->has_error("different types 'i32' and 'i64'"));
}

TEST(sema_let_is_immutable)
{
    auto r = check("fn f() {\n let x = 1\n x = 2\n}");
    EXPECT(r->has_error("declared with 'let'"));
}

TEST(sema_read_param_is_immutable)
{
    auto r = check("fn f(x: i32) {\n x = 2\n}");
    EXPECT(r->has_error("declare it 'inout'"));
}

TEST(sema_inout_needs_marker)
{
    auto r = check("fn g(inout x: i32) {\n}\nfn f() {\n var a = 1\n g(a)\n}");
    EXPECT(r->has_error("must be marked with '&'"));
}

TEST(sema_marker_on_read_param)
{
    auto r = check("fn g(x: i32) {\n}\nfn f() {\n var a = 1\n g(&a)\n}");
    EXPECT(r->has_error("is not inout"));
}

TEST(sema_inout_needs_mutable)
{
    auto r = check("fn g(inout x: i32) {\n}\nfn f() {\n let a = 1\n g(&a)\n}");
    EXPECT(r->has_error("not mutable"));
}

TEST(sema_inout_exclusivity)
{
    auto r = check(R"(
fn two(inout a: i32, inout b: i32) {
}
fn mixed(inout a: i32, b: i32) {
}
fn f() {
    var x = 1
    two(&x, &x)
    mixed(&x, x)
}
)");
    EXPECT(r->has_error("passed as inout more than once"));
    EXPECT(r->has_error("passed as inout and also read"));
}

TEST(sema_missing_return)
{
    auto r = check("fn f(a: bool) -> i32 {\n if a {\n return 1\n }\n}");
    EXPECT(r->has_error("must return a value of type 'i32' on every path"));
}

TEST(sema_returns_on_every_path)
{
    auto r = check(R"(
fn sign(a: i32) -> i32 {
    if a > 0 {
        return 1
    } else if a < 0 {
        return -1
    } else {
        return 0
    }
}
fn forever() -> i32 {
    loop {
    }
}
)");
    EXPECT(!r->diagnostics.has_errors());
}

TEST(sema_loop_with_break_needs_return)
{
    auto r = check("fn f() -> i32 {\n loop {\n break\n }\n}");
    EXPECT(r->has_error("on every path"));
}

TEST(sema_break_outside_loop)
{
    auto r = check("fn f() {\n break\n}");
    EXPECT(r->has_error("'break' outside of a loop"));
}

TEST(sema_unknown_names)
{
    auto r = check("fn f() {\n let a = b\n g()\n let c: Foo = 1\n}");
    EXPECT(r->has_error("unknown name 'b'"));
    EXPECT(r->has_error("unknown function 'g'"));
    EXPECT(r->has_error("unknown type 'Foo'"));
}

TEST(sema_redeclaration_and_shadowing)
{
    auto r = check("fn f() {\n let a = 1\n let a = 2\n}\nfn g() {\n let a = 1\n {\n let a = true\n }\n}");
    EXPECT(r->error_count() == 1);
    EXPECT(r->has_error("already declared in this scope"));
}

TEST(sema_expression_statement_must_be_call)
{
    auto r = check("fn f() {\n 1 + 2\n}");
    EXPECT(r->has_error("has no effect"));
}

TEST(sema_interpolation_only_in_print)
{
    auto r = check("fn f() {\n let x = 1\n let s = \"{x}\"\n}");
    EXPECT(r->has_error("string interpolation is only supported"));
}

TEST(sema_main_signature)
{
    auto r = check("fn main(a: i32) -> bool {\n return true\n}");
    EXPECT(r->has_error("'main' must not take parameters"));
    EXPECT(r->has_error("'main' must return nothing or 'i32'"));
}

TEST(sema_unsigned_negation)
{
    auto r = check("fn f(a: u32) {\n let b = -a\n}");
    EXPECT(r->has_error("cannot negate a value of type 'u32'"));
}
