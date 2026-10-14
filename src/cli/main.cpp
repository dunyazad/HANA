#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "lang/lexer.h"
#include "platform/native/native_file_system.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitSourceErrors = 1;
constexpr int kExitUsage = 2;

struct CheckOptions {
    bool dump_tokens = false;
    std::string path;
};

void print_usage()
{
    std::fputs("HANA " HANA_VERSION "\n"
               "\n"
               "usage:\n"
               "  hana check [--tokens] <file.hana>   check a source file\n"
               "  hana run <file.hana>                run an app (not implemented yet)\n"
               "  hana build <file.hana>              build an app (not implemented yet)\n"
               "  hana --version\n"
               "\n"
               "check options:\n"
               "  --tokens    print the token stream\n",
               stderr);
}

bool parse_check_options(const std::vector<std::string_view>& args, CheckOptions& options)
{
    for (std::string_view arg : args) {
        if (arg == "--tokens") {
            options.dump_tokens = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "hana check: unknown option '%.*s'\n", static_cast<int>(arg.size()),
                         arg.data());
            return false;
        } else if (options.path.empty()) {
            options.path = std::string(arg);
        } else {
            std::fputs("hana check: only one source file is supported\n", stderr);
            return false;
        }
    }
    if (options.path.empty()) {
        std::fputs("hana check: missing source file\n", stderr);
        return false;
    }
    return true;
}

void print_diagnostics(const std::string& path, const std::vector<hana::lang::Diagnostic>& diagnostics)
{
    for (const hana::lang::Diagnostic& diagnostic : diagnostics) {
        std::fprintf(stderr, "%s:%u:%u: error: %s\n", path.c_str(),
                     static_cast<unsigned>(diagnostic.location.line),
                     static_cast<unsigned>(diagnostic.location.column), diagnostic.message.c_str());
    }
}

int check_source(const CheckOptions& options, const std::string& source)
{
    hana::lang::Lexer lexer(source);
    const std::vector<hana::lang::Token> tokens = lexer.tokenize();
    if (options.dump_tokens) {
        std::fputs(hana::lang::dump_tokens(tokens).c_str(), stdout);
    }
    print_diagnostics(options.path, lexer.diagnostics());
    return lexer.diagnostics().empty() ? kExitOk : kExitSourceErrors;
}

int run_check(const std::vector<std::string_view>& args)
{
    CheckOptions options;
    if (!parse_check_options(args, options)) {
        print_usage();
        return kExitUsage;
    }

    hana::platform::NativeFileSystem file_system;
    int exit_code = kExitUsage;
    file_system.read_text_file(options.path, [&](hana::platform::FileReadResult result) {
        if (!result.ok) {
            std::fprintf(stderr, "hana check: %s\n", result.error.c_str());
            exit_code = kExitUsage;
            return;
        }
        exit_code = check_source(options, result.contents);
    });
    return exit_code;
}

int run_not_implemented(std::string_view command)
{
    std::fprintf(stderr, "hana %.*s: not implemented yet\n", static_cast<int>(command.size()),
                 command.data());
    return kExitUsage;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) {
        print_usage();
        return kExitUsage;
    }

    const std::string_view command = args.front();
    const std::vector<std::string_view> rest(args.begin() + 1, args.end());

    if (command == "--version") {
        std::puts("HANA " HANA_VERSION);
        return kExitOk;
    }
    if (command == "--help" || command == "-h") {
        print_usage();
        return kExitOk;
    }
    if (command == "check") {
        return run_check(rest);
    }
    if (command == "run" || command == "build") {
        return run_not_implemented(command);
    }

    std::fprintf(stderr, "hana: unknown command '%.*s'\n", static_cast<int>(command.size()),
                 command.data());
    print_usage();
    return kExitUsage;
}
