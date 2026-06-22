#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: gen-user-gdb.sh <user-program>" >&2
  exit 2
fi

prog="$1"
root="${XV6_REPO_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
out="${XV6_USER_GDB_SCRIPT:-$root/.direnv/xv6-user-gdb}"
target="$root/user/_$prog"

mkdir -p "$(dirname "$out")"

if [ ! -e "$target" ]; then
  echo "warning: $target does not exist yet; build xv6 before debugging it" >&2
fi

cat >"$out" << EOF
# Load user program symbols. xv6 user binaries are linked at address 0.
add-symbol-file "$target" 0
EOF

echo "Generated GDB script: $out"
