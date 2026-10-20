"""Create versioned NRO, matching source and LGPL relink materials locally."""
from pathlib import Path
import argparse, hashlib, json, os, shlex, shutil, subprocess, tarfile, tempfile, zipfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument("--build",type=Path,required=True);p.add_argument("--output",type=Path,required=True);p.add_argument("--version",default="1.0.2");p.add_argument("--source-root",type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[1];build=a.build.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sha=lambda p:hashlib.file_digest(open(p,"rb"),"sha256").hexdigest()
nro=build/"mkdax.nro"
with nro.open("rb") as f:f.seek(16);assert f.read(4)==b"NRO0"
shutil.copy2(nro,out/"mkdax.nro");shutil.copy2(root/"installer/LICENSES.txt",out/"LICENSES.txt")
source_root=(a.source_root or root).resolve()
commit=subprocess.check_output(["git","-C",str(source_root),"rev-parse","HEAD"],text=True).strip()
source=out/("mkda-nx-v"+a.version+"-source.tar.gz")
with source.open("wb") as f:subprocess.run(["git","-C",str(source_root),"archive","--format=tar.gz","--prefix=mkda-nx/",commit],stdout=f,check=True)
line=subprocess.check_output(["ninja","-C",str(build),"-t","commands","mkda_recomp"],text=True).splitlines()[-1]
argv=shlex.split(line.split(" && ")[1] if " && " in line else line)
with tempfile.TemporaryDirectory(prefix="mkda-relink-") as tmp:
    staging=Path(tmp);(staging/"deps/lib").mkdir(parents=True)
    # Application objects plus exact runtime archives. No retail data files.
    for word in argv:
        if word.endswith((".o",".a")):
            path=Path(word)
            if path.is_absolute():
                target=staging/"deps/lib"/path.name
            else:
                path=build/path;target=staging/word
            target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
    # System dependencies remain the normal devkitA64 toolchain, while these
    # additional installed libraries are retained for exact binary relinking.
    for name in ["libnx.a","libpthread.a","libz.a","libEGL.a","libglapi.a","libdrm_nouveau.a"]:
        locations=[Path("/opt/devkitpro/libnx/lib")/name,Path("/opt/devkitpro/portlibs/switch/lib")/name]
        found=next((x for x in locations if x.exists()),None)
        if found:shutil.copy2(found,staging/"deps/lib"/name)
    rewritten=[]
    for word in argv:
        if word.endswith(".a") and Path(word).is_absolute():word="${ROOT}/deps/lib/"+Path(word).name
        elif word.startswith("-L"):word="-L${ROOT}/deps/lib"
        else:word=word.replace(str(build),"${ROOT}").replace("/opt/devkitpro","${DEVKITPRO}")
        rewritten.append(word)
    rewritten[rewritten.index("-o")+1]="${ROOT}/rebuilt/mkda_recomp.elf"
    (staging/"link.json").write_text(json.dumps({"version":a.version,"source_commit":commit,"argv":rewritten},indent=2)+"\n")
    shutil.copy2(root/"tools/relink.py",staging/"relink.py");shutil.copy2(build/"mkdax.nacp",staging/"mkdax.nacp");shutil.copy2(out/"LICENSES.txt",staging/"LICENSES.txt")
    (staging/"README.txt").write_text("Matching source commit: "+commit+"\nInstall devkitA64 r29.2 and libnx 4.12.0. Run python3 relink.py.\nReplace xboxrecomp/src/apu/libxbox_apu.a with your modified LGPL APU archive and run again.\nComplete modified SDK source is in the matching source archive. Binary distribution does not remove any original-game rights.\n")
    # Test the actual supplied relinking command, not just the archive layout.
    subprocess.run(["python3",str(staging/"relink.py")],check=True)
    if sha(staging/"rebuilt/mkdax.nro")!=sha(nro):raise SystemExit("Relinked NRO differs from release")
    print("PASS: supplied objects and command reproduce the exact release NRO",flush=True)
    with tarfile.open(out/("mkda-nx-v"+a.version+"-relink.tar.gz"),"w:gz") as t:
        for path in sorted(staging.rglob("*")):
            if path.is_file() and "rebuilt" not in path.relative_to(staging).parts:t.add(path,arcname=str(path.relative_to(staging)))
with zipfile.ZipFile(out/("mkda-nx-v"+a.version+"-switch.zip"),"w",zipfile.ZIP_DEFLATED) as z:
    for path,name in [(nro,"mkdax.nro"),(root/"installer/LICENSES.txt","LICENSES.txt"),(root/"installer/mkdax_env.txt","mkdax_env.txt")]:
        z.write(path,"switch/mkdax/"+name)
manifest={"version":a.version,"source_commit":commit,"nroSha256":sha(nro),"nroUrl":"./release/mkdax.nro","status":"release"}
(out/"release.json").write_text(json.dumps(manifest,indent=2)+"\n")
(out/"SHA256SUMS").write_text("".join(sha(p)+"  "+p.name+"\n" for p in sorted(out.iterdir()) if p.is_file() and p.name!="SHA256SUMS"))
print("Release package:",out,flush=True)
