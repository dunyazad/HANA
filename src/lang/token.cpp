#include "lang/token.h"

#include <algorithm>
#include <cstdio>

namespace hana::lang {

std::string_view token_kind_name(TokenKind kind)
{
    switch (kind) {
    case TokenKind::EndOfFile: return "EndOfFile";
    case TokenKind::Newline: return "Newline";
    case TokenKind::Error: return "Error";
    case TokenKind::Identifier: return "Identifier";
    case TokenKind::IntLiteral: return "IntLiteral";
    case TokenKind::FloatLiteral: return "FloatLiteral";
    case TokenKind::SizeLiteral: return "SizeLiteral";
    case TokenKind::StringLiteral: return "StringLiteral";
    case TokenKind::InterpStart: return "InterpStart";
    case TokenKind::InterpMid: return "InterpMid";
    case TokenKind::InterpEnd: return "InterpEnd";
    case TokenKind::KwApp: return "KwApp";
    case TokenKind::KwScene: return "KwScene";
    case TokenKind::KwState: return "KwState";
    case TokenKind::KwAction: return "KwAction";
    case TokenKind::KwTrue: return "KwTrue";
    case TokenKind::KwFalse: return "KwFalse";
    case TokenKind::LParen: return "LParen";
    case TokenKind::RParen: return "RParen";
    case TokenKind::LBrace: return "LBrace";
    case TokenKind::RBrace: return "RBrace";
    case TokenKind::Comma: return "Comma";
    case TokenKind::Colon: return "Colon";
    case TokenKind::Dot: return "Dot";
    case TokenKind::Plus: return "Plus";
    case TokenKind::Minus: return "Minus";
    case TokenKind::Star: return "Star";
    case TokenKind::Slash: return "Slash";
    case TokenKind::Percent: return "Percent";
    case TokenKind::Assign: return "Assign";
    case TokenKind::Equal: return "Equal";
    case TokenKind::NotEqual: return "NotEqual";
    case TokenKind::Less: return "Less";
    case TokenKind::LessEqual: return "LessEqual";
    case TokenKind::Greater: return "Greater";
    case TokenKind::GreaterEqual: return "GreaterEqual";
    case TokenKind::AndAnd: return "AndAnd";
    case TokenKind::OrOr: return "OrOr";
    case TokenKind::Bang: return "Bang";
    }
    return "Unknown";
}

static std::string escape_for_dump(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'"; break;
        default: out += c; break;
        }
    }
    return out;
}

static std::string format_location(const SourceLocation& location)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u:%u", static_cast<unsigned>(location.line),
                  static_cast<unsigned>(location.column));
    return buffer;
}

std::string dump_tokens(const std::vector<Token>& tokens)
{
    std::string out;
    for (const Token& token : tokens) {
        std::string location = format_location(token.location);
        location.resize(std::max<std::size_t>(location.size(), 8), ' ');
        std::string kind(token_kind_name(token.kind));
        kind.resize(std::max<std::size_t>(kind.size(), 14), ' ');
        out += location;
        out += kind;
        out += '\'';
        out += escape_for_dump(token.text);
        out += "'\n";
    }
    return out;
}

} // namespace hana::lang
