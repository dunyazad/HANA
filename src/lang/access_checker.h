#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "lang/ast.h"
#include "lang/sema.h"
#include "support/diagnostic.h"

namespace hana::lang {

// Checks that a place is not changed or moved while part of it is still in
// use. A place is in use from where it is evaluated until the operation
// that needs it runs:
//   - an argument that is passed by pointer (inout, or a read of a value
//     that is not Copy) and the receiver of a method, until the call runs,
//     so 'g(p.name, h(&p))' is an error;
//   - the left operand of '+' or a comparison when it is not Copy, while
//     the right operand runs;
//   - a list being indexed, while the index runs;
//   - a variable matched by 'match', in an arm that binds a part of it that
//     is not Copy (the name shares the variable's data);
//   - the list of a 'for x in xs' or 'for inout x in xs', for the body.
// A Copy value is read when it is evaluated, so it is never in use later.
// Conflicts inside one call ('f(&p, p)') are checked by semantic analysis.
// Runs on a module that passed semantic analysis without errors.
class AccessChecker {
public:
    explicit AccessChecker(DiagnosticEngine& diagnostics);

    void check(const Module& module);

private:
    struct Hold {
        Place place;
        SourceLocation location;
        std::string note;
    };

    // Statements
    void walk_block(const Block& block);
    void walk_statement(const Stmt& stmt);
    void walk_assign(const AssignStmt& assign);
    void walk_for(const ForStmt& stmt);

    // Expressions, in the order they run. A consumed value moves when it is
    // a place that is not Copy.
    void walk(const Expr& expr, bool consuming);
    void walk_place_parts(const Expr& expr);
    void walk_binary(const BinaryExpr& binary);
    void walk_index(const IndexExpr& index);
    void walk_call(const CallExpr& call);
    void walk_argument(const Expr& arg, ParamConvention convention, const std::string& callee,
                       std::vector<Place>& changed);
    void walk_receiver(const Expr& object, ParamConvention convention, const std::string& callee,
                       std::vector<Place>& changed);
    void walk_match(const MatchExpr& match, bool consuming);
    static bool binds_shared_part(const Pattern& pattern);

    void hold(const Place& place, const SourceLocation& location, std::string note);
    void check_change(const Place& place, const Expr& expr, const char* verb, std::size_t limit);
    void error_once(const SourceLocation& location, const std::string& message,
                    const SourceLocation& note_location, const std::string& note);

    DiagnosticEngine& diagnostics_;
    std::vector<Hold> holds_;
    std::set<std::uint32_t> reported_; // source offsets
};

} // namespace hana::lang
