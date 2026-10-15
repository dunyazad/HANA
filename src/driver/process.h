#pragma once

#include <string>
#include <vector>

namespace hana::driver {

// Runs a program found through PATH with the given arguments (args[0] is the
// program) and waits for it. Returns its exit code, 128 + signal number when
// it was killed by a signal, or -1 when it could not be started.
int run_process(const std::vector<std::string>& args);

} // namespace hana::driver
