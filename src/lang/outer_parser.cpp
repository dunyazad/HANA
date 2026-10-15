#include "lang/outer_parser.h"

#include <utility>

#include "lang/code_parser.h"
#include "support/utf8.h"

namespace hana::lang {

namespace {

bool is_identifier_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_identifier_continue(char c)
{
    return is_identifier_start(c) || (c >= '0' && c <= '9');
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// A one-line body of a function with a result is an expression whose value
// is returned: 'double: fn<i32(x: i32)> x * 2'.
void turn_expression_into_return(Block& block)
{
    if (block.statements.size() != 1 || block.statements.front()->kind != StmtKind::Expr) {
        return;
    }
    auto& stmt = static_cast<ExprStmt&>(*block.statements.front());
    auto ret = std::make_unique<ReturnStmt>(stmt.location);
    ret->value = std::move(stmt.expr);
    block.statements.front() = std::move(ret);
}

} // namespace

OuterParser::OuterParser(const SourceFile& file, DiagnosticEngine& diagnostics)
    : source_(file.text)
    , diagnostics_(diagnostics)
{
    if (source_.substr(0, 3) == "\xEF\xBB\xBF") {
        location_.offset = 3;
    }
}

// ---------------------------------------------------------------------------
// Module and entries

std::unique_ptr<Module> OuterParser::parse()
{
    auto module = std::make_unique<Module>();
    skip_trivia();
    if (at_end()) {
        error(location_, "empty file; a HANA module is an object: { ... }");
        return module;
    }
    if (peek() != '{') {
        error(location_, "a HANA module is an object '{ ... }', found " + describe_here());
        return module;
    }
    advance();
    parse_entries(*module);
    skip_trivia();
    if (!at_end()) {
        error(location_, "unexpected " + describe_here() + " after the module object");
    }
    return module;
}

void OuterParser::parse_entries(Module& module)
{
    for (;;) {
        skip_trivia();
        if (at_end()) {
            error(location_, "expected '}' to close the module object");
            return;
        }
        if (peek() == '}') {
            advance();
            return;
        }
        failed_ = false;
        if (!parse_entry(module) || failed_) {
            skip_value();
        }
        skip_trivia();
        if (peek() == ',') {
            advance();
            continue;
        }
        if (peek() != '}' && !at_end()) {
            error(location_, "expected ',' between entries, found " + describe_here());
            skip_value();
            if (peek() == ',') {
                advance();
            }
        }
    }
}

bool OuterParser::parse_entry(Module& module)
{
    bool is_pub = false;
    if (looking_at_word("pub")) {
        advance_by(3);
        skip_trivia();
        is_pub = true;
    }
    SourceLocation key_location;
    std::optional<std::string> key = parse_key(key_location);
    if (!key) {
        return false;
    }
    skip_trivia();
    if (!expect_char(':', "':' after the key")) {
        return false;
    }
    skip_trivia();
    if (*key == "use") {
        parse_use(key_location);
        return !failed_;
    }
    if (looking_at_word("fn")) {
        auto function = parse_function(*key, key_location, is_pub, nullptr);
        if (function) {
            module.functions.push_back(std::move(function));
            return true;
        }
        return false;
    }
    if (looking_at_word("struct") || looking_at_word("enum")) {
        auto decl = parse_type_decl(*key, key_location, is_pub, looking_at_word("enum"));
        if (decl) {
            module.types.push_back(std::move(decl));
            return true;
        }
        return false;
    }
    for (const char* word : {"trait", "extend"}) {
        if (looking_at_word(word)) {
            error(location_, std::string("'") + word + "' is not supported yet");
            return false;
        }
    }
    error(key_location, "'" + *key + "': only functions, structs, enums and 'use' are supported "
                                     "at the top level for now");
    return false;
}

std::optional<std::string> OuterParser::parse_key(SourceLocation& location)
{
    location = location_;
    if (peek() == '"') {
        return read_json_string();
    }
    if (std::optional<std::string> name = read_identifier()) {
        return name;
    }
    error(location_, "expected a key, found " + describe_here());
    return std::nullopt;
}

void OuterParser::parse_use(const SourceLocation& location)
{
    if (peek() != '[') {
        error(location_, "'use' takes an array of module names: use: [\"std.io\"]");
        return;
    }
    advance();
    skip_trivia();
    bool empty = true;
    while (!at_end() && peek() != ']') {
        empty = false;
        if (peek() != '"' || !read_json_string()) {
            error(location_, "expected a module name string in 'use'");
            return;
        }
        skip_trivia();
        if (peek() == ',') {
            advance();
            skip_trivia();
        }
    }
    if (!expect_char(']', "']' to close 'use'")) {
        return;
    }
    if (!empty) {
        error(location, "modules are not supported yet; only the prelude is available");
        failed_ = false; // the entry itself was read completely
    }
}

// ---------------------------------------------------------------------------
// Structs and enums

// ('struct' | 'enum') ['copy'] '{' members '}'. A struct member is a field
// 'name: Type [= default]', an enum member a case 'Name: { fields }';
// both may have methods 'name: fn...'.
std::unique_ptr<TypeDecl> OuterParser::parse_type_decl(std::string name,
                                                       const SourceLocation& location, bool is_pub,
                                                       bool is_enum)
{
    advance_by(is_enum ? 4 : 6); // 'enum' or 'struct'
    auto decl = std::make_unique<TypeDecl>();
    decl->name = std::move(name);
    decl->location = location;
    decl->is_pub = is_pub;
    decl->is_enum = is_enum;
    const std::string kind = is_enum ? "enum" : "struct";
    skip_trivia();
    if (looking_at_word("copy")) {
        decl->is_copy = true;
        advance_by(4);
        skip_trivia();
    }
    if (peek() == '<') {
        error(location_, "generic " + kind + "s are not supported yet");
        return nullptr;
    }
    if (!expect_char('{', is_enum ? "'{' and the cases of the enum"
                                  : "'{' and the members of the struct")) {
        return nullptr;
    }
    for (;;) {
        skip_trivia();
        if (peek() == '}') {
            advance();
            return decl;
        }
        if (at_end()) {
            error(location_, "expected '}' to close the " + kind + " '" + decl->name + "'");
            return nullptr;
        }
        if (!parse_type_member(*decl) || failed_) {
            skip_rest_of_object();
            return nullptr;
        }
        skip_trivia();
        if (peek() == ',') {
            advance();
        } else if (peek() != '}') {
            error(location_, "expected ',' between " + kind + " members, found " + describe_here());
            skip_rest_of_object();
            return nullptr;
        }
    }
}

// After an error inside an object, skips past its closing '}' so the
// remaining members are not read as entries of the enclosing object.
void OuterParser::skip_rest_of_object()
{
    while (!at_end()) {
        skip_value();
        if (peek() == '}') {
            advance();
            return;
        }
        advance(); // ','
    }
}

bool OuterParser::parse_type_member(TypeDecl& decl)
{
    SourceLocation key_location;
    const bool quoted = peek() == '"';
    std::optional<std::string> key = parse_key(key_location);
    if (!key) {
        return false;
    }
    if (quoted && (key->empty() || !is_identifier_start((*key)[0]))) {
        error(key_location, "operator overloading ('" + *key + "') is not supported yet");
        return false;
    }
    skip_trivia();
    if (!expect_char(':', "':' after the member name")) {
        return false;
    }
    skip_trivia();
    if (looking_at_word("fn")) {
        auto method = parse_function(std::move(*key), key_location, false, &decl);
        if (!method) {
            return false;
        }
        decl.methods.push_back(std::move(method));
        return true;
    }
    if (decl.is_enum) {
        return parse_case(decl, std::move(*key), key_location);
    }
    return parse_field(decl.fields, std::move(*key), key_location);
}

// 'Name: { field: Type, ... }' or 'Name: {}' for a case without data.
bool OuterParser::parse_case(TypeDecl& decl, std::string name, const SourceLocation& location)
{
    if (peek() != '{') {
        error(location_, "an enum case is an object of its fields, as in '" + name +
                             ": { value: i32 }', or '" + name + ": {}' without data");
        return false;
    }
    advance();
    CaseDecl& entry = decl.cases.emplace_back();
    entry.name = std::move(name);
    entry.location = location;
    for (;;) {
        skip_trivia();
        if (peek() == '}') {
            advance();
            return true;
        }
        SourceLocation field_location;
        std::optional<std::string> field = parse_key(field_location);
        if (!field) {
            return false;
        }
        skip_trivia();
        if (!expect_char(':', "':' and a type after the field name")) {
            return false;
        }
        skip_trivia();
        if (!parse_field(entry.fields, std::move(*field), field_location)) {
            return false;
        }
        skip_trivia();
        if (peek() == ',') {
            advance();
        } else if (peek() != '}') {
            error(location_, "expected ',' between fields, found " + describe_here());
            return false;
        }
    }
}

bool OuterParser::parse_field(std::vector<FieldDecl>& fields, std::string name,
                              const SourceLocation& location)
{
    FieldDecl field;
    field.name = std::move(name);
    field.location = location;
    std::optional<TypeExpr> type = parse_type();
    if (!type) {
        return false;
    }
    field.type = std::move(*type);
    skip_spaces_on_line();
    if (peek() == '=') {
        advance();
        skip_spaces_on_line();
        field.default_value = parse_field_default();
        if (!field.default_value) {
            return false;
        }
    }
    fields.push_back(std::move(field));
    return true;
}

// The default value is a one-line expression ending like a one-line body.
ExprPtr OuterParser::parse_field_default()
{
    if (at_end() || peek() == ',' || peek() == '}' || peek() == '\n' || peek() == '\r' ||
        looking_at("//") || looking_at("/*")) {
        error(location_, "expected a default value after '='");
        return nullptr;
    }
    std::optional<CodeRegion> region = parse_inline_code();
    if (!region) {
        return nullptr;
    }
    CodeLexer lexer(source_, *region, diagnostics_);
    const std::vector<Token> tokens = lexer.tokenize();
    CodeParser parser(tokens, diagnostics_);
    return parser.parse_standalone_expression();
}

// ---------------------------------------------------------------------------
// Functions

std::unique_ptr<FnDecl> OuterParser::parse_function(std::string name,
                                                    const SourceLocation& location, bool is_pub,
                                                    TypeDecl* owner)
{
    advance_by(2); // 'fn'
    auto function = std::make_unique<FnDecl>();
    function->name = std::move(name);
    function->location = location;
    function->is_pub = is_pub;
    function->owner = owner;

    Signature signature;
    std::optional<CodeRegion> body;
    skip_spaces_on_line();
    if (peek() == '<') {
        if (!parse_short_signature(signature)) {
            return nullptr;
        }
        body = parse_body();
    } else if (peek() == '{') {
        if (!parse_object_signature(signature, body)) {
            return nullptr;
        }
    } else {
        body = parse_body();
    }
    if (!body) {
        return nullptr;
    }
    if (signature.self_convention && owner == nullptr) {
        error(signature.self_location, "'self' can only be used by a method inside a struct");
        return nullptr;
    }
    function->self_convention = signature.self_convention;
    function->self_location = signature.self_location;
    function->params = std::move(signature.params);
    function->return_type = std::move(signature.return_type);
    function->body = parse_code(*body, function->return_type.has_value() && !body->multiline);
    return function;
}

// '<' [return type] '(' params ')' '>'. The return type is a type name, or
// a parenthesized list of types for several results.
bool OuterParser::parse_short_signature(Signature& signature)
{
    advance(); // '<'
    skip_trivia();
    if (peek() == '(') {
        if (paren_group_followed_by_paren()) {
            error(location_, "several return values (tuples) are not supported yet");
            return false;
        }
        if (!parse_param_group(signature)) {
            return false;
        }
    } else {
        std::optional<TypeExpr> result = parse_type();
        if (!result) {
            return false;
        }
        if (result->name != "void") {
            signature.return_type = std::move(result);
        }
        skip_trivia();
        if (peek() != '(') {
            error(location_, "expected '(' and the parameters after the return type, found " +
                                 describe_here());
            return false;
        }
        if (!parse_param_group(signature)) {
            return false;
        }
    }
    skip_trivia();
    return expect_char('>', "'>' to close the function signature");
}

// fn { in: { name: type, ... }, out: type, do: body }
bool OuterParser::parse_object_signature(Signature& signature, std::optional<CodeRegion>& body)
{
    advance(); // '{'
    for (;;) {
        skip_trivia();
        if (peek() == '}') {
            advance();
            break;
        }
        SourceLocation key_location;
        std::optional<std::string> key = parse_key(key_location);
        if (!key) {
            return false;
        }
        skip_trivia();
        if (!expect_char(':', "':' after the key")) {
            return false;
        }
        skip_trivia();
        if (*key == "in") {
            if (peek() != '{') {
                error(location_, "'in' takes an object of parameters: in: { x: i32 }");
                return false;
            }
            advance();
            for (;;) {
                skip_trivia();
                if (peek() == '}') {
                    advance();
                    break;
                }
                if (!parse_signature_param(signature)) {
                    return false;
                }
                skip_trivia();
                if (peek() == ',') {
                    advance();
                }
            }
        } else if (*key == "out") {
            signature.return_type = parse_type();
            if (!signature.return_type) {
                return false;
            }
            if (signature.return_type->name == "void") {
                signature.return_type.reset();
            }
        } else if (*key == "do") {
            body = parse_body();
            if (!body) {
                return false;
            }
        } else {
            error(key_location, "unknown function property '" + *key + "'; expected in, out or do");
            return false;
        }
        skip_trivia();
        if (peek() == ',') {
            advance();
        }
    }
    if (!body) {
        error(location_, "the function has no 'do' body");
        return false;
    }
    return true;
}

bool OuterParser::parse_param_group(Signature& signature)
{
    advance(); // '('
    skip_trivia();
    if (peek() == ')') {
        advance();
        return true;
    }
    for (;;) {
        if (!parse_signature_param(signature)) {
            return false;
        }
        skip_trivia();
        if (peek() == ',') {
            advance();
            skip_trivia();
            if (peek() == ')') {
                advance();
                return true;
            }
            continue;
        }
        return expect_char(')', "')' after the parameters");
    }
}

// Adds one parameter to the signature. 'self' is kept apart from the other
// parameters and must come first.
bool OuterParser::parse_signature_param(Signature& signature)
{
    Param param;
    bool is_self = false;
    if (!parse_param(param, is_self)) {
        return false;
    }
    if (!is_self) {
        signature.params.push_back(std::move(param));
        return true;
    }
    if (signature.self_convention || !signature.params.empty()) {
        error(param.location, "'self' must be the first parameter");
        return false;
    }
    signature.self_convention = param.convention;
    signature.self_location = param.location;
    return true;
}

// [inout | sink] name ':' type, or [inout | sink] 'self' without a type.
bool OuterParser::parse_param(Param& param, bool& is_self)
{
    if (looking_at_word("inout")) {
        param.convention = ParamConvention::Inout;
        advance_by(5);
        skip_trivia();
    } else if (looking_at_word("sink")) {
        param.convention = ParamConvention::Sink;
        advance_by(4);
        skip_trivia();
    }
    param.location = location_;
    std::optional<std::string> name = read_identifier();
    if (!name) {
        error(location_, "expected a parameter name, found " + describe_here());
        return false;
    }
    param.name = std::move(*name);
    skip_trivia();
    if (param.name == "self") {
        if (peek() == ':') {
            error(location_, "'self' has no type; it is always the struct the method belongs to");
            return false;
        }
        is_self = true;
        return true;
    }
    if (!expect_char(':', "':' and a type after the parameter name")) {
        return false;
    }
    skip_trivia();
    std::optional<TypeExpr> type = parse_type();
    if (!type) {
        return false;
    }
    param.type = std::move(*type);
    return true;
}

std::optional<TypeExpr> OuterParser::parse_type()
{
    const SourceLocation location = location_;
    std::optional<std::string> name = read_identifier();
    if (!name) {
        if (peek() == '*' || peek() == '(' || peek() == '[') {
            error(location_, "this kind of type is not supported yet");
        } else {
            error(location_, "expected a type, found " + describe_here());
        }
        return std::nullopt;
    }
    if (peek() == '<') {
        error(location_, "generic types are not supported yet");
        return std::nullopt;
    }
    return TypeExpr{std::move(*name), location};
}

// ---------------------------------------------------------------------------
// Code regions

std::optional<CodeRegion> OuterParser::parse_body()
{
    skip_spaces_on_line();
    if (looking_at("'''")) {
        return parse_code_block();
    }
    if (at_end() || peek() == ',' || peek() == '}' || peek() == '\n' || peek() == '\r' ||
        looking_at("//") || looking_at("/*")) {
        error(location_, "expected a function body: a ''' block or a one-line statement");
        return std::nullopt;
    }
    return parse_inline_code();
}

std::optional<CodeRegion> OuterParser::parse_code_block()
{
    const SourceLocation open_location = location_;
    advance_by(3);
    const std::size_t close = find_on_line("'''");
    if (close != std::string_view::npos) {
        // One-line block: ''' count = 0 '''
        CodeRegion region;
        region.begin = location_.offset;
        region.start = location_;
        region.end = static_cast<std::uint32_t>(close);
        while (location_.offset < close) {
            advance();
        }
        advance_by(3);
        regions_.push_back(region);
        return region;
    }
    return parse_multiline_block(open_location);
}

std::optional<CodeRegion> OuterParser::parse_multiline_block(const SourceLocation& open_location)
{
    skip_spaces_on_line();
    if (peek() == '#') {
        while (!at_end() && peek() != '\n') {
            advance();
        }
    }
    if (!at_end() && peek() != '\n' && !(peek() == '\r' && peek(1) == '\n')) {
        error(location_, "code in a multi-line ''' block starts on the next line");
        return std::nullopt;
    }
    while (!at_end() && peek() != '\n') {
        advance();
    }
    if (at_end()) {
        error(open_location, "unterminated ''' block");
        return std::nullopt;
    }
    advance(); // newline after the opening '''

    CodeRegion region;
    region.multiline = true;
    region.begin = location_.offset;
    region.start = location_;
    for (;;) {
        if (at_end()) {
            error(open_location, "unterminated ''' block");
            return std::nullopt;
        }
        const std::size_t line_begin = location_.offset;
        std::uint32_t spaces = 0;
        while (peek(spaces) == ' ') {
            ++spaces;
        }
        if (source_.substr(line_begin + spaces, 3) == "'''") {
            region.end = static_cast<std::uint32_t>(line_begin);
            region.base_indent = spaces;
            advance_by(spaces + 3);
            regions_.push_back(region);
            return region;
        }
        const std::size_t inner = find_on_line("'''");
        if (inner != std::string_view::npos) {
            SourceLocation at = location_;
            while (location_.offset < inner) {
                advance();
            }
            at = location_;
            error(at, "the closing ''' of a multi-line block must be on its own line");
            return std::nullopt;
        }
        while (!at_end() && peek() != '\n') {
            advance();
        }
        if (!at_end()) {
            advance();
        }
    }
}

// A one-line value ends at ',', '}' or ']' outside brackets, at the end of
// the line, or at a '//' or '/*' comment.
std::optional<CodeRegion> OuterParser::parse_inline_code()
{
    CodeRegion region;
    region.begin = location_.offset;
    region.start = location_;
    std::uint32_t last_code_end = location_.offset;
    int depth = 0;
    while (!at_end()) {
        const char c = peek();
        if (depth == 0 && (c == ',' || c == '}' || c == ']' || c == '\n' || c == '\r' ||
                           looking_at("//") || looking_at("/*"))) {
            break;
        }
        if (c == '"' || c == '\'') {
            if (c == '\'' && looking_at("'''")) {
                error(location_, "''' cannot appear inside a one-line value");
                return std::nullopt;
            }
            skip_string(c);
            last_code_end = location_.offset;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            --depth;
        }
        advance();
        if (c != ' ' && c != '\t') {
            last_code_end = location_.offset;
        }
    }
    region.end = last_code_end;
    regions_.push_back(region);
    return region;
}

Block OuterParser::parse_code(const CodeRegion& region, bool returns_value)
{
    CodeLexer lexer(source_, region, diagnostics_);
    const std::vector<Token> tokens = lexer.tokenize();
    CodeParser parser(tokens, diagnostics_);
    Block block = parser.parse_body();
    if (returns_value) {
        turn_expression_into_return(block);
    }
    return block;
}

// ---------------------------------------------------------------------------
// Scanning helpers

void OuterParser::skip_trivia()
{
    for (;;) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            advance();
        } else if (!skip_comment()) {
            return;
        }
    }
}

