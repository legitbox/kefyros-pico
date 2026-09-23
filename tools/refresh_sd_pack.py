#!/usr/bin/env python3
"""Refresh the tracked SD service ZIP and optionally apply it to a mounted card.

The existing ZIP contains the default wallpaper, sounds, help and core icons.
Repo-owned assets/ overrides keep newer icons and pages in sync with firmware.
Only paths in the service ZIP are written to a card; user data is left alone.
"""

import argparse
import os
from pathlib import Path
import re
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parent.parent
PACK = ROOT / "dist" / "kefyros_sd.zip"
EMPTY_DIRS = (
    "apps/",
    "kefyros/chats/",
    "kefyros/fonts/",
    "kefyros/games/planetx3/",
    "kefyros/music/",
    "kefyros/notes/",
    "kefyros/roms/gb/",
    "kefyros/saves/gb/",
    "kefyros/saves/planetx3/",
    "kefyros/spineko/",
)


def safe_name(name: str) -> bool:
    parts = Path(name.replace("\\", "/")).parts
    return bool(parts) and not name.startswith("/") and ".." not in parts and ":" not in name


def make_pack() -> dict[str, bytes]:
    if not PACK.is_file():
        raise SystemExit(f"Missing base service pack: {PACK}")
    with zipfile.ZipFile(PACK) as src:
        if src.testzip() is not None:
            raise SystemExit("Base service pack failed ZIP CRC validation")
        files = {name: src.read(name) for name in src.namelist() if not name.endswith("/")}
    if any(not safe_name(name) for name in files):
        raise SystemExit("Unsafe path in base service pack")

    for asset_dir, target_dir in (("icons", "kefyros/icons"), ("help", "kefyros/help")):
        source = ROOT / "assets" / asset_dir
        for path in source.rglob("*"):
            if path.is_file():
                name = f"{target_dir}/{path.relative_to(source).as_posix()}"
                files[name] = path.read_bytes()

    launcher = (ROOT / "ui" / "launcher.c").read_text(encoding="utf-8")
    ids = re.findall(r'\{\s*"([a-z0-9_]+)",\s*"[^"]+",\s*app_', launcher)
    missing = [app_id for app_id in ids if f"kefyros/icons/{app_id}.png" not in files]
    if missing:
        raise SystemExit(f"Missing launcher icons: {', '.join(missing)}")

    firstrun = (ROOT / "ui" / "firstrun.c").read_text(encoding="utf-8")
    required = re.findall(r'"(/kefyros/[^"\n]+)"', firstrun.split("static const char *REQUIRED[]", 1)[1].split("};", 1)[0])
    missing = [name for name in required if name.lstrip("/") not in files]
    if missing:
        raise SystemExit(f"Missing boot-required assets: {', '.join(missing)}")

    fd, temp_name = tempfile.mkstemp(prefix="kefyros_sd_", suffix=".zip", dir=PACK.parent)
    os.close(fd)
    try:
        with zipfile.ZipFile(temp_name, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as out:
            for name in sorted(set(EMPTY_DIRS) | set(files)):
                if name.endswith("/"):
                    out.writestr(name, b"")
                else:
                    out.writestr(name, files[name])
        with zipfile.ZipFile(temp_name) as check:
            if check.testzip() is not None:
                raise SystemExit("Updated service pack failed ZIP CRC validation")
        os.replace(temp_name, PACK)
    finally:
        if os.path.exists(temp_name):
            os.unlink(temp_name)
    return files


def stage_card(card: Path, files: dict[str, bytes]) -> None:
    if not card.is_dir():
        raise SystemExit(f"Card root does not exist: {card}")
    changed = 0
    for name in EMPTY_DIRS:
        (card / name).mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        target = card / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_file() and target.read_bytes() == data:
            continue
        temp = target.with_name(target.name + ".kefyros-new")
        try:
            temp.write_bytes(data)
            os.replace(temp, target)
        finally:
            if temp.exists():
                temp.unlink()
        if target.read_bytes() != data:
            raise SystemExit(f"Card verification failed: {target}")
        changed += 1
    print(f"Staged {changed} changed service files onto {card}; personal files were untouched")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--card", type=Path, help="Mounted SD card root, e.g. E:\\")
    args = parser.parse_args()
    pack_files = make_pack()
    print(f"Updated {PACK} ({len(pack_files)} files)")
    if args.card:
        stage_card(args.card, pack_files)
