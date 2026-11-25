#!/usr/bin/env bash
# Opened-input bounds and deterministic KEY signal/job-control regressions.
set -euo pipefail

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# ROOT is the C++ source tree. This directory may sit inside it (src/tests)
# or beside it at the repository root (tests/).
ROOT="$(cd "$TESTS/.." && pwd -P)"
[[ -f "$ROOT/compile_jdvrif.sh" ]] || ROOT="$ROOT/src"
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
    -I"$ROOT" "$TESTS/test_io_security.cpp" "$ROOT/file_utils.cpp" \
    "$ROOT/key_input.cpp" "$ROOT/key_codec.cpp" "$ROOT/signal_utils.cpp" -lsodium \
    -Wl,--wrap=pselect -Wl,--wrap=tcsetattr -Wl,--wrap=tcgetattr -Wl,--wrap=read -o "$WORK/test_io_security"

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
    # The child may exit right after its last write: drain what it left, then
    # check once more before calling it a timeout.
    while select.select([fd], [], [], 0)[0]:
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
    if condition(output):
        return bytes(output)
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

# A FIFO standing in for a validated input path must be refused without
# blocking in open(2) (no writer ever appears here), while a regular file opens.
fifo = work / "swapped.jpg"
os.mkfifo(fifo)
result = subprocess.run([helper, "open-input", str(fifo)], capture_output=True, timeout=10)
assert result.returncode == 1 and b"refused non-regular input" in result.stderr, result
result = subprocess.run([helper, "open-input", str(small)], capture_output=True, timeout=10)
assert result.returncode == 0 and b"opened" in result.stdout, result
print("[PASS] input opens refuse FIFOs without blocking")

# A terminal swapped in for an input path is refused after the open; O_NOCTTY
# keeps that open from making it a session leader's controlling terminal.
master, slave = pty.openpty()
try:
    result = subprocess.run([helper, "noctty", os.ttyname(slave)], capture_output=True, timeout=10)
    assert result.returncode == 0 and b"no controlling terminal acquired" in result.stdout, result
finally:
    os.close(master)
    os.close(slave)
print("[PASS] input opens never acquire a controlling terminal")

result = subprocess.run([helper, "reraise-blocked"], capture_output=True, timeout=10)
assert result.returncode == -signal.SIGTERM, result.returncode
print("[PASS] a cancellation signal is re-raised even while it is blocked")

# Embedded filenames: ordinary UTF-8 is fine; terminal-rewriting code points
# (C1 controls, bidi formatting) and malformed UTF-8 are not.
safe_names = ["a.txt", "caf\u00e9.txt", "\u65e5\u672c.txt", "emoji\U0001F600.txt"]
unsafe_names = [
    "photo\u202egpj.exe".encode(), "x\u009b.txt".encode(), "x\u200e.txt".encode(),
    "x\u2066.txt".encode(), "x\u061c.txt".encode(),
    b"x\xff.txt", b"\xc0\xae.txt", b"\xed\xa0\x80.txt", b"x\xe2\x80.txt",
]
for name in safe_names:
    out = subprocess.run([helper, "filename", name], capture_output=True, check=True).stdout
    assert out.strip() == b"safe", (name, out)
for name in unsafe_names:
    out = subprocess.run([helper.encode(), b"filename", name], capture_output=True, check=True).stdout
    assert out.strip() == b"unsafe", (name, out)
print("[PASS] embedded filenames reject terminal-rewriting and malformed UTF-8")

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
print("[PASS] pending and race-window KEY cancellation restores stdin flags")

for entered, expected in ((b"112d-rXXU-ifSr-RnXL-GbXg-8Egx\bs\n", b"000102030405060708090a0b0c0d0e0f"),
                          (b"112drXXUifSrRnXLGbXg8Egs\n", b"000102030405060708090a0b0c0d0e0f"),
                          (b"112d-rXXU-ifSr-RnXL-GbXg-8EgsXXXXX\b\b\b\b\b\n", b"000102030405060708090a0b0c0d0e0f")):
    result = subprocess.run([helper, "key"], input=entered, capture_output=True, check=True)
    assert b"parsed " + expected in result.stdout
