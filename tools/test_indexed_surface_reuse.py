"""Replay a local fighter capture after its texture addresses held render targets.

The retail fixture stays outside Git. This reconstructs allocation reuse without
injecting any controller input or changing the captured texture data.
"""
from pathlib import Path
import gzip
import os
import struct
import subprocess

root = Path(__file__).resolve().parents[1]
directory = root / "logs/switch-johnny-arms"
fixture = directory / "johnny-arms.fixture.gz"
if not fixture.exists():
    raise SystemExit("Local Scorpion graphics capture required")
data = gzip.decompress(fixture.read_bytes())
at, draw, insertion, template = 16, 0, None, None
targets = {}
while at < len(data):
    kind, size = struct.unpack_from("<II", data, at)
    payload = data[at + 8:at + 8 + size]
    at += 8 + size
    if kind == 3 and template is None:
        template = payload
    if kind != 2:
        continue
    if insertion is None:
        insertion = at
    attrs = struct.unpack_from("<I", payload, 912)[0]
    indices = struct.unpack_from("<I", payload, 32)[0]
    regs = struct.unpack_from("<2048I", payload, 1048 + attrs * 4 + indices * 4)
    address = struct.unpack_from("<I", payload, 224)[0]
    format_word = regs[0x1B04 // 4]
    # This capture's two fighters, identified from per-draw inspection.
    if 29 <= draw <= 62:
        assert (format_word >> 8) & 255 == 0x0B
        targets[address] = (1 << ((format_word >> 20) & 15),
                            1 << ((format_word >> 24) & 15))
    draw += 1
assert template is not None and insertion is not None and len(targets) == 17
prefix = bytearray()
for address, (width, height) in targets.items():
    clear = bytearray(template)
    struct.pack_into("<9I", clear, 0, address, width, height, width * 4, 4, 1, 1, 0, 0)
    struct.pack_into("<4I", clear, len(clear) - 16, 0xF0, 0xFFFFFFFF, 0, 1)
    prefix += struct.pack("<II", 3, len(clear)) + clear
stale = directory / "stale-surfaces.fixture.gz"
stale.write_bytes(gzip.compress(data[:insertion] + prefix + data[insertion:]))
for queue in ("0", "1"):
    frames = []
    for name, source in (("clean", fixture), ("reuse", stale)):
        output = directory / f"indexed-{queue}-{name}-"
        env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
                   RECOMP_REPLAY=str(source), RECOMP_QUIET="1", RECOMP_GL_THREAD=queue,
                   RECOMP_GL_SCALE="1.5", RECOMP_GL_MSAA="2",
                   RECOMP_GL_DUMP=str(output.relative_to(root)) + ",1")
        with (directory / f"indexed-{queue}-{name}.txt").open("w") as log:
            subprocess.run([str(root / "build/linux/mkda_recomp"), "game", "saves"],
                           cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=30)
        frames.append(Path(str(output) + "00001.bmp").read_bytes())
    assert frames[0] == frames[1], f"Stale surfaces override fighter palettes (queue={queue})"
    print(f"PASS indexed fighter colours survive render-target address reuse (queue={queue})")
