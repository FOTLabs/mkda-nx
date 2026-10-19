"""Request SIGUSR1 backtraces from this project's opt-in Linux probe only."""
from pathlib import Path
import ctypes
import os
import signal
import time

root = Path(__file__).resolve().parents[1]
binary = os.fsencode(root / "build/linux/mkda_recomp")
matches = []
for entry in Path("/proc").iterdir():
    if not entry.name.isdecimal():
        continue
    try:
        command = (entry / "cmdline").read_bytes().split(b"\0")[0]
        environment = (entry / "environ").read_bytes().split(b"\0")
    except (OSError, PermissionError):
        continue
    settings = [item.partition(b"=")[2] for item in environment
                if item.startswith(b"MKDA_SAMPLE_SECS=")]
    sampler_enabled = settings and settings[0].isdigit() and 1 <= int(settings[0]) <= 600
    if command == binary and sampler_enabled:
        matches.append(entry)
if len(matches) != 1:
    raise SystemExit(f"Expected one opt-in probe in this workspace; found {len(matches)}")
process = matches[0]
status = dict(line.split(":", 1) for line in (process / "status").read_text().splitlines()
              if ":" in line)
if not int(status.get("SigCgt", "0").strip(), 16) & (1 << (signal.SIGUSR1 - 1)):
    # WSL1 reports a zero SigCgt even after sigaction succeeds. A backtrace
    # from the probe's scheduled SIGUSR1 demonstrates the handler is active.
    log = root / "logs/boot-linux.txt"
    if b"[MKDA] Native sample" not in log.read_bytes():
        raise SystemExit("No installed handler reported; wait for the scheduled native sample")
libc = ctypes.CDLL(None, use_errno=True)
libc.tgkill.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
libc.tgkill.restype = ctypes.c_int
for task in sorted((process / "task").iterdir(), key=lambda item: int(item.name)):
    print(f"Sample pid={process.name} tid={task.name}", flush=True)
    if libc.tgkill(int(process.name), int(task.name), signal.SIGUSR1):
        error = ctypes.get_errno()
        if error != 3:  # a worker can exit between enumeration and signalling
            raise OSError(error, os.strerror(error))
    time.sleep(0.1)
