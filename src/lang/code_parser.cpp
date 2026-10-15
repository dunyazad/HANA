#include "lang/code_parser.h"

#include <cstdint>
#include <cstdlib>
#include <utility>

#include "support/utf8.h"

namespace hana::lang {

namespace {

// Binary operator levels above comparison, loosest first:
// '|', '^', '&', shifts, additive, multiplicative.
constexpr int kBinaryLevels = 6;

std::optional<BinaryOp> binary_op_at_level(int level, TokenKind kind)
{
    switch (level) {
    case 0: return kind == TokenKind::Pipe ? std::optional(BinaryOp::BitOr) : std::nullopt;
    case 1: return kind == TokenKind::Caret ? std::optional(BinaryOp::BitXor) : std::nullopt;
    case 2: return kind == TokenKind::Amp ? std::optional(BinaryOp::BitAnd) : std::nullopt;
    case 3:
        if (kind == TokenKind::Shl) {
            return BinaryOp::Shl;
        }
        return kind == TokenKind::Shr ? std::optional(BinaryOp::Shr) : std::nullopt;
    case 4:
        if (kind == TokenKind::Plus) {
            return BinaryOp::Add;
        }
        return kind == TokenKind::Minus ? std::optional(BinaryOp::Sub) : std::nullopt;
    default:
        if (kind == TokenKind::Star) {
            return BinaryOp::Mul;
        }
        if (kind == TokenKind::Slash) {
            return BinaryOp::Div;
        }
        return kind == TokenKind::Percent ? std::optional(BinaryOp::Rem) : std::nullopt;
    }
}

std::optional<BinaryOp> comparison_op(TokenKind kind)
{
    switch (kind) {
    case TokenKind::EqEq: return BinaryOp::Eq;
    case TokenKind::BangEq: return BinaryOp::Ne;
    case TokenKind::Lt: return BinaryOp::Lt;
    case TokenKind::LtEq: return BinaryOp::Le;
    case TokenKind::Gt: return BinaryOp::Gt;
    case TokenKind::GtEq: return BinaryOp::Ge;
    default: return std::nullopt;
    }
}

bool is_assignment_token(TokenKind kind)
{
    switch (kind) {
    case TokenKind::Eq:
    case TokenKind::PlusEq:
    case TokenKind::MinusEq:
    case TokenKind::StarEq:
    case TokenKind::SlashEq:
    case TokenKind::PercentEq:
    case TokenKind::AmpEq:
    case TokenKind::PipeEq:
    case TokenKind::CaretEq:
    case TokenKind::ShlEq:
    case TokenKind::ShrEq:
        return true;
    default:
        return false;
    }
}

std::optional<BinaryOp> compound_assignment_op(TokenKind kind)
{
    switch (kind) {
    case TokenKind::PlusEq: return BinaryOp::Add;
    case TokenKind::MinusEq: return BinaryOp::Sub;
    case TokenKind::StarEq: return BinaryOp::Mul;
    case TokenKind::SlashEq: return BinaryOp::Div;
    case TokenKind::PercentEq: return BinaryOp::Rem;
    case TokenKind::AmpEq: return BinaryOp::BitAnd;
    case TokenKind::PipeEq: return BinaryOp::BitOr;
    case TokenKind::CaretEq: return BinaryOp::BitXor;
    case TokenKind::ShlEq: return BinaryOp::Shl;
    case TokenKind::ShrEq: return BinaryOp::Shr;
    default: return std::nullopt;
    }
}

// Parses digits with an optional 0x / 0b prefix and '_' separators.
// Returns false when the value does not fit in 64 bits.
bool parse_integer_text(const std::string& text, std::uint64_t& value)
{
    std::size_t index = 0;
    unsigned radix = 10;
    if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        radix = 16;
        index = 2;
    } else if (text.size() > 1 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        radix = 2;
        index = 2;
    }
    value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '_') {
            continue;
        }
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        } else {
            digit = static_cast<unsigned>(c - 'A' + 10);
        }
        if (value > (UINT64_MAX - digit) / radix) {
            return false;
        }
        value = value * radix + digit;
    }
    return true;
}

std::string without_underscores(const std::string& text)
{
    std::string out;
    for (char c : text) {
        if (c != '_') {
            out += c;
        }
    }
    return out;
}

} // namespace

