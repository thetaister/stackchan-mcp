"""Run-length codecs for StackChan face sets (RGB565 LE frames).

"RLE1": header b"RLE1" + uint32 LE raw size, then tokens: uint16 LE ctrl;
  ctrl & 0x8000 -> repeat the next uint16 pixel (ctrl & 0x7FFF) times; else ctrl literal pixels follow.
"RLE2" (Mochi firmware v18, 24-Oct-2026): header b"RLE2" + uint32 LE raw size + uint32 LE frame size (bytes),
  then tokens: uint16 LE ctrl, kind = ctrl >> 14, n = ctrl & 0x3FFF pixels:
    0 literal (n pixels follow)   1 repeat (the next uint16 pixel, n times)
    2 copy n pixels from the same place in the previous frame
    3 copy n pixels from the same place in frame 0 (never past the end of that frame)
  Every frame of a face set shares its background with frame 0 and most of its face with the frame before,
  so a set shrinks several times over (Tai 24-Oct: 'psram_oom' loading TV).
Usage: python rle565.py <set>.rgb565 [<out>.rle] [--v1]   (RLE2 unless --v1; the output is checked by decoding)
"""
import struct, sys
from pathlib import Path

MAGIC = b"RLE1"
MAGIC2 = b"RLE2"
FRAME_BYTES = 160 * 120 * 2


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
        if j - i >= 3:
            flush()
            out.extend(struct.pack("<HH", 0x8000 | (j - i), px[i]))
            i = j
        else:
            lit.append(px[i]); i += 1
    flush()
    return bytes(out)


def _runs(b):
    """For a bool array: how many Trues start at each index (0 where False)."""
    import numpy as np
    b = b.astype(np.int32)   # position of the next False at or after i, minus i
    idx = np.arange(len(b))
    nxt = np.where(b == 0, idx, len(b))
    nxt = np.minimum.accumulate(nxt[::-1])[::-1]
    return (nxt - idx).astype(np.int64)


def encode2(raw: bytes, frame_bytes: int = FRAME_BYTES) -> bytes:
    import numpy as np
    px = np.frombuffer(raw, "<u2")
    n, F = len(px), frame_bytes // 2
    MAXN = 0x3FFF
    prev = np.zeros(n, bool); prev[F:] = px[F:] == px[:-F]
    f0 = np.zeros(n, bool); f0[F:] = px[F:] == np.tile(px[:F], n // F - 1)
    same = np.zeros(n, bool); same[:-1] = px[:-1] == px[1:]
    r_prev, r_f0, r_same = _runs(prev), _runs(f0), _runs(same) + 1
    pos_in = np.arange(n) % F
    r_f0 = np.minimum(r_f0, F - pos_in)                  # a frame-0 copy stays inside frame 0
    out = bytearray(MAGIC2 + struct.pack("<II", len(raw), frame_bytes))
    lit_start = -1
    def flush(end):
        nonlocal lit_start
        s = lit_start
        while s < end:
            m = min(MAXN, end - s)
            out.extend(struct.pack("<H", m)); out.extend(px[s:s + m].astype("<u2").tobytes()); s += m
        lit_start = -1
    i = 0
    while i < n:
        a, b, c = int(r_prev[i]), int(r_f0[i]), int(r_same[i])
        best = max(a, b)
        if best >= 2 and best >= c:
            if lit_start >= 0: flush(i)
            m = min(best, MAXN)
            out.extend(struct.pack("<H", (2 << 14 if a >= b else 3 << 14) | m)); i += m
        elif c >= 3:
            if lit_start >= 0: flush(i)
            m = min(c, MAXN)
            out.extend(struct.pack("<HH", (1 << 14) | m, int(px[i]))); i += m
        else:
            if lit_start < 0: lit_start = i
            i += 1
    if lit_start >= 0: flush(n)
    return bytes(out)


def decode(data: bytes) -> bytes:
    if data[:4] == MAGIC2:
        return decode2(data)
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


def decode2(data: bytes) -> bytes:
    """The firmware's RLE2 decoder, in Python (same checks)."""
    raw_size, F = struct.unpack_from("<II", data, 4)
    out = bytearray(raw_size); o = 0; i = 12
    while i < len(data):
        ctrl = struct.unpack_from("<H", data, i)[0]; i += 2
        kind, m = ctrl >> 14, (ctrl & 0x3FFF) * 2
        assert o + m <= raw_size
        if kind == 0:
            out[o:o + m] = data[i:i + m]; i += m
        elif kind == 1:
            out[o:o + m] = data[i:i + 2] * (m // 2); i += 2
        elif kind == 2:
            assert o >= F; out[o:o + m] = out[o - F:o - F + m]
        else:
            s = o % F; assert o >= F and s + m <= F; out[o:o + m] = out[s:s + m]
        o += m
    assert o == raw_size and i == len(data)
    return bytes(out)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    src = Path(args[0]); dst = Path(args[1]) if len(args) > 1 else src.with_suffix(".rle")
    raw = src.read_bytes()
    enc = encode(raw) if "--v1" in sys.argv else encode2(raw)
    assert decode(enc) == raw
    dst.write_bytes(enc)
    print(f"{src.name}: {len(raw):,} -> {len(enc):,} bytes ({len(raw)/len(enc):.1f}x, {enc[:4].decode()}) -> {dst}")
