#include "driver/driver.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>

#include <unistd.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include "codegen/codegen.h"
#include "driver/process.h"
#include "lang/code_lexer.h"
#include "lang/formatter.h"
#include "lang/outer_parser.h"
#include "lang/sema.h"
#include "support/diagnostic.h"

namespace hana::driver {

namespace {

struct Frontend {
    SourceFile file;
    DiagnosticEngine diagnostics;
    std::vector<lang::CodeRegion> code_regions;
    std::unique_ptr<lang::Module> module;
    std::unique_ptr<lang::Program> program;
};

void tool_error(const std::string& message)
{
    std::fprintf(stderr, "hana: error: %s\n", message.c_str());
}

bool read_source(const std::string& path, SourceFile& file)
{
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) {
        tool_error("cannot open '" + path + "'");
        return false;
    }
    file.path = path;
    file.text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (stream.bad()) {
        tool_error("cannot read '" + path + "'");
        return false;
    }
    return true;
}

void print_diagnostics(const Frontend& frontend)
{
    for (const Diagnostic& diagnostic : frontend.diagnostics.diagnostics()) {
        std::fputs(format_diagnostic(frontend.file, diagnostic).c_str(), stderr);
    }
}

// Lexes, parses and checks the file. Semantic analysis runs only on a
// syntactically valid module, so syntax errors do not cause follow-up noise.
// Returns true when there are no errors.
bool run_frontend(Frontend& frontend)
{
    lang::OuterParser parser(frontend.file, frontend.diagnostics);
    frontend.module = parser.parse();
    frontend.code_regions = parser.code_regions();
    if (frontend.diagnostics.has_errors()) {
        return false;
    }
    lang::Sema sema(frontend.diagnostics);
    frontend.program = sema.check(*frontend.module);
    return !frontend.diagnostics.has_errors();
}

bool load_and_check(const std::string& path, Frontend& frontend)
{
    if (!read_source(path, frontend.file)) {
        return false;
    }
    const bool ok = run_frontend(frontend);
    print_diagnostics(frontend);
    return ok;
}

struct Backend {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::TargetMachine> target;
    std::unique_ptr<llvm::Module> module;
};

struct GenerateOptions {
    bool optimize = false;
    bool sanitize_address = false;
};

bool generate_code(const Frontend& frontend, const GenerateOptions& generate, Backend& backend)
{
    const bool optimize = generate.optimize;
    codegen::CodegenOptions options;
    options.check_overflow = !optimize;
    std::string error;
    backend.target = codegen::create_host_target_machine(optimize, error);
    if (!backend.target) {
        tool_error(error);
        return false;
    }
    backend.module = codegen::generate_module(backend.context, *backend.target, *frontend.module,
                                              *frontend.program, frontend.file.path, options);
    std::string verify_message;
    llvm::raw_string_ostream verify_stream(verify_message);
    if (llvm::verifyModule(*backend.module, &verify_stream)) {
        tool_error("internal compiler error: generated invalid LLVM IR\n" + verify_message);
        return false;
    }
    if (optimize) {
        codegen::optimize_module(*backend.module, *backend.target);
    }
    if (generate.sanitize_address) {
        codegen::instrument_address_sanitizer(*backend.module, *backend.target);
    }
    return true;
}

std::string linker_program()
{
    const char* configured = std::getenv("HANA_CC");
    return (configured != nullptr && *configured != '\0') ? configured : "cc";
}

// The runtime library is found through HANA_RUNTIME, or next to the hana
// executable (build tree), or in ../lib relative to it (installed layout).
std::string find_runtime_library()
{
    if (const char* configured = std::getenv("HANA_RUNTIME"); configured != nullptr && *configured) {
        return configured;
    }
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return {};
    }
    const std::filesystem::path candidates[] = {
        self.parent_path() / "libhana_rt.a",
        self.parent_path().parent_path() / "lib" / "libhana_rt.a",
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate, error)) {
            return candidate.string();
        }
    }
    return {};
}

bool link_executable(const std::string& object_path, const std::string& output_path,
                     bool sanitize_address)
{
    const std::string runtime = find_runtime_library();
    if (runtime.empty()) {
        tool_error("cannot find the runtime library libhana_rt.a (set HANA_RUNTIME to its path)");
        return false;
    }
    const std::string linker = linker_program();
    std::vector<std::string> command{linker, object_path, runtime, "-o", output_path};
    if (sanitize_address) {
        command.push_back("-fsanitize=address");
    }
    const int status = run_process(command);
    if (status == -1) {
        tool_error("cannot run the linker '" + linker + "' (set HANA_CC to choose another)");
        return false;
    }
    if (status != 0) {
        tool_error("linking failed (linker '" + linker + "' exited with " +
                   std::to_string(status) + ")");
        return false;
    }
    return true;
}

