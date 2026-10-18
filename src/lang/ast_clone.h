#pragma once

#include <memory>

#include "lang/ast.h"

namespace hana::lang {

// Deep copies of what the parser produced. Fields set by semantic analysis
// are not copied.
ExprPtr clone_expr(const Expr& expr);
PatternPtr clone_pattern(const Pattern& pattern);
StmtPtr clone_stmt(const Stmt& stmt);
Block clone_block(const Block& block);
std::unique_ptr<FnDecl> clone_function(const FnDecl& function);

} // namespace hana::lang