for entered in (b"0\n", b"15753985819510608748\n", b"112d-rXXU-ifSr-RnXL-GbXg-8Eg\n"):
    result = subprocess.run([helper, "key"], input=entered, capture_output=True)
    assert result.returncode == 1 and b"well-formed recovery KEY." in result.stderr
print("[PASS] piped KEY parsing and correction retain existing behavior")

for action in ("interrupt", "suspend"):
    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    before_flags = fcntl.fcntl(slave, fcntl.F_GETFL)
    # A separate process group in our session is not orphaned, so SIGTSTP
    # retains its real job-control behavior without requiring an outer shell.
    proc = subprocess.Popen([helper, "key"], stdin=slave, stdout=slave, stderr=slave,
                            process_group=0, close_fds=True)
    try:
        wait_for(proc, master, lambda data: (b"KEY: " in data or b"KEY: " in data) and
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
                raise AssertionError("KEY reader did not suspend")
            assert fcntl.fcntl(slave, fcntl.F_GETFL) == before_flags
            # Emulate the shell restoring cooked mode while its job is stopped.
            termios.tcsetattr(slave, termios.TCSANOW, before)
            os.kill(proc.pid, signal.SIGCONT)
            wait_for(proc, master, lambda _: not
                     (termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)))
            os.write(master, b"112d-rXXU-ifSr-RnXL-GbXg-8Egs\n")
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
print("[PASS] KEY interrupt and suspend/resume restore terminal state")

# Canonical mode ignores VMIN/VTIME, so values left by an earlier program
# survive until KEY entry clears ICANON. VMIN > 1 would withhold readiness for
# the last keystrokes; VMIN = VTIME = 0 lets a stale read report EOF.
for vmin in (4, 0):
    master, slave = pty.openpty()
    stale = termios.tcgetattr(slave)
    stale[6][termios.VMIN] = vmin
    stale[6][termios.VTIME] = 0
    termios.tcsetattr(slave, termios.TCSANOW, stale)
    before = termios.tcgetattr(slave)
    proc = subprocess.Popen([helper, "key"], stdin=slave, stdout=slave, stderr=slave,
                            process_group=0, close_fds=True)
    try:
        wait_for(proc, master, lambda data: b"KEY: " in data and
                 not (termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)))
        os.write(master, b"112d-rXXU-ifSr-RnXL-GbXg-8Egs\n")
        output = wait_for(proc, master, lambda data: b"parsed 0001" in data and
                          data.rstrip().endswith(b"0e0f"))
        assert b"parsed 000102030405060708090a0b0c0d0e0f" in output, output
        proc.wait(timeout=5)
        assert proc.returncode == 0, proc.returncode
        assert termios.tcgetattr(slave) == before
    finally:
        finish(proc)
        os.close(master)
        os.close(slave)
print("[PASS] KEY entry ignores stale VMIN/VTIME left by earlier programs")
# tcsetattr interrupted by a signal (jdvrif's handlers do not set SA_RESTART):
# entering raw mode retries instead of refusing the terminal. The restore
# never waits for output to drain, so output held back by XOFF cannot hang it.
for mode in ("eintr-raw", "xoff-restore"):
    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    proc = subprocess.Popen([helper, mode], stdin=slave, stdout=slave, stderr=slave,
                            process_group=0, close_fds=True)
    try:
        wait_for(proc, master, lambda data: b"KEY: " in data and
                 not (termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)))
        os.write(master, b"112d-rXXU-ifSr-RnXL-GbXg-8Egs\n")
        output = wait_for(proc, master, lambda data: b"parsed" in data or b"Error" in data)
        proc.wait(timeout=5)
        assert proc.returncode == 0, (mode, proc.returncode, output)
        assert termios.tcgetattr(slave) == before, mode
    finally:
        finish(proc)
        os.close(master)
        os.close(slave)
