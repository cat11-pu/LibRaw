"""Generate the property-test corpus.

valid/     50 well-formed uncompressed CFA DNGs (varied geometry, bit depth,
           CFA pattern, black/white levels).
malformed/ 200 corrupted DNGs, four mutation classes in rotation:
           0 truncation, 1 metadata misalignment, 2 oversized geometry,
           3 illegal compression. Every mutation records its file offset
           in manifest.json so failures can be mapped back to a byte.
"""
import json
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dng import write_dng

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VALID_DIR = os.path.join(ROOT, "valid")
MAL_DIR = os.path.join(ROOT, "malformed")

rng = random.Random(0xC0FFEE)

CFAS = ["RGGB", "BGGR", "GRBG", "GBRG"]


def synth_pixels(width, height, bits, seed):
    r = random.Random(seed)
    mx = (1 << bits) - 1
    px = []
    for y in range(height):
        for x in range(width):
            v = (x * 7 + y * 13 + r.randint(0, 31)) & mx
            px.append(v)
    return px


def make_valid_specs():
    specs = []
    # LibRaw requires raw_width/raw_height >= 22 to accept a file as raw
    dims = [(22, 22), (24, 23), (32, 32), (31, 27), (64, 48), (100, 23),
            (129, 65), (256, 192), (320, 240), (511, 257), (640, 480),
            (1024, 768)]
    for i in range(50):
        w, h = dims[i % len(dims)]
        bits = 16 if (i % 2 == 0) else 8
        cfa = CFAS[i % 4]
        black = [0, 16, 64, 512][i % 4] if bits == 16 else [0, 4, 16, 60][i % 4]
        white = (1 << bits) - 1
        specs.append(dict(width=w, height=h, bits=bits, cfa=cfa,
                          black=black, white=white, seed=1000 + i))
    return specs


def gen_valid():
    os.makedirs(VALID_DIR, exist_ok=True)
    manifest = []
    for i, s in enumerate(make_valid_specs()):
        name = "valid_%02d.dng" % i
        path = os.path.join(VALID_DIR, name)
        px = synth_pixels(s["width"], s["height"], s["bits"], s["seed"])
        write_dng(path, s["width"], s["height"], s["bits"], s["cfa"],
                  s["black"], s["white"], px)
        manifest.append(dict(file=name, **s))
    return manifest


def find_tag_offsets(data, wanted):
    """Return {tag: (entry_off, value_off, type, count)} for tags in wanted."""
    ifd_off = struct.unpack_from("<I", data, 4)[0]
    n = struct.unpack_from("<H", data, ifd_off)[0]
    out = {}
    sizes = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 10: 8}
    for i in range(n):
        off = ifd_off + 2 + 12 * i
        tag, typ, count = struct.unpack_from("<HHI", data, off)
        if tag in wanted:
            size = sizes.get(typ, 1) * count
            voff = off + 8 if size <= 4 else struct.unpack_from("<I", data, off + 8)[0]
            out[tag] = (off, voff, typ, count)
    return out


