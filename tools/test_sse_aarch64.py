"""Compare ARM SSE1 arithmetic with a native x86 hardware oracle."""
from pathlib import Path
import math
import struct
import subprocess

root = Path(__file__).resolve().parents[1]
runtime = root / "sdk/templates/runtime"
backend = root / "sdk/src/platform/recomp_sse_aarch64.c"
source = root / "tests/test_sse_environment.c"
results = []
for host, compiler, runner in (
    ("native", "gcc", []),
    ("aarch64", "aarch64-linux-gnu-gcc", ["qemu-aarch64", "-L", "/usr/aarch64-linux-gnu"]),
):
    binary = root / "build/linux" / ("test_sse_" + host)
    subprocess.run([compiler, "-O2", "-fno-fast-math", "-I", str(runtime),
                    str(source), str(backend), "-lm", "-o", str(binary)], check=True)
    output = subprocess.check_output([*runner, str(binary)], timeout=30)
    (root / "build/linux" / ("sse_" + host + ".txt")).write_bytes(output)
    results.append([line.split() for line in output.decode().splitlines()])
    unary = root / "build/linux" / ("test_xmm_unary_" + host)
    subprocess.run([compiler, "-O2", "-I", str(runtime),
                    str(root / "sdk/tests/test_xmm_unary.c"),
                    str(backend), "-lm", "-o", str(unary)], check=True)
    subprocess.run([*runner, str(unary)], check=True, timeout=10)

def floating(bits):
    return struct.unpack("<f", struct.pack("<I", int(bits, 16)))[0]

assert len(results[0]) == len(results[1])
failures = []
for expected, actual in zip(*results):
    assert expected[:4] == actual[:4]
    same_result = expected[4] == actual[4]
    if expected[1] in ("5", "6") and not same_result:
        reference, computed = floating(expected[4]), floating(actual[4])
        # SSE reciprocals permit relative error <= 1.5 * 2^-12. NaN payloads,
        # zero signs and infinities must still match exactly.
        same_result = (math.isfinite(reference) and reference != 0 and
                       math.isfinite(computed) and
                       abs(reference - computed) <= abs(reference) * (1.5 * 2**-12))
    if not same_result or expected[5] != actual[5]:
        failures.append("x86: " + " ".join(expected) + " | ARM: " + " ".join(actual))
(root / "logs/sse-conformance.txt").write_text(
    f"Cases: {len(results[0])}; mismatches: {len(failures)}\n" + "\n".join(failures) + "\n")
if failures:
    print("\n".join(failures[:20]))
    raise SystemExit(f"{len(failures)} mismatches / {len(results[0])}; see logs/sse-conformance.txt")
print(f"ARM SSE conformance passed: {len(results[0])} cases, host FPCR/FPSR preserved")
