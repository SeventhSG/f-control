#!/usr/bin/env bash
# Compiles and runs the host test suites. No ESP-IDF, no board, no network.
#
# Uses whichever C compiler is present. On Windows that is usually MSVC from a
# Visual Studio install, which needs its environment loaded first, so we find
# it with vswhere rather than assuming a path.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${root}/test/host/build"
mkdir -p "$out"

suites=(
  "fcp:${root}/components/fcp/fcp.c:${root}/components/fcp/test/test_fcp.c:${root}/components/fcp/include"
)

run_suite() {
  local name="$1" src="$2" test_src="$3" inc="$4"

  if command -v gcc >/dev/null 2>&1; then
    gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$inc" -o "$out/$name" "$src" "$test_src"
    "$out/$name"
    return
  fi

  if command -v clang >/dev/null 2>&1; then
    clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$inc" -o "$out/$name" "$src" "$test_src"
    "$out/$name"
    return
  fi

  if [ -x "/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" ]; then
    # MSYS_NO_PATHCONV stops the shell rewriting Windows paths, and because it
    # also stops it rewriting //c into /c, the switch is written plainly.
    MSYS_NO_PATHCONV=1 cmd /c "$(cygpath -w "${root}/test/host/msvc.bat")" \
      "$(cygpath -w "$out")" "$name" "$(cygpath -w "$inc")" \
      "$(cygpath -w "$src")" "$(cygpath -w "$test_src")"
    return
  fi

  echo "No C compiler found. Install gcc, clang, or Visual Studio build tools." >&2
  exit 1
}

status=0
for suite in "${suites[@]}"; do
  IFS=: read -r name src test_src inc <<< "$suite"
  echo "=== $name ==="
  if ! run_suite "$name" "$src" "$test_src" "$inc"; then
    status=1
  fi
  echo
done

exit "$status"
