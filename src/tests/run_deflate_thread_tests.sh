#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-deflate-threads.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -pthread \
    -I"$ROOT" "$ROOT/tests/test_deflate_threads.cpp" "${COMPRESSION_SOURCE:-$ROOT/compression.cpp}" \
    "$ROOT/file_utils.cpp" "$ROOT/signal_utils.cpp" -lz -ldeflate -lsodium \
    -o "$WORK/test_deflate_threads"
"${CXX:-g++}" -std=c++23 -shared -fPIC -pthread \
    "$ROOT/tests/refuse_carrier_threads.cpp" -ldl -o "$WORK/refuse_threads.so"
python3 - "$WORK" <<'PY'
import os
import pathlib
import random
import re
import resource
import subprocess
import sys
import zlib

work = pathlib.Path(sys.argv[1])
cpus = sorted(os.sched_getaffinity(0))[:3]
assert len(cpus) == 3, 'This regression needs at least three available CPUs to select parallel deflate'
size = 17 * 1024 * 1024 + 123  # Multiple 6 MiB windows and a short final slice.
rng = random.Random(42)
pattern = rng.randbytes(8192)
text = (pattern * (size // len(pattern) + 1))[:size]
noise = rng.randbytes(size)
(work / 'text').write_bytes(text)
(work / 'noise').write_bytes(noise)
counter = 0

def run(name, allowed=None, discard=False, expected=size, limited=False):
    global counter
    counter += 1
    output = work / f'out-{counter}'
    env = dict(os.environ)
    if allowed is not None:
        env.update(LD_PRELOAD=str(work / 'refuse_threads.so'),
                   JDVRIF_TEST_THREADS_ALLOWED=str(allowed))
    def limits():
        os.sched_setaffinity(0, cpus)
        if limited:
            resource.setrlimit(resource.RLIMIT_NPROC, (0, 0))
    result = subprocess.run([str(work / 'test_deflate_threads'), str(work / name),
                             str(output), str(expected), 'discard' if discard else 'keep'],
                            env=env, preexec_fn=limits, capture_output=True, timeout=60)
    if expected != size:
        assert result.returncode == 1 and b'changed while compressing' in result.stderr, result
        assert not output.exists()
        return
    assert result.returncode == 0, result.stderr
    if allowed is not None:
        counts = re.search(rb'thread attempts=(\d+) refused=(\d+)', result.stderr)
        assert counts and int(counts[2]) > 0, result.stderr
    if discard and name == 'noise':
        assert result.stdout == b'discarded\n' and not output.exists(), result
        return
    stream = output.read_bytes()
    assert zlib.decompress(stream) == (text if name == 'text' else noise)
    return stream

for name in ('text', 'noise'):
    reference = run(name)
    assert run(name, 0) == reference
    assert run(name, 1) == reference
    assert run(name, limited=True) == reference
    print(f'[PASS] {name}: normal/refused/partial team/process limit streams match and inflate')
run('noise', 0, discard=True)
run('text', 0, discard=True)
run('text', 0, expected=size - 100)
run('text', 1, expected=size + 100)
print('[PASS] discard policy and input-size errors preserved during fallback')

# Faults mid-run end it promptly: workers waiting for a free slot must notice.
def fault(policy):
    output = work / f'fault-{policy}'
    result = subprocess.run([str(work / 'test_deflate_threads'), str(work / 'noise'),
                             str(output), str(size), policy],
                            preexec_fn=lambda: os.sched_setaffinity(0, cpus),
                            capture_output=True, timeout=20)
    assert not output.exists(), result
    return result
result = fault('cancel')
assert result.returncode == 0 and result.stdout.startswith(b'cancelled in '), result
result = fault('write-limit')
assert result.returncode == 1 and b'Write' in result.stderr, result
result = fault('head-oom')
assert result.returncode == 0 and result.stdout == b'out of memory\n', result
print('[PASS] Ctrl+C, a failed write and a failed slice mid-run stop every worker')

# Optional existing Rust unit-test binary. Its test harness creates one thread
# before entering the test, so allow that one (or two for a partial worker team).
if rust_test := os.environ.get('RUST_DEFLATE_TEST_BIN'):
    for allowed in (1, 2):
        env = dict(os.environ, LD_PRELOAD=str(work / 'refuse_threads.so'),
                   JDVRIF_TEST_THREADS_ALLOWED=str(allowed))
        result = subprocess.run([rust_test, '--exact',
            'conceal_helpers::tests::parallel_deflate_inflates_and_is_thread_count_independent',
            '--test-threads=1'], env=env, capture_output=True, timeout=60)
        assert result.returncode == 0 and b'1 passed' in result.stdout, (result.stdout, result.stderr)
        counts = re.search(rb'thread attempts=(\d+) refused=(\d+)', result.stderr)
        assert counts and int(counts[2]) > 0, result.stderr
    print('[PASS] Rust deflate round trips and error handling with refused/partial worker teams')
PY
