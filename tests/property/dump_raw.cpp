// Dump the unprocessed Bayer mosaic of a valid raw file for pixel-exact
// comparison against the independent reference decoder.
// Usage: dump_raw <input.dng> <output.dump>
// Output format: "RWD1", u32 raw_width, u32 raw_height, then
// raw_width*raw_height little-endian u16 samples (row-major).
#include <cstdio>
#include <cstdlib>
#include "libraw/libraw.h"

static void put_u32(FILE *f, unsigned v)
{
    unsigned char b[4] = {(unsigned char)(v & 0xff),
                          (unsigned char)((v >> 8) & 0xff),
                          (unsigned char)((v >> 16) & 0xff),
                          (unsigned char)((v >> 24) & 0xff)};
    fwrite(b, 1, 4, f);
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: dump_raw <in> <out>\n");
        return 2;
    }
    LibRaw proc;
    int ret = proc.open_file(argv[1]);
    if (ret != LIBRAW_SUCCESS)
    {
        fprintf(stderr, "open: %s\n", libraw_strerror(ret));
        return 1;
    }
    ret = proc.unpack();
    if (ret != LIBRAW_SUCCESS)
    {
        fprintf(stderr, "unpack: %s\n", libraw_strerror(ret));
        return 1;
    }
    libraw_rawdata_t *rd = &proc.imgdata.rawdata;
    unsigned w = proc.imgdata.sizes.raw_width;
    unsigned h = proc.imgdata.sizes.raw_height;
    if (!rd->raw_image || !w || !h)
    {
        fprintf(stderr, "no raw mosaic (raw_image=%p %ux%u)\n",
                (void *)rd->raw_image, w, h);
        return 1;
    }
    FILE *out = fopen(argv[2], "wb");
    if (!out)
    {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }
    fwrite("RWD1", 1, 4, out);
    put_u32(out, w);
    put_u32(out, h);
    for (unsigned long long i = 0; i < (unsigned long long)w * h; i++)
    {
        unsigned short v = rd->raw_image[i];
        unsigned char b[2] = {(unsigned char)(v & 0xff),
                              (unsigned char)((v >> 8) & 0xff)};
        fwrite(b, 1, 2, out);
    }
    fclose(out);
    proc.recycle();
    return 0;
}