CodeParser::CodeParser(const std::vector<Token>& tokens, DiagnosticEngine& diagnostics)
    : tokens_(tokens)
    , diagnostics_(diagnostics)
{
}

// ---------------------------------------------------------------------------
// Statements

Block CodeParser::parse_body()
{
    Block block;
    block.location = peek().location;
    parse_statements_until(TokenKind::EndOfInput, block);
    block.end_location = peek().location;
    return block;
}

ExprPtr CodeParser::parse_standalone_expression()
{
    ExprPtr expr = parse_expression();
    while (match(TokenKind::Newline)) {
    }
    if (!panic_ && !check(TokenKind::EndOfInput)) {
        error_at(peek(), "expected end of the value, found " + describe_token(peek()));
    }
    return expr;
}

void CodeParser::parse_statements_until(TokenKind end, Block& block)
{
    for (;;) {
        while (match(TokenKind::Newline)) {
        }
        if (check(end) || check(TokenKind::EndOfInput)) {
            return;
        }
        if (check(TokenKind::Indent)) {
            error_at(peek(), "unexpected indentation");
            synchronize();
            continue;
        }
        const std::size_t before = position_;
        StmtPtr statement = parse_statement();
        if (statement) {
            block.statements.push_back(std::move(statement));
        }
        if (panic_) {
            synchronize();
        }
        if (position_ == before) {
            advance();
        }
    }
}

StmtPtr CodeParser::parse_statement()
{
    switch (peek().kind) {
    case TokenKind::KwIf: return parse_if();
    case TokenKind::KwWhile: return parse_while();
    case TokenKind::KwFor:
        error_at(peek(), "'for' loops are not supported yet");
        return nullptr;
    case TokenKind::KwMatch: {
        std::unique_ptr<MatchExpr> match = parse_match(false);
        if (!match) {
            return nullptr;
        }
        return std::make_unique<MatchStmt>(std::move(match));
    }
    case TokenKind::KwElif:
    case TokenKind::KwElse:
        error_at(peek(), "'" + peek().text + "' without a matching 'if'");
        return nullptr;
    default: break;
    }
    StmtPtr statement = parse_simple_statement();
    if (statement && !panic_) {
        expect_line_end();
    }
    return statement;
}

StmtPtr CodeParser::parse_simple_statement()
{
    switch (peek().kind) {
    case TokenKind::KwLet:
    case TokenKind::KwVar: return parse_let();
    case TokenKind::KwReturn: return parse_return();
    case TokenKind::KwBreak: return std::make_unique<SimpleStmt>(StmtKind::Break, advance().location);
    case TokenKind::KwContinue:
        return std::make_unique<SimpleStmt>(StmtKind::Continue, advance().location);
    case TokenKind::KwPass: return std::make_unique<SimpleStmt>(StmtKind::Pass, advance().location);
    case TokenKind::KwIf:
    case TokenKind::KwWhile:
        error_at(peek(), "'" + peek().text + "' must start its own line");
        return nullptr;
    default: return parse_expression_statement();
    }
}

StmtPtr CodeParser::parse_let()
{
    auto let = std::make_unique<LetStmt>(peek().location);
    let->is_mutable = advance().kind == TokenKind::KwVar;
    let->name_location = peek().location;
    if (!check(TokenKind::Identifier)) {
        error_at(peek(), "expected a variable name, found " + describe_token(peek()));
        return nullptr;
    }
    let->name = advance().text;
    if (match(TokenKind::Colon)) {
        let->declared_type = parse_type();
        if (!let->declared_type) {
            return nullptr;
        }
    }
    if (!check(TokenKind::Eq)) {
        error_at(peek(), std::string("'") + (let->is_mutable ? "var" : "let") +
                             "' needs an initial value: '= <expression>'");
        return nullptr;
    }
    advance();
    let->init = parse_value();
    return let;
}

StmtPtr CodeParser::parse_return()
{
    auto ret = std::make_unique<ReturnStmt>(advance().location);
    if (!at_line_end()) {
        ret->value = parse_value();
    }
    return ret;
}

