#include "lang/token.h"

#include <algorithm>

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
    case TokenKind::CharLiteral: return "CharLiteral";
    case TokenKind::StringLiteral: return "StringLiteral";
    case TokenKind::InterpStart: return "InterpStart";
    case TokenKind::InterpMid: return "InterpMid";
    case TokenKind::InterpEnd: return "InterpEnd";
    case TokenKind::KwFn: return "KwFn";
    case TokenKind::KwLet: return "KwLet";
    case TokenKind::KwVar: return "KwVar";
    case TokenKind::KwReturn: return "KwReturn";
    case TokenKind::KwIf: return "KwIf";
    case TokenKind::KwElse: return "KwElse";
    case TokenKind::KwWhile: return "KwWhile";
    case TokenKind::KwLoop: return "KwLoop";
    case TokenKind::KwFor: return "KwFor";
    case TokenKind::KwIn: return "KwIn";
    case TokenKind::KwMatch: return "KwMatch";
    case TokenKind::KwBreak: return "KwBreak";
    case TokenKind::KwContinue: return "KwContinue";
    case TokenKind::KwStruct: return "KwStruct";
    case TokenKind::KwEnum: return "KwEnum";
    case TokenKind::KwTrait: return "KwTrait";
    case TokenKind::KwImpl: return "KwImpl";
    case TokenKind::KwPub: return "KwPub";
    case TokenKind::KwImport: return "KwImport";
    case TokenKind::KwAs: return "KwAs";
    case TokenKind::KwUnsafe: return "KwUnsafe";
    case TokenKind::KwExtern: return "KwExtern";
    case TokenKind::KwTrue: return "KwTrue";
    case TokenKind::KwFalse: return "KwFalse";
    case TokenKind::KwInout: return "KwInout";
    case TokenKind::KwSink: return "KwSink";
    case TokenKind::KwBind: return "KwBind";
    case TokenKind::KwYield: return "KwYield";
    case TokenKind::KwSubscript: return "KwSubscript";
    case TokenKind::LParen: return "LParen";
    case TokenKind::RParen: return "RParen";
    case TokenKind::LBrace: return "LBrace";
    case TokenKind::RBrace: return "RBrace";
    case TokenKind::LBracket: return "LBracket";
    case TokenKind::RBracket: return "RBracket";
    case TokenKind::Comma: return "Comma";
    case TokenKind::Colon: return "Colon";
    case TokenKind::Semicolon: return "Semicolon";
    case TokenKind::Dot: return "Dot";
    case TokenKind::DotDot: return "DotDot";
    case TokenKind::DotDotEq: return "DotDotEq";
    case TokenKind::Arrow: return "Arrow";
    case TokenKind::FatArrow: return "FatArrow";
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
    case TokenKind::Bang: return "Bang";
    case TokenKind::AmpAmp: return "AmpAmp";
    case TokenKind::PipePipe: return "PipePipe";
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
    case TokenKind::EndOfFile: return "end of file";
    case TokenKind::Newline: return "end of line";
    case TokenKind::StringLiteral:
    case TokenKind::InterpStart: return "string literal";
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
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
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
        out += location;
        out += kind;
        out += '\'';
        out += escape_for_dump(token.text);
        out += '\'';
        if (!token.suffix.empty()) {
            out += " suffix=" + token.suffix;
        }
        out += '\n';
    }
    return out;
}

} // namespace hana::lang
