# emacs-sync

One binary that puts my Emacs config on any machine and makes sure every
package in it is installed and working.

The config (`config/dot-emacs.el` → `~/.emacs`, `config/emacs-custom.el` →
`~/.emacs.custom.el`) is compiled **into** the binary, so a fresh machine needs
nothing except the binary and Emacs.

## On a new machine

Download the binary for the platform from the GitHub Releases page, then:

```sh
chmod +x emacs-sync-*
./emacs-sync-macos-arm64 install      # or emacs-sync-linux-x86_64 / -linux-arm64
```

`install` will:

1. Find Emacs (on macOS it prefers `/Applications/Emacs.app`; override with
   `--emacs PATH` or `EMACS_SYNC_EMACS`).
2. Write `~/.emacs` and `~/.emacs.custom.el`, backing up any existing file that
   differs to `<file>.bak.<timestamp>` (use `--force` to skip backups).
3. Install every package the config uses, from the archives the config
   declares (GNU ELPA, NonGNU ELPA, MELPA), and build vterm's native module.
4. Verify: load `~/.emacs` in a fresh Emacs and `require` every package. It
   exits non-zero if anything fails.

The package list is read from the config itself, so there's no second list to
keep up to date. It's every entry in `package-selected-packages` plus any
`(require 'x)` in `.emacs` that Emacs doesn't ship with.

### Prerequisites

* GNU Emacs 28.1+ (the tool runs on 27.1, but current magit needs 28)
* For vterm's native module: a C compiler, `make`, `cmake`, `libtool`
  * macOS: `xcode-select --install && brew install cmake libtool`
  * Debian/Ubuntu: `sudo apt install build-essential cmake libtool-bin`
  * Fedora: `sudo dnf install gcc make cmake libtool`
* `git`, for magit

## Other commands

```
emacs-sync status     # are ~/.emacs and ~/.emacs.custom.el identical to the bundled copies?
emacs-sync diff       # unified diff bundled vs. on-disk
emacs-sync packages   # (re)install packages for whatever config is in ~, then verify
emacs-sync verify     # just verify
emacs-sync print .emacs
emacs-sync capture    # copy ~/.emacs* into this source tree (source builds only)
```

`--home DIR` points any command at a different home directory, which is handy
for trying things out without touching your real one.

## Changing the config

```sh
# edit ~/.emacs as usual, then:
./build/emacs-sync capture        # copies ~/.emacs* into config/
cmake --build build               # re-embeds them
tests/smoke.sh                    # optional: full end-to-end test in a throwaway home
git commit -am "emacs: ..." && git push
git tag v0.x.y && git push --tags # CI builds and publishes release binaries
```

## Building from source

```sh
cmake -S . -B build
cmake --build build
```

C++17 and CMake 3.16+. No third-party dependencies.

## Tests

`tests/smoke.sh` runs `install` into an empty temporary home and starts a real
Emacs daemon there (a normal startup, not `--batch`). Then `tests/smoke.el`
uses each package for real: it opens `.rs`/`.go` files, expands a region, adds
a cursor, runs `magit-status` on a git repo, runs a command in vterm, runs
clang-format through the reformatter-defined `cf`, and so on. CI runs it on
Linux (x86_64 and arm64) and macOS.

## Layout

```
config/            the Emacs config that gets embedded
resources/         bootstrap.el: package install + verification, run by Emacs in batch mode
src/main.cpp       CLI
src/platform.*     everything OS-specific (paths, finding Emacs, spawning processes)
cmake/embed.cmake  turns the files above into a C++ source at build time
tests/             end-to-end smoke test
```

Windows support is stubbed in `src/platform.cpp`. It handles home-directory
resolution the way Emacs does (`%HOME%`, then `%APPDATA%`), finding
`emacs.exe`, and spawning processes. It hasn't been built or tested yet.
