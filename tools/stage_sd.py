#!/usr/bin/env python3
# Stage the PicoCalc SD card (mounted at /mnt/e on this PC) with art the firmware
# loads at runtime: wallpapers (aspect-preserving downscale to a panel-friendly
# RGB565 .bin) + app icons + a notes dir. NOTHING is baked into the firmware.
import os, sys, struct
from PIL import Image

def cover_crop(im, w, h):
    """Scale to cover (aspect preserved) then center-crop to exactly w x h, so the
    runtime renders it 1:1 (no per-frame software transform = fast)."""
    sw, sh = im.size
    scale = max(w / sw, h / sh)
    nw, nh = int(sw * scale + 0.5), int(sh * scale + 0.5)
    im = im.resize((nw, nh), Image.LANCZOS)
    ox, oy = (nw - w) // 2, (nh - h) // 2
    return im.crop((ox, oy, ox + w, oy + h))

def write_lvgl_rgb565_bin(im, path):
    """Emit a LVGL v9 binary image: 12-byte lv_image_header_t + LE RGB565 data."""
    im = im.convert("RGB")
    w, h = im.size
    hdr = struct.pack("<BBHHHHH", 0x19, 0x12, 0, w, h, w * 2, 0)  # magic,cf,flags,w,h,stride,rsv
    rgb = im.tobytes()  # RGB888, row-major
    out = bytearray(hdr)
    out += bytearray(w * h * 2)
    o = 0
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out[12 + o] = v & 0xFF
        out[13 + o] = (v >> 8) & 0xFF
        o += 2
    open(path, "wb").write(out)

SD       = "/mnt/e/kefyros"
WALLS    = os.path.join(SD, "wallpapers")
ICONS    = os.path.join(SD, "icons")
NOTES    = os.path.join(SD, "notes")
GBROMS   = os.path.join(SD, "roms", "gb")
GBSAVES  = os.path.join(SD, "saves", "gb")
DL       = "/mnt/c/Users/Matas/Downloads"
LVGLIMG  = "/home/legitbox/kefyros-pico/lib/lvgl/scripts/LVGLImage.py"
TMP      = "/tmp/kf_stage"

for d in (WALLS, ICONS, NOTES, GBROMS, GBSAVES, TMP):
    os.makedirs(d, exist_ok=True)

# ---- wallpapers: aspect-preserving 'contain' fit into 320x320, then RGB565 .bin ----
# single wallpaper, cover-cropped to exactly 320x320 (fills the screen, 1:1, fast)
for f in os.listdir(WALLS):                       # remove any previous wallpapers
    os.remove(os.path.join(WALLS, f))
WP = [
    ("HDdXYOIW8AA1Myb.jpg", "wall"),
]
for src, name in WP:
    p = os.path.join(DL, src)
    if not os.path.exists(p):
        sys.stderr.write("MISSING wallpaper %s\n" % p); continue
    im = Image.open(p).convert("RGB")
    w0, h0 = im.size
    im = cover_crop(im, 320, 320)                 # aspect-preserving cover + center crop
    out_bin = os.path.join(WALLS, name + ".bin")
    write_lvgl_rgb565_bin(im, out_bin)
    sys.stderr.write("wallpaper %-9s %dx%d -> %dx%d  -> %s.bin (%d bytes)\n" %
                     (name, w0, h0, im.size[0], im.size[1], name, os.path.getsize(out_bin)))

# ---- app icons: 48x48 PNGs, named by the launcher's app id ----
ICON_MAP = {
    "calc": "calculator.png", "files": "files.png", "wifi": "wifi.png",
    "settings": "settings.png", "appearance": "wallpaper.png",
    "editor": "editor.png", "electronics": "resistor.png",
    "music": "music.png",     # FLAC player tile (recursively indexes /kefyros/music)
    "gb": "gb.png",           # Game Boy / Game Boy Color emulator
    "bluetooth": "bt.png",   # Bluetooth audio manager (kept in repo)
}
import shutil
for appid, fn in ICON_MAP.items():
    src = (os.path.join(os.path.dirname(__file__), "..", "assets", "icons", "bluetooth.png" if appid == "bluetooth" else "gb.png")
           if appid in ("bluetooth", "gb") else os.path.join(DL, fn))
    if not os.path.exists(src):
        sys.stderr.write("MISSING icon %s\n" % src); continue
    shutil.copyfile(src, os.path.join(ICONS, appid + ".png"))
    sys.stderr.write("icon %-11s <- %s\n" % (appid, fn))

# ---- a welcome note so /kefyros/notes isn't empty ----
with open(os.path.join(NOTES, "welcome.txt"), "w") as f:
    f.write("Welcome to Kefyros on the PicoCalc.\nNotes live here on the SD card.\n")

sys.stderr.write("\n=== SD contents ===\n")
for root, _, files in os.walk(SD):
    for fl in files:
        fp = os.path.join(root, fl)
        sys.stderr.write("  %-44s %d bytes\n" % (fp.replace(SD, "/kefyros"), os.path.getsize(fp)))
