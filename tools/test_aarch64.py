"""Validate the Switch context backend using Linux AArch64 binaries in QEMU."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
for name, sources in (
    ("frame_pacing", ["tests/test_frame_pacing.c", "src/frame_pacing.c"]),
    ("native_context", ["tests/test_native_context.c"]),
    ("guest_memory_alias", ["tests/test_guest_memory_alias.c"]),
    ("movie_clock", ["tests/test_movie_clock.c", "src/recomp_manual.c"]),
    ("movie_blit", ["tests/test_movie_blit.c", "src/memory.c", "generated/gen/recomp_0041.c"]),
    ("coroutines", ["tests/test_coroutines.c", "src/coroutines.c"]),
    ("guest_dispatchers", ["tests/test_guest_dispatchers.c", "src/coroutines.c",
                           "generated/gen/recomp_0013.c"]),
):
    binary = root / "build/linux" / ("test_" + name + "_aarch64")
    # glibc's fortified longjmp assumes a single stack. This test deliberately
    # unwinds a fiber into the scheduler, as the game does during level changes.
    subprocess.run(["aarch64-linux-gnu-gcc", "-O2", "-fno-strict-aliasing", "-fwrapv", "-U_FORTIFY_SOURCE",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-D_FORTIFY_SOURCE=0", "-DMKDA_AARCH64_CONTEXT",
                    "-I", str(root / "generated/gen"),
                    *[str(root / source) for source in sources],
                    str(root / "sdk/src/platform/recomp_sse_aarch64.c"),
                    str(root / "src/native_context_aarch64.S"),
                    "-lm", "-o", str(binary)], check=True)
    subprocess.run(["qemu-aarch64", "-L", "/usr/aarch64-linux-gnu", str(binary)],
                   check=True, timeout=20)
print("AArch64 context and guest coroutine checks passed")
