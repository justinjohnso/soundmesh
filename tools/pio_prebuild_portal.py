Import("env")

from pathlib import Path
import gzip


ROOT = Path(env["PROJECT_DIR"])
DIST = ROOT / "portal" / "dist"
DATA = ROOT / "data"
FILES = ("index.html", "app.js", "app.css")


def should_sync_portal():
    pio_env = env["PIOENV"]
    return pio_env in ("tx", "rx", "combo")


def sync_portal_assets():
    if not DIST.exists():
        print("[portal-sync] portal/dist missing, keeping existing data/ assets")
        return

    DATA.mkdir(parents=True, exist_ok=True)
    for path in DATA.glob("*"):
        if path.is_file():
            path.unlink()

    for filename in FILES:
        src = DIST / filename
        if not src.exists():
            raise RuntimeError(f"[portal-sync] missing {src}")
        dst = DATA / f"{filename}.gz"
        with src.open("rb") as fsrc:
            raw = fsrc.read()
        with gzip.open(dst, "wb", compresslevel=9) as fdst:
            fdst.write(raw)
        print(f"[portal-sync] wrote {dst.name} ({dst.stat().st_size} bytes)")


if should_sync_portal():
    sync_portal_assets()

