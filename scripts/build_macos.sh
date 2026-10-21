#!/usr/bin/env bash
# Builds FLIRT natively on macOS (Apple silicon) into build/macos:
#   scripts/build_macos.sh [extra cmake arguments]
# The programs are then build/macos/prefix/bin/flirt-convert, flirt-info and
# flirt-tui; then tools/compare into build/macos-<tool>.
# Needs Xcode's command line tools, Homebrew's cmake, ninja, qt@5 and boost,
# and ROOT 6 (root-config on PATH, or ROOTSYS set). Installs nothing itself.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fail() { echo "error: $*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || fail "this script is for macOS; on Linux see README (Building)"

# ROOT: root-config on the caller's PATH, else $ROOTSYS/bin.
rootconfig=$(command -v root-config || true)
if [ -z "$rootconfig" ] && [ -n "${ROOTSYS:-}" ] && [ -x "$ROOTSYS/bin/root-config" ]; then
    rootconfig=$ROOTSYS/bin/root-config
fi
[ -n "$rootconfig" ] || fail "ROOT not found: put root-config on PATH (source <root>/bin/thisroot.sh) or set ROOTSYS.
ROOT 6 for macOS: https://root.cern/install/ (binary release, or build from source)"
rootbin=$(dirname "$rootconfig")

brew=$(command -v brew || true)
for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -z "$brew" ] && [ -x "$b" ] && brew=$b; done
[ -n "$brew" ] || fail "Homebrew not found (https://brew.sh); then: brew install qt@5 boost cmake ninja"
brewbin=$("$brew" --prefix)/bin
qt=$("$brew" --prefix qt@5 2> /dev/null || true)
boost=$("$brew" --prefix boost 2> /dev/null || true)
[ -n "$qt" ] && [ -f "$qt/lib/cmake/Qt5Core/Qt5CoreConfig.cmake" ] || fail "Qt 5 not found: brew install qt@5"
[ -n "$boost" ] && [ -d "$boost/include/boost" ] || fail "Boost not found: brew install boost"
for tool in cmake ninja; do
    [ -x "$brewbin/$tool" ] || fail "$tool not found in $brewbin: brew install $tool"
done
[ -x /usr/bin/c++ ] && xcrun --show-sdk-path > /dev/null 2>&1 || fail "no C++ compiler: xcode-select --install"
if [ ! -f "$repo/external/FTXUI/CMakeLists.txt" ]; then
    fail "external/FTXUI is missing: git submodule update --init external/FTXUI
(or build without flirt-tui: scripts/build_macos.sh -DFLIRT_BUILD_TUI=OFF)"
fi

# A clean PATH: other toolchains on the caller's PATH (e.g. anaconda's Boost,
# ninja and install_name_tool) must not end up in the build.
export PATH=$rootbin:$brewbin:/usr/bin:/bin:/usr/sbin:/sbin
unset CPATH LIBRARY_PATH CMAKE_PREFIX_PATH

cmake -S "$repo" -B "$repo/build/macos" -G Ninja -Wno-dev \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_PREFIX_PATH="$qt;$boost" "$@"
ninja -C "$repo/build/macos"
echo "built: $repo/build/macos/prefix/bin/flirt-convert, flirt-info"

# The checking tools (ROOT only, no FLIRT code), each in build/macos-<tool>.
for tool in compare; do
    cmake -S "$repo/tools/$tool" -B "$repo/build/macos-$tool" -G Ninja -Wno-dev
    ninja -C "$repo/build/macos-$tool"
    echo "built: $repo/build/macos-$tool/$tool"
done
echo "to run them by name in this shell: source '$repo/build/macos/env.sh' (tcsh: env.csh)"