def mut_truncate(data, _tags, r):
    """Cut the file at a random offset."""
    choice = r.random()
    if choice < 0.3:
        cut = r.randrange(4, min(64, len(data)))
    elif choice < 0.7:
        cut = r.randrange(len(data) // 4, len(data))
    else:
        cut = r.randrange(1, len(data))
    return data[:cut], [dict(offset=cut, detail="truncate to %d of %d bytes"
                             % (cut, len(data)))]


def mut_misalign(data, tags, r):
    """Point metadata at misaligned or wild offsets."""
    offs = []
    out = bytearray(data)
    targets = [t for t in (273, 50714, 50717, 50721, 50728, 271, 272)
               if t in tags]
    r.shuffle(targets)
    for tag in targets[:3]:
        entry_off, voff, typ, count = tags[tag]
        mode = r.randrange(3)
        if mode == 0:
            newoff = r.randrange(1, len(data)) | 1
        elif mode == 1:
            newoff = len(data) + r.randrange(1, 1 << 20)
        else:
            newoff = r.randrange(0, 8)
        struct.pack_into("<I", out, entry_off + 8, newoff)
        offs.append(dict(offset=entry_off + 8, tag=tag,
                         detail="tag %d value-offset -> %d" % (tag, newoff)))
    if r.random() < 0.5:
        ifd_off = struct.unpack_from("<I", data, 4)[0]
        n = struct.unpack_from("<H", data, ifd_off)[0]
        new_n = n + r.choice([1, 7, 255, 4096])
        struct.pack_into("<H", out, ifd_off, new_n & 0xFFFF)
        offs.append(dict(offset=ifd_off, tag="IFDcount",
                         detail="IFD entry count %d -> %d" % (n, new_n & 0xFFFF)))
    return bytes(out), offs


def mut_oversize(data, tags, r):
    """Inflate geometry and counts to huge values."""
    out = bytearray(data)
    offs = []
    for tag, val in ((256, r.choice([0, 65535, 1 << 20, 0xFFFFFFFF])),
                     (257, r.choice([0, 65535, 1 << 20, 0xFFFFFFFF])),
                     (278, r.choice([0, 1 << 24, 0xFFFFFFFF])),
                     (279, r.choice([0, 1 << 30, 0xFFFFFFFF]))):
        if tag not in tags:
            continue
        entry_off, voff, typ, count = tags[tag]
        if typ == 4:
            struct.pack_into("<I", out, voff, val & 0xFFFFFFFF)
        else:
            struct.pack_into("<H", out, voff, val & 0xFFFF)
        offs.append(dict(offset=voff, tag=tag,
                         detail="tag %d -> %d" % (tag, val)))
    return bytes(out), offs


ILLEGAL_COMPS = [0, 6, 7, 8, 99, 32767, 32773, 34892, 65535]
ILLEGAL_BITS = [0, 1, 3, 7, 9, 14, 24, 32, 64, 65535]


def mut_compression(data, tags, r):
    """Illegal Compression / BitsPerSample / Photometric values."""
    out = bytearray(data)
    offs = []
    picks = [(259, r.choice(ILLEGAL_COMPS)), (258, r.choice(ILLEGAL_BITS))]
    if r.random() < 0.5:
        picks.append((262, r.choice([0, 1, 2, 5, 6, 65535])))
    for tag, val in picks:
        if tag not in tags:
            continue
        entry_off, voff, typ, count = tags[tag]
        struct.pack_into("<H", out, voff, val & 0xFFFF)
        offs.append(dict(offset=voff, tag=tag,
                         detail="tag %d -> %d" % (tag, val)))
    return bytes(out), offs


MUTATORS = [("truncate", mut_truncate), ("misalign", mut_misalign),
            ("oversize", mut_oversize), ("compression", mut_compression)]


def gen_malformed(valid_manifest):
    os.makedirs(MAL_DIR, exist_ok=True)
    manifest = []
    for i in range(200):
        base = valid_manifest[i % len(valid_manifest)]
        src = os.path.join(VALID_DIR, base["file"])
        with open(src, "rb") as f:
            data = f.read()
        tags = find_tag_offsets(data, {256, 257, 258, 259, 262, 273, 278, 279,
                                       271, 272, 50714, 50717, 50721, 50728})
        r = random.Random(7000 + i)
        mname, mfun = MUTATORS[i % 4]
        mutated, offsets = mfun(data, tags, r)
        if i % 8 == 7:
            mname2, mfun2 = MUTATORS[(i + 1) % 4]
            tags2 = find_tag_offsets(mutated, set(tags)) if mname != "truncate" \
                else (tags if len(mutated) == len(data) else {})
            try:
                mutated, off2 = mfun2(mutated, tags2, r)
                offsets += off2
                mname += "+" + mname2
            except Exception:
                pass
        name = "malformed_%03d.dng" % i
        with open(os.path.join(MAL_DIR, name), "wb") as f:
            f.write(mutated)
        manifest.append(dict(file=name, base=base["file"], mutation=mname,
                             size=len(mutated), offsets=offsets))
    return manifest


def main():
    vm = gen_valid()
    mm = gen_malformed(vm)
    with open(os.path.join(VALID_DIR, "manifest.json"), "w") as f:
        json.dump(vm, f, indent=1)
    with open(os.path.join(MAL_DIR, "manifest.json"), "w") as f:
        json.dump(mm, f, indent=1)
    print("valid: %d files, malformed: %d files" % (len(vm), len(mm)))


if __name__ == "__main__":
    main()
