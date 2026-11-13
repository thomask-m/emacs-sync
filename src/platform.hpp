#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Everything OS-specific lives behind this interface so that adding Windows
// (or anything else) only means touching platform.cpp.
namespace es::platform {

namespace fs = std::filesystem;

enum class Os { MacOS, Linux, Windows, Other };

Os current_os();
const char* os_name(Os os);

// The directory Emacs treats as "~". On POSIX this is $HOME. On Windows Emacs
// uses %HOME% if set, otherwise %APPDATA%, so we mirror that.
std::optional<fs::path> emacs_home();

// Look up an executable on PATH (plus a few well-known install locations for
// Emacs). Returns nullopt if not found.
std::optional<fs::path> find_executable(const std::string& name);
std::optional<fs::path> find_emacs();

// Run a program with arguments, inheriting stdio. `extra_env` entries are
// "NAME=value" strings added to (or overriding) the current environment.
// Returns the exit code, or -1 if the program could not be started.
int run(const fs::path& program, const std::vector<std::string>& args,
        const std::vector<std::string>& extra_env = {});

// Run a program and capture its stdout. Returns nullopt on failure.
std::optional<std::string> capture(const fs::path& program,
                                   const std::vector<std::string>& args);

// One-line hints for installing things with the host's package manager.
std::string emacs_install_hint();

} // namespace es::platform
