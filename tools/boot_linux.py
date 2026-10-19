"""Bounded Linux boot probe; capture diagnostics without simulated input."""
from pathlib import Path
import os
import subprocess
import threading
import shutil
from datetime import datetime, timezone
import fcntl

root = Path(__file__).resolve().parents[1]

# Keep concurrent probes from sharing the same save images and log stream.
(root / "logs").mkdir(exist_ok=True)
probe_lock = (root / "logs/boot-linux.lock").open("a")
try:
    fcntl.flock(probe_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    raise SystemExit("Another Linux boot probe is still running")
binary = (root / "build/linux/mkda_recomp").resolve()
for process in Path("/proc").iterdir():
    if not process.name.isdigit():
        continue
    try:
        if (process / "exe").resolve() == binary:
            raise SystemExit(f"Linux guest process {process.name} is still running")
    except OSError:
        continue
environment = dict(os.environ)
probe_seconds = int(environment.get("MKDA_PROBE_SECS", "30"))
if not 1 <= probe_seconds <= 600:
    raise ValueError("MKDA_PROBE_SECS must be between 1 and 600")
environment.setdefault("RECOMP_WATCHDOG_SECS", "15")
environment.setdefault("SDL_VIDEODRIVER", "offscreen")
environment.setdefault("SDL_AUDIODRIVER", "dummy")
environment.setdefault("RECOMP_PB_EXEC", "1")
environment.setdefault("RECOMP_VBLANK", "1")
(root / "logs/frames").mkdir(exist_ok=True)

def sample_threads():
    with (root / "logs/sample-thread-ids.txt").open("w") as log:
        subprocess.run([os.sys.executable, str(root / "tools/sample_linux_threads.py")],
                       cwd=root, env=environment, stdout=log, stderr=subprocess.STDOUT)

sample_timer = None
if environment.get("MKDA_SAMPLE_ALL_SECS"):
    sample_seconds = int(environment["MKDA_SAMPLE_ALL_SECS"])
    native_sample_seconds = int(environment.get("MKDA_SAMPLE_SECS", "0"))
    if not 0 < native_sample_seconds < sample_seconds < probe_seconds:
        raise ValueError("Thread sampling must follow a timed native sample and precede the probe limit")
    sample_timer = threading.Timer(sample_seconds, sample_threads)
    sample_timer.daemon = True
environment.setdefault("RECOMP_GL_DUMP", "logs/frames/frame,60")
current_log = root / "logs/boot-linux.txt"
if current_log.exists():
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    shutil.copy2(current_log, root / "logs" / ("boot-" + stamp + ".txt"))
with current_log.open("w") as log:
    if sample_timer:
        sample_timer.start()
    try:
        result = subprocess.run(
            [str(binary), str(root / "game"),
             str(root / "saves")], cwd=root, env=environment,
            stdout=log, stderr=subprocess.STDOUT, timeout=probe_seconds)
        code = result.returncode
    except subprocess.TimeoutExpired:
        code = 124
    finally:
        if sample_timer:
            sample_timer.cancel()
    log.write(f"\n[PROBE] exit={code} (124 means {probe_seconds}-second timeout)\n")
print("Boot probe exit:", code, "-- see logs/boot-linux.txt")
raise SystemExit(code if code >= 0 else 128 - code)
