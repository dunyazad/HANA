#include "lang/parser.h"

#include <cstdlib>
#include <string>
#include <utility>

#include "support/utf8.h"

namespace hana::lang {

namespace {

// Binding strength of binary operators; 0 means "not a binary operator".
// See docs/language.md section 2.4.
constexpr int kComparisonPrecedence = 3;

int binary_precedence(TokenKind kind)
{
    switch (kind) {
    case TokenKind::PipePipe: return 1;
    case TokenKind::AmpAmp: return 2;
    case TokenKind::EqEq:
    case TokenKind::BangEq:
    case TokenKind::Lt:
    case TokenKind::LtEq:
    case TokenKind::Gt:
    case TokenKind::GtEq: return kComparisonPrecedence;
    case TokenKind::Pipe: return 4;
    case TokenKind::Caret: return 5;
    case TokenKind::Amp: return 6;
    case TokenKind::Shl:
    case TokenKind::Shr: return 7;
    case TokenKind::Plus:
    case TokenKind::Minus: return 8;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent: return 9;
    default: return 0;
    }
}

BinaryOp binary_op_for(TokenKind kind)
{
    switch (kind) {
    case TokenKind::PipePipe: return BinaryOp::Or;
    case TokenKind::AmpAmp: return BinaryOp::And;
    case TokenKind::EqEq: return BinaryOp::Eq;
    case TokenKind::BangEq: return BinaryOp::Ne;
    case TokenKind::Lt: return BinaryOp::Lt;
    case TokenKind::LtEq: return BinaryOp::Le;
    case TokenKind::Gt: return BinaryOp::Gt;
    case TokenKind::GtEq: return BinaryOp::Ge;
    case TokenKind::Pipe: return BinaryOp::BitOr;
    case TokenKind::Caret: return BinaryOp::BitXor;
    case TokenKind::Amp: return BinaryOp::BitAnd;
    case TokenKind::Shl: return BinaryOp::Shl;
    case TokenKind::Shr: return BinaryOp::Shr;
    case TokenKind::Plus: return BinaryOp::Add;
    case TokenKind::Minus: return BinaryOp::Sub;
    case TokenKind::Star: return BinaryOp::Mul;
    case TokenKind::Slash: return BinaryOp::Div;
    default: return BinaryOp::Rem;
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

const char* unsupported_declaration_name(TokenKind kind)
{
    switch (kind) {
    case TokenKind::KwStruct: return "'struct' declarations";
    case TokenKind::KwEnum: return "'enum' declarations";
    case TokenKind::KwTrait: return "'trait' declarations";
    case TokenKind::KwImpl: return "'impl' blocks";
    case TokenKind::KwImport: return "'import' declarations";
    case TokenKind::KwExtern: return "'extern' declarations";
    case TokenKind::KwUnsafe: return "'unsafe' declarations";
    default: return nullptr;
    }
}

// Parses digits written with an optional 0x / 0b prefix and '_' separators.
// Returns false on overflow of 64 bits.
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

Parser::Parser(const std::vector<Token>& tokens, DiagnosticEngine& diagnostics)
    : tokens_(tokens)
    , diagnostics_(diagnostics)
{
}

// ---------------------------------------------------------------------------
// Declarations

std::unique_ptr<Module> Parser::parse_module()
{
    auto module = std::make_unique<Module>();
    skip_separators();
    while (!check(TokenKind::EndOfFile)) {
        const std::size_t before = position_;
        parse_top_level_item(*module);
        if (panic_) {
            synchronize_top_level();
        }
        if (position_ == before) {
            advance();
        }
        skip_separators();
    }
    return module;
}

void Parser::parse_top_level_item(Module& module)
{
    const bool is_pub = match(TokenKind::KwPub);
    if (check(TokenKind::KwFn)) {
        auto function = parse_function(is_pub);
        if (function) {
            module.functions.push_back(std::move(function));
        }
        return;
    }
    if (unsupported_declaration_name(peek().kind) != nullptr) {
        skip_unsupported_declaration(peek());
        return;
    }
    error_at(peek(), "expected a declaration such as 'fn', found " + describe_token(peek()));
}

std::unique_ptr<FnDecl> Parser::parse_function(bool is_pub)
{
    advance(); // 'fn'
    auto function = std::make_unique<FnDecl>();
    function->is_pub = is_pub;
    function->location = peek().location;
    if (!check(TokenKind::Identifier)) {
        error_at(peek(), "expected a function name, found " + describe_token(peek()));
        return nullptr;
    }
    function->name = advance().text;
    if (check(TokenKind::Lt)) {
        error_at(peek(), "generic functions are not supported yet");
        return nullptr;
    }
    if (!expect(TokenKind::LParen, "'(' after the function name") || !parse_params(*function)) {
        return nullptr;
    }
    if (match(TokenKind::Arrow)) {
        function->return_type = parse_type();
        if (!function->return_type) {
            return nullptr;
        }
    }
    skip_newlines();
    if (!parse_block(function->body)) {
        return nullptr;
    }
    return function;
}

bool Parser::parse_params(FnDecl& function)
{
    if (match(TokenKind::RParen)) {
        return true;
    }
    for (;;) {
        std::optional<Param> param = parse_param();
        if (!param) {
            return false;
        }
        function.params.push_back(std::move(*param));
        if (!match(TokenKind::Comma) || check(TokenKind::RParen)) {
            break;
        }
    }
    return expect(TokenKind::RParen, "')' after the parameters");
}

std::optional<Param> Parser::parse_param()
{
    Param param;
    if (match(TokenKind::KwInout)) {
        param.convention = ParamConvention::Inout;
    } else if (match(TokenKind::KwSink)) {
        param.convention = ParamConvention::Sink;
    }
    param.location = peek().location;
    if (!check(TokenKind::Identifier)) {
        error_at(peek(), "expected a parameter name, found " + describe_token(peek()));
        return std::nullopt;
    }
    param.name = advance().text;
    if (!expect(TokenKind::Colon, "':' and a type after the parameter name")) {
        return std::nullopt;
    }
    std::optional<TypeExpr> type = parse_type();
    if (!type) {
        return std::nullopt;
    }
    param.type = std::move(*type);
    return param;
}

std::optional<TypeExpr> Parser::parse_type()
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
    if (check(TokenKind::Star) || check(TokenKind::LBracket) || check(TokenKind::LParen) ||
        check(TokenKind::KwFn)) {
        error_at(peek(), "this kind of type is not supported yet");
        return std::nullopt;
    }
    error_at(peek(), "expected a type, found " + describe_token(peek()));
    return std::nullopt;
}

void Parser::skip_unsupported_declaration(const Token& keyword)
{
    error_at(keyword, std::string(unsupported_declaration_name(keyword.kind)) +
                          " are not supported yet");
}

// ---------------------------------------------------------------------------
// Statements

bool Parser::parse_block(Block& block)
{
    block.location = peek().location;
    if (!expect(TokenKind::LBrace, "'{'")) {
        return false;
    }
    for (;;) {
        skip_separators();
        if (check(TokenKind::RBrace)) {
            block.end_location = advance().location;
            return true;
        }
        if (check(TokenKind::EndOfFile)) {
            error_at(peek(), "expected '}' to close the block");
            return false;
        }
        const std::size_t before = position_;
        StmtPtr statement = parse_statement();
        if (statement) {
            block.statements.push_back(std::move(statement));
        }
        if (panic_) {
            synchronize_statement();
        } else {
            expect_statement_end();
        }
        if (position_ == before) {
            advance();
        }
    }
}

StmtPtr Parser::parse_statement()
{
    switch (peek().kind) {
    case TokenKind::KwLet:
    case TokenKind::KwVar: return parse_let();
    case TokenKind::KwReturn: return parse_return();
    case TokenKind::KwIf: return parse_if();
    case TokenKind::KwWhile: return parse_while();
    case TokenKind::KwLoop: return parse_loop();
    case TokenKind::KwFor: return parse_for();
    case TokenKind::KwBreak: return std::make_unique<BreakStmt>(advance().location);
    case TokenKind::KwContinue: return std::make_unique<ContinueStmt>(advance().location);
    case TokenKind::LBrace: return parse_block_statement();
    case TokenKind::KwMatch:
        error_at(peek(), "'match' is not supported yet");
        return nullptr;
    default: return parse_expression_statement();
    }
}

StmtPtr Parser::parse_let()
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
    let->init = parse_expression();
    return let;
}

StmtPtr Parser::parse_return()
{
    auto ret = std::make_unique<ReturnStmt>(advance().location);
    if (!at_statement_end()) {
        ret->value = parse_expression();
    }
    return ret;
}

StmtPtr Parser::parse_if()
{
    auto stmt = std::make_unique<IfStmt>(advance().location);
    stmt->condition = parse_expression();
    skip_newlines();
    if (!parse_block(stmt->then_block)) {
        return nullptr;
    }
    if (!next_significant_is(TokenKind::KwElse)) {
        return stmt;
    }
    skip_newlines();
    const SourceLocation else_location = advance().location;
    if (check(TokenKind::KwIf)) {
        stmt->else_branch = parse_if();
        return stmt->else_branch ? std::move(stmt) : nullptr;
    }
    auto else_block = std::make_unique<BlockStmt>(else_location);
    skip_newlines();
    if (!parse_block(else_block->block)) {
        return nullptr;
    }
    stmt->else_branch = std::move(else_block);
    return stmt;
}

StmtPtr Parser::parse_while()
{
    auto stmt = std::make_unique<WhileStmt>(advance().location);
    stmt->condition = parse_expression();
    skip_newlines();
    if (!parse_block(stmt->body)) {
        return nullptr;
    }
    return stmt;
}

StmtPtr Parser::parse_loop()
{
    auto stmt = std::make_unique<LoopStmt>(advance().location);
    skip_newlines();
    if (!parse_block(stmt->body)) {
        return nullptr;
    }
    return stmt;
}

StmtPtr Parser::parse_for()
{
    auto stmt = std::make_unique<ForStmt>(advance().location);
    if (!check(TokenKind::Identifier)) {
        error_at(peek(), "expected a loop variable name, found " + describe_token(peek()));
        return nullptr;
    }
    stmt->name = advance().text;
    if (!expect(TokenKind::KwIn, "'in' after the loop variable")) {
        return nullptr;
    }
    stmt->iterable = parse_expression();
    skip_newlines();
    if (!parse_block(stmt->body)) {
        return nullptr;
    }
    return stmt;
}

StmtPtr Parser::parse_block_statement()
{
    auto stmt = std::make_unique<BlockStmt>(peek().location);
    if (!parse_block(stmt->block)) {
        return nullptr;
    }
    return stmt;
}

StmtPtr Parser::parse_expression_statement()
{
    ExprPtr expr = parse_expression();
    if (!is_assignment_token(peek().kind)) {
        return std::make_unique<ExprStmt>(std::move(expr));
    }
    auto assign = std::make_unique<AssignStmt>(expr->location);
    assign->op_location = peek().location;
    assign->op = compound_assignment_op(advance().kind);
    assign->target = std::move(expr);
    assign->value = parse_expression();
    return assign;
}

bool Parser::expect_statement_end()
{
    if (at_statement_end()) {
        return true;
    }
    error_at(peek(), "expected end of statement, found " + describe_token(peek()));
    synchronize_statement();
    return false;
}

// ---------------------------------------------------------------------------
// Expressions

ExprPtr Parser::parse_expression()
{
    return parse_binary(1);
}

ExprPtr Parser::parse_binary(int min_precedence)
{
    ExprPtr lhs = parse_unary();
    for (;;) {
        const int precedence = binary_precedence(peek().kind);
        if (precedence == 0 || precedence < min_precedence) {
            return lhs;
        }
        const Token& op = advance();
        ExprPtr rhs = parse_binary(precedence + 1);
        lhs = std::make_unique<BinaryExpr>(op.location, binary_op_for(op.kind), std::move(lhs),
                                           std::move(rhs));
        if (precedence == kComparisonPrecedence &&
            binary_precedence(peek().kind) == kComparisonPrecedence) {
            error_at(peek(), "comparison operators cannot be chained; combine them with '&&'");
            return lhs;
        }
    }
}

ExprPtr Parser::parse_unary()
{
    std::optional<UnaryOp> op;
    switch (peek().kind) {
    case TokenKind::Minus: op = UnaryOp::Neg; break;
    case TokenKind::Bang: op = UnaryOp::Not; break;
    case TokenKind::Tilde: op = UnaryOp::BitNot; break;
    case TokenKind::Amp: op = UnaryOp::InoutMark; break;
    case TokenKind::Star: op = UnaryOp::Deref; break;
    default: break;
    }
    if (!op) {
        return parse_postfix(parse_primary());
    }
    const SourceLocation location = advance().location;
    return std::make_unique<UnaryExpr>(location, *op, parse_unary());
}

ExprPtr Parser::parse_postfix(ExprPtr expr)
{
    for (;;) {
        if (check(TokenKind::LParen)) {
            expr = parse_call(std::move(expr));
        } else if (check(TokenKind::Dot)) {
            const SourceLocation location = advance().location;
            if (!check(TokenKind::Identifier)) {
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

ExprPtr Parser::parse_call(ExprPtr callee)
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

ExprPtr Parser::parse_primary()
{
    const Token& token = peek();
    switch (token.kind) {
    case TokenKind::IntLiteral: return parse_int_literal();
    case TokenKind::FloatLiteral: return parse_float_literal();
    case TokenKind::CharLiteral: return parse_char_literal();
    case TokenKind::StringLiteral:
    case TokenKind::InterpStart: return parse_string_literal();
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
        advance();
        return std::make_unique<BoolLiteralExpr>(token.location, token.kind == TokenKind::KwTrue);
    case TokenKind::Identifier:
        advance();
        return std::make_unique<NameExpr>(token.location, token.text);
    case TokenKind::LParen: return parse_parenthesized();
    case TokenKind::Error:
        // Already reported by the lexer.
        advance();
        return std::make_unique<ErrorExpr>(token.location);
    case TokenKind::KwIf: return unsupported_expression("'if' expressions are");
    case TokenKind::KwMatch: return unsupported_expression("'match' expressions are");
    case TokenKind::KwFn: return unsupported_expression("closures are");
    case TokenKind::LBracket: return unsupported_expression("list literals are");
    case TokenKind::KwUnsafe: return unsupported_expression("'unsafe' blocks are");
    default: break;
    }
    error_at(token, "expected an expression, found " + describe_token(token));
    return std::make_unique<ErrorExpr>(token.location);
}

ExprPtr Parser::parse_int_literal()
{
    const Token& token = advance();
    auto lit = std::make_unique<IntLiteralExpr>(token.location);
    lit->text = token.text;
    lit->suffix = token.suffix;
    if (!parse_integer_text(token.text, lit->value)) {
        lit->overflowed = true;
    }
    return lit;
}

ExprPtr Parser::parse_float_literal()
{
    const Token& token = advance();
    auto lit = std::make_unique<FloatLiteralExpr>(token.location);
    lit->text = token.text;
    lit->suffix = token.suffix;
    lit->value = std::strtod(without_underscores(token.text).c_str(), nullptr);
    return lit;
}

ExprPtr Parser::parse_char_literal()
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

ExprPtr Parser::parse_string_literal()
{
    const Token& first = advance();
    auto lit = std::make_unique<StringLiteralExpr>(first.location);
    lit->texts.push_back(first.text);
    if (first.kind == TokenKind::StringLiteral) {
        return lit;
    }
    for (;;) {
        if (check(TokenKind::InterpMid) || check(TokenKind::InterpEnd)) {
            error_at(peek(), "empty interpolation '{}' in string literal; write '\\{' for a brace");
            lit->parts.push_back(std::make_unique<ErrorExpr>(peek().location));
        } else {
            lit->parts.push_back(parse_expression());
        }
        if (check(TokenKind::InterpMid)) {
            lit->texts.push_back(advance().text);
            continue;
        }
        if (check(TokenKind::InterpEnd)) {
            lit->texts.push_back(advance().text);
            return lit;
        }
        error_at(peek(), "expected '}' to close the string interpolation, found " +
                             describe_token(peek()));
        lit->texts.emplace_back();
        return lit;
    }
}

ExprPtr Parser::parse_parenthesized()
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

ExprPtr Parser::unsupported_expression(const char* what)
{
    const Token& token = peek();
    error_at(token, std::string(what) + " not supported yet");
    return std::make_unique<ErrorExpr>(token.location);
}

// ---------------------------------------------------------------------------
// Token access

const Token& Parser::peek(std::size_t ahead) const
{
    const std::size_t index = position_ + ahead;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
}

const Token& Parser::advance()
{
    const Token& token = tokens_[position_];
    if (position_ + 1 < tokens_.size()) {
        ++position_;
    }
    return token;
}

bool Parser::check(TokenKind kind) const
{
    return peek().kind == kind;
}

bool Parser::match(TokenKind kind)
{
    if (!check(kind)) {
        return false;
    }
    advance();
    return true;
}

bool Parser::expect(TokenKind kind, const char* what)
{
    if (match(kind)) {
        return true;
    }
    error_at(peek(), std::string("expected ") + what + ", found " + describe_token(peek()));
    return false;
}

bool Parser::at_statement_end() const
{
    switch (peek().kind) {
    case TokenKind::Newline:
    case TokenKind::Semicolon:
    case TokenKind::RBrace:
    case TokenKind::EndOfFile:
        return true;
    default:
        return false;
    }
}

void Parser::skip_newlines()
{
    while (check(TokenKind::Newline)) {
        advance();
    }
}

void Parser::skip_separators()
{
    while (check(TokenKind::Newline) || check(TokenKind::Semicolon)) {
        advance();
    }
}

bool Parser::next_significant_is(TokenKind kind) const
{
    std::size_t ahead = 0;
    while (peek(ahead).kind == TokenKind::Newline) {
        ++ahead;
    }
    return peek(ahead).kind == kind;
}

// ---------------------------------------------------------------------------
// Error handling

void Parser::error_at(const Token& token, const std::string& message)
{
    if (token.kind == TokenKind::Error) {
        // The lexer has already reported this token.
        panic_ = true;
        return;
    }
    error_at(token.location, message);
}

void Parser::error_at(const SourceLocation& location, const std::string& message)
{
    if (panic_) {
        return;
    }
    diagnostics_.error(location, message);
    panic_ = true;
}

void Parser::synchronize_statement()
{
    // Skip to the end of the current statement: a separator or the '}' that
    // closes the enclosing block, ignoring nested blocks.
    int depth = 0;
    while (!check(TokenKind::EndOfFile)) {
        const TokenKind kind = peek().kind;
        if (depth == 0 && (kind == TokenKind::Newline || kind == TokenKind::Semicolon ||
                           kind == TokenKind::RBrace)) {
            break;
        }
        if (kind == TokenKind::LBrace) {
            ++depth;
        } else if (kind == TokenKind::RBrace) {
            --depth;
        }
        advance();
    }
    panic_ = false;
}

void Parser::synchronize_top_level()
{
    // Skip to the next 'fn' or 'pub' outside any block that starts a line or
    // follows a '}'. The '}' case matters when an unclosed '(' made the lexer
    // drop the line breaks of the rest of the file.
    int depth = 0;
    while (!check(TokenKind::EndOfFile)) {
        const TokenKind kind = peek().kind;
        const TokenKind previous =
            position_ == 0 ? TokenKind::Newline : tokens_[position_ - 1].kind;
        const bool starts_item = previous == TokenKind::Newline || previous == TokenKind::RBrace;
        if (depth <= 0 && starts_item && (kind == TokenKind::KwFn || kind == TokenKind::KwPub)) {
            break;
        }
        if (kind == TokenKind::LBrace) {
            ++depth;
        } else if (kind == TokenKind::RBrace) {
            --depth;
        }
        advance();
    }
    panic_ = false;
}

} // namespace hana::lang