bool build_executable(const Frontend& frontend, const GenerateOptions& generate,
                      const std::string& object_path, const std::string& output_path)
{
    Backend backend;
    if (!generate_code(frontend, generate, backend)) {
        return false;
    }
    std::string error;
    if (!codegen::emit_object_file(*backend.module, *backend.target, object_path, error)) {
        tool_error(error);
        return false;
    }
    return link_executable(object_path, output_path, generate.sanitize_address);
}

std::string default_output_path(const std::string& source_path)
{
    return std::filesystem::path(source_path).stem().string();
}

// Creates a private temporary directory; removed by the destructor.
class TempDirectory {
public:
    TempDirectory()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "hana-XXXXXX").string();
        if (mkdtemp(pattern.data()) != nullptr) {
            path_ = pattern;
        }
    }
    ~TempDirectory()
    {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    bool valid() const { return !path_.empty(); }
    std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

} // namespace

int run_check(const CheckOptions& options)
{
    Frontend frontend;
    if (!read_source(options.path, frontend.file)) {
        return kExitUsage;
    }
    const bool ok = run_frontend(frontend);
    if (options.dump_tokens) {
        for (const lang::CodeRegion& region : frontend.code_regions) {
            DiagnosticEngine ignored;
            lang::CodeLexer lexer(frontend.file.text, region, ignored);
            std::fputs(lang::dump_tokens(lexer.tokenize()).c_str(), stdout);
        }
    }
    if (options.dump_ast && frontend.module) {
        std::fputs(lang::dump_module(*frontend.module).c_str(), stdout);
    }
    if (options.json_diagnostics) {
        std::fputs(diagnostics_to_json(frontend.file, frontend.diagnostics.diagnostics()).c_str(),
                   stdout);
    } else {
        print_diagnostics(frontend);
    }
    return ok ? kExitOk : kExitCompileError;
}

int run_format(const FormatOptions& options)
{
    Frontend frontend;
    if (!read_source(options.path, frontend.file)) {
        return kExitUsage;
    }
    const std::optional<std::string> formatted =
        lang::format_source(frontend.file, frontend.diagnostics);
    if (!formatted) {
        print_diagnostics(frontend);
        tool_error("'" + options.path + "' has syntax errors and was not formatted");
        return kExitCompileError;
    }
    const bool changed = *formatted != frontend.file.text;
    if (options.to_stdout) {
        std::fputs(formatted->c_str(), stdout);
        return kExitOk;
    }
    if (options.check_only) {
        if (changed) {
            std::fprintf(stderr, "%s: not formatted\n", options.path.c_str());
        }
        return changed ? kExitCompileError : kExitOk;
    }
    if (!changed) {
        return kExitOk;
    }
    std::ofstream out(options.path, std::ios::out | std::ios::binary | std::ios::trunc);
    out << *formatted;
    if (!out) {
        tool_error("cannot write '" + options.path + "'");
        return kExitToolFailure;
    }
    return kExitOk;
}

int run_build(const BuildOptions& options)
{
    Frontend frontend;
    if (!load_and_check(options.path, frontend)) {
        return frontend.file.path.empty() ? kExitUsage : kExitCompileError;
    }
    if (frontend.program->main == nullptr && !options.emit_ir) {
        tool_error("'" + options.path + "' has no 'main' function");
        return kExitCompileError;
    }
    if (options.emit_ir) {
        Backend backend;
        if (!generate_code(frontend, GenerateOptions{options.optimize, options.sanitize_address}, backend)) {
            return kExitToolFailure;
        }
        std::fputs(codegen::module_to_ir_text(*backend.module).c_str(), stdout);
        return kExitOk;
    }
    TempDirectory temp;
    if (!temp.valid()) {
        tool_error("cannot create a temporary directory");
        return kExitToolFailure;
    }
    const std::string output = options.output.empty() ? default_output_path(options.path)
                                                      : options.output;
    if (!build_executable(frontend, GenerateOptions{options.optimize, options.sanitize_address}, temp.file("main.o"), output)) {
        return kExitToolFailure;
    }
    return kExitOk;
}

int run_program(const RunOptions& options)
{
    Frontend frontend;
    if (!load_and_check(options.path, frontend)) {
        return frontend.file.path.empty() ? kExitUsage : kExitCompileError;
    }
    if (frontend.program->main == nullptr) {
        tool_error("'" + options.path + "' has no 'main' function");
        return kExitCompileError;
    }
    TempDirectory temp;
    if (!temp.valid()) {
        tool_error("cannot create a temporary directory");
        return kExitToolFailure;
    }
    const std::string executable = temp.file("program");
    if (!build_executable(frontend, GenerateOptions{options.optimize, options.sanitize_address}, temp.file("main.o"), executable)) {
        return kExitToolFailure;
    }
    std::vector<std::string> args{executable};
    args.insert(args.end(), options.program_args.begin(), options.program_args.end());
    std::fflush(stdout);
    const int status = run_process(args);
    if (status == -1) {
        tool_error("cannot start the compiled program");
        return kExitToolFailure;
    }
    return status;
}

} // namespace hana::driver
