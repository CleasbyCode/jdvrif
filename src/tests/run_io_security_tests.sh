#!/usr/bin/env bash
# Opened-input bounds and deterministic PIN signal/job-control regressions.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BIN="${JDVRIF_BIN:-$ROOT/jdvrif}"
if [[ ${1:-} == --bin && $# == 2 ]]; then
    BIN="$2"
elif [[ $# != 0 ]]; then
    echo "Usage: tests/run_io_security_tests.sh [--bin <path>]" >&2
    exit 2
fi
if [[ "$BIN" != /* ]]; then BIN="$(pwd -P)/${BIN#./}"; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-io-security.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -I"$ROOT" "$ROOT/tests/test_io_security.cpp" "$ROOT/file_utils.cpp" \
    "$ROOT/pin_input.cpp" "$ROOT/signal_utils.cpp" -lsodium \
    -Wl,--wrap=pselect -o "$WORK/test_io_security"

python3 - "$WORK/test_io_security" "$BIN" "$WORK" <<'PY'
import os
import fcntl
import pathlib
import pty
import resource
import select
import signal
import subprocess
import sys
import termios
import time

helper, binary, work_arg = sys.argv[1:]
work = pathlib.Path(work_arg)


def wait_for(proc, fd, condition, timeout=10):
    output = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition(output):
            return bytes(output)
        readable, _, _ = select.select([fd], [], [], 0.02)
        if readable:
            output.extend(os.read(fd, 4096))
        if proc.poll() is not None:
            break
    raise AssertionError(f"timed out waiting for child: {output!r}")


def finish(proc):
    if proc.poll() is None:
        proc.kill()
    proc.communicate(timeout=5)


small = work / "small.jpg"
small.write_bytes(bytes(range(32)))
result = subprocess.run([helper, "read", str(small), "32"], capture_output=True, check=True)
assert b"read 32 bytes" in result.stdout
for limit in (0, 31):
    result = subprocess.run([helper, "read", str(small), str(limit)], capture_output=True)
    assert result.returncode == 1 and b"permitted size limit" in result.stderr

# This helper is compiled without sanitizers, so an address-space limit is
# meaningful even when the full CLI supplied with --bin uses ASan.
large = work / "large.jpg"
with large.open("wb") as stream:
    stream.truncate(512 * 1024 * 1024)
result = subprocess.run(
    [helper, "read", str(large), str(20 * 1024 * 1024)], capture_output=True,
    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_AS, (128 * 1024 * 1024,) * 2),
)
assert result.returncode == 1 and b"permitted size limit" in result.stderr, result
print("[PASS] opened-input limit rejects before allocation and accepts exact boundary")

for mode, data in (("pending", b""), ("wait-race", b""),
                   ("ready-race", b"1"), ("drained-race", b"1")):
    proc = subprocess.Popen([helper, mode], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        if data:
            proc.stdin.write(data)
            proc.stdin.flush()
        # Keep the pipe writer open: EOF would conceal a blocked-read bug.
        proc.wait(timeout=5)
        stdout, stderr = proc.communicate(timeout=5)
        assert proc.returncode == 0 and b"cancelled SIGINT" in stdout, (stdout, stderr)
    finally:
        finish(proc)
print("[PASS] pending and race-window PIN cancellation restores stdin flags")

for entered, expected in ((b"1234\b5\n", b"1235"),
                          (b"18446744073709551615\n", b"18446744073709551615"),
                          (b"184467440737095516150\b\n", b"18446744073709551615")):
    result = subprocess.run([helper, "pin"], input=entered, capture_output=True, check=True)
    assert b"parsed " + expected in result.stdout
for entered in (b"0\n", b"0123\n", b"18446744073709551616\n"):
    result = subprocess.run([helper, "pin"], input=entered, capture_output=True)
    assert result.returncode == 1 and b"well-formed recovery PIN" in result.stderr
print("[PASS] piped PIN parsing and correction retain existing behavior")

for action in ("interrupt", "suspend"):
    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    before_flags = fcntl.fcntl(slave, fcntl.F_GETFL)
    # A separate process group in our session is not orphaned, so SIGTSTP
    # retains its real job-control behavior without requiring an outer shell.
    proc = subprocess.Popen([helper, "pin"], stdin=slave, stdout=slave, stderr=slave,
                            process_group=0, close_fds=True)
    try:
        wait_for(proc, master, lambda data: b"PIN: " in data and
                 not (termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)))
        if action == "interrupt":
            os.kill(proc.pid, signal.SIGINT)
        else:
            os.kill(proc.pid, signal.SIGTSTP)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                pid, status = os.waitpid(proc.pid, os.WUNTRACED | os.WNOHANG)
                if pid:
                    assert os.WIFSTOPPED(status), status
                    break
                time.sleep(0.01)
            else:
                raise AssertionError("PIN reader did not suspend")
            assert fcntl.fcntl(slave, fcntl.F_GETFL) == before_flags
            # Emulate the shell restoring cooked mode while its job is stopped.
            termios.tcsetattr(slave, termios.TCSANOW, before)
            os.kill(proc.pid, signal.SIGCONT)
            wait_for(proc, master, lambda _: not
                     (termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)))
            os.write(master, b"123\n")
        proc.wait(timeout=5)
        assert proc.returncode == 0, proc.returncode
        assert termios.tcgetattr(slave) == before
    finally:
        if proc.poll() is None:
            os.kill(proc.pid, signal.SIGCONT)
            proc.kill()
            proc.wait(timeout=5)
        os.close(master)
        os.close(slave)
print("[PASS] PIN interrupt and suspend/resume restore terminal state")

# The carrier path validates the name before prompting, then opens it again
# after the PIN. Exercise both in-place growth and replacement during that gap.
for replacement in (False, True):
    case = work / ("replace" if replacement else "grow")
    case.mkdir()
    image = case / "input.jpg"
    image.write_bytes(b"ordinary image bytes" * 100)
    proc = subprocess.Popen([binary, "recover", str(image)], cwd=case,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        prefix = wait_for(proc, proc.stdout.fileno(), lambda data: b"PIN: " in data)
        target = case / "replacement.jpg" if replacement else image
        with target.open("wb") as stream:
            stream.truncate(64 * 1024 * 1024)
        if replacement:
            target.replace(image)
        stdout, _ = proc.communicate(input=b"1\n", timeout=10)
        output = prefix + stdout
        assert proc.returncode != 0 and b"permitted size limit" in output, output
    finally:
        finish(proc)
print("[PASS] prompt-time carrier growth and replacement preserve the 20 MiB limit")
PY
