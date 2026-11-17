"""Minimal little-endian DNG (TIFF) writer plus independent reference decoder.

Used by the property tests: the writer produces the 50 valid files, the
decoder is the independent reference implementation for pixel comparison.
Only uncompressed (Compression=1) CFA DNGs with 8/16-bit samples are
supported, which is exactly the subset the generator emits.
"""
import struct

T_SUBFILE = 254
T_WIDTH = 256
T_HEIGHT = 257
T_BITS = 258
T_COMPRESSION = 259
T_PHOTOMETRIC = 262
T_MAKE = 271
T_MODEL = 272
T_STRIP_OFFSETS = 273
T_ORIENTATION = 274
T_SAMPLES_PER_PIXEL = 277
T_ROWS_PER_STRIP = 278
T_STRIP_BYTE_COUNTS = 279
T_PLANAR = 284
T_CFA_REPEAT = 33421
T_CFA_PATTERN = 33422
T_DNG_VERSION = 50706
T_DNG_BACKWARD = 50707
T_UNIQUE_MODEL = 50708
T_BLACK_LEVEL = 50714
T_WHITE_LEVEL = 50717
T_COLOR_MATRIX1 = 50721
T_AS_SHOT_NEUTRAL = 50728
T_CALIB_ILLUMINANT1 = 50778

TYPE_SIZES = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 10: 8}

CFA_PATTERNS = {
    "RGGB": [0, 1, 1, 2],
    "BGGR": [2, 1, 1, 0],
    "GRBG": [1, 0, 2, 1],
    "GBRG": [1, 2, 0, 1],
}


def _entry(tag, typ, count, value_bytes):
    if len(value_bytes) <= 4:
        return struct.pack("<HHI", tag, typ, count) + value_bytes.ljust(4, b"\0")
    return (tag, typ, count, value_bytes)


