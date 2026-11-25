#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jdvrif-carrier-threads.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -pthread \
    -I"$ROOT" "$ROOT/tests/test_carrier_threads.cpp" "$ROOT/twitter_juniward.cpp" \
    "$ROOT/twitter_stc.cpp" "$ROOT/twitter_jpeg_codec.cpp" "$ROOT/signal_utils.cpp" \
    -ljpeg -lsodium -o "$WORK/test_carrier_threads"
# The same test built the way release builds are, so the pinned cost digest
# below is checked against the vectorised kernel too.
"${CXX:-g++}" -std=c++23 -O3 -march=native -Wall -Wextra -Wpedantic -pthread \
    -I"$ROOT" "$ROOT/tests/test_carrier_threads.cpp" "$ROOT/twitter_juniward.cpp" \
    "$ROOT/twitter_stc.cpp" "$ROOT/twitter_jpeg_codec.cpp" "$ROOT/signal_utils.cpp" \
    -ljpeg -lsodium -o "$WORK/test_carrier_threads_native"
"${CXX:-g++}" -std=c++23 -shared -fPIC -pthread \
    "$ROOT/tests/refuse_carrier_threads.cpp" -ldl -o "$WORK/refuse_threads.so"
# Key-derived carrier state (the keyed shuffle and the WipingAllocator blocks
# the carriers keep their layouts in) is zeroed before it is released.
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic \
    -I"$ROOT" "$ROOT/tests/test_carrier_wipe.cpp" -o "$WORK/test_carrier_wipe"
"$WORK/test_carrier_wipe"
# Again with ASan's container-overflow checks on std::vector: wiping must only
# touch elements the vector holds.
"${CXX:-g++}" -std=c++23 -O1 -g -fsanitize=address -D_GLIBCXX_SANITIZE_VECTOR \
    -I"$ROOT" "$ROOT/tests/test_carrier_wipe.cpp" -o "$WORK/test_carrier_wipe_asan"
ASAN_OPTIONS=detect_container_overflow=1 "$WORK/test_carrier_wipe_asan"
python3 - "$WORK" <<'PY'
import os
import pathlib
import re
import resource
import subprocess
import sys

work = pathlib.Path(sys.argv[1])
for mode in ('luminance', 'costs', 'stc', 'helper'):
    def run(threads, allowed=None, limited=False):
        env = dict(os.environ, OMP_NUM_THREADS=str(threads), OMP_DYNAMIC='FALSE')
        if allowed is not None:
            env.update(LD_PRELOAD=str(work / 'refuse_threads.so'),
                       JDVRIF_TEST_THREADS_ALLOWED=str(allowed))
        def limit():
            resource.setrlimit(resource.RLIMIT_NPROC, (0, 0))
        result = subprocess.run([str(work / 'test_carrier_threads'), mode], env=env,
                                capture_output=True, timeout=60,
                                preexec_fn=limit if limited else None)
        assert result.returncode == 0, (mode, threads, allowed, result.stderr)
        if allowed is not None:
            counts = re.search(rb'thread attempts=(\d+) refused=(\d+)', result.stderr)
            assert counts and int(counts[2]) > 0, result.stderr
        return result.stdout
    serial = run(1)
    assert run(4) == serial, mode
    assert run(4, 0) == serial, mode
    assert run(4, 1) == serial, mode
    assert run(4, limited=True) == serial, mode
    print(f'[PASS] {mode}: serial/parallel/refused/partial team/process limit agree')

# Single-threaded: the table-driven CRC-32 against its bitwise definition.
for binary in ('test_carrier_threads', 'test_carrier_threads_native'):
    result = subprocess.run([str(work / binary), 'crc'], capture_output=True, timeout=60)
    assert result.returncode == 0, (binary, result.stderr)
print('[PASS] crc: -O2 and -O3 -march=native builds match the bitwise CRC-32')
result = subprocess.run([str(work / 'test_carrier_threads'), 'envelope'], capture_output=True, timeout=60)
assert result.returncode == 0, result.stderr
print('[PASS] envelope: declared length must match the bytes after the header exactly')
result = subprocess.run([str(work / 'test_carrier_threads'), 'bounds'], capture_output=True, timeout=60)
assert result.returncode == 0, result.stderr
print('[PASS] bounds: empty carrier geometry and four-component JPEGs are refused')

# J-UNIWARD costs decide which coefficients the carrier changes, so every
# build (scalar or vectorised) and any rewrite of the kernel must reproduce
# these exact float bits.
# Likewise the STC trellis, which is vectorised two lanes wide without AVX
# and four with it: both widths must choose exactly the same paths.
PINNED = {'costs': b'9600d1138021e8dc\n', 'stc': b'a2649fd8aee37969\n'}
for mode, digest in PINNED.items():
    for binary in ('test_carrier_threads', 'test_carrier_threads_native'):
        result = subprocess.run([str(work / binary), mode], capture_output=True, timeout=60,
                                env=dict(os.environ, OMP_NUM_THREADS='4'))
        assert result.returncode == 0 and result.stdout == digest, (mode, binary, result)
    print(f'[PASS] {mode}: -O2 and -O3 -march=native builds match the pinned digest')

# The thread count follows the OpenMP settings as libgomp reads them, capped
# by the CPUs this process may use; malformed settings are ignored.
cpus = len(os.sched_getaffinity(0))
def count(**settings):
    env = {k: v for k, v in os.environ.items() if not k.startswith('OMP_')}
    env.update(settings)
    result = subprocess.run([str(work / 'test_carrier_threads'), 'threads'], env=env,
                            capture_output=True, timeout=60)
    assert result.returncode == 0, result.stderr
    return int(result.stdout)
assert count() == cpus
assert count(OMP_NUM_THREADS='1') == 1
assert count(OMP_NUM_THREADS=' +2 , 4') == min(2, cpus)
assert count(OMP_NUM_THREADS=str(cpus + 7)) == cpus
assert count(OMP_THREAD_LIMIT='1') == 1
assert count(OMP_NUM_THREADS='3', OMP_THREAD_LIMIT='2') == min(2, cpus)
for malformed in ('0', '-2', 'abc', '4x', '4,', '4,0', ''):
    assert count(OMP_NUM_THREADS=malformed) == cpus, malformed
    assert count(OMP_THREAD_LIMIT=malformed) == cpus, malformed
print(f'[PASS] thread count follows OMP_NUM_THREADS / OMP_THREAD_LIMIT ({cpus} CPUs available)')
PY
