#!/usr/bin/env bash
# Compiles and runs the host test suites. No ESP-IDF, no board, no network.
#
# Uses whichever C compiler is present. On Windows that is usually MSVC from a
# Visual Studio install, which needs its environment loaded first, so the build
# is handed to msvc.bat where cmd owns the quoting rules.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${root}/test/host/build"
mkdir -p "$out"

# name | include dirs, comma separated | sources, comma separated
suites=(
  "fcp|${root}/components/fcp/include|${root}/components/fcp/fcp.c,${root}/components/fcp/test/test_fcp.c"
  "fmesh|${root}/components/fcp/include,${root}/components/fmesh/include|${root}/components/fcp/fcp.c,${root}/components/fmesh/fmesh.c,${root}/components/fmesh/test/test_fmesh.c"
)

run_suite() {
  local name="$1" incs="$2" srcs="$3"

  local -a inc_args=() src_list=()
  local IFS=,
  read -ra inc_list <<< "$incs"
  read -ra src_list <<< "$srcs"
  unset IFS

  local posix_cc
  for posix_cc in gcc clang; do
    if command -v "$posix_cc" >/dev/null 2>&1; then
      inc_args=()
      local d
      for d in "${inc_list[@]}"; do inc_args+=("-I$d"); done
      "$posix_cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
        -fsanitize=address,undefined \
        "${inc_args[@]}" -o "$out/$name" "${src_list[@]}"
      "$out/$name"
      return
    fi
  done

  if [ -x "/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" ]; then
    # Semicolon separated include list, which msvc.bat prepends to INCLUDE.
    local win_incs="" d
    for d in "${inc_list[@]}"; do
      win_incs+="$(cygpath -w "$d");"
    done

    local -a win_srcs=() s
    for s in "${src_list[@]}"; do win_srcs+=("$(cygpath -w "$s")"); done

    # MSYS_NO_PATHCONV stops the shell rewriting Windows paths, and because it
    # also stops it rewriting //c into /c, the switch is written plainly.
    MSYS_NO_PATHCONV=1 FCP_INC="$win_incs" \
      cmd /c "$(cygpath -w "${root}/test/host/msvc.bat")" \
      "$(cygpath -w "$out")" "$name" "${win_srcs[@]}"
    return
  fi

  echo "No C compiler found. Install gcc, clang, or Visual Studio build tools." >&2
  exit 1
}

status=0
for suite in "${suites[@]}"; do
  IFS='|' read -r name incs srcs <<< "$suite"
  echo "=== $name ==="
  if ! run_suite "$name" "$incs" "$srcs"; then
    status=1
  fi
  echo
done

exit "$status"
