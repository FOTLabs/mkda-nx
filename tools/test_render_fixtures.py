"""Replay the user's missing-menu capture in both GL execution modes.

The retail-data fixture remains local and excluded from Git. The stone arch
behind the menu must contain texture detail, rather than a flat fog colour.
"""
from pathlib import Path
import os
import struct
import subprocess

root = Path(__file__).resolve().parents[1]
fixture = root / "logs/switch-render-issue/render-issue.fixture.gz"
if not fixture.is_file():
    raise SystemExit("Local Switch menu fixture is required")
outputs = []
for mode in ("0", "1"):
    prefix = f"logs/frames/menu-regression-{mode}-"
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               RECOMP_REPLAY=str(fixture), RECOMP_QUIET="1", RECOMP_GL_THREAD=mode,
               RECOMP_GL_DUMP=prefix + ",1")
    with (root / f"logs/menu-regression-{mode}.txt").open("w") as log:
        subprocess.run([str(root / "build/linux/mkda_recomp"), "game", "saves"],
                       cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=30)
    outputs.append((root / (prefix + "00001.bmp")).read_bytes())
assert outputs[0] == outputs[1], "Threaded menu replay pixels differ"
data = outputs[0]
offset = struct.unpack_from("<I", data, 10)[0]
width, height = struct.unpack_from("<ii", data, 18)
assert width == 640 and abs(height) == 480
assert struct.unpack_from("<H", data, 28)[0] == 24
stride = (width * 3 + 3) & ~3
row = abs(height) - 1 - 80 if height > 0 else 80
start = offset + row * stride
colours = {data[start + x * 3:start + x * 3 + 3] for x in range(360, 600)}
assert len(colours) > 20, "Menu corridor is still flat instead of textured"
print(f"Menu background restored ({len(colours)} colours on arch); queue pixels match")

for msaa in ("0", "2"):
    prefix = f"logs/frames/menu-scaled-msaa{msaa}-"
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               RECOMP_REPLAY=str(fixture), RECOMP_QUIET="1", RECOMP_GL_THREAD="1",
               RECOMP_GL_SCALE="1.5", RECOMP_GL_MSAA=msaa,
               RECOMP_GL_DUMP=prefix + ",1")
    with (root / f"logs/menu-scaled-msaa{msaa}.txt").open("w") as log:
        subprocess.run([str(root / "build/linux/mkda_recomp"), "game", "saves"],
                       cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=30)
    data = (root / (prefix + "00001.bmp")).read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    assert width == 960 and abs(height) == 720
    stride = (width * 3 + 3) & ~3
    blue = 0
    for y in range(460, 580):
        row = abs(height) - 1 - y if height > 0 else y
        for x in range(560, 830):
            b, g, r = data[offset + row * stride + x * 3:offset + row * stride + x * 3 + 3]
            blue += b > 80 and b > r * 1.6 and b > g * 1.6
    assert blue == 0, f"Scaled UI has {blue} blue atlas-border pixels (MSAA={msaa})"
print("Scaled text is free of blue atlas edges with MSAA off and 2x")

movie = root / "logs/mkda-yuv.fixture.gz"
if movie.is_file():
    frames = []
    for mode, gpu in (("0", "0"), ("1", "0"), ("0", "1"), ("1", "1")):
        prefix = f"logs/frames/movie-regression-{mode}-gpu{gpu}-"
        env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
                   RECOMP_REPLAY=str(movie), RECOMP_QUIET="1", RECOMP_GL_THREAD=mode,
                   RECOMP_GL_DUMP=prefix + ",1", RECOMP_GL_YUV_GPU=gpu)
        log_path = root / f"logs/movie-regression-{mode}-gpu{gpu}.txt"
        with log_path.open("w") as log:
            subprocess.run([str(root / "build/linux/mkda_recomp"), "game", "saves"],
                           cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=30)
        if gpu == "1":
            assert "GPU YUY2/UYVY conversion ready" in log_path.read_text(), "GPU path fell back"
        frames.append([(root / (prefix + f"{i:05}.bmp")).read_bytes()
                       for i in range(1, 4)])
    assert all(f == frames[0] for f in frames), "CPU/GPU or queued movie pixels differ"
    assert len(set(frames[0])) == 3, "Movie fixture must contain three distinct frames"
    print("Three distinct movie frames match across CPU/GPU conversion and both GL modes")
    prefix = "logs/frames/movie-scaled-msaa2-"
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               RECOMP_REPLAY=str(movie), RECOMP_QUIET="1", RECOMP_GL_THREAD="1",
               RECOMP_GL_YUV_GPU="1", RECOMP_GL_SCALE="1.5", RECOMP_GL_MSAA="2",
               RECOMP_GL_DUMP=prefix + ",1")
    with (root / "logs/movie-scaled-msaa2.txt").open("w") as log:
        subprocess.run([str(root / "build/linux/mkda_recomp"), "game", "saves"],
                       cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=30)
    for frame in (2, 3):
        data = (root / (prefix + f"{frame:05}.bmp")).read_bytes()
        offset = struct.unpack_from("<I", data, 10)[0]
        width, height = struct.unpack_from("<ii", data, 18)
        assert width == 960 and abs(height) == 720
        stride = (width * 3 + 3) & ~3
        blue = white = 0
        for y in range(625, 680):
            row = abs(height) - 1 - y if height > 0 else y
            for x in range(30, 930):
                b, g, r = data[offset + row * stride + x * 3:offset + row * stride + x * 3 + 3]
                blue += b > 80 and b > r * 1.6 and b > g * 1.6
                white += min(b, g, r) > 150
        assert blue == 0, f"Movie subtitles have {blue} blue edge pixels"
        assert white > 200, "Expected subtitles are missing"
    print("Movie subtitles are present without blue edges at 1.5x/2x MSAA")

# The user's Konquest capture: bright secondary vertex colours are material
# inputs to fixed-function lighting, not an unlit specular overlay.
mountains = root / "logs/switch-konquest-review/konquest-mountains.fixture.gz"
if mountains.is_file():
    frames = []
    for mode in ("0", "1"):
        prefix = f"logs/frames/mountains-regression-{mode}-"
        env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
                   RECOMP_REPLAY=str(mountains), RECOMP_QUIET="1", RECOMP_GL_THREAD=mode,
                   RECOMP_GL_DUMP=prefix + ",1")
        with (root / f"logs/mountains-regression-{mode}.txt").open("w") as log:
            subprocess.run([str(root / "build/linux/mkda_recomp"), "game", "saves"],
                           cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=30)
        data = (root / (prefix + "00001.bmp")).read_bytes()
        frames.append(data)
        offset = struct.unpack_from("<I", data, 10)[0]
        width, height = struct.unpack_from("<ii", data, 18)
        assert width == 640 and abs(height) == 480
        stride = (width * 3 + 3) & ~3
        white = 0
        colours = set()
        for y in range(160, 420):
            row = abs(height) - 1 - y if height > 0 else y
            for x in range(150, 620):
                b, g, r = data[offset + row*stride + x*3:offset + row*stride + x*3 + 3]
                white += min(b, g, r) > 130 and max(b, g, r) - min(b, g, r) < 12
                colours.add((b, g, r))
        assert white < 1000, f"Konquest mountains still contain {white} white pixels"
        assert len(colours) > 500, "Expected textured mountain detail is missing"
    assert frames[0] == frames[1], "Queued mountain replay differs"
    print("Konquest mountain detail restored; white overlay absent in both GL modes")
