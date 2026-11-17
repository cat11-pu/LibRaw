// Property-test harness for malformed inputs.
// Usage: fuzz_one <file>
// Contract: whatever the input, this program must
//   - never crash (sanitizers abort the process -> runner flags it)
//   - never read/write out of bounds (ASan/UBSan)
//   - never leak (LSan at exit)
// It always exits 0 on ordinary LibRaw errors; nonzero exits come only from
// sanitizer aborts or uncaught exceptions, both of which are test failures.
#include <cstdio>
#include <exception>
#include "libraw/libraw.h"

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: fuzz_one <file>\n");
        return 2;
    }
    int ret_open = LIBRAW_UNSPECIFIED_ERROR;
    int ret_unpack = LIBRAW_UNSPECIFIED_ERROR;
    int ret_process = LIBRAW_UNSPECIFIED_ERROR;
    try
    {
        LibRaw proc;
        ret_open = proc.open_file(argv[1]);
        if (ret_open == LIBRAW_SUCCESS)
        {
            ret_unpack = proc.unpack();
            if (ret_unpack == LIBRAW_SUCCESS)
            {
                // keep post-processing bounded: skip absurd geometries that
                // only exist because a mutation inflated the dimensions
                unsigned long long px =
                    (unsigned long long)proc.imgdata.sizes.raw_width *
                    proc.imgdata.sizes.raw_height;
                if (px > 0 && px < 40000000ULL)
                {
                    proc.raw2image();
                    ret_process = proc.dcraw_process();
                }
            }
        }
        proc.recycle();
    }
    catch (const std::exception &e)
    {
        // LibRaw must report errors via return codes, not exceptions
        fprintf(stderr, "HARNESS-FAIL exception: %s\n", e.what());
        return 3;
    }
    catch (...)
    {
        fprintf(stderr, "HARNESS-FAIL unknown exception\n");
        return 3;
    }
    printf("RESULT open=%d unpack=%d process=%d\n", ret_open, ret_unpack,
           ret_process);
    return 0;
}