StmtPtr CodeParser::parse_if()
{
    auto stmt = std::make_unique<IfStmt>(advance().location);
    stmt->condition = parse_expression();
    if (!parse_suite(stmt->then_block)) {
        return nullptr;
    }
    if (check(TokenKind::KwElif)) {
        Block else_block;
        else_block.location = peek().location;
        StmtPtr nested = parse_if();
        if (!nested) {
            return nullptr;
        }
        else_block.end_location = nested->location;
        else_block.statements.push_back(std::move(nested));
        stmt->else_block = std::move(else_block);
    } else if (check(TokenKind::KwElse)) {
        advance();
        Block else_block;
        if (!parse_suite(else_block)) {
            return nullptr;
        }
        stmt->else_block = std::move(else_block);
    }
    return stmt;
}

StmtPtr CodeParser::parse_while()
{
    auto stmt = std::make_unique<WhileStmt>(advance().location);
    stmt->condition = parse_expression();
    if (!parse_suite(stmt->body)) {
        return nullptr;
    }
    return stmt;
}

// suite: ':' NEWLINE INDENT statements DEDENT   or   ':' simple_statement NEWLINE
bool CodeParser::parse_suite(Block& block)
{
    if (!expect(TokenKind::Colon, "':' to start the block")) {
        return false;
    }
    block.location = peek().location;
    if (!check(TokenKind::Newline)) {
        StmtPtr statement = parse_simple_statement();
        if (!statement || panic_) {
            return false;
        }
        block.statements.push_back(std::move(statement));
        block.end_location = peek().location;
        return expect_line_end();
    }
    advance();
    while (match(TokenKind::Newline)) {
    }
    if (!check(TokenKind::Indent)) {
        error_at(peek(), "expected an indented block after ':'");
        return false;
    }
    advance();
    parse_statements_until(TokenKind::Dedent, block);
    block.end_location = peek().location;
    match(TokenKind::Dedent);
    return true;
}

StmtPtr CodeParser::parse_expression_statement()
{
    ExprPtr expr = parse_expression();
    if (!is_assignment_token(peek().kind)) {
        return std::make_unique<ExprStmt>(std::move(expr));
    }
    auto assign = std::make_unique<AssignStmt>(expr->location);
    assign->op_location = peek().location;
    assign->op = compound_assignment_op(advance().kind);
    assign->target = std::move(expr);
    assign->value = parse_value();
    return assign;
}

bool CodeParser::expect_line_end()
{
    if (at_line_end()) {
        match(TokenKind::Newline);
        return true;
    }
    if (position_ > 0 && tokens_[position_ - 1].kind == TokenKind::Dedent) {
        return true; // a 'match' expression ended the line with its arms
    }
    error_at(peek(), "expected end of line, found " + describe_token(peek()));
    return false;
}

// ---------------------------------------------------------------------------
// match

// The value of 'let', an assignment or 'return': an expression, or a
// 'match' whose arms follow on indented lines.
ExprPtr CodeParser::parse_value()
{
    if (!check(TokenKind::KwMatch)) {
        return parse_expression();
    }
    std::unique_ptr<MatchExpr> match = parse_match(true);
    if (!match) {
        return std::make_unique<ErrorExpr>(peek().location);
    }
    return match;
}

// 'match' subject ':' NEWLINE INDENT arm+ DEDENT
std::unique_ptr<MatchExpr> CodeParser::parse_match(bool is_expression)
{
    auto match_expr = std::make_unique<MatchExpr>(advance().location);
    match_expr->subject = parse_expression();
    if (!expect(TokenKind::Colon, "':' after the value of 'match'")) {
        return nullptr;
    }
    if (!check(TokenKind::Newline)) {
        error_at(peek(), "the arms of 'match' go on the following lines, indented");
        return nullptr;
    }
    while (match(TokenKind::Newline)) {
    }
    if (!match(TokenKind::Indent)) {
        error_at(peek(), "expected the indented arms of 'match'");
        return nullptr;
    }
    while (!check(TokenKind::Dedent) && !check(TokenKind::EndOfInput)) {
        if (match(TokenKind::Newline)) {
            continue;
        }
        const std::size_t before = position_;
        if (!parse_match_arm(*match_expr, is_expression) || panic_) {
            synchronize();
        }
        if (position_ == before) {
            advance();
        }
    }
    match(TokenKind::Dedent);
    return match_expr;
}