def write_dng(path, width, height, bits, cfa_name, black, white, pixels,
              make=b"PropertyTest", model=b"SynthCam"):
    """Write a valid uncompressed CFA DNG. pixels: row-major list of ints."""
    cfa = CFA_PATTERNS[cfa_name]
    entries = []
    entries.append(_entry(T_SUBFILE, 4, 1, struct.pack("<I", 0)))
    entries.append(_entry(T_WIDTH, 4, 1, struct.pack("<I", width)))
    entries.append(_entry(T_HEIGHT, 4, 1, struct.pack("<I", height)))
    entries.append(_entry(T_BITS, 3, 1, struct.pack("<H", bits)))
    entries.append(_entry(T_COMPRESSION, 3, 1, struct.pack("<H", 1)))
    entries.append(_entry(T_PHOTOMETRIC, 3, 1, struct.pack("<H", 32803)))
    entries.append(_entry(T_MAKE, 2, len(make) + 1, make + b"\0"))
    entries.append(_entry(T_MODEL, 2, len(model) + 1, model + b"\0"))
    entries.append((T_STRIP_OFFSETS, 4, 1, b"STRP"))
    entries.append(_entry(T_ORIENTATION, 3, 1, struct.pack("<H", 1)))
    entries.append(_entry(T_SAMPLES_PER_PIXEL, 3, 1, struct.pack("<H", 1)))
    entries.append(_entry(T_ROWS_PER_STRIP, 4, 1, struct.pack("<I", height)))
    nbytes = width * height * (bits // 8)
    entries.append(_entry(T_STRIP_BYTE_COUNTS, 4, 1, struct.pack("<I", nbytes)))
    entries.append(_entry(T_PLANAR, 3, 1, struct.pack("<H", 1)))
    entries.append(_entry(T_CFA_REPEAT, 3, 2, struct.pack("<HH", 2, 2)))
    entries.append(_entry(T_CFA_PATTERN, 1, 4, bytes(cfa)))
    entries.append(_entry(T_DNG_VERSION, 1, 4, bytes([1, 4, 0, 0])))
    entries.append(_entry(T_DNG_BACKWARD, 1, 4, bytes([1, 1, 0, 0])))
    uniq = b"SynthCam-" + cfa_name.encode()
    entries.append(_entry(T_UNIQUE_MODEL, 2, len(uniq) + 1, uniq + b"\0"))
    entries.append(_entry(T_BLACK_LEVEL, 3, 1, struct.pack("<H", black)))
    entries.append(_entry(T_WHITE_LEVEL, 3, 1, struct.pack("<H", white)))
    cm_bytes = b"".join(struct.pack("<iI", v, 1) for v in
                        [1, 0, 0, 0, 1, 0, 0, 0, 1])
    entries.append(_entry(T_COLOR_MATRIX1, 10, 9, cm_bytes))
    entries.append(_entry(T_AS_SHOT_NEUTRAL, 5, 3, struct.pack("<II", 1, 1) * 3))
    entries.append(_entry(T_CALIB_ILLUMINANT1, 3, 1, struct.pack("<H", 21)))

    entries.sort(key=lambda e: e[0] if isinstance(e, tuple)
                 else struct.unpack("<H", e[:2])[0])

    ifd_off = 8
    n = len(entries)
    data_off = ifd_off + 2 + 12 * n + 4
    out_of_line = b""
    packed = []
    for e in entries:
        if isinstance(e, bytes):
            packed.append(e)
            continue
        tag, typ, count, vb = e
        if vb == b"STRP":
            packed.append((tag, typ, count, "STRIP"))
            continue
        off = data_off + len(out_of_line)
        packed.append(struct.pack("<HHII", tag, typ, count, off))
        out_of_line += vb
        if len(out_of_line) % 2:
            out_of_line += b"\0"

    strip_off = data_off + len(out_of_line)
    final = []
    for p in packed:
        if isinstance(p, tuple):
            tag, typ, count, _ = p
            final.append(struct.pack("<HHII", tag, typ, count, strip_off))
        else:
            final.append(p)

    if bits == 16:
        pix = b"".join(struct.pack("<H", v) for v in pixels)
    else:
        pix = bytes(pixels)

    with open(path, "wb") as f:
        f.write(b"II*\x00")
        f.write(struct.pack("<I", ifd_off))
        f.write(struct.pack("<H", n))
        for p in final:
            f.write(p)
        f.write(struct.pack("<I", 0))
        f.write(out_of_line)
        f.write(pix)
    return strip_off


def read_dng(path):
    """Independent reference decoder. Returns (width, height, cfa, pixels)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"II*\x00":
        raise ValueError("not a little-endian TIFF")
    ifd_off = struct.unpack_from("<I", data, 4)[0]
    n = struct.unpack_from("<H", data, ifd_off)[0]
    tags = {}
    for i in range(n):
        off = ifd_pos = ifd_off + 2 + 12 * i
        tag, typ, count = struct.unpack_from("<HHI", data, off)
        size = TYPE_SIZES[typ] * count
        if size <= 4:
            raw = data[off + 8:off + 8 + size]
        else:
            voff = struct.unpack_from("<I", data, off + 8)[0]
            raw = data[voff:voff + size]
        tags[tag] = (typ, count, raw)

    def u16(tag, idx=0):
        return struct.unpack_from("<H", tags[tag][2], idx * 2)[0]

    def u32(tag, idx=0):
        if tags[tag][0] == 3:
            return u16(tag, idx)
        return struct.unpack_from("<I", tags[tag][2], idx * 4)[0]

    width = u32(T_WIDTH)
    height = u32(T_HEIGHT)
    bits = u16(T_BITS)
    comp = u16(T_COMPRESSION)
    if comp != 1:
        raise ValueError("reference decoder only handles uncompressed DNG")
    strip = u32(T_STRIP_OFFSETS)
    cfa = list(tags[T_CFA_PATTERN][2][:4])

    if bits == 16:
        count = width * height
        pixels = list(struct.unpack_from("<%dH" % count, data, strip))
    elif bits == 8:
        pixels = list(data[strip:strip + width * height])
    else:
        raise ValueError("unsupported bit depth %d" % bits)
    return width, height, cfa, pixels
