#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lang/code_lexer.h"
#include "lang/code_parser.h"
#include "lang/outer_parser.h"
#include "lang/sema.h"
#include "support/diagnostic.h"

namespace hana::test {

struct Compiled {
    SourceFile file;
    DiagnosticEngine diagnostics;
    std::vector<lang::Token> tokens;
    std::unique_ptr<lang::Module> module;
    std::unique_ptr<lang::Program> program;

    bool has_error(std::string_view needle) const
    {
        for (const Diagnostic& diagnostic : diagnostics.diagnostics()) {
            if (diagnostic.severity == Severity::Error &&
                diagnostic.message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::size_t error_count() const
    {
        std::size_t count = 0;
        for (const Diagnostic& diagnostic : diagnostics.diagnostics()) {
            count += diagnostic.severity == Severity::Error ? 1 : 0;
        }
        return count;
    }
};

inline std::vector<lang::TokenKind> kinds_of(const std::vector<lang::Token>& tokens)
{
    std::vector<lang::TokenKind> kinds;
    for (const lang::Token& token : tokens) {
        kinds.push_back(token.kind);
    }
    return kinds;
}

// Lexes 'code' as the inside of a multi-line ''' block whose closing ''' has
// no indentation.
inline std::unique_ptr<Compiled> lex_block(std::string_view code)
{
    auto result = std::make_unique<Compiled>();
    result->file.text = std::string(code);
    lang::CodeRegion region;
    region.end = static_cast<std::uint32_t>(result->file.text.size());
    region.multiline = true;
    lang::CodeLexer lexer(result->file.text, region, result->diagnostics);
    result->tokens = lexer.tokenize();
    return result;
}

// Lexes 'code' as a one-line value.
inline std::unique_ptr<Compiled> lex_line(std::string_view code)
{
    auto result = std::make_unique<Compiled>();
    result->file.text = std::string(code);
    lang::CodeRegion region;
    region.end = static_cast<std::uint32_t>(result->file.text.size());
    lang::CodeLexer lexer(result->file.text, region, result->diagnostics);
    result->tokens = lexer.tokenize();
    return result;
}

inline std::unique_ptr<Compiled> parse(std::string_view source)
{
    auto result = std::make_unique<Compiled>();
    result->file.path = "test.hana";
    result->file.text = std::string(source);
    lang::OuterParser parser(result->file, result->diagnostics);
    result->module = parser.parse();
    return result;
}

// Runs semantic analysis only when parsing succeeded, like the driver.
inline std::unique_ptr<Compiled> check(std::string_view source)
{
    auto result = parse(source);
    if (!result->diagnostics.has_errors()) {
        lang::Sema sema(result->diagnostics);
        result->program = sema.check(*result->module);
    }
    return result;
}

} // namespace hana::test
