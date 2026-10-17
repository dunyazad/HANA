#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lang/ast.h"
#include "lang/code_lexer.h"
#include "support/diagnostic.h"

namespace hana::lang {

// Reads the JSON layer of a module: the top-level object, its keys and the
// function headers, and hands every code region to CodeLexer/CodeParser.
//
// Supported at this stage: 'use', function and struct entries. A function is
//   name: fn<R(params)> body      short form ('void' or nothing for no result)
//   name: fn { in: {...}, out: T, do: body }   object form
//   name: fn body                 no parameters, no result
// where body is a ''' block or a one-line code value. A struct or enum is
//   Name: struct { field: T, field: T = default, method: fn... }
//   Name: enum { Case: { field: T }, Empty: {}, method: fn... }
//   Name: struct copy { ... }     a Copy type (also 'enum copy')
// and its methods may take 'self', 'inout self' or 'sink self' first.
class OuterParser {
public:
    OuterParser(const SourceFile& file, DiagnosticEngine& diagnostics);

    std::unique_ptr<Module> parse();

    // Code regions found while parsing, in source order (for --tokens).
    const std::vector<CodeRegion>& code_regions() const { return regions_; }

private:
    struct Signature {
        std::vector<Param> params;
        std::optional<TypeExpr> return_type;
        std::optional<ParamConvention> self_convention;
        SourceLocation self_location;
    };

    void parse_entries(Module& module);
    bool parse_entry(Module& module);
    std::optional<std::string> parse_key(SourceLocation& location);
    void parse_use(const SourceLocation& location);
    std::unique_ptr<TypeDecl> parse_type_decl(std::string name, const SourceLocation& location,
                                              bool is_pub, bool is_enum);
    bool parse_type_member(TypeDecl& decl);
    bool parse_case(TypeDecl& decl, std::string name, const SourceLocation& location);
    bool parse_field(std::vector<FieldDecl>& fields, std::string name,
                     const SourceLocation& location);
    ExprPtr parse_field_default();
    std::unique_ptr<FnDecl> parse_function(std::string name, const SourceLocation& location,
                                           bool is_pub, TypeDecl* owner);
    bool parse_short_signature(Signature& signature);
    bool parse_object_signature(Signature& signature, std::optional<CodeRegion>& body);
    bool parse_param_group(Signature& signature);
    bool parse_signature_param(Signature& signature);
    bool parse_param(Param& param, bool& is_self);
    std::optional<TypeExpr> parse_type();
    std::optional<TypeExpr> parse_type_without_suffix();
    std::optional<TypeExpr> parse_tuple_type();
    std::optional<CodeRegion> parse_body();
    std::optional<CodeRegion> parse_code_block();
    std::optional<CodeRegion> parse_multiline_block(const SourceLocation& open_location);
    std::optional<CodeRegion> parse_inline_code();
    Block parse_code(const CodeRegion& region, bool returns_value);

    // Scanning helpers
    void skip_trivia();
    void skip_spaces_on_line();
    bool skip_comment();
    std::optional<std::string> read_identifier();
    std::optional<std::string> read_json_string();
    bool looking_at(std::string_view text) const;
    bool looking_at_word(std::string_view word) const;
    bool expect_char(char c, const char* what);
    void skip_value();
    void skip_rest_of_object();
    void skip_string(char quote);
    void skip_code_block();
    std::size_t find_on_line(std::string_view text) const;
    bool paren_group_followed_by_paren() const;

    void error(const SourceLocation& location, const std::string& message);
    std::string describe_here() const;

    bool at_end() const;
    char peek(std::size_t ahead = 0) const;
    char advance();
    void advance_by(std::size_t count);

    std::string_view source_;
    DiagnosticEngine& diagnostics_;
    SourceLocation location_;
    std::vector<CodeRegion> regions_;
    // Set after an error inside an entry; the entry is skipped.
    bool failed_ = false;
};

} // namespace hana::lang