void OuterParser::skip_spaces_on_line()
{
    while (peek() == ' ' || peek() == '\t') {
        advance();
    }
}

bool OuterParser::skip_comment()
{
    if (looking_at("//")) {
        while (!at_end() && peek() != '\n') {
            advance();
        }
        return true;
    }
    if (looking_at("/*")) {
        const SourceLocation start = location_;
        advance_by(2);
        while (!at_end() && !looking_at("*/")) {
            advance();
        }
        if (at_end()) {
            error(start, "unterminated '/*' comment");
            return true;
        }
        advance_by(2);
        return true;
    }
    return false;
}

std::optional<std::string> OuterParser::read_identifier()
{
    if (!is_identifier_start(peek())) {
        return std::nullopt;
    }
    std::string name;
    while (is_identifier_continue(peek())) {
        name += advance();
    }
    return name;
}

// JSON string with its escapes: \" \\ \/ \b \f \n \r \t \uXXXX.
std::optional<std::string> OuterParser::read_json_string()
{
    const SourceLocation start = location_;
    advance(); // opening quote
    std::string text;
    for (;;) {
        if (at_end() || peek() == '\n') {
            error(start, "unterminated string");
            return std::nullopt;
        }
        const char c = advance();
        if (c == '"') {
            return text;
        }
        if (c != '\\') {
            text += c;
            continue;
        }
        const char e = advance();
        switch (e) {
        case '"': text += '"'; break;
        case '\\': text += '\\'; break;
        case '/': text += '/'; break;
        case 'b': text += '\b'; break;
        case 'f': text += '\f'; break;
        case 'n': text += '\n'; break;
        case 'r': text += '\r'; break;
        case 't': text += '\t'; break;
        case 'u': {
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i) {
                const int digit = hex_value(peek());
                if (digit < 0) {
                    error(location_, "invalid \\u escape in string");
                    return std::nullopt;
                }
                value = value * 16 + static_cast<std::uint32_t>(digit);
                advance();
            }
            append_utf8(text, value);
            break;
        }
        default:
            error(location_, "invalid escape sequence in string");
            return std::nullopt;
        }
    }
}

