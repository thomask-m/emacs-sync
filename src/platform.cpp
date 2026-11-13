#include "platform.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace es::platform {

Os current_os() {
#if defined(_WIN32)
    return Os::Windows;
#elif defined(__APPLE__)
    return Os::MacOS;
#elif defined(__linux__)
    return Os::Linux;
#else
    return Os::Other;
#endif
}

const char* os_name(Os os) {
    switch (os) {
    case Os::MacOS: return "macOS";
    case Os::Linux: return "Linux";
    case Os::Windows: return "Windows";
    default: return "unknown";
    }
}

static std::optional<std::string> getenv_str(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

std::optional<fs::path> emacs_home() {
    if (auto home = getenv_str("HOME")) return fs::path(*home);
#ifdef _WIN32
    if (auto appdata = getenv_str("APPDATA")) return fs::path(*appdata);
    if (auto profile = getenv_str("USERPROFILE")) return fs::path(*profile);
#endif
    return std::nullopt;
}

static bool is_executable_file(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
#ifdef _WIN32
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

std::optional<fs::path> find_executable(const std::string& name) {
    fs::path as_path(name);
    if (as_path.has_parent_path()) {
        if (is_executable_file(as_path)) return as_path;
        return std::nullopt;
    }
    auto path_env = getenv_str("PATH");
    if (!path_env) return std::nullopt;
#ifdef _WIN32
    const char sep = ';';
    const std::vector<std::string> exts = {".exe", ".cmd", ".bat", ""};
#else
    const char sep = ':';
    const std::vector<std::string> exts = {""};
#endif
    size_t start = 0;
    while (start <= path_env->size()) {
        size_t end = path_env->find(sep, start);
        if (end == std::string::npos) end = path_env->size();
        std::string dir = path_env->substr(start, end - start);
        if (!dir.empty()) {
            for (const auto& ext : exts) {
                fs::path candidate = fs::path(dir) / (name + ext);
                if (is_executable_file(candidate)) return candidate;
            }
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<fs::path> find_emacs() {
    if (auto override_path = getenv_str("EMACS_SYNC_EMACS")) {
        return find_executable(*override_path);
    }
    if (current_os() == Os::MacOS) {
        // Prefer the GUI build: Homebrew's `emacs` formula is terminal-only,
        // so it can't exercise GUI-only settings (scroll bars, fonts, ...).
        for (const char* app : {"/Applications/Emacs.app/Contents/MacOS/Emacs",
                                "~/Applications/Emacs.app/Contents/MacOS/Emacs"}) {
            fs::path p = app;
            if (app[0] == '~') {
                auto home = getenv_str("HOME");
                if (!home) continue;
                p = fs::path(*home) / std::string(app + 2);
            }
            if (is_executable_file(p)) return p;
        }
    }
    if (auto p = find_executable("emacs")) return p;
    // Common locations that may not be on PATH (e.g. a GUI-launched shell).
    std::vector<fs::path> fallbacks;
    switch (current_os()) {
    case Os::MacOS:
        fallbacks = {"/opt/homebrew/bin/emacs", "/usr/local/bin/emacs"};
        break;
    case Os::Linux:
        fallbacks = {"/usr/bin/emacs", "/usr/local/bin/emacs", "/snap/bin/emacs"};
        break;
    case Os::Windows:
        if (auto pf = getenv_str("ProgramFiles")) {
            std::error_code ec;
            fs::path base = fs::path(*pf) / "Emacs";
            if (fs::is_directory(base, ec)) {
                for (const auto& entry : fs::recursive_directory_iterator(base, ec)) {
                    if (entry.path().filename() == "emacs.exe") fallbacks.push_back(entry.path());
                }
            }
        }
        break;
    default: break;
    }
    for (const auto& p : fallbacks) {
        if (is_executable_file(p)) return p;
    }
    return std::nullopt;
}

#ifdef _WIN32

// Quote one argument following the MSVCRT command-line parsing rules.
static std::string quote_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

int run(const fs::path& program, const std::vector<std::string>& args,
        const std::vector<std::string>& extra_env) {
    for (const auto& kv : extra_env) _putenv(kv.c_str());
    std::vector<std::string> quoted;
    quoted.push_back(quote_arg(program.string()));
    for (const auto& a : args) quoted.push_back(quote_arg(a));
    std::vector<const char*> argv;
    for (const auto& q : quoted) argv.push_back(q.c_str());
    argv.push_back(nullptr);
    intptr_t rc = _spawnv(_P_WAIT, program.string().c_str(), argv.data());
    return static_cast<int>(rc);
}

std::optional<std::string> capture(const fs::path& program, const std::vector<std::string>& args) {
    std::string cmd = quote_arg(program.string());
    for (const auto& a : args) cmd += " " + quote_arg(a);
    FILE* pipe = _popen(("\"" + cmd + "\"").c_str(), "r");
    if (!pipe) return std::nullopt;
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) out.append(buf, n);
    if (_pclose(pipe) != 0) return std::nullopt;
    return out;
}

#else

static std::vector<std::string> build_env(const std::vector<std::string>& extra_env) {
    std::map<std::string, std::string> env;
    std::vector<std::string> order;
    auto add = [&](const std::string& kv) {
        auto eq = kv.find('=');
        std::string key = kv.substr(0, eq);
        if (env.find(key) == env.end()) order.push_back(key);
        env[key] = kv;
    };
    for (char** e = environ; *e != nullptr; ++e) add(*e);
    for (const auto& kv : extra_env) add(kv);
    std::vector<std::string> out;
    for (const auto& k : order) out.push_back(env[k]);
    return out;
}

static int spawn_and_wait(const fs::path& program, const std::vector<std::string>& args,
                          const std::vector<std::string>& extra_env, int stdout_fd) {
    std::vector<std::string> argv_store;
    argv_store.push_back(program.string());
    argv_store.insert(argv_store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argv_store) argv.push_back(a.data());
    argv.push_back(nullptr);

    auto env_store = build_env(extra_env);
    std::vector<char*> envp;
    for (auto& e : env_store) envp.push_back(e.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (stdout_fd >= 0) posix_spawn_file_actions_adddup2(&actions, stdout_fd, STDOUT_FILENO);

    pid_t pid;
    int err = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (err != 0) return -1;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

int run(const fs::path& program, const std::vector<std::string>& args,
        const std::vector<std::string>& extra_env) {
    return spawn_and_wait(program, args, extra_env, -1);
}

std::optional<std::string> capture(const fs::path& program, const std::vector<std::string>& args) {
    int fds[2];
    if (pipe(fds) != 0) return std::nullopt;
    // Spawn asynchronously from the reader's point of view: the child writes
    // into the pipe while we drain it, so large outputs cannot deadlock.
    std::string out;
    int rc = -1;
    pid_t reader = fork();
    if (reader < 0) {
        close(fds[0]);
        close(fds[1]);
        return std::nullopt;
    }
    if (reader == 0) {
        close(fds[0]);
        int code = spawn_and_wait(program, args, {}, fds[1]);
        close(fds[1]);
        _exit(code < 0 ? 127 : code);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    if (waitpid(reader, &status, 0) >= 0 && WIFEXITED(status)) rc = WEXITSTATUS(status);
    if (rc != 0) return std::nullopt;
    return out;
}

#endif

std::string emacs_install_hint() {
    switch (current_os()) {
    case Os::MacOS:
        return "brew install emacs   (or: brew install --cask emacs)";
    case Os::Linux:
        return "sudo apt install emacs   |   sudo dnf install emacs   |   sudo pacman -S emacs";
    case Os::Windows:
        return "winget install GNU.Emacs   (or download from https://ftp.gnu.org/gnu/emacs/windows/)";
    default:
        return "install GNU Emacs 27.1 or newer from https://www.gnu.org/software/emacs/";
    }
}

} // namespace es::platform
