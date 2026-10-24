# STACKCHAN-LOCAL-PATCH rle2 (24-Oct-2026): the gateway also accepts "RLE2" face sets (local-patches/rle565.py: frames
# copy unchanged pixels from frame 0 / the previous frame, ~10-30x smaller than RLE1). Needs Mochi firmware v18 to
# decode them. Apply AFTER patch_rle.py (it edits that patch's line). Idempotent.
import pathlib, shutil, sys

TAG = "RLE2: Mochi v18"
OLD = 'if payload[:4] != b"RLE1" or int.from_bytes(payload[4:8], "little") != 90 * kimg_bytes:'
NEW = 'if payload[:4] not in (b"RLE1", b"RLE2") or int.from_bytes(payload[4:8], "little") != 90 * kimg_bytes:  # RLE2: Mochi v18'


def main(pkg: pathlib.Path) -> bool:
    p = pkg / "gateway.py"
    s = p.read_text(encoding="utf-8")
    if TAG in s:
        print("  already patched: gateway.py (rle2)"); return True
    if s.count(OLD) != 1:
        print("  ERROR: anchor not found in gateway.py (rle2) - run patch_rle.py first"); return False
    shutil.copy2(p, p.with_suffix(".py.rle2.orig"))
    p.write_text(s.replace(OLD, NEW), encoding="utf-8")
    print("  patched: gateway.py (rle2)")
    return True


if __name__ == "__main__":
    if len(sys.argv) > 1:
        pkg = pathlib.Path(sys.argv[1])
    else:
        import stackchan_mcp
        pkg = pathlib.Path(stackchan_mcp.__file__).parent
    sys.exit(0 if main(pkg) else 1)
