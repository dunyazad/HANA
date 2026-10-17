#pragma once

#include <memory>
#include <string>

#include "lang/ast.h"
#include "lang/symbols.h"

namespace llvm {
class LLVMContext;
class Module;
class TargetMachine;
} // namespace llvm

namespace hana::codegen {

// Initializes the LLVM native target once per process.
void initialize_native_target();

// Creates a target machine for the host. Returns null and sets error on
// failure.
std::unique_ptr<llvm::TargetMachine> create_host_target_machine(bool optimize, std::string& error);

struct CodegenOptions {
    // Debug builds stop with a panic on integer overflow; release builds wrap.
    bool check_overflow = true;
};

// Lowers a checked module to LLVM IR. The module must have passed semantic
// analysis without errors. source_path is used in runtime error messages.
// The generated code calls the C runtime library in runtime/hana_rt.h.
std::unique_ptr<llvm::Module> generate_module(llvm::LLVMContext& context,
                                              llvm::TargetMachine& target,
                                              const lang::Module& module,
                                              const lang::Program& program,
                                              const std::string& source_path,
                                              const CodegenOptions& options);

// Runs the LLVM optimization pipeline (O2) on the module.
void optimize_module(llvm::Module& module, llvm::TargetMachine& target);

// Adds AddressSanitizer checks to every function of the module, so memory
// errors of the generated code are reported when the program runs. The
// program must be linked with the AddressSanitizer runtime.
void instrument_address_sanitizer(llvm::Module& module, llvm::TargetMachine& target);

// Writes textual LLVM IR.
std::string module_to_ir_text(const llvm::Module& module);

// Writes a native object file. Returns false and sets error on failure.
bool emit_object_file(llvm::Module& module, llvm::TargetMachine& target, const std::string& path,
                      std::string& error);

} // namespace hana::codegen
