"""Focused Linux runtime and lifter checks for the current bring-up changes."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
toolkit = (root / "sdk" if (root / "sdk").is_dir() else root / "toolkit/xboxrecomp")
subprocess.run([sys.executable, "-m", "unittest", "tools.recomp.test_mxcsr",
                "tools.recomp.test_port_io", "tools.recomp.test_memory_fences",
                "tools.recomp.test_lifter_sse", "tools.recomp.test_interrupt_control",
                "tools.recomp.test_indirect_frame"],
               cwd=toolkit, check=True)
for name, sources in (
    ("frame_pacing", [root / "tests/test_frame_pacing.c", root / "src/frame_pacing.c"]),
    ("mxcsr", [toolkit / "tests/test_mxcsr.c"]),
    ("native_context", [root / "tests/test_native_context.c"]),
    ("guest_memory_alias", [root / "tests/test_guest_memory_alias.c"]),
    ("tiled_alias", [root / "tests/test_tiled_alias.c"]),
    ("memory", [root / "tests/test_memory.c", root / "src/memory.c"]),
    ("input", [root / "tests/test_input.c", root / "src/input.c"]),
    ("movie_clock", [root / "tests/test_movie_clock.c", root / "src/recomp_manual.c"]),
    ("movie_blit", [root / "tests/test_movie_blit.c", root / "src/memory.c",
                     root / "generated/gen/recomp_0041.c"]),
    ("heap_diagnostics", [root / "tests/test_heap_diagnostics.c", root / "src/heap_diagnostics.c"]),
    ("coroutines", [root / "tests/test_coroutines.c", root / "src/coroutines.c"]),
    ("guest_dispatchers", [root / "tests/test_guest_dispatchers.c", root / "src/coroutines.c",
                           root / "generated/gen/recomp_0013.c"]),
    ("xdk_fence", [root / "tests/test_xdk_fence.c", root / "src/xdk_hooks.c",
                   toolkit / "src/game/xdk_hooks.c"]),
):
    binary = root / "build/linux" / ("test_" + name)
    binary.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["gcc", "-O2",
                    "-fno-strict-aliasing", "-fwrapv",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-I", str(root / "generated/gen"), *map(str, sources),
                    "-o", str(binary), "-lm"], check=True)
    subprocess.run([str(binary)], check=True)
subprocess.run(["cmake", "--build", str(root / "build/linux"), "--target",
                "mkda_interrupt_test", "-j", "4"], check=True)
subprocess.run([str(root / "build/linux/mkda_interrupt_test")], check=True, timeout=10)
subprocess.run(["cmake", "--build", str(root / "build/linux"), "--target",
                "mkda_event_poll_test", "-j", "4"], check=True)
subprocess.run([str(root / "build/linux/mkda_event_poll_test")], check=True, timeout=10)
subprocess.run([sys.executable, str(root / "tools/test_shutdown.py")], cwd=root, check=True)
print("Focused checks passed")
