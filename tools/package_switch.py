"""Package the current Switch ELF as the canonical MKDA homebrew NRO."""
from pathlib import Path
import hashlib
import os
import subprocess
import json

root = Path(__file__).resolve().parents[1]
build = root / "build/switch"
binary = build / "mkda_recomp.elf"
if not binary.is_file():
    raise SystemExit("Run tools/build_switch.py first")
tools = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro")) / "tools/bin"
nacp = build / "mkdax.nacp"
nro = build / "mkdax.nro"
subprocess.run([str(tools / "nacptool"), "--create", "Mortal Kombat Deadly Alliance",
                "FOTLabs", json.loads((root / "config/release.json").read_text())["version"], str(nacp)], check=True)
subprocess.run([str(tools / "elf2nro"), str(binary), str(nro),
                "--nacp=" + str(nacp)], check=True)
print(f"{nro}: {nro.stat().st_size:,} bytes")
print("SHA256:", hashlib.sha256(nro.read_bytes()).hexdigest())
