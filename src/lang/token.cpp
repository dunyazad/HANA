#include "lang/token.h"

#include <algorithm>

namespace hana::lang {

std::string_view token_kind_name(TokenKind kind)
{
    switch (kind) {
    case TokenKind::EndOfInput: return "EndOfInput";
    case TokenKind::Newline: return "Newline";
    case TokenKind::Indent: return "Indent";
    case TokenKind::Dedent: return "Dedent";
    case TokenKind::Error: return "Error";
    case TokenKind::Identifier: return "Identifier";
    case TokenKind::IntLiteral: return "IntLiteral";
    case TokenKind::FloatLiteral: return "FloatLiteral";
    case TokenKind::CharLiteral: return "CharLiteral";
    case TokenKind::StringLiteral: return "StringLiteral";
    case TokenKind::FStringStart: return "FStringStart";
    case TokenKind::FStringMid: return "FStringMid";
    case TokenKind::FStringEnd: return "FStringEnd";
    case TokenKind::FStringWhole: return "FStringWhole";
    case TokenKind::KwLet: return "KwLet";
    case TokenKind::KwVar: return "KwVar";
    case TokenKind::KwIf: return "KwIf";
    case TokenKind::KwElif: return "KwElif";
    case TokenKind::KwElse: return "KwElse";
    case TokenKind::KwWhile: return "KwWhile";
    case TokenKind::KwFor: return "KwFor";
    case TokenKind::KwIn: return "KwIn";
    case TokenKind::KwMatch: return "KwMatch";
    case TokenKind::KwBreak: return "KwBreak";
    case TokenKind::KwContinue: return "KwContinue";
    case TokenKind::KwReturn: return "KwReturn";
    case TokenKind::KwPass: return "KwPass";
    case TokenKind::KwAnd: return "KwAnd";
    case TokenKind::KwOr: return "KwOr";
    case TokenKind::KwNot: return "KwNot";
    case TokenKind::KwTrue: return "KwTrue";
    case TokenKind::KwFalse: return "KwFalse";
    case TokenKind::KwNull: return "KwNull";
    case TokenKind::KwAs: return "KwAs";
    case TokenKind::KwFn: return "KwFn";
    case TokenKind::KwInout: return "KwInout";
    case TokenKind::KwSink: return "KwSink";
    case TokenKind::KwUnsafe: return "KwUnsafe";
    case TokenKind::LParen: return "LParen";
    case TokenKind::RParen: return "RParen";
    case TokenKind::LBracket: return "LBracket";
    case TokenKind::RBracket: return "RBracket";
    case TokenKind::LBrace: return "LBrace";
    case TokenKind::RBrace: return "RBrace";
    case TokenKind::Comma: return "Comma";
    case TokenKind::Colon: return "Colon";
    case TokenKind::Dot: return "Dot";
    case TokenKind::DotDot: return "DotDot";
    case TokenKind::DotDotEq: return "DotDotEq";
    case TokenKind::Arrow: return "Arrow";
    case TokenKind::Question: return "Question";
    case TokenKind::Plus: return "Plus";
    case TokenKind::Minus: return "Minus";
    case TokenKind::Star: return "Star";
    case TokenKind::Slash: return "Slash";
    case TokenKind::Percent: return "Percent";
    case TokenKind::Amp: return "Amp";
    case TokenKind::Pipe: return "Pipe";
    case TokenKind::Caret: return "Caret";
    case TokenKind::Tilde: return "Tilde";
    case TokenKind::Shl: return "Shl";
    case TokenKind::Shr: return "Shr";
    case TokenKind::Eq: return "Eq";
    case TokenKind::EqEq: return "EqEq";
    case TokenKind::BangEq: return "BangEq";
    case TokenKind::Lt: return "Lt";
    case TokenKind::LtEq: return "LtEq";
    case TokenKind::Gt: return "Gt";
    case TokenKind::GtEq: return "GtEq";
    case TokenKind::PlusEq: return "PlusEq";
    case TokenKind::MinusEq: return "MinusEq";
    case TokenKind::StarEq: return "StarEq";
    case TokenKind::SlashEq: return "SlashEq";
    case TokenKind::PercentEq: return "PercentEq";
    case TokenKind::AmpEq: return "AmpEq";
    case TokenKind::PipeEq: return "PipeEq";
    case TokenKind::CaretEq: return "CaretEq";
    case TokenKind::ShlEq: return "ShlEq";
    case TokenKind::ShrEq: return "ShrEq";
    }
    return "Unknown";
}

std::string describe_token(const Token& token)
{
    switch (token.kind) {
    case TokenKind::EndOfInput: return "end of code";
    case TokenKind::Newline: return "end of line";
    case TokenKind::Indent: return "an indented block";
    case TokenKind::Dedent: return "the end of the block";
    case TokenKind::StringLiteral:
    case TokenKind::FStringStart:
    case TokenKind::FStringWhole: return "string literal";
    case TokenKind::IntLiteral:
    case TokenKind::FloatLiteral: return "number '" + token.text + "'";
    case TokenKind::CharLiteral: return "character literal";
    case TokenKind::Identifier: return "identifier '" + token.text + "'";
    default: return "'" + token.text + "'";
    }
}

static std::string escape_for_dump(std::string_view text)
{
    std::string out;
    for (char c : text) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\0': out += "\\0"; break;
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string dump_tokens(const std::vector<Token>& tokens)
{
    std::string out;
    for (const Token& token : tokens) {
        std::string location =
            std::to_string(token.location.line) + ":" + std::to_string(token.location.column);
        location.resize(std::max<std::size_t>(location.size(), 8), ' ');
        std::string kind(token_kind_name(token.kind));
        kind.resize(std::max<std::size_t>(kind.size(), 14), ' ');
        out += location + kind + "'" + escape_for_dump(token.text) + "'";
        if (!token.suffix.empty()) {
            out += " suffix=" + token.suffix;
        }
        out += '\n';
    }
    return out;
}

} // namespace hana::lang
