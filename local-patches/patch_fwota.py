# STACKCHAN-LOCAL-PATCH fwota (24-Oct-2026, Mochi firmware v19): load_avatar_set mode "firmware" stages an app image
# (firmware-bin\mochi\xiaozhi.bin) like an avatar set; the robot streams it into her spare OTA slot over Wi-Fi, checks
# its sha256 and restarts into it (rollback if it never reaches the gateway). Apply AFTER patch_rle.py. Idempotent.
import pathlib, shutil, sys

TAG = "STACKCHAN-LOCAL-PATCH fwota"
EDITS = {
    "capture_server.py": [
        ('    if mode not in ("layered", "matrix", "panel", "matrix_rle"):  # STACKCHAN-LOCAL-PATCH panel rle',
         '    if mode not in ("layered", "matrix", "panel", "matrix_rle", "firmware"):  # STACKCHAN-LOCAL-PATCH panel rle fwota'),
    ],
    "stdio_server.py": [
        ('        if mode not in ("layered", "matrix", "panel", "matrix_rle"):  # STACKCHAN-LOCAL-PATCH panel rle',
         '        if mode not in ("layered", "matrix", "panel", "matrix_rle", "firmware"):  # STACKCHAN-LOCAL-PATCH panel rle fwota'),
        ('                            "enum": ["layered", "matrix", "panel", "matrix_rle"],  # STACKCHAN-LOCAL-PATCH panel rle',
         '                            "enum": ["layered", "matrix", "panel", "matrix_rle", "firmware"],  # STACKCHAN-LOCAL-PATCH panel rle fwota'),
    ],
    "gateway.py": [
        ('            "matrix_rle": len(payload),   # STACKCHAN-LOCAL-PATCH rle (checked above)\n',
         '            "matrix_rle": len(payload),   # STACKCHAN-LOCAL-PATCH rle (checked above)\n'
         '            "firmware": len(payload) if payload[:1] == b"\\xe9" and 65536 <= len(payload) <= 0x3F0000 else -1,  # STACKCHAN-LOCAL-PATCH fwota: an ESP app image\n'),
    ],
}


def main(pkg: pathlib.Path) -> bool:
    ok = True
    for name, edits in EDITS.items():
        p = pkg / name
        s = p.read_text(encoding="utf-8")
        if TAG in s:
            print(f"  already patched: {name} (fwota)"); continue
        new_s = s
        for old, new in edits:
            if new_s.count(old) != 1:
                print(f"  ERROR: anchor not found in {name} (fwota) - run patch_rle.py first"); ok = False; break
            new_s = new_s.replace(old, new)
        else:
            shutil.copy2(p, p.with_suffix(".py.fwota.orig"))
            p.write_text(new_s, encoding="utf-8")
            print(f"  patched: {name} (fwota)")
    return ok


if __name__ == "__main__":
    if len(sys.argv) > 1:
        pkg = pathlib.Path(sys.argv[1])
    else:
        import stackchan_mcp
        pkg = pathlib.Path(stackchan_mcp.__file__).parent
    sys.exit(0 if main(pkg) else 1)
