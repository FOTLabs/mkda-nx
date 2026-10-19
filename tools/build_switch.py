"""Compile the initial Switch ELF; packaging and deployment are separate work."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
(root / "logs").mkdir(exist_ok=True)
environment = dict(os.environ)
environment.setdefault("DEVKITPRO", "/opt/devkitpro")
toolchain = Path(environment["DEVKITPRO"]) / "cmake/Switch.cmake"
if not toolchain.is_file():
    raise SystemExit(f"Existing devkitPro toolchain not found: {toolchain}")
for name, command in (
    ("configure-switch", ["cmake", "-S", ".", "-B", "build/switch", "-G", "Ninja",
                          f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release"]),
    ("build-switch", ["cmake", "--build", "build/switch", "-j", "4"]),
):
    with (root / "logs" / (name + ".txt")).open("w") as log:
        subprocess.run(command, cwd=root, env=environment, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
print("Switch ELF compile check passed: build/switch/mkda_recomp.elf")
print("Run tools/package_switch.py to create the NRO.")
