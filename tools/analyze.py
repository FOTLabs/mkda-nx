"""Build this title's analysis databases in its isolated framework checkout."""
from pathlib import Path
import os, subprocess, sys
root = Path(__file__).resolve().parents[1]
toolkit = Path(os.environ.get("XBOXRECOMP_DIR", str((root / "sdk" if (root / "sdk").is_dir() else root / "toolkit/xboxrecomp"))))
xbe = root / "game/default.xbe"
(root / "logs").mkdir(exist_ok=True)
(root / "analysis").mkdir(exist_ok=True)
seeds = root / "config/seed_functions.json"
disasm_args = ["tools.disasm", str(xbe), "--analysis-json", str(root / "analysis/xbe.json")]
if seeds.exists():
    disasm_args.extend(["--seed-functions", str(seeds)])
steps = [
 ("xbe", ["tools.xbe_parser", str(xbe), "--json", str(root / "analysis/xbe.json")]),
 ("disasm", disasm_args),
 ("func-id", ["tools.func_id", str(xbe)]),
 ("abi", ["tools.abi_analysis", str(xbe)]),
]
for name, args in steps:
    print("Running", name, flush=True)
    with open(root / "logs" / (name + ".txt"), "w") as log:
        result = subprocess.run([sys.executable, "-m", *args], cwd=toolkit, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit("Failed " + name + "; see logs/" + name + ".txt")
print("Analysis complete", flush=True)
