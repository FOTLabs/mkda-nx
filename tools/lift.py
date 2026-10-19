"""Generate MKDA code using this project's isolated analysis databases (WSL)."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
toolkit = (root / "sdk" if (root / "sdk").is_dir() else root / "toolkit/xboxrecomp")
xbe = root / "game/default.xbe"
identity = json.loads((root / "config/project.json").read_text())
if hashlib.sha256(xbe.read_bytes()).hexdigest() != identity["xbe_sha256"]:
    raise SystemExit("XBE does not match the verified project identity")
out = root / "generated"
out.mkdir(exist_ok=True)
(root / "logs").mkdir(exist_ok=True)
with (root / "logs/lift.txt").open("w") as log:
    result = subprocess.run([
        sys.executable, "-m", "tools.recomp", str(xbe), "--all",
        "--game-name", identity["title"], "--split", "200",
        "--mmio-sections", "DSOUND,XPP",
        "--output-dir", str(out), "--gen-dir", str(out / "gen"),
        # The option is comma separated; this title's absolute path itself has
        # commas, so use paths relative to the toolkit working directory.
        "--exclude-manual", ",".join(os.path.relpath(root / "src" / name, toolkit) for name in ("coroutines.c", "memory.c", "xdk_hooks.c", "recomp_manual.c")),
    ], cwd=toolkit, stdout=log, stderr=subprocess.STDOUT)
if result.returncode:
    raise SystemExit("Lift failed; see logs/lift.txt")
print("Lift complete:", out)
