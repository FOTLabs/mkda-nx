"""Run Kano's retail ending locally, without completing arcade mode.

Requires prepared game assets and build/linux/mkda_recomp. Uses isolated saves,
no controller input, and no Switch connection. Each run is bounded at 110 s.
"""
from pathlib import Path
import json
import os
import re
import subprocess

root = Path(__file__).resolve().parents[1]
saves = root / "build/ending-preview/saves"
saves.mkdir(parents=True, exist_ok=True)
(root / "logs").mkdir(exist_ok=True)
results = {}
for mode in ("unpaced", "paced"):
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               MKDA_ENDING_PREVIEW="kano", RECOMP_QUIET="1", RECOMP_GL_THREAD="1",
               RECOMP_GL_PREWARM_BLOCK="1",
               RECOMP_GL_DUMP=f"logs/ending-preview-{mode}-,300")
    env.pop("MKDA_ENDING_PREVIEW_UNPACED", None)
    if mode == "unpaced":
        env["MKDA_ENDING_PREVIEW_UNPACED"] = "1"
    log_path = root / f"logs/ending-preview-{mode}.txt"
    with log_path.open("w") as log:
        subprocess.run([str(root / "build/linux/mkda_recomp"), "game", str(saves)],
                       cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=110)
    text = log_path.read_text()
    assert "endings.ssf" in text and "end_kano.msx" in text
    complete = re.search(r"Complete ticks=(\d+) elapsed=([\d.]+) next-context=20", text)
    assert complete, f"Retail ending did not complete ({mode})"
    ticks, seconds = int(complete[1]), float(complete[2])
    results[mode] = {"ticks": ticks, "seconds": seconds}
    print(f"Kano ending {mode}: {ticks} ticks in {seconds:.3f}s", flush=True)
assert results["paced"]["ticks"] == results["unpaced"]["ticks"]
assert results["paced"]["ticks"] > 2000
assert results["paced"]["seconds"] >= (results["paced"]["ticks"] - 1) / 60 - 0.05
(root / "logs/ending-preview-result.json").write_text(json.dumps(results, indent=2) + "\n")
print("PASS complete retail ending follows the 60Hz scene clock")