bool OuterParser::looking_at(std::string_view text) const
{
    return source_.substr(location_.offset, text.size()) == text;
}

bool OuterParser::looking_at_word(std::string_view word) const
{
    return looking_at(word) && !is_identifier_continue(peek(word.size()));
}

bool OuterParser::expect_char(char c, const char* what)
{
    if (peek() == c) {
        advance();
        return true;
    }
    error(location_, std::string("expected ") + what + ", found " + describe_here());
    return false;
}

// Skips the rest of a broken entry: up to a ',' or '}' outside any bracket,
// string or ''' block.
void OuterParser::skip_value()
{
    int depth = 0;
    while (!at_end()) {
        const char c = peek();
        if (depth <= 0 && (c == ',' || c == '}')) {
            return;
        }
        if (looking_at("'''")) {
            skip_code_block();
        } else if (c == '"') {
            skip_string('"');
        } else if (!skip_comment()) {
            if (c == '{' || c == '[' || c == '(') {
                ++depth;
            } else if (c == '}' || c == ']' || c == ')') {
                --depth;
            }
            advance();
        }
    }
}

void OuterParser::skip_string(char quote)
{
    advance();
    while (!at_end() && peek() != '\n') {
        const char c = advance();
        if (c == '\\' && !at_end()) {
            advance();
        } else if (c == quote) {
            return;
        }
    }
}