// pattern ['if' guard] ':' (value | block)
bool CodeParser::parse_match_arm(MatchExpr& match_expr, bool is_expression)
{
    MatchArm arm;
    arm.location = peek().location;
    arm.pattern = parse_pattern();
    if (!arm.pattern) {
        return false;
    }
    if (match(TokenKind::KwIf)) {
        arm.guard = parse_or();
    }
    if (is_expression) {
        if (!expect(TokenKind::Colon, "':' and the value of the arm")) {
            return false;
        }
        arm.value = parse_expression();
        if (panic_ || !expect_line_end()) {
            return false;
        }
    } else if (!parse_suite(arm.body)) {
        return false;
    }
    match_expr.arms.push_back(std::move(arm));
    return true;
}

// '_', a name, a literal, a range 'a..b' / 'a..=b', or an enum case
// 'Case' / 'Case(p, ...)' whose patterns match its fields in order.
PatternPtr CodeParser::parse_pattern()
{
    auto pattern = std::make_unique<Pattern>();
    pattern->location = peek().location;
    if (check(TokenKind::Identifier)) {
        pattern->name = advance().text;
        if (pattern->name == "_") {
            pattern->kind = PatternKind::Wildcard;
            return pattern;
        }
        if (!match(TokenKind::LParen)) {
            pattern->kind = PatternKind::Binding;
            return pattern;
        }
        pattern->kind = PatternKind::Case;
        pattern->has_arguments = true;
        while (!check(TokenKind::RParen)) {
            PatternPtr argument = parse_pattern();
            if (!argument) {
                return nullptr;
            }
            pattern->arguments.push_back(std::move(argument));
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        if (!expect(TokenKind::RParen, "')' after the patterns of the case")) {
            return nullptr;
        }
        return pattern;
    }
    pattern->value = parse_pattern_literal();
    if (!pattern->value) {
        return nullptr;
    }
    pattern->kind = PatternKind::Literal;
    if (check(TokenKind::DotDot) || check(TokenKind::DotDotEq)) {
        pattern->kind = PatternKind::Range;
        pattern->inclusive = advance().kind == TokenKind::DotDotEq;
        pattern->end = parse_pattern_literal();
        if (!pattern->end) {
            return nullptr;
        }
    }
    return pattern;
}

ExprPtr CodeParser::parse_pattern_literal()
{
    const Token& token = peek();
    switch (token.kind) {
    case TokenKind::Minus: {
        advance();
        if (!check(TokenKind::IntLiteral)) {
            error_at(peek(), "expected an integer after '-' in a pattern");
            return nullptr;
        }
        return std::make_unique<UnaryExpr>(token.location, UnaryOp::Neg, parse_int_literal());
    }
    case TokenKind::IntLiteral: return parse_int_literal();
    case TokenKind::CharLiteral: return parse_char_literal();
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
        advance();
        return std::make_unique<BoolLiteralExpr>(token.location, token.kind == TokenKind::KwTrue);
    case TokenKind::FloatLiteral:
        error_at(token, "floating-point values cannot be used as patterns");
        return nullptr;
    case TokenKind::StringLiteral:
        error_at(token, "'str' patterns are not supported yet");
        return nullptr;
    default:
        error_at(token, "expected a pattern, found " + describe_token(token));
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Expressions

ExprPtr CodeParser::parse_expression()
{
    ExprPtr value = parse_or();
    if (!check(TokenKind::KwIf)) {
        return value;
    }
    const SourceLocation location = advance().location;
    ExprPtr condition = parse_or();
    if (!expect(TokenKind::KwElse, "'else' in the conditional expression 'a if cond else b'")) {
        return std::make_unique<ErrorExpr>(location);
    }
    ExprPtr otherwise = parse_expression();
    return std::make_unique<ConditionalExpr>(location, std::move(value), std::move(condition),
                                             std::move(otherwise));
}

ExprPtr CodeParser::parse_or()
{
    ExprPtr lhs = parse_and();
    while (check(TokenKind::KwOr)) {
        const SourceLocation location = advance().location;
        lhs = std::make_unique<BinaryExpr>(location, BinaryOp::Or, std::move(lhs), parse_and());
    }
    return lhs;
}

ExprPtr CodeParser::parse_and()
{
    ExprPtr lhs = parse_not();
    while (check(TokenKind::KwAnd)) {
        const SourceLocation location = advance().location;
        lhs = std::make_unique<BinaryExpr>(location, BinaryOp::And, std::move(lhs), parse_not());
    }
    return lhs;
}

// As in Python, 'not' binds looser than comparison: 'not a == b' is 'not (a == b)'.
ExprPtr CodeParser::parse_not()
{
    if (!check(TokenKind::KwNot)) {
        return parse_comparison();
    }
    const SourceLocation location = advance().location;
    return std::make_unique<UnaryExpr>(location, UnaryOp::Not, parse_not());
}

ExprPtr CodeParser::parse_comparison()
{
    ExprPtr lhs = parse_binary(0);
    const std::optional<BinaryOp> op = comparison_op(peek().kind);
    if (!op) {
        return lhs;
    }
    const SourceLocation location = advance().location;
    lhs = std::make_unique<BinaryExpr>(location, *op, std::move(lhs), parse_binary(0));
    if (comparison_op(peek().kind)) {
        error_at(peek(), "comparisons cannot be chained; combine them with 'and'");
    }
    return lhs;
}

ExprPtr CodeParser::parse_binary(int level)
{
    if (level >= kBinaryLevels) {
        return parse_cast();
    }
    ExprPtr lhs = parse_binary(level + 1);
    for (;;) {
        const std::optional<BinaryOp> op = binary_op_at_level(level, peek().kind);
        if (!op) {
            return lhs;
        }
        const SourceLocation location = advance().location;
        lhs = std::make_unique<BinaryExpr>(location, *op, std::move(lhs), parse_binary(level + 1));
    }
}

// 'x as T' binds tighter than '*' and looser than prefix operators, so
// '-x as u32' is '(-x) as u32'.
ExprPtr CodeParser::parse_cast()
{
    ExprPtr expr = parse_unary();
    while (check(TokenKind::KwAs)) {
        const SourceLocation location = advance().location;
        std::optional<TypeExpr> target = parse_type();
        if (!target) {
            return std::make_unique<ErrorExpr>(location);
        }
        expr = std::make_unique<CastExpr>(location, std::move(expr), std::move(*target));
    }
    return expr;
}

ExprPtr CodeParser::parse_unary()
{
    std::optional<UnaryOp> op;
    switch (peek().kind) {
    case TokenKind::Minus: op = UnaryOp::Neg; break;
    case TokenKind::Tilde: op = UnaryOp::BitNot; break;
    case TokenKind::Amp: op = UnaryOp::InoutMark; break;
    default: break;
    }
    if (!op) {
        return parse_postfix(parse_primary());
    }
    const SourceLocation location = advance().location;
    return std::make_unique<UnaryExpr>(location, *op, parse_unary());
}

ExprPtr CodeParser::parse_postfix(ExprPtr expr)
{
    for (;;) {
        if (check(TokenKind::LParen)) {
            expr = parse_call(std::move(expr));
        } else if (check(TokenKind::Dot)) {
            const SourceLocation location = advance().location;
            if (!check(TokenKind::Identifier) && !check(TokenKind::IntLiteral)) {
                error_at(peek(), "expected a member name after '.', found " + describe_token(peek()));
                return expr;
            }
            expr = std::make_unique<MemberExpr>(location, std::move(expr), advance().text);
        } else if (check(TokenKind::LBracket)) {
            const SourceLocation location = advance().location;
            ExprPtr index = parse_expression();
            expect(TokenKind::RBracket, "']' after the index");
            expr = std::make_unique<IndexExpr>(location, std::move(expr), std::move(index));
        } else if (check(TokenKind::Question)) {
            expr = std::make_unique<TryExpr>(advance().location, std::move(expr));
        } else {
            return expr;
        }
    }
}

ExprPtr CodeParser::parse_call(ExprPtr callee)
{
    auto call = std::make_unique<CallExpr>(advance().location, std::move(callee));
    if (match(TokenKind::RParen)) {
        return call;
    }
    for (;;) {
        call->args.push_back(parse_expression());
        if (!match(TokenKind::Comma) || check(TokenKind::RParen)) {
            break;
        }
    }
    expect(TokenKind::RParen, "')' after the arguments");
    return call;
}

ExprPtr CodeParser::parse_primary()
{
    const Token& token = peek();
    switch (token.kind) {
    case TokenKind::IntLiteral: return parse_int_literal();
    case TokenKind::FloatLiteral: return parse_float_literal();
    case TokenKind::CharLiteral: return parse_char_literal();
    case TokenKind::StringLiteral: return parse_string_literal();
    case TokenKind::FStringStart:
    case TokenKind::FStringWhole: return parse_format_string();
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
        advance();
        return std::make_unique<BoolLiteralExpr>(token.location, token.kind == TokenKind::KwTrue);
    case TokenKind::KwNull: return std::make_unique<NullExpr>(advance().location);
    case TokenKind::Identifier:
        if (peek(1).kind == TokenKind::LBrace ||
            (peek(1).kind == TokenKind::Dot && peek(2).kind == TokenKind::Identifier &&
             peek(3).kind == TokenKind::LBrace)) {
            return parse_struct_literal();
        }
        advance();
        return std::make_unique<NameExpr>(token.location, token.text);
    case TokenKind::LParen: return parse_parenthesized();
    case TokenKind::Error:
        advance(); // already reported by the lexer
        panic_ = true;
        return std::make_unique<ErrorExpr>(token.location);
    case TokenKind::LBracket: return unsupported("list literals are");
    case TokenKind::LBrace: return unsupported("object values in code are");
    case TokenKind::KwFn: return unsupported("closures are");
    case TokenKind::KwMatch:
        error_at(token, "a 'match' expression can only be the value of 'let', an assignment or "
                        "'return'");
        return std::make_unique<ErrorExpr>(token.location);
    default: break;
    }
    error_at(token, "expected an expression, found " + describe_token(token));
    return std::make_unique<ErrorExpr>(token.location);
}

// Name ['.' Case] '{' [field ':' expression {',' field ':' expression} [',']] '}'
ExprPtr CodeParser::parse_struct_literal()
{
    const Token& name = advance();
    auto lit = std::make_unique<StructLiteralExpr>(name.location, name.text);
    std::string written = lit->type_name;
    if (match(TokenKind::Dot)) {
        lit->case_location = peek().location;
        lit->case_name = advance().text;
        written += "." + lit->case_name;
    }
    advance(); // '{'
    while (!check(TokenKind::RBrace)) {
        if (!check(TokenKind::Identifier)) {
            error_at(peek(), "expected a field name in '" + written + " { ... }', found " +
                                 describe_token(peek()));
            return lit;
        }
        StructLiteralExpr::FieldInit field;
        field.location = peek().location;
        field.name = advance().text;
        if (!expect(TokenKind::Colon, "':' and a value after the field name")) {
            return lit;
        }
        field.value = parse_expression();
        lit->fields.push_back(std::move(field));
        if (!match(TokenKind::Comma)) {
            break;
        }
    }
    expect(TokenKind::RBrace, "'}' to close the struct value");
    return lit;
}

ExprPtr CodeParser::parse_int_literal()
{
    const Token& token = advance();
    auto lit = std::make_unique<IntLiteralExpr>(token.location);
    lit->text = token.text;
    lit->suffix = token.suffix;
    lit->overflowed = !parse_integer_text(token.text, lit->value);
    return lit;
}

ExprPtr CodeParser::parse_float_literal()
{
    const Token& token = advance();
    auto lit = std::make_unique<FloatLiteralExpr>(token.location);
    lit->text = token.text;
    lit->suffix = token.suffix;
    lit->value = std::strtod(without_underscores(token.text).c_str(), nullptr);
    return lit;
}

ExprPtr CodeParser::parse_char_literal()
{
    const Token& token = advance();
    std::size_t length = 0;
    const std::uint32_t value = decode_utf8(token.text, length);
    if (value == kInvalidCodePoint || length != token.text.size()) {
        diagnostics_.error(token.location, "invalid UTF-8 in character literal");
        return std::make_unique<ErrorExpr>(token.location);
    }
    return std::make_unique<CharLiteralExpr>(token.location, value);
}

ExprPtr CodeParser::parse_string_literal()
{
    const Token& token = advance();
    auto lit = std::make_unique<StringLiteralExpr>(token.location);
    lit->texts.push_back(token.text);
    return lit;
}

ExprPtr CodeParser::parse_format_string()
{
    const Token& first = advance();
    auto lit = std::make_unique<StringLiteralExpr>(first.location);
    lit->is_format = true;
    lit->texts.push_back(first.text);
    if (first.kind == TokenKind::FStringWhole) {
        return lit;
    }
    for (;;) {
        if (check(TokenKind::FStringMid) || check(TokenKind::FStringEnd)) {
            error_at(peek(), "empty '{}' in a format string; write '\\{' for a literal brace");
            lit->parts.push_back(std::make_unique<ErrorExpr>(peek().location));
        } else {
            lit->parts.push_back(parse_expression());
        }
        if (check(TokenKind::FStringMid)) {
            lit->texts.push_back(advance().text);
            continue;
        }
        if (check(TokenKind::FStringEnd)) {
            lit->texts.push_back(advance().text);
            return lit;
        }
        error_at(peek(), "expected '}' to close the interpolation, found " + describe_token(peek()));
        lit->texts.emplace_back();
        return lit;
    }
}

ExprPtr CodeParser::parse_parenthesized()
{
    const Token& open = advance();
    if (check(TokenKind::RParen)) {
        error_at(peek(), "empty parentheses are not an expression");
        return std::make_unique<ErrorExpr>(open.location);
    }
    ExprPtr inner = parse_expression();
    if (check(TokenKind::Comma)) {
        error_at(peek(), "tuples are not supported yet");
        return inner;
    }
    expect(TokenKind::RParen, "')'");
    return inner;
}

ExprPtr CodeParser::unsupported(const std::string& what)
{
    const Token& token = peek();
    error_at(token, what + " not supported yet");
    return std::make_unique<ErrorExpr>(token.location);
}

std::optional<TypeExpr> CodeParser::parse_type()
{
    if (check(TokenKind::Identifier)) {
        TypeExpr type{peek().text, peek().location};
        advance();
        if (check(TokenKind::Lt)) {
            error_at(peek(), "generic types are not supported yet");
            return std::nullopt;
        }
        return type;
    }
    if (check(TokenKind::Star) || check(TokenKind::LParen) || check(TokenKind::LBracket)) {
        error_at(peek(), "this kind of type is not supported yet");
        return std::nullopt;
    }
    error_at(peek(), "expected a type, found " + describe_token(peek()));
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Token access

const Token& CodeParser::peek(std::size_t ahead) const
{
    const std::size_t index = position_ + ahead;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
}

const Token& CodeParser::advance()
{
    const Token& token = tokens_[position_];
    if (position_ + 1 < tokens_.size()) {
        ++position_;
    }
    return token;
}

bool CodeParser::check(TokenKind kind) const
{
    return peek().kind == kind;
}

bool CodeParser::match(TokenKind kind)
{
    if (!check(kind)) {
        return false;
    }
    advance();
    return true;
}

bool CodeParser::expect(TokenKind kind, const char* what)
{
    if (match(kind)) {
        return true;
    }
    error_at(peek(), std::string("expected ") + what + ", found " + describe_token(peek()));
    return false;
}

bool CodeParser::at_line_end() const
{
    switch (peek().kind) {
    case TokenKind::Newline:
    case TokenKind::EndOfInput:
    case TokenKind::Dedent:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Error handling

void CodeParser::error_at(const Token& token, const std::string& message)
{
    if (panic_) {
        return;
    }
    panic_ = true;
    if (token.kind == TokenKind::Error) {
        return; // already reported by the lexer
    }
    diagnostics_.error(token.location, message);
}

// Skips the rest of the logical line, including a block opened on it.
void CodeParser::synchronize()
{
    int depth = 0;
    while (!check(TokenKind::EndOfInput)) {
        const TokenKind kind = peek().kind;
        if (kind == TokenKind::Indent) {
            ++depth;
        } else if (kind == TokenKind::Dedent) {
            if (depth == 0) {
                break;
            }
            --depth;
            advance();
            if (depth == 0 && !check(TokenKind::KwElif) && !check(TokenKind::KwElse)) {
                break;
            }
            continue;
        } else if (kind == TokenKind::Newline && depth == 0) {
            advance();
            if (!check(TokenKind::Indent)) {
                break;
            }
            continue;
        }
        advance();
    }
    panic_ = false;
}

} // namespace hana::lang
