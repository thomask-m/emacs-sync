// emacs-sync: install a known-good Emacs config (baked into this binary) on
// the current machine, install every package it uses, and verify they load.

#include "embedded.hpp"
#include "platform.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace pf = es::platform;

namespace {

// Which embedded file goes where, relative to the Emacs home directory.
struct ConfigFile {
    const char* id;          // id in embedded.hpp / CMakeLists.txt
    const char* home_rel;    // destination relative to ~
    const char* source_rel;  // path in the source tree (for `capture`)
};

const ConfigFile kConfigFiles[] = {
    {"dot_emacs", ".emacs", "config/dot-emacs.el"},
    {"emacs_custom", ".emacs.custom.el", "config/emacs-custom.el"},
};

struct Options {
    std::string command;
    std::optional<fs::path> home;
    std::optional<fs::path> emacs;
    bool force = false;
    bool skip_packages = false;
    bool skip_verify = false;
    std::vector<std::string> positional;
};

void print_usage() {
    std::cout <<
R"(emacs-sync )" EMACS_SYNC_VERSION R"( - keep one Emacs config in sync across machines

Usage: emacs-sync <command> [options]

Commands:
  install     Write the bundled config to ~ (backing up anything it replaces),
              install every package the config uses, then verify they load.
  packages    Install missing packages for the config already in ~, then verify.
  verify      Check that the config in ~ loads and every package is usable.
  status      Show whether the files in ~ match the bundled config.
  diff        Show a diff between the bundled config and the files in ~.
  print FILE  Print a bundled file to stdout (.emacs or .emacs.custom.el).
  capture     Copy the files in ~ into this tool's source tree, so you can
              rebuild and commit them (only works from a source checkout).
  help        Show this message.

Options:
  --home DIR       Treat DIR as the home directory instead of $HOME.
  --emacs PATH     Use this Emacs executable (default: search PATH).
  --force          install: overwrite without making backups.
  --skip-packages  install: only write the config files.
  --skip-verify    install/packages: don't run the verification step.
)";
}

std::optional<Options> parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "error: " << flag << " needs a value\n";
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };
        if (a == "--home") {
            auto v = need_value("--home");
            if (!v) return std::nullopt;
            o.home = fs::absolute(*v);
        } else if (a == "--emacs") {
            auto v = need_value("--emacs");
            if (!v) return std::nullopt;
            o.emacs = fs::path(*v);
        } else if (a == "--force") {
            o.force = true;
        } else if (a == "--skip-packages") {
            o.skip_packages = true;
        } else if (a == "--skip-verify") {
            o.skip_verify = true;
        } else if (a == "-h" || a == "--help") {
            o.command = "help";
        } else if (a == "--version") {
            o.command = "version";
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown option " << a << "\n";
            return std::nullopt;
        } else if (o.command.empty()) {
            o.command = a;
        } else {
            o.positional.push_back(a);
        }
    }
    if (o.command.empty()) o.command = "help";
    return o;
}

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, std::string_view data) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".emacs-sync-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string_view embedded_or_die(const char* id) {
    auto data = es::embedded::get(id);
    if (!data) {
        std::cerr << "internal error: missing embedded file " << id << "\n";
        std::exit(2);
    }
    return *data;
}

std::optional<fs::path> resolve_home(const Options& o) {
    if (o.home) return o.home;
    auto h = pf::emacs_home();
    if (!h) std::cerr << "error: could not determine home directory (set HOME or use --home)\n";
    return h;
}

std::optional<fs::path> resolve_emacs(const Options& o) {
    std::optional<fs::path> emacs = o.emacs ? pf::find_executable(o.emacs->string()) : pf::find_emacs();
    if (!emacs) {
        std::cerr << "error: Emacs not found";
        if (o.emacs) std::cerr << " at " << o.emacs->string();
        std::cerr << ".\nInstall it with:\n  " << pf::emacs_install_hint()
                  << "\nor point to it with --emacs PATH.\n";
        return std::nullopt;
    }
    auto version = pf::capture(*emacs, {"--batch", "-Q", "--eval",
                                        "(princ (format \"%d %d\" emacs-major-version emacs-minor-version))"});
    if (!version) {
        std::cerr << "error: could not run " << emacs->string() << "\n";
        return std::nullopt;
    }
    int major = 0, minor = 0;
    std::istringstream(*version) >> major >> minor;
    std::cout << "Using Emacs " << major << "." << minor << " at " << emacs->string() << "\n";
    if (major < 27) {
        std::cerr << "error: Emacs 27.1 or newer is required.\n";
        return std::nullopt;
    }
    return emacs;
}