print("[PASS] interrupted terminal-mode changes are retried, and echo is always restored")

# Job control at the KEY prompt, from a real controlling terminal: a forked
# session leader plays the shell.
#   ctrl-z-kill: Ctrl+Z, then `kill %1`. The shell takes the terminal back and
#     sends SIGTERM and SIGCONT. The job must end, not stop again on SIGTTOU by
#     restoring terminal settings from the background.
#   bg-kill: Ctrl+Z, `bg`, then `kill %1`. Resumed in the background, the job
#     must not try to re-enter raw mode (SIGTTOU would stop it with SIGTERM
#     blocked), so one SIGTERM ends it.
#   bg-fg: Ctrl+Z, `bg`, then `fg`. It waits in the background without
#     stopping, and on `fg` re-enters raw mode and reads the KEY unechoed.
#   start-bg-kill: started in the background, the job stops on SIGTTOU
#     before touching the terminal; one `kill %1` must end it.
#   start-bg-tostop-kill: started in the background with `stty tostop`, the
#     job stops on SIGTTOU writing the prompt; one `kill %1` must end it, not
#     leave it stopped again by the wait for the foreground.
#   start-bg-tostop-cli: the same with jdvrif itself, which stops first on
#     its "Image file" line: the interrupted print fails, and the job must end
#     by the signal rather than stop again writing the error.
#   start-bg-fg: started in the background while the shell's line editor
#     has the terminal raw; after `fg` (the shell restores cooked mode) the
#     job reads the KEY unechoed and restores cooked mode, not the editor's.
#   lost-fg-kill / lost-fg-fg: while the job reads, the shell takes the
#     terminal back without stopping it (tcsetpgrp) and the user types into
#     the shell. The job must wait instead of reading -- the read would stop
#     it on SIGTTIN with O_NONBLOCK left on the shared terminal and SIGTERM
#     masked -- then end on one kill, or re-enter raw mode on fg.
#   read-lost-fg-fg: the foreground is lost between readiness and the read
#     itself: the read must not stop on SIGTTIN either.
#   blip-fg / blip-cont-fg: the foreground is lost for that read only (EIO)
#     and is back -- with the pending SIGCONT of `fg`, or without one, as
#     after another process's tcsetpgrp -- before the read's error is looked
#     at. That is a resume, not a read error: the KEY is still read.
# None may leave the shell's terminal settings changed.
def job_control(scenario):
    status_read, status_write = os.pipe()
    shell = os.fork()
    if shell == 0:
        os.close(status_read)
        try:
            os.setsid()
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            signal.signal(signal.SIGTTOU, signal.SIG_IGN)
            if scenario in ("start-bg-tostop-kill", "start-bg-tostop-cli"):
                modes = termios.tcgetattr(slave)
                modes[3] |= termios.TOSTOP
                termios.tcsetattr(slave, termios.TCSANOW, modes)
            shell_modes = termios.tcgetattr(slave)
            if scenario == "start-bg-fg":
                # The line editor's modes, as readline sets them at a prompt.
                editor_modes = termios.tcgetattr(slave)
                editor_modes[3] &= ~(termios.ECHO | termios.ICANON)
                termios.tcsetattr(slave, termios.TCSANOW, editor_modes)
            job = os.fork()
            if job == 0:
                os.setpgid(0, 0)
                for fd in (0, 1, 2):
                    os.dup2(slave, fd)
                signal.signal(signal.SIGTTOU, signal.SIG_DFL)
                if scenario == "start-bg-tostop-cli":
                    os.execv(binary, [binary, "recover", str(cli_image)])
                os.execv(helper, [helper, {"read-lost-fg-fg": "key-lose-fg", "blip-fg": "key-blip-fg",
                                           "blip-cont-fg": "key-blip-cont"}.get(scenario, "key")])
            os.setpgid(job, job)

            def wait_stopped():
                _, status = os.waitpid(job, os.WUNTRACED)
                assert os.WIFSTOPPED(status), status
                return os.WSTOPSIG(status)

            def raw_mode():
                return not termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)

            def wait_raw():
                deadline = time.monotonic() + 10
                while not raw_mode():
                    if time.monotonic() > deadline:
                        raise AssertionError("KEY reader never entered raw mode")
                    time.sleep(0.01)

            def take_terminal_back():
                os.tcsetpgrp(slave, os.getpgrp())
                modes = termios.tcgetattr(slave)
                modes[3] |= termios.ECHO | termios.ICANON
                termios.tcsetattr(slave, termios.TCSANOW, modes)
                # Read back: tcgetattr reports VMIN/VTIME as ints or bytes
                # depending on ICANON, so compare like with like.
                return termios.tcgetattr(slave)

            if scenario.startswith("start-bg"):
                assert wait_stopped() == signal.SIGTTOU, "expected a SIGTTOU stop"
                if scenario == "start-bg-fg":
                    shell_modes = take_terminal_back()
                else:
                    assert termios.tcgetattr(slave) == shell_modes, "raw mode set from the background"
            elif scenario.startswith("blip"):
                os.tcsetpgrp(slave, job)
                wait_raw()
                os.write(master, b"x")  # the read it wakes fails with EIO
                time.sleep(0.5)
                pid, status = os.waitpid(job, os.WUNTRACED | os.WNOHANG)
                if pid:
                    raise AssertionError(
                        f"after the foreground blip: stopped by signal {os.WSTOPSIG(status)}"
                        if os.WIFSTOPPED(status) else f"after the foreground blip: ended {status}")
                shell_modes = take_terminal_back()
            elif "lost-fg" in scenario:
                os.tcsetpgrp(slave, job)
                wait_raw()
                if scenario.startswith("lost-fg"):
                    shell_modes = take_terminal_back()
                    os.write(master, b"ls\n")  # typed at the shell's prompt
                else:
                    os.write(master, b"x")  # the job gives the terminal away on seeing it
                    deadline = time.monotonic() + 5
                    while os.tcgetpgrp(slave) != os.getpgrp():
                        if time.monotonic() > deadline:
                            raise AssertionError("the job never lost the foreground")
                        time.sleep(0.01)
                    shell_modes = take_terminal_back()
                time.sleep(0.5)
                pid, status = os.waitpid(job, os.WUNTRACED | os.WNOHANG)
                if pid:
                    raise AssertionError(
                        f"after losing the foreground: stopped by signal {os.WSTOPSIG(status)}"
                        if os.WIFSTOPPED(status) else f"after losing the foreground: ended {status}")
                assert not fcntl.fcntl(slave, fcntl.F_GETFL) & os.O_NONBLOCK, \
                    "O_NONBLOCK left on the shell's terminal"
                assert termios.tcgetattr(slave) == shell_modes, "changed the terminal in the background"
            else:
                os.tcsetpgrp(slave, job)
                wait_raw()
                os.kill(job, signal.SIGTSTP)
                wait_stopped()
                shell_modes = take_terminal_back()
                if scenario != "ctrl-z-kill":
                    os.kill(job, signal.SIGCONT)  # bg
                    time.sleep(0.5)
                    pid, status = os.waitpid(job, os.WUNTRACED | os.WNOHANG)
                    if pid:
                        raise AssertionError(
                            f"after bg: stopped by signal {os.WSTOPSIG(status)}"
                            if os.WIFSTOPPED(status) else f"after bg: ended {status}")
                    assert termios.tcgetattr(slave) == shell_modes, "bg changed the terminal"

            if scenario.endswith("-fg"):
                os.tcsetpgrp(slave, job)
                os.kill(job, signal.SIGCONT)  # fg
                wait_raw()
                os.write(master, b"112d-rXXU-ifSr-RnXL-GbXg-8Egs\n")
                _, status = os.waitpid(job, 0)
                os.tcsetpgrp(slave, os.getpgrp())
                output = b""
                while select.select([master], [], [], 0.1)[0]:
                    try:
                        chunk = os.read(master, 4096)
                    except OSError:
                        break
                    if not chunk:
                        break
                    output += chunk
                if not (os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0):
                    outcome = f"KEY read after fg failed: {status} {output!r}"
                elif b"parsed 000102" not in output or b"112d-rXXU" in output:
                    outcome = f"KEY after fg not read unechoed: {output!r}"
                elif termios.tcgetattr(slave) != shell_modes:
                    outcome = "changed the shell's terminal settings"
                else:
                    outcome = "ok"
            else:
                os.kill(job, signal.SIGTERM)
                os.kill(job, signal.SIGCONT)
                outcome = "did not finish"
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    pid, status = os.waitpid(job, os.WUNTRACED | os.WNOHANG)
                    if pid:
                        if os.WIFSTOPPED(status):
                            outcome = f"stopped again by signal {os.WSTOPSIG(status)}"
                        elif termios.tcgetattr(slave) != shell_modes:
                            outcome = "changed the shell's terminal settings"
                        else:
                            outcome = "ok"
                        break
                    time.sleep(0.01)
            os.write(status_write, outcome.encode())
        except BaseException as error:
            os.write(status_write, f"harness error: {error!r}".encode())
        try:
            os.kill(job, signal.SIGKILL)
            os.waitpid(job, 0)
        except Exception:
            pass
        os._exit(0)
    os.close(status_write)
    os.waitpid(shell, 0)
    outcome = os.read(status_read, 4096).decode()
    assert outcome == "ok", f"KEY reader, {scenario}: {outcome}"


