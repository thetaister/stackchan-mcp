"""Run-length codec for StackChan face sets (RGB565 LE frames). Format "RLE1":
header b"RLE1" + uint32 LE raw size, then tokens: uint16 LE ctrl;
ctrl & 0x8000 -> repeat the next uint16 pixel (ctrl & 0x7FFF) times; else ctrl literal pixels follow.
"""
import struct, sys
from pathlib import Path

MAGIC = b"RLE1"


def encode(raw: bytes) -> bytes:
    px = struct.unpack(f"<{len(raw)//2}H", raw)
    out = bytearray(MAGIC + struct.pack("<I", len(raw)))
    i, n = 0, len(px)
    lit = []
    def flush():
        while lit:
            chunk = lit[:0x7FFF]; del lit[:0x7FFF]
            out.extend(struct.pack("<H", len(chunk))); out.extend(struct.pack(f"<{len(chunk)}H", *chunk))
    while i < n:
        j = i
        while j < n and px[j] == px[i] and j - i < 0x7FFF:
            j += 1
        run = j - i
        if run >= 3:
            flush()
            out.extend(struct.pack("<HH", 0x8000 | run, px[i]))
        else:
            lit.extend(px[i:j])
        i = j
    flush()
    return bytes(out)


def decode(data: bytes) -> bytes:
    assert data[:4] == MAGIC
    raw_size = struct.unpack_from("<I", data, 4)[0]
    out = bytearray(); i = 8
    while i < len(data):
        ctrl = struct.unpack_from("<H", data, i)[0]; i += 2
        if ctrl & 0x8000:
            out += data[i:i + 2] * (ctrl & 0x7FFF); i += 2
        else:
            out += data[i:i + 2 * ctrl]; i += 2 * ctrl
    assert len(out) == raw_size, (len(out), raw_size)
    return bytes(out)


if __name__ == "__main__":
    src = Path(sys.argv[1]); dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix(".rle")
    raw = src.read_bytes(); enc = encode(raw)
    assert decode(enc) == raw
    dst.write_bytes(enc)
    print(f"{src.name}: {len(raw):,} -> {len(enc):,} bytes ({len(raw)/len(enc):.1f}x) -> {dst}")
