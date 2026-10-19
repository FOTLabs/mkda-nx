"""Relink release application objects with a replacement LGPL library archive.
Run from the extracted relink package: python3 relink.py --devkitpro /opt/devkitpro
To modify the APU, replace xboxrecomp/src/apu/libxbox_apu.a with a compatible
archive rebuilt from sdk/src/apu in the matching source package, then run again.
"""
from pathlib import Path
import argparse, json, os, subprocess
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--devkitpro",default=os.environ.get("DEVKITPRO","/opt/devkitpro"))
args=parser.parse_args();root=Path(__file__).resolve().parent
os.environ["DEVKITPRO"]=args.devkitpro
meta=json.loads((root/"link.json").read_text())
def expand(s):return s.replace("${ROOT}",str(root)).replace("${DEVKITPRO}",args.devkitpro)
command=[expand(s) for s in meta["argv"]]
(root/"rebuilt").mkdir(exist_ok=True)
subprocess.run(command,cwd=root,check=True)
subprocess.run([str(Path(args.devkitpro)/"tools/bin/elf2nro"),str(root/"rebuilt/mkda_recomp.elf"),str(root/"rebuilt/mkdax.nro"),"--nacp="+str(root/"mkdax.nacp")],check=True)
print("Relinked NRO:",root/"rebuilt/mkdax.nro")