cli_image = work / "job-control.jpg"
cli_image.write_bytes(b"ordinary image bytes" * 100)
for scenario in ("ctrl-z-kill", "bg-kill", "bg-fg", "start-bg-kill", "start-bg-tostop-kill",
                 "start-bg-tostop-cli", "start-bg-fg", "lost-fg-kill", "lost-fg-fg",
                 "read-lost-fg-fg", "blip-fg", "blip-cont-fg"):
    job_control(scenario)
print("[PASS] a KEY reader suspended, backgrounded or started in the background "
      "ends on one kill %1, waits in the background, and resumes on fg")

# A raw-mode set interrupted by a cancellation signal is not retried: one
# SIGINT/SIGTERM must end a set stopped on SIGTTOU or held by XOFF. That holds
# for the re-apply after a resume too, which runs inside the read loop where
# cancellation is otherwise masked outside pselect.
# One landing just before raw mode is entered cancels without entering it.
# The restore while unwinding must not wait on that held output again.
for mode, sets in (("eintr-cancel", 1), ("reapply-cancel", 2), ("snapshot-cancel", 0),
                   ("xoff-cancel", 1)):
    master, slave = pty.openpty()
    proc = subprocess.Popen([helper, mode], stdin=slave, stdout=slave, stderr=slave,
                            process_group=0, close_fds=True)
    try:
        output = wait_for(proc, master, lambda data: b"cancelled" in data or b"Error" in data)
        proc.wait(timeout=5)
        assert proc.returncode == 0, (mode, proc.returncode, output)
        assert f"after {sets} raw-mode sets".encode() in output, (mode, output)
    finally:
        finish(proc)
        os.close(master)
        os.close(slave)
