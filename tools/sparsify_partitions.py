"""Reclaim zero runs in this project's NTFS partition images, preserving bytes."""
from pathlib import Path
import ctypes
from ctypes import wintypes
import hashlib
import os
import struct

if os.name != "nt":
    raise SystemExit("Run with native Windows Python, e.g. py -3 tools/sparsify_partitions.py")
import msvcrt
root = Path(__file__).resolve().parents[1]
ioctl = ctypes.WinDLL("kernel32", use_last_error=True).DeviceIoControl
ioctl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                  ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                  ctypes.c_void_p]
ioctl.restype = wintypes.BOOL

for number in range(1, 6):
    path = root / "saves" / f"Partition{number}.img"
    if not path.exists():
        continue
    before = hashlib.sha256()
    zero_runs = []
    offset = 0
    run_start = None
    with path.open("r+b", buffering=0) as stream:
        handle = msvcrt.get_osfhandle(stream.fileno())
        returned = wintypes.DWORD()
        if not ioctl(handle, 0x900C4, None, 0, None, 0, ctypes.byref(returned), None):
            raise ctypes.WinError(ctypes.get_last_error())
        while block := stream.read(1024 * 1024):
            before.update(block)
            zero = block.count(0) == len(block)
            if zero and run_start is None:
                run_start = offset
            if not zero and run_start is not None:
                zero_runs.append((run_start, offset))
                run_start = None
            offset += len(block)
        if run_start is not None:
            zero_runs.append((run_start, offset))
        for start, end in zero_runs:
            request = ctypes.create_string_buffer(struct.pack("qq", start, end))
            if not ioctl(handle, 0x980C8, request, 16, None, 0,
                         ctypes.byref(returned), None):
                raise ctypes.WinError(ctypes.get_last_error())
        stream.seek(0)
        after = hashlib.file_digest(stream, "sha256").hexdigest()
    if after != before.hexdigest():
        raise SystemExit(f"Content verification failed: {path}")
    print(path.name, "verified", after, "zero bytes reclaimed:",
          sum(end - start for start, end in zero_runs), flush=True)
