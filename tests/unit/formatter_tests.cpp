#include <optional>
#include <string>

#include "helpers.h"
#include "lang/formatter.h"
#include "test.h"

using namespace hana::test;

namespace {

std::optional<std::string> format(const std::string& text)
{
    hana::SourceFile file{"test.hana", text};
    hana::DiagnosticEngine diagnostics;
    return hana::lang::format_source(file, diagnostics);
}

} // namespace

TEST(formatter_reindents_json_and_code)
{
    const std::string input =
        "{\n"
        "      f: fn '''\n"
        "            if true:\n"
        "                 pass   \n"
        "       ''',\n"
        " g: fn pass,\n"
        "}\n\n\n";
    const std::string expected =
        "{\n"
        "  f: fn '''\n"
        "    if true:\n"
        "      pass\n"
        "  ''',\n"
        "  g: fn pass,\n"
        "}\n";
    const auto result = format(input);
    EXPECT(result && *result == expected);
}

TEST(formatter_is_idempotent)
{
    const std::string input =
        "{\n  a: fn<i64(x: i64,\n        y: i64)> x + y,\n  b: fn '''\n    print(a(1,\n           2))\n  ''',\n}\n";
    const auto once = format(input);
    EXPECT(once.has_value());
    const auto twice = format(*once);
    EXPECT(twice && *twice == *once);
}

TEST(formatter_keeps_comments_and_continuations)
{
    const std::string input =
        "{\n"
        "  // keep me\n"
        "  f: fn '''\n"
        "    while true:\n"
        "    # belongs to the loop body\n"
        "      g(1,\n"
        "        2)   # trailing\n"
        "  ''',\n"
        "}\n";
    const std::string expected =
        "{\n"
        "  // keep me\n"
        "  f: fn '''\n"
        "    while true:\n"
        "      # belongs to the loop body\n"
        "      g(1,\n"
        "        2)   # trailing\n"
        "  ''',\n"
        "}\n";
    const auto result = format(input);
    EXPECT(result && *result == expected);
}

TEST(formatter_keeps_block_comment_lines)
{
    const std::string input = "{\n  /* one\n        two */\n  f: fn pass,\n}\n";
    const auto result = format(input);
    EXPECT(result && *result == input);
}

TEST(formatter_refuses_syntax_errors)
{
    EXPECT(!format("{ f: fn '''\n  let = 1\n''' }").has_value());
}

TEST(formatter_normalizes_line_endings)
{
    const auto result = format("{\r\n  f: fn pass,\r\n}\r\n");
    EXPECT(result && *result == "{\n  f: fn pass,\n}\n");
}

TEST(json_diagnostics_shape)
{
    hana::SourceFile file{"a\"b.hana", ""};
    std::vector<hana::Diagnostic> diagnostics;
    diagnostics.push_back({hana::Severity::Error, {3, 9, 0}, "bad \"thing\"\n"});
    diagnostics.push_back({hana::Severity::Note, {1, 5, 0}, "declared here"});
    const std::string json = hana::diagnostics_to_json(file, diagnostics);
    EXPECT(json.find("\"file\": \"a\\\"b.hana\"") != std::string::npos);
    EXPECT(json.find("\"error_count\": 1") != std::string::npos);
    EXPECT(json.find("\"message\": \"bad \\\"thing\\\"\\n\"") != std::string::npos);
    EXPECT(json.find("\"notes\": [{\"line\": 1, \"column\": 5, \"message\": \"declared here\"}]") !=
           std::string::npos);
}
