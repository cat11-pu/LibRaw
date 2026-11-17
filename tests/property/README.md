# LibRaw property tests

Two property suites over a synthetic DNG corpus, built against LibRaw
compiled with AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer
(`-fsanitize=address,undefined -fno-sanitize-recover=all`).

## Layout

- `gen/dng.py` - minimal DNG writer plus the independent reference decoder
  used for pixel comparison.
- `gen/make_corpus.py` - generates `valid/` (50 files) and `malformed/`
  (200 files) plus per-file mutation manifests with byte offsets.
- `fuzz_one.cpp` - malformed-input harness: `open_file` + `unpack` +
  `raw2image` + `dcraw_process`, all errors via return codes.
- `dump_raw.cpp` - dumps the unprocessed Bayer mosaic of a valid file.
- `run_malformed.py` - property runner: zero crashes / zero OOB / zero leaks,
  one sanitizer-instrumented process per file, 120 s timeout.
- `run_compare.py` - pixel-exact comparison of all 50 valid files against
  the reference decoder.
- `counterexamples/` - any failing input is copied here with its mutation
  offsets and the sanitizer diagnostic.

## Corpus

Valid: 50 uncompressed CFA DNGs, 8/16-bit, geometry 22x22 .. 1024x768
(including odd sizes), all four 2x2 CFA patterns, varied black levels.

Malformed: 200 files, four mutation classes in rotation (50 each), every
8th file gets a second combined mutation:

- `truncate` - file cut at a random offset (header, IFD, or pixel data)
- `misalign` - tag value-offsets redirected to odd / out-of-file / header
  positions, IFD entry count inflated
- `oversize` - width/height/rows-per-strip/byte-counts set to 0, 65535,
  2^20, 2^30 or 0xFFFFFFFF
- `compression` - illegal Compression (0, 6, 7, 8, 99, 32767, ...),
  BitsPerSample (0, 1, 3, 7, 9, 14, 24, 32, 64, ...) and Photometric values

Every mutation records the exact byte offset(s) it touched in
`malformed/manifest.json`, so any failure maps back to a file offset.

## Results (this checkout)

Malformed suite: **200/200 clean** - zero crashes, zero out-of-bounds,
zero leaks. LibRaw rejected 87 files at open, 26 at open with data-format
error, 51 at unpack, and fully decoded 36; all terminations were clean
error returns.

Pixel comparison: **50/50 files identical**, 5,471,296 pixels compared,
zero mismatches vs the independent reference decoder.

## Counterexample found and fixed

`counterexamples/malformed_065.dng` (kept as a regression artifact):
a `misalign` mutation redirected StripOffsets into the TIFF header, and
the resulting extreme pixel values triggered a signed integer overflow
(UBSan) in `ahd_interpolate_build_homogeneity_map()`
(`src/demosaic/ahd_demosaic.cpp:215`): `SQR()` of a short difference up to
65535 overflows `int`. Fixed by widening `abdiff`/`abeps` to
`unsigned long long` and computing the squares in `long long`.

## Reproduce

    python3 gen/make_corpus.py     # regenerate corpus
    python3 run_malformed.py       # 200-file property suite
    python3 run_compare.py         # 50-file pixel-exact comparison

Rebuild the instrumented library/harnesses with `build/compile.sh` +
`ar rcs build/libraw_san.a build/*.o` (excluding
`postprocessing_postprocessing_ph.o`, a no-postprocessing placeholder),
then link as in the git history of this directory.