print("[PASS] a cancellation interrupting a raw-mode set, re-applied ones included, cancels without retrying")

# KEY characters typed but not yet read when entry is cancelled are discarded,
# not left for the shell to read (and echo) as a command.
master, slave = pty.openpty()
before = termios.tcgetattr(slave)
proc = subprocess.Popen([helper, "typeahead-cancel"], stdin=slave, stdout=slave, stderr=slave,
                        process_group=0, close_fds=True)
try:
    wait_for(proc, master, lambda data: b"KEY: " in data and
             not (termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)))
    os.write(master, b"112d-rXXU\n")
    output = wait_for(proc, master, lambda data: b"cancelled" in data or b"Error" in data)
    proc.wait(timeout=5)
    assert proc.returncode == 0, (proc.returncode, output)
    assert termios.tcgetattr(slave) == before
    os.set_blocking(slave, False)
    try:
        left = os.read(slave, 4096)
    except BlockingIOError:
        left = b""
    assert left == b"", f"typed-ahead KEY left for the shell: {left!r}"
finally:
    finish(proc)
    os.close(master)
    os.close(slave)
print("[PASS] a cancelled KEY entry discards typed-ahead KEY characters")

# A terminal that fails every read (hung up) is a read error, not a resume to
# retry for ever.
master, slave = pty.openpty()
proc = subprocess.Popen([helper, "key-eio"], stdin=slave, stdout=slave, stderr=slave,
                        process_group=0, close_fds=True)
