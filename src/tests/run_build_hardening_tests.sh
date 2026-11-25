#!/usr/bin/env bash
# Build hardening regressions:
#  - the template header generator reproduces every template byte for byte;
#  - the templates' object carries CET (IBT/SHSTK) and non-executable-stack
#    notes, so it cannot be the input that makes the linker drop CET;
#  - the built executable has a non-executable stack, and IBT and SHSTK
#    wherever the toolchain's own start files allow them.
# Runs in the CLI tree (CMake, embed_templates.cmake, jdvrif) and the web
# tree (shell scripts, generate_template_header.sh, jdv_in + jdv_out).
# Usage: run_build_hardening_tests.sh [--bin path/to/executable]...
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
BINS=()
while [[ "${1:-}" == "--bin" ]]; do BINS+=("$2"); shift 2; done
if (( ${#BINS[@]} == 0 )); then
    if [[ -f "$ROOT/embed_templates.cmake" ]]; then BINS=("$ROOT/jdvrif")
    else BINS=("$ROOT/jdv_in" "$ROOT/jdv_out"); fi
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-hardening.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
CXX="${CXX:-g++}"
TEMPLATES="templates/default_icc_template.bin;templates/bluesky_exif_template.bin;templates/photoshop_segment_template.bin;templates/xmp_segment_template.bin"

# 1. Generator round trip: the real templates, plus lengths around the
# 16-byte line width.
mkdir -p "$WORK/gen/templates"
python3 - "$WORK/gen/templates" <<'PY'
import pathlib, random, sys
rng = random.Random(7)
for n in (1, 15, 16, 17, 32, 33):
    (pathlib.Path(sys.argv[1]) / f'len_{n}.bin').write_bytes(rng.randbytes(n))
PY
SYNTHETIC="templates/len_1.bin;templates/len_15.bin;templates/len_16.bin;templates/len_17.bin;templates/len_32.bin;templates/len_33.bin"
# generate OUTPUT SOURCE_DIR "a.bin;b.bin": whichever generator this tree builds with.
generate() {
    if [[ -f "$ROOT/embed_templates.cmake" ]]; then
        cmake -DSOURCE_DIR="$2" -DOUTPUT="$1" "-DTEMPLATES=$3" -P "$ROOT/embed_templates.cmake"
    else
        mkdir -p "$(dirname -- "$1")"
        local list
        IFS=';' read -r -a list <<< "$3"
        bash "$ROOT/generate_template_header.sh" "$1" "$2" "${list[@]}"
    fi
}
generate "$WORK/real/template_assets_data.h" "$ROOT" "$TEMPLATES"
generate "$WORK/gen/synthetic.h" "$WORK/gen" "$SYNTHETIC"
cat > "$WORK/check.cpp" <<'CPP'
#include "template_assets.h"
#include "synthetic.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>
static bool same(std::span<const unsigned char> bytes, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<unsigned char> file{std::istreambuf_iterator<char>(in), {}};
    if (in.bad() || !std::ranges::equal(bytes, file)) {
        std::cerr << "mismatch: " << path << '\n';
        return false;
    }
    return true;
}
int main(int, char** argv) {
    const std::string root = argv[1], gen = argv[2];
    bool ok = same(defaultIccTemplateBytes(), root + "/templates/default_icc_template.bin");
    ok &= same(blueskyExifTemplateBytes(), root + "/templates/bluesky_exif_template.bin");
    ok &= same(photoshopSegmentTemplateBytes(), root + "/templates/photoshop_segment_template.bin");
    ok &= same(xmpSegmentTemplateBytes(), root + "/templates/xmp_segment_template.bin");
    using namespace jdvrif_templates;
    ok &= same(len_1, gen + "/len_1.bin") & same(len_15, gen + "/len_15.bin") &
          same(len_16, gen + "/len_16.bin") & same(len_17, gen + "/len_17.bin") &
          same(len_32, gen + "/len_32.bin") & same(len_33, gen + "/len_33.bin");
    return ok ? 0 : 1;
}
CPP
"$CXX" -std=c++23 -O2 -Wall -Wextra -I"$ROOT" -I"$WORK/real" -I"$WORK/gen" \
    "$WORK/check.cpp" "$ROOT/template_assets.cpp" -lsodium -o "$WORK/check"
"$WORK/check" "$ROOT" "$WORK/gen/templates"
echo "[PASS] generated template arrays match the template files byte for byte"

ARCH="$(uname -m)"
# 2. The templates' object, compiled as the release build compiles it.
if [[ "$ARCH" == "x86_64" ]]; then
    "$CXX" -std=c++23 -O2 -fcf-protection=full -I"$ROOT" -I"$WORK/real" \
        -c "$ROOT/template_assets.cpp" -o "$WORK/template_assets.o"
    readelf -nW "$WORK/template_assets.o" | grep -q 'x86 feature: IBT, SHSTK' || {
        echo "[FAIL] template_assets.o lacks the IBT/SHSTK property" >&2; exit 1; }
    readelf -SW "$WORK/template_assets.o" | grep -q '\.note\.GNU-stack' || {
        echo "[FAIL] template_assets.o lacks .note.GNU-stack" >&2; exit 1; }
    echo "[PASS] template_assets.o carries IBT, SHSTK and .note.GNU-stack"
fi

# 2b. Release compile flags: libstdc++ bounds checks, zero-filled locals,
# and control-flow protection for this architecture only.
if [[ -f "$ROOT/embed_templates.cmake" ]]; then
    COMMANDS="$(ls -1 "$ROOT"/build/*-release-*/compile_commands.json 2>/dev/null | head -n 1 || true)"
    FLAG_SOURCE="$COMMANDS"
else
    FLAG_SOURCE="$ROOT/compile_jdv_in.sh $ROOT/compile_jdv_out.sh"
fi
if [[ -n "${FLAG_SOURCE// /}" ]]; then
    for wanted in _GLIBCXX_ASSERTIONS -ftrivial-auto-var-init=zero; do
        for source in $FLAG_SOURCE; do
            grep -q -- "$wanted" "$source" || { echo "[FAIL] release build lacks $wanted ($source)" >&2; exit 1; }
        done
    done
    if [[ -f "$ROOT/embed_templates.cmake" && "$ARCH" == "x86_64" ]]; then
        grep -q -- "-fcf-protection=full" "$FLAG_SOURCE" ||
            { echo "[FAIL] x86_64 release build lacks -fcf-protection=full" >&2; exit 1; }
    fi
    echo "[PASS] release flags include _GLIBCXX_ASSERTIONS and -ftrivial-auto-var-init=zero"
else
    echo "[SKIP] no release build configuration found"
fi

# 3. The executables.
echo 'int main() { return 0; }' > "$WORK/probe.cpp"
TOOLCHAIN_CET=0
if [[ "$ARCH" == "x86_64" ]] &&
    "$CXX" -fcf-protection=full -Wl,-z,cet-report=error "$WORK/probe.cpp" -o "$WORK/probe" 2>/dev/null; then
    TOOLCHAIN_CET=1
fi
for BIN in "${BINS[@]}"; do
    if [[ ! -x "$BIN" ]]; then
        echo "[SKIP] no executable at $BIN"
        continue
    fi
    name="$(basename -- "$BIN")"
    stack="$(readelf -lW "$BIN" | awk '$1 == "GNU_STACK" {print $7}')"
    [[ "$stack" == "RW" ]] || { echo "[FAIL] $name GNU_STACK flags: '$stack'" >&2; exit 1; }
    echo "[PASS] $name: stack is RW (not executable)"
    if (( TOOLCHAIN_CET )); then
        readelf -nW "$BIN" | grep -q 'x86 feature: IBT, SHSTK' || {
            echo "[FAIL] $name lacks IBT/SHSTK although the toolchain supports them" >&2; exit 1; }
        echo "[PASS] $name: marked IBT and SHSTK"
    elif [[ "$ARCH" == "x86_64" ]]; then
        echo "[SKIP] $name: toolchain start files lack CET marks, so no executable can carry IBT/SHSTK here"
    fi
done