// Runs the embedded bootstrap.el in batch mode with the given mode.
int run_bootstrap(const fs::path& emacs, const fs::path& home, const std::string& mode) {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / ("emacs-sync-" + timestamp());
    fs::create_directories(dir, ec);
    fs::path script = dir / "bootstrap.el";
    if (!write_file(script, embedded_or_die("bootstrap"))) {
        std::cerr << "error: could not write " << script.string() << "\n";
        return 1;
    }
    std::vector<std::string> env = {
        "HOME=" + home.string(),
        "EMACS_SYNC_MODE=" + mode,
        "EMACS_SYNC_INIT=" + (home / ".emacs").string(),
        "EMACS_SYNC_CUSTOM=" + (home / ".emacs.custom.el").string(),
    };
    std::cout.flush();
    int rc = pf::run(emacs, {"--batch", "-Q", "-l", script.string()}, env);
    fs::remove_all(dir, ec);
    if (rc < 0) {
        std::cerr << "error: failed to start " << emacs.string() << "\n";
        return 1;
    }
    return rc;
}

int cmd_status(const Options& o) {
    auto home = resolve_home(o);
    if (!home) return 1;
    int differing = 0;
    for (const auto& f : kConfigFiles) {
        fs::path dest = *home / f.home_rel;
        auto on_disk = read_file(dest);
        const char* state;
        if (!on_disk) {
            state = "missing";
            ++differing;
        } else if (*on_disk == embedded_or_die(f.id)) {
            state = "up to date";
        } else {
            state = "differs";
            ++differing;
        }
        std::cout << "  " << dest.string() << ": " << state << "\n";
    }
    if (differing) std::cout << "Run `emacs-sync install` to update, or `emacs-sync diff` to see changes.\n";
    return differing ? 1 : 0;
}

int cmd_diff(const Options& o) {
    auto home = resolve_home(o);
    if (!home) return 1;
    auto diff = pf::find_executable("diff");
    if (!diff) {
        std::cerr << "error: `diff` not found on PATH\n";
        return 1;
    }
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / ("emacs-sync-diff-" + timestamp());
    fs::create_directories(dir, ec);
    int rc = 0;
    for (const auto& f : kConfigFiles) {
        fs::path bundled = dir / (std::string("bundled") + f.home_rel);
        write_file(bundled, embedded_or_die(f.id));
        fs::path dest = *home / f.home_rel;
        if (!fs::exists(dest)) {
            std::cout << dest.string() << " does not exist\n";
            rc = 1;
            continue;
        }
        int r = pf::run(*diff, {"-u", "--label", std::string("bundled/") + f.home_rel,
                                "--label", dest.string(), bundled.string(), dest.string()});
        if (r != 0) rc = 1;
    }
    fs::remove_all(dir, ec);
    return rc;
}

int cmd_print(const Options& o) {
    if (o.positional.size() != 1) {
        std::cerr << "usage: emacs-sync print <.emacs|.emacs.custom.el>\n";
        return 1;
    }
    for (const auto& f : kConfigFiles) {
        if (o.positional[0] == f.home_rel || o.positional[0] == f.id) {
            std::cout << embedded_or_die(f.id);
            return 0;
        }
    }
    std::cerr << "error: unknown file " << o.positional[0] << "\n";
    return 1;
}