try:
    wait_for(proc, master, lambda data: b"KEY: " in data and
             not (termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)))
    os.write(master, b"1")  # one EIO is retried as a resume, which flushes it
    time.sleep(0.5)
    os.write(master, b"2")
    output = wait_for(proc, master, lambda data: b"Read Error" in data)
    proc.wait(timeout=5)
    assert proc.returncode != 0, proc.returncode
finally:
    finish(proc)
    os.close(master)
    os.close(slave)
print("[PASS] a terminal failing every read ends KEY entry with a read error")

# Echo output held back by XOFF (Ctrl+S): a stop and resume while the echo
# waits interrupts its write (SIGCONT is hooked without SA_RESTART). Entry must
# carry on and read the KEY once output resumes (Ctrl+Q), and one SIGTERM must
# end it while output is still held.
def held_echo(cancel):
    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    proc = subprocess.Popen([helper, "key-unbuffered"], stdin=slave, stdout=slave, stderr=slave,
                            process_group=0, close_fds=True)
    try:
        wait_for(proc, master, lambda data: b"KEY: " in data and
                 not (termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)))
        os.write(master, b"112d-rXXU-ifSr-RnXL-GbXg-8Egs")
        wait_for(proc, master, lambda data: b"****-****-****-****-****-****" in data)
        os.write(master, b"\x13")  # Ctrl+S
        time.sleep(0.2)
        os.write(master, b"\n")
        time.sleep(0.5)  # the final newline's echo is now waiting
        if cancel:
            os.kill(proc.pid, signal.SIGTERM)
            proc.wait(timeout=5)
            assert proc.returncode == -signal.SIGTERM, proc.returncode
        else:
            os.kill(proc.pid, signal.SIGSTOP)
            _, status = os.waitpid(proc.pid, os.WUNTRACED)
            assert os.WIFSTOPPED(status), status
            os.kill(proc.pid, signal.SIGCONT)
            time.sleep(0.2)
            os.write(master, b"\x11")  # Ctrl+Q
            output = wait_for(proc, master, lambda data: b"parsed 0001" in data and
                              data.rstrip().endswith(b"0e0f") or b"Error" in data)
            assert b"parsed 000102030405060708090a0b0c0d0e0f" in output, output
            proc.wait(timeout=5)
            assert proc.returncode == 0, proc.returncode
        assert termios.tcgetattr(slave)[3] == before[3]
    finally:
        if proc.poll() is None:
            os.write(master, b"\x11")
        finish(proc)
        os.close(master)
        os.close(slave)


held_echo(cancel=False)
held_echo(cancel=True)
print("[PASS] KEY echo held by XOFF survives a stop and resume, and one SIGTERM ends it")

