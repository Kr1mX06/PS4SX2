#!/usr/bin/env bash
# ThinLTO objects need a current lld, but the console refuses images laid out by
# it (CE-41839-5). Fold them into one native object, then link with OpenOrbis lld.
set -euo pipefail
real="$(dirname "$0")/real/ld.lld"
args=()
objs=()
slot=-1
for a in "$@"; do
  if [[ "$a" == CMakeFiles/vm_boot.dir/*.obj ]]; then
    objs+=("$a")
    if ((slot < 0)); then
      slot=${#args[@]}
      args+=(vm_boot_lto.o)
    fi
  else
    args+=("$a")
  fi
done
if ((${#objs[@]})); then
  ld.lld-21 -r -m elf_x86_64 --lto-O3 --thinlto-jobs=4 -o vm_boot_lto.o "${objs[@]}"
fi
exec "$real" "${args[@]}"
