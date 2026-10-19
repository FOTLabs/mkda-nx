"""Extract the local retail image and record its file inventory; no network."""
from pathlib import Path
import hashlib, json, os, shutil, sys
root = Path(__file__).resolve().parents[1]
toolkit = Path(os.environ.get("XBOXRECOMP_DIR", str((root / "sdk" if (root / "sdk").is_dir() else root / "toolkit/xboxrecomp"))))
sys.path.insert(0, str(toolkit))
from tools.xiso.xdvdfs import Xiso
images = [Path(sys.argv[1]).resolve()] if len(sys.argv) == 2 else list(root.glob("*.iso"))
if len(images) != 1:
    raise SystemExit("Expected exactly one local ISO")
game = root / "game"
game.mkdir(exist_ok=True)
with Xiso(str(images[0])) as image:
    entries = list(image.walk())
    required = sum(e.size for _, e in entries)
    if shutil.disk_usage(root).free < required + 1024**3:
        raise SystemExit("Need extracted size plus 1 GiB free space")
    inventory = []
    for directory, entry in entries:
        relative = Path(directory) / entry.name
        target = (game / relative).resolve()
        if not target.is_relative_to(game.resolve()):
            raise SystemExit("Disc path escapes game directory")
        if target.exists():
            raise SystemExit("Refusing to overwrite existing asset: " + str(relative))
        image.extract(entry, str(target))
        inventory.append({"path": relative.as_posix(), "size": entry.size})
    manifest = {"iso": images[0].name, "iso_bytes": images[0].stat().st_size,
                "partition_base": hex(image.base), "extracted_bytes": required,
                "files": inventory}
xbe = next(game.glob("*.[xX][bB][eE]"))
manifest["xbe_sha256"] = hashlib.sha256(xbe.read_bytes()).hexdigest()
(root / "analysis").mkdir(exist_ok=True)
(root / "analysis/disc_inventory.json").write_text(json.dumps(manifest, indent=2) + "\n")
print("Extracted", len(inventory), "files;", required, "bytes", flush=True)
print("XBE SHA256", manifest["xbe_sha256"], flush=True)
