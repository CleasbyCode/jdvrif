#!/usr/bin/env bash
# Interrupted fsync(2) and stdout writes around conceal's output commit and
# KEY delivery, driven by an LD_PRELOAD fault shim (output_fault_shim.c):
#   - fsync EINTR with no signal is retried; with a cancelling signal pending
#     it ends the run as that signal (no "Write Error"), leaving no image;
#   - a signal interrupting the KEY-prefix flush cancels the run, not
#     "Failed to deliver recovery KEY";
#   - a stdout failure after the whole KEY was written keeps the image and
#     reports the report failure, not a KEY-delivery failure.
# Works for any jdvrif-compatible binary (C++ or Rust): pass --bin. For the
# web variant's jdv_in, which takes no "conceal" subcommand, also set
# JDVRIF_CONCEAL_SUBCOMMAND="".
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$TESTS/.." && pwd -P)"
[[ -f "$ROOT/compile_jdvrif.sh" ]] || ROOT="$ROOT/src"
BIN="${JDVRIF_BIN:-$ROOT/jdvrif}"
if [[ ${1:-} == --bin && $# == 2 ]]; then
    BIN="$2"
elif [[ $# != 0 ]]; then
    echo "Usage: tests/run_output_interrupt_tests.sh [--bin <path>]" >&2
    exit 2
fi
if [[ "$BIN" != /* ]]; then BIN="$(pwd -P)/${BIN#./}"; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-output-interrupt.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

"${CC:-gcc}" -O2 -Wall -Wextra -shared -fPIC "$TESTS/output_fault_shim.c" \
    -o "$WORK/shim.so" -ldl -lpthread

python3 - "$BIN" "$WORK" "$TESTS/testdata/covers/cover_default.jpg" <<'PY'
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import time

binary, work = sys.argv[1], pathlib.Path(sys.argv[2])

cover = pathlib.Path(sys.argv[3])
(work / "secret.txt").write_text("interrupted output test\n")

KEY = re.compile(rb"Recovery KEY: \[\*\*\*[1-9A-HJ-NP-Za-km-z]{4}(-[1-9A-HJ-NP-Za-km-z]{4}){5}")
EXPECT = {
    # fault: (exit status, images kept, KEY shown, stderr must contain / must not contain)
    "none": (0, 1, True, None),
    "fsync-eintr": (0, 1, True, None),
    "fsync-sigint": (-signal.SIGINT, 0, False, None),
    "prefix-sigint": (-signal.SIGINT, 0, False, None),
    "after-key-full": (1, 1, True, "Failed to report committed output image"),
}
for fault, (status, images, key_shown, message) in EXPECT.items():
    run = work / fault
    run.mkdir()
    shutil.copy(cover, run / "cover.jpg")
    shutil.copy(work / "secret.txt", run)
    env = dict(os.environ, JDVRIF_FAULT=fault, LD_PRELOAD=str(work / "shim.so"))
    subcommand = os.environ.get("JDVRIF_CONCEAL_SUBCOMMAND", "conceal")
    command = [binary] + ([subcommand] if subcommand else []) + ["-r", "cover.jpg", "secret.txt"]
    proc = subprocess.Popen(command, cwd=run,
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if fault == "prefix-sigint":
        time.sleep(3)  # leave the stdout pipe full until the shim's SIGINT lands
    out, err = proc.communicate(timeout=120)
    got = (proc.returncode, len(list(run.glob("jrif_*"))), bool(KEY.search(out)))
    assert got == (status, images, key_shown), (fault, got, err.decode(errors="replace"))
    assert b"deliver recovery KEY" not in err and b"Write Error" not in err, (fault, err)
    if message:
        assert message.encode() in err, (fault, err)
    print(f"[PASS] {fault}: exit {proc.returncode}, {got[1]} image(s), KEY shown: {key_shown}")
PY
