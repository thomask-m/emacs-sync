#!/bin/sh
# End-to-end test: install the bundled config into an empty, throwaway home
# directory, start a real Emacs daemon there (normal startup, not --batch),
# and exercise every package via tests/smoke.el.
#
# Usage: tests/smoke.sh [path/to/emacs-sync] [path/to/emacs]
# Your real ~ is never touched.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
bin=${1:-"$here/../build/emacs-sync"}
emacs=${2:-}
if [ -z "$emacs" ]; then
  if [ -x /Applications/Emacs.app/Contents/MacOS/Emacs ]; then
    emacs=/Applications/Emacs.app/Contents/MacOS/Emacs
  else
    emacs=$(command -v emacs)
  fi
fi
emacsclient=$(dirname "$emacs")/bin/emacsclient
[ -x "$emacsclient" ] || emacsclient=$(dirname "$emacs")/emacsclient
[ -x "$emacsclient" ] || emacsclient=$(command -v emacsclient)

work=$(mktemp -d "${TMPDIR:-/tmp}/emacs-sync-smoke.XXXXXX")
home="$work/home"
mkdir -p "$home"
sock="es-smoke-$$"  # a name, not a path: socket paths are limited to ~104 chars
out="$work/results.txt"
cleanup() {
  HOME="$home" "$emacsclient" -s "$sock" --eval '(kill-emacs)' >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

echo "== install into $home"
EMACS_SYNC_EMACS="$emacs" "$bin" install --home "$home"

echo "== starting daemon ($emacs)"
HOME="$home" "$emacs" --bg-daemon="$sock" >"$work/daemon.log" 2>&1 || {
  cat "$work/daemon.log"; exit 1; }

echo "== running smoke tests"
HOME="$home" EMACS_SYNC_SMOKE_OUT="$out" "$emacsclient" -s "$sock" \
  --eval "(progn (setenv \"EMACS_SYNC_SMOKE_OUT\" \"$out\") (load \"$here/smoke.el\" nil t) t)" >/dev/null

cat "$out"
if grep -q 'FAIL' "$out"; then
  echo "== SMOKE TESTS FAILED"
  exit 1
fi
echo "== all smoke tests passed"