int cmd_capture(const Options& o) {
    auto home = resolve_home(o);
    if (!home) return 1;
    fs::path src = EMACS_SYNC_SOURCE_DIR;
    if (!fs::exists(src / "CMakeLists.txt")) {
        std::cerr << "error: source tree " << src.string()
                  << " not found; `capture` only works from a source build.\n";
        return 1;
    }
    for (const auto& f : kConfigFiles) {
        fs::path from = *home / f.home_rel;
        auto data = read_file(from);
        if (!data) {
            std::cerr << "error: cannot read " << from.string() << "\n";
            return 1;
        }
        fs::path to = src / f.source_rel;
        if (!write_file(to, *data)) {
            std::cerr << "error: cannot write " << to.string() << "\n";
            return 1;
        }
        std::cout << "  " << from.string() << " -> " << to.string() << "\n";
    }
    std::cout << "Captured. Rebuild (cmake --build build) and commit to publish.\n";
    return 0;
}

int do_verify(const fs::path& emacs, const fs::path& home) {
    std::cout << "Verifying config in " << home.string() << "...\n";
    int rc = run_bootstrap(emacs, home, "verify");
    std::cout << (rc == 0 ? "Verification passed.\n" : "Verification FAILED.\n");
    return rc == 0 ? 0 : 1;
}

int do_packages(const Options& o, const fs::path& emacs, const fs::path& home) {
    std::cout << "Installing packages...\n";
    if (run_bootstrap(emacs, home, "install") != 0) {
        std::cerr << "Package installation had errors (see above).\n";
        return 1;
    }
    if (o.skip_verify) return 0;
    return do_verify(emacs, home);
}

int cmd_verify(const Options& o) {
    auto home = resolve_home(o);
    if (!home) return 1;
    auto emacs = resolve_emacs(o);
    if (!emacs) return 1;
    return do_verify(*emacs, *home);
}

int cmd_packages(const Options& o) {
    auto home = resolve_home(o);
    if (!home) return 1;
    auto emacs = resolve_emacs(o);
    if (!emacs) return 1;
    return do_packages(o, *emacs, *home);
}

int cmd_install(const Options& o) {
    auto home = resolve_home(o);
    if (!home) return 1;
    // Check for Emacs before touching any files.
    std::optional<fs::path> emacs;
    if (!o.skip_packages) {
        emacs = resolve_emacs(o);
        if (!emacs) return 1;
    }

    std::cout << "Writing config to " << home->string() << " (" << pf::os_name(pf::current_os()) << ")\n";
    const std::string stamp = timestamp();
    for (const auto& f : kConfigFiles) {
        fs::path dest = *home / f.home_rel;
        std::string_view bundled = embedded_or_die(f.id);
        auto existing = read_file(dest);
        if (existing && *existing == bundled) {
            std::cout << "  " << dest.string() << ": already up to date\n";
            continue;
        }
        if (existing && !o.force) {
            fs::path backup = dest;
            backup += ".bak." + stamp;
            std::error_code ec;
            fs::copy_file(dest, backup, ec);
            if (ec) {
                std::cerr << "error: could not back up " << dest.string() << ": " << ec.message() << "\n";
                return 1;
            }
            std::cout << "  backed up " << dest.string() << " -> " << backup.string() << "\n";
        }
        if (!write_file(dest, bundled)) {
            std::cerr << "error: could not write " << dest.string() << "\n";
            return 1;
        }
        std::cout << "  wrote " << dest.string() << "\n";
    }
    if (o.skip_packages) return 0;
    return do_packages(o, *emacs, *home);
}

} // namespace

int main(int argc, char** argv) {
    auto opts = parse_args(argc, argv);
    if (!opts) {
        print_usage();
        return 2;
    }
    const std::string& c = opts->command;
    if (c == "install") return cmd_install(*opts);
    if (c == "packages") return cmd_packages(*opts);
    if (c == "verify") return cmd_verify(*opts);
    if (c == "status") return cmd_status(*opts);
    if (c == "diff") return cmd_diff(*opts);
    if (c == "print") return cmd_print(*opts);
    if (c == "capture") return cmd_capture(*opts);
    if (c == "version") {
        std::cout << "emacs-sync " << EMACS_SYNC_VERSION << "\n";
        return 0;
    }
    if (c == "help") {
        print_usage();
        return 0;
    }
    std::cerr << "error: unknown command " << c << "\n";
    print_usage();
    return 2;
}
