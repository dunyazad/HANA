#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "lang/ast.h"
#include "lang/symbols.h"
#include "support/diagnostic.h"

namespace hana::lang {

// Checks that a value is not used after it was moved. Values of types that
// are not Copy move when they are stored ('let', assignment, struct field),
// passed to a 'sink' parameter or returned. Moving a field moves the whole
// variable it belongs to, and a name bound by a 'match' pattern is part of
// the matched variable. Runs on a module that passed semantic analysis
// without errors.
//
// The analysis follows control flow: after 'if' a value is moved when it is
// moved on either branch, and a value moved inside a loop is moved again at
// the start of the next iteration. Assigning a new value to a moved
// variable makes it usable again.
class MoveChecker {
public:
    explicit MoveChecker(DiagnosticEngine& diagnostics);

    void check(const Module& module);

private:
    // Variables moved on some path reaching the current point, with the
    // location of one such move.
    struct State {
        bool reachable = true;
        std::unordered_map<const VarSymbol*, SourceLocation> moved;
    };

    struct Loop {
        std::vector<State> breaks;
        std::vector<State> continues;
    };

    void check_function(const FnDecl& function);

    // Statements
    void walk_block(const Block& block, State& state);
    void walk_statement(const Stmt& stmt, State& state);
    void walk_assign(const AssignStmt& assign, State& state);
    void walk_if(const IfStmt& stmt, State& state);
    void walk_while(const WhileStmt& stmt, State& state);
    void walk_for(const ForStmt& stmt, State& state);
    void walk_loop(const Expr* condition, bool infinite, const Block& body, State& state);
    void walk_match(const MatchExpr& match, bool consuming, State& state);
    // A name bound by a pattern, and the types of the values it lies in.
    struct BoundName {
        const VarSymbol* symbol;
        std::vector<const Type*> containers;
        const Pattern* pattern;
    };
    struct Alias {
        const VarSymbol* root;
        std::vector<const Type*> containers;
    };

    static void collect_bindings(const Pattern& pattern, std::vector<const VarSymbol*>& out);
    static void collect_bound_names(const Pattern& pattern, const Type* type,
                                    std::vector<const Type*>& containers,
                                    std::vector<BoundName>& out);
    void check_owned_binding(const BoundName& bound);
    static std::vector<const Type*> member_containers(const Expr& expr);
    bool check_partial_move(const std::vector<const Type*>& containers, const Expr& moved);
    void check_temporary_member_move(const MemberExpr& moved);
    const VarSymbol* root_variable(const Expr& expr) const;
    const VarSymbol* alias_of(const VarSymbol& var) const;

    // Expressions: 'use' reads a value, 'consume' moves it when it is not
    // Copy.
    void use(const Expr& expr, State& state);
    void use_value(const Expr& expr, State& state);
    void consume(const Expr& expr, State& state);
    void move_variable(const VarSymbol& var, const Expr& moved, State& state);
    void walk_call(const CallExpr& call, State& state);
    void walk_arguments(const CallExpr& call, const FunctionSymbol& function, State& state);
    void walk_branches(const Expr& condition, const Expr& then_value, const Expr& else_value,
                       bool consuming, State& state);

    static void merge(State& into, const State& other);
    static bool same_moves(const State& a, const State& b);
    void error_once(const SourceLocation& location, const std::string& message,
                    const SourceLocation* note_location, const std::string& note);

    DiagnosticEngine& diagnostics_;
    std::vector<Loop> loops_;
    // Names bound by a pattern, mapped to the variable of the matched value
    // when that value is a variable or a field of one.
    std::unordered_map<const VarSymbol*, Alias> aliases_;
    // Loops are walked until their state settles, so the same error can be
    // found more than once; each location is reported once.
    std::set<std::uint32_t> reported_; // source offsets
};

} // namespace hana::lang