void OuterParser::skip_code_block()
{
    advance_by(3);
    while (!at_end() && !looking_at("'''")) {
        advance();
    }
    advance_by(3);
}

// At '(': true when the matching ')' is followed by another '(', as in
// fn<(i32, i32)(a: i32)> where the first group lists the result types.
bool OuterParser::paren_group_followed_by_paren() const
{
    int depth = 0;
    std::size_t index = location_.offset;
    for (; index < source_.size(); ++index) {
        if (source_[index] == '(') {
            ++depth;
        } else if (source_[index] == ')' && --depth == 0) {
            break;
        }
    }
    for (++index; index < source_.size(); ++index) {
        const char c = source_[index];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return c == '(';
        }
    }
    return false;
}

std::size_t OuterParser::find_on_line(std::string_view text) const
{
    for (std::size_t i = location_.offset; i < source_.size() && source_[i] != '\n'; ++i) {
        if (source_.substr(i, text.size()) == text) {
            return i;
        }
    }
    return std::string_view::npos;
}

void OuterParser::error(const SourceLocation& location, const std::string& message)
{
    if (!failed_) {
        diagnostics_.error(location, message);
    }
    failed_ = true;
}

std::string OuterParser::describe_here() const
{
    if (at_end()) {
        return "end of file";
    }
    if (peek() == '\n' || peek() == '\r') {
        return "end of line";
    }
    std::size_t length = 1;
    while (location_.offset + length < source_.size() &&
           is_utf8_continuation(source_[location_.offset + length])) {
        ++length;
    }
    return "'" + std::string(source_.substr(location_.offset, length)) + "'";
}

bool OuterParser::at_end() const
{
    return location_.offset >= source_.size();
}

char OuterParser::peek(std::size_t ahead) const
{
    const std::size_t index = location_.offset + ahead;
    return index < source_.size() ? source_[index] : '\0';
}

char OuterParser::advance()
{
    if (at_end()) {
        return '\0';
    }
    const char c = source_[location_.offset];
    ++location_.offset;
    if (c == '\n') {
        ++location_.line;
        location_.column = 1;
    } else if (!is_utf8_continuation(c)) {
        ++location_.column;
    }
    return c;
}

void OuterParser::advance_by(std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        advance();
    }
}

} // namespace hana::lang