# A cancellation signal the parent left ignored (nohup; a script's `cmd &`)
# stays ignored; the others get jdvrif's handler.
proc = subprocess.run([helper, "dispositions"], capture_output=True, timeout=10,
                      preexec_fn=lambda: (signal.signal(signal.SIGHUP, signal.SIG_IGN),
                                          signal.signal(signal.SIGINT, signal.SIG_IGN)))
assert proc.returncode == 0 and proc.stdout.decode().split() == \
    ["HUP=ignored", "INT=ignored", "QUIT=handled", "TERM=handled"], proc
proc = subprocess.run([helper, "dispositions"], capture_output=True, timeout=10)
assert proc.returncode == 0 and proc.stdout.decode().split() == \
    ["HUP=handled", "INT=handled", "QUIT=handled", "TERM=handled"], proc
print("[PASS] inherited SIG_IGN dispositions are kept")

# A stop landing as raw mode is entered -- before the settings are read back,
# or after but before the first read -- must still have its continue noticed:
# the shell restores cooked mode meanwhile.
for stop_mode in ("stop-after-raw", "stop-after-verify"):
    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    proc = subprocess.Popen([helper, stop_mode], stdin=slave, stdout=slave, stderr=slave,
                            process_group=0, close_fds=True)
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            pid, status = os.waitpid(proc.pid, os.WUNTRACED | os.WNOHANG)
            if pid:
                assert os.WIFSTOPPED(status), status
                break
            time.sleep(0.01)
        else:
            raise AssertionError(f"{stop_mode}: KEY reader did not stop")
        # Emulate the shell restoring cooked mode while its job is stopped.
        termios.tcsetattr(slave, termios.TCSANOW, before)
        os.kill(proc.pid, signal.SIGCONT)
        wait_for(proc, master, lambda data: b"KEY: " in data and not
                 (termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)))
        os.write(master, b"112d-rXXU-ifSr-RnXL-GbXg-8Egs\n")
        output = wait_for(proc, master, lambda data: b"parsed 0001" in data and
                          data.rstrip().endswith(b"0e0f"))
        assert b"112d-rXXU" not in output, (stop_mode, output)
        proc.wait(timeout=5)
        assert proc.returncode == 0, (stop_mode, proc.returncode)
        assert termios.tcgetattr(slave) == before, stop_mode
    finally:
        if proc.poll() is None:
            os.kill(proc.pid, signal.SIGCONT)
        finish(proc)
        os.close(master)
        os.close(slave)
print("[PASS] stop while entering raw mode still re-applies raw mode")

# The carrier path validates the name before prompting, then opens it again
# after the KEY. Exercise both in-place growth and replacement during that gap.
for replacement in (False, True):
    case = work / ("replace" if replacement else "grow")
    case.mkdir()
    image = case / "input.jpg"
    image.write_bytes(b"ordinary image bytes" * 100)
    proc = subprocess.Popen([binary, "recover", str(image)], cwd=case,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        prefix = wait_for(proc, proc.stdout.fileno(), lambda data: (b"KEY: " in data or b"KEY: " in data))
        target = case / "replacement.jpg" if replacement else image
        with target.open("wb") as stream:
            stream.truncate(64 * 1024 * 1024)
        if replacement:
            target.replace(image)
        stdout, _ = proc.communicate(input=b"112d-rXXU-ifSr-RnXL-GbXg-8Egs\n", timeout=10)
        output = prefix + stdout
        assert proc.returncode != 0 and b"permitted size limit" in output, output
    finally:
        finish(proc)
print("[PASS] prompt-time carrier growth and replacement preserve the 20 MiB limit")
PY

"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -I"$ROOT" "$TESTS/test_output_faults.cpp" "$ROOT/file_utils.cpp" "$ROOT/signal_utils.cpp" \
    -lsodium -Wl,--wrap=open -o "$WORK/test_output_faults"
"$WORK/test_output_faults"
echo "[PASS] failed drain is not repeated and a throwing StagingFile closes its fd"
