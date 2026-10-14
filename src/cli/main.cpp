#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "driver/driver.h"

namespace {

using hana::driver::kExitOk;
using hana::driver::kExitUsage;

void print_usage(std::FILE* out)
{
    std::fputs("HANA " HANA_VERSION "\n"
               "\n"
               "usage:\n"
               "  hana run [-O] <file.hana> [-- args...]   build and run a program\n"
               "  hana build [-O] [-o <output>] <file.hana> build an executable\n"
               "  hana build --emit-ir [-O] <file.hana>     print LLVM IR\n"
               "  hana check [--tokens] [--ast] <file.hana> check a source file\n"
               "  hana --version\n"
               "\n"
               "options:\n"
               "  -O          optimize\n"
               "  --tokens    print the token stream\n"
               "  --ast       print the syntax tree\n"
               "\n"
               "environment:\n"
               "  HANA_CC     C compiler used for linking (default: cc)\n",
               out);
}

bool usage_error(const std::string& message)
{
    std::fprintf(stderr, "hana: %s\n\n", message.c_str());
    print_usage(stderr);
    return false;
}

bool take_source_path(std::string_view arg, std::string& path)
{
    if (!path.empty()) {
        return usage_error("only one source file is supported");
    }
    path = std::string(arg);
    return true;
}

bool parse_check(const std::vector<std::string_view>& args, hana::driver::CheckOptions& options)
{
    for (std::string_view arg : args) {
        if (arg == "--tokens") {
            options.dump_tokens = true;
        } else if (arg == "--ast") {
            options.dump_ast = true;
        } else if (arg.starts_with("-")) {
            return usage_error("unknown option '" + std::string(arg) + "' for 'check'");
        } else if (!take_source_path(arg, options.path)) {
            return false;
        }
    }
    return !options.path.empty() || usage_error("missing source file");
}

bool parse_build(const std::vector<std::string_view>& args, hana::driver::BuildOptions& options)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-O") {
            options.optimize = true;
        } else if (arg == "--emit-ir") {
            options.emit_ir = true;
        } else if (arg == "-o") {
            if (i + 1 >= args.size()) {
                return usage_error("'-o' needs an output path");
            }
            options.output = std::string(args[++i]);
        } else if (arg.starts_with("-")) {
            return usage_error("unknown option '" + std::string(arg) + "' for 'build'");
        } else if (!take_source_path(arg, options.path)) {
            return false;
        }
    }
    return !options.path.empty() || usage_error("missing source file");
}

bool parse_run(const std::vector<std::string_view>& args, hana::driver::RunOptions& options)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--") {
            for (++i; i < args.size(); ++i) {
                options.program_args.emplace_back(args[i]);
            }
            break;
        }
        if (arg == "-O") {
            options.optimize = true;
        } else if (arg.starts_with("-")) {
            return usage_error("unknown option '" + std::string(arg) + "' for 'run'");
        } else if (!take_source_path(arg, options.path)) {
            return false;
        }
    }
    return !options.path.empty() || usage_error("missing source file");
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) {
        print_usage(stderr);
        return kExitUsage;
    }
    const std::string_view command = args.front();
    const std::vector<std::string_view> rest(args.begin() + 1, args.end());

    if (command == "--version") {
        std::puts("HANA " HANA_VERSION);
        return kExitOk;
    }
    if (command == "--help" || command == "-h") {
        print_usage(stdout);
        return kExitOk;
    }
    if (command == "check") {
        hana::driver::CheckOptions options;
        return parse_check(rest, options) ? hana::driver::run_check(options) : kExitUsage;
    }
    if (command == "build") {
        hana::driver::BuildOptions options;
        return parse_build(rest, options) ? hana::driver::run_build(options) : kExitUsage;
    }
    if (command == "run") {
        hana::driver::RunOptions options;
        return parse_run(rest, options) ? hana::driver::run_program(options) : kExitUsage;
    }
    usage_error("unknown command '" + std::string(command) + "'");
    return kExitUsage;
}
