# STACKCHAN-LOCAL-PATCH rle: load_avatar_set mode "matrix_rle" = run-length coded face set (faces\*.rle,
# made by faces\rle565.py, ~75x smaller, loads in ~1 s instead of ~27 s). Needs Mochi firmware v5.
# Apply AFTER patch_panel.py. Idempotent.
import pathlib, shutil, sys

TAG = "STACKCHAN-LOCAL-PATCH rle"
EDITS = {
    "capture_server.py": [
        ('    if mode not in ("layered", "matrix", "panel"):  # STACKCHAN-LOCAL-PATCH panel',
         '    if mode not in ("layered", "matrix", "panel", "matrix_rle"):  # STACKCHAN-LOCAL-PATCH panel rle'),
    ],
    "stdio_server.py": [
        ('        if mode not in ("layered", "matrix", "panel"):  # STACKCHAN-LOCAL-PATCH panel',
         '        if mode not in ("layered", "matrix", "panel", "matrix_rle"):  # STACKCHAN-LOCAL-PATCH panel rle'),
        ('                            "enum": ["layered", "matrix", "panel"],  # STACKCHAN-LOCAL-PATCH panel',
         '                            "enum": ["layered", "matrix", "panel", "matrix_rle"],  # STACKCHAN-LOCAL-PATCH panel rle'),
    ],
    "gateway.py": [
        ('        kimg_bytes = 160 * 120 * 2  # 38_400 — matches AvatarSet::kImageBytes\n',
         '        kimg_bytes = 160 * 120 * 2  # 38_400 — matches AvatarSet::kImageBytes\n'
         '        if mode == "matrix_rle":  # STACKCHAN-LOCAL-PATCH rle: header "RLE1" + raw size of a matrix set\n'
         '            if payload[:4] != b"RLE1" or int.from_bytes(payload[4:8], "little") != 90 * kimg_bytes:\n'
         '                return {"ok": False, "error": "bad_rle_payload"}\n'),
        ('            "panel":   320 * 240 * 2,     # 153_600  STACKCHAN-LOCAL-PATCH panel\n        }.get(mode)',
         '            "panel":   320 * 240 * 2,     # 153_600  STACKCHAN-LOCAL-PATCH panel\n'
         '            "matrix_rle": len(payload),   # STACKCHAN-LOCAL-PATCH rle (checked above)\n        }.get(mode)'),
    ],
}


def main(pkg: pathlib.Path) -> bool:
    ok = True
    for name, edits in EDITS.items():
        p = pkg / name
        s = p.read_text(encoding="utf-8")
        if TAG in s:
            print(f"  already patched: {name} (rle)"); continue
        new_s = s
        for old, new in edits:
            if new_s.count(old) != 1:
                print(f"  ERROR: anchor not found in {name} (rle) - run patch_panel.py first"); ok = False; break
            new_s = new_s.replace(old, new)
        else:
            shutil.copy2(p, p.with_suffix(".py.rle.orig"))
            p.write_text(new_s, encoding="utf-8")
            print(f"  patched: {name} (rle)")
    return ok


if __name__ == "__main__":
    if len(sys.argv) > 1:
        pkg = pathlib.Path(sys.argv[1])
    else:
        import stackchan_mcp
        pkg = pathlib.Path(stackchan_mcp.__file__).parent
    sys.exit(0 if main(pkg) else 1)
