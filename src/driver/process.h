#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hana::driver {

// Where the child's standard output goes.
enum class ChildOutput {
    Inherit,  // the same standard output as hana (the program being run)
    ToStderr, // hana's standard error (tools such as the linker), so that
              // hana's standard output carries only the program's output
};

// Runs a program found through PATH with the given arguments (args[0] is the
// program) and waits for it. Returns its exit code, 128 + signal number when
// it was killed by a signal (POSIX), or -1 when it could not be started.
// Arguments are UTF-8.
int run_process(const std::vector<std::string>& args, ChildOutput output = ChildOutput::Inherit);

// Path of the running executable, or an empty path when it is unknown.
std::filesystem::path current_executable_path();

// Creates a new private directory under the system temporary directory.
// Returns an empty path on failure.
std::filesystem::path create_temp_directory();

// Makes standard output write bytes unchanged. On Windows the C runtime
// otherwise turns every LF into CRLF; elsewhere this does nothing.
void set_stdout_binary();

} // namespace hana::driver
