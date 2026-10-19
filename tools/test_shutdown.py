"""Validate queue joins, watchdog cancellation and mapped-memory teardown."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
environment = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy")
binary = root / "build/linux/mkda_gl_shutdown_test"
with (root / "logs/test-shutdown.txt").open("w") as log:
    subprocess.run(["cmake", "--build", "build/linux", "--target", "mkda_gl_shutdown_test", "-j", "4"],
                   cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
    for arguments in (("1",), ("1", "idle"), ("0",),
                      ("1", "idle", "game/default.xbe")):
        subprocess.run([str(binary), *arguments], cwd=root, env=environment,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=8)
    fixture = root / "logs/mkda-kata.fixture.gz"
    if fixture.is_file():
        outputs = []
        for mode in ("0", "1"):
            prefix = f"build/linux/shutdown-replay-{mode}-"
            env = dict(environment, RECOMP_GL_DUMP=prefix + ",1")
            subprocess.run([str(binary), mode, "idle", "game/default.xbe", str(fixture)],
                           cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=30)
            outputs.append((root / (prefix + "00001.bmp")).read_bytes())
        assert outputs[0] == outputs[1], "Threaded and unthreaded fixture pixels differ"
        print("MKDA fixture replay pixels match; GL contexts released in both modes")
print("Queue, watchdog and mapped-memory shutdown checks passed; logs/test-shutdown.txt")
