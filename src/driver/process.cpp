#include "driver/process.h"

#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <random>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

extern char** environ;
#endif

namespace hana::driver {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// Appends one argument quoted so that CommandLineToArgvW and the C runtime
// parse it back unchanged.
void append_quoted_argument(const std::wstring& arg, std::wstring& command_line)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        command_line += arg;
        return;
    }
    command_line += L'"';
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            command_line.append(backslashes * 2 + 1, L'\\');
        } else {
            command_line.append(backslashes, L'\\');
        }
        backslashes = 0;
        command_line += c;
    }
    command_line.append(backslashes * 2, L'\\');
    command_line += L'"';
}

std::wstring build_command_line(const std::vector<std::string>& args)
{
    std::wstring command_line;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            command_line += L' ';
        }
        append_quoted_argument(widen(args[i]), command_line);
    }
    return command_line;
}

} // namespace

int run_process(const std::vector<std::string>& args, ChildOutput output)
{
    if (args.empty()) {
        return -1;
    }
    std::wstring command_line = build_command_line(args);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    // The child's standard output and error both go to an inheritable copy
    // of hana's standard error.
    HANDLE error_copy = nullptr;
    if (output == ChildOutput::ToStderr) {
        std::fflush(stdout);
        std::fflush(stderr);
        if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
                             GetCurrentProcess(), &error_copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            return -1;
        }
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = error_copy;
        startup.hStdError = error_copy;
    }
    PROCESS_INFORMATION process{};
    // A null application name makes CreateProcessW search PATH and append
    // ".exe" when the program name has no extension.
    const BOOL started = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0,
                                        nullptr, nullptr, &startup, &process);
    if (error_copy != nullptr) {
        CloseHandle(error_copy);
    }
    if (!started) {
        return -1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    const BOOL ok = GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (!ok) {
        return -1;
    }
    return static_cast<int>(exit_code);
}

std::filesystem::path current_executable_path()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path create_temp_directory()
{
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
        return {};
    }
    std::random_device device;
    std::mt19937_64 generator(device());
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path candidate =
            base / ("hana-" + std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(generator() % 1000000000));
        // create_directory reports false when the directory already exists,
        // so a name is never shared with another process.
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error) {
            return {};
        }
    }
    return {};
}

void set_stdout_binary()
{
    std::fflush(stdout);
    _setmode(_fileno(stdout), _O_BINARY);
}

#else

int run_process(const std::vector<std::string>& args, ChildOutput output)
{
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        return -1;
    }
    if (output == ChildOutput::ToStderr) {
        std::fflush(stdout);
        std::fflush(stderr);
        posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
    }
    pid_t pid = 0;
    const int spawn_error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_error != 0) {
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

std::filesystem::path current_executable_path()
{
    std::error_code error;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return {};
    }
    return self;
}

std::filesystem::path create_temp_directory()
{
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
        return {};
    }
    std::string pattern = (base / "hana-XXXXXX").string();
    if (mkdtemp(pattern.data()) == nullptr) {
        return {};
    }
    return std::filesystem::path(pattern);
}

void set_stdout_binary() {}

#endif

} // namespace hana::driver
