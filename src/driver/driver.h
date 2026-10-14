#pragma once

#include <string>
#include <vector>

namespace hana::driver {

struct CheckOptions {
    std::string path;
    bool dump_tokens = false;
    bool dump_ast = false;
};

struct BuildOptions {
    std::string path;
    std::string output;   // executable path; derived from the source name if empty
    bool optimize = false;
    bool emit_ir = false; // print LLVM IR instead of linking
};

struct RunOptions {
    std::string path;
    std::vector<std::string> program_args;
    bool optimize = false;
};

// Exit codes of the hana tool itself.
constexpr int kExitOk = 0;
constexpr int kExitCompileError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitToolFailure = 3;

int run_check(const CheckOptions& options);
int run_build(const BuildOptions& options);
// Builds into a temporary directory, runs the program and returns its exit
// code.
int run_program(const RunOptions& options);

} // namespace hana::driver
