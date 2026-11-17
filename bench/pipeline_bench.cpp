// Benchmark harness for the raw preview pipeline:
//   unpack -> raw2image (row copy) -> scale_colors (white balance)
//          -> pre_interpolate -> lin_interpolate (demosaic)
// Usage:
//   pipeline_bench ref [reps]   - dump reference pixels, print timings
//   pipeline_bench cmp [reps]   - compare pixels vs reference, print timings
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "libraw/libraw.h"

static double now_ms()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static uint64_t xs_state;
class BenchRaw : public LibRaw
{
public:
  using LibRaw::lin_interpolate;
  using LibRaw::pre_interpolate;
  using LibRaw::scale_colors;
};

static uint64_t xs_next()
{
  xs_state ^= xs_state << 13;
  xs_state ^= xs_state >> 7;
  xs_state ^= xs_state << 17;
  return xs_state;
}

static std::vector<uint16_t> gen_raw(int w, int h)
{
  xs_state = 0x9e3779b97f4a7c15ULL ^ (uint64_t(w) << 32) ^ uint64_t(h);
  std::vector<uint16_t> raw(size_t(w) * h);
  for (int r = 0; r < h; r++)
    for (int c = 0; c < w; c++)
    {
      uint64_t v = 300 + (xs_next() % 14000) + uint64_t((r * 7 + c * 13) & 1023);
      raw[size_t(r) * w + c] = uint16_t(v > 16383 ? 16383 : v);
    }
  return raw;
}

struct Times
{
  double unpack = 0, raw2image = 0, scale_colors = 0, pre_interp = 0,
         lin_interp = 0, process = 0;
};

static void set_params(LibRaw &rp)
{
  rp.imgdata.params.user_mul[0] = 2.0f;
  rp.imgdata.params.user_mul[1] = 1.0f;
  rp.imgdata.params.user_mul[2] = 1.6f;
  rp.imgdata.params.user_mul[3] = 1.0f;
  rp.imgdata.params.user_qual = 0; // bilinear demosaic
  rp.imgdata.params.output_bps = 16;
  rp.imgdata.params.no_auto_bright = 1;
}

static int open_synth(LibRaw &rp, const uint16_t *raw, int w, int h,
                      unsigned char procflags)
{
  return rp.open_bayer((const unsigned char *)raw, unsigned(w * h * 2), w, h,
                       0, 0, 0, 0, procflags, LIBRAW_OPENBAYER_RGGB, 0, 0,
                       256);
}

// Manual stage-by-stage drive (mirrors dcraw_process for our params).
static bool run_stages(const uint16_t *raw, int w, int h, Times &t)
{
  BenchRaw rp;
  set_params(rp);
  if (open_synth(rp, raw, w, h, 0) != LIBRAW_SUCCESS)
    return false;
  double t0 = now_ms();
  if (rp.unpack() != LIBRAW_SUCCESS)
    return false;
  double t1 = now_ms();
  if (rp.raw2image_ex(1) != LIBRAW_SUCCESS)
    return false;
  double t2 = now_ms();
  rp.adjust_maximum();
  rp.scale_colors();
  double t3 = now_ms();
  rp.pre_interpolate();
  double t4 = now_ms();
  rp.lin_interpolate();
  double t5 = now_ms();
  t.unpack = t1 - t0;
  t.raw2image = t2 - t1;
  t.scale_colors = t3 - t2;
  t.pre_interp = t4 - t3;
  t.lin_interp = t5 - t4;
  return true;
}

// Full dcraw_process; returns the final image in `out` when non-null.
static bool run_full(const uint16_t *raw, int w, int h, Times &t,
                     std::vector<uint16_t> *out, int variant)
{
  LibRaw rp;
  set_params(rp);
  if (open_synth(rp, raw, w, h, variant == 1 ? 2 : 0) != LIBRAW_SUCCESS)
    return false;
  if (variant == 1)
  {
    // 2D black-level pattern: exercises the cblack[4]/cblack[5] branch.
    rp.imgdata.color.cblack[4] = 2;
    rp.imgdata.color.cblack[5] = 4;
    for (int i = 0; i < 8; i++)
      rp.imgdata.color.cblack[6 + i] = (i * 7) % 9;
  }
  double t0 = now_ms();
  if (rp.unpack() != LIBRAW_SUCCESS)
    return false;
  if (rp.dcraw_process() != LIBRAW_SUCCESS)
    return false;
  double t1 = now_ms();
  t.process = t1 - t0;
  if (out)
  {
    size_t n = size_t(rp.imgdata.sizes.iwidth) * rp.imgdata.sizes.iheight * 4;
    out->resize(n + 2);
    (*out)[0] = uint16_t(rp.imgdata.sizes.iwidth);
    (*out)[1] = uint16_t(rp.imgdata.sizes.iheight);
    memcpy(out->data() + 2, rp.imgdata.image, n * 2);
  }
  return true;
}

// Manual drive with per-channel cblack set right before scale_colors:
// exercises the cblack[0..3] branch of scale_colors_loop.
static bool run_cblack_branch(const uint16_t *raw, int w, int h,
                              std::vector<uint16_t> *out)
{
  BenchRaw rp;
  set_params(rp);
  if (open_synth(rp, raw, w, h, 0) != LIBRAW_SUCCESS)
    return false;
  if (rp.unpack() != LIBRAW_SUCCESS)
    return false;
  if (rp.raw2image_ex(1) != LIBRAW_SUCCESS)
    return false;
  rp.imgdata.color.cblack[0] = 7;
  rp.imgdata.color.cblack[1] = 3;
  rp.imgdata.color.cblack[2] = 5;
  rp.imgdata.color.cblack[3] = 2;
  rp.adjust_maximum();
  rp.scale_colors();
  rp.pre_interpolate();
  rp.lin_interpolate();
  if (out)
  {
    size_t n = size_t(rp.imgdata.sizes.iwidth) * rp.imgdata.sizes.iheight * 4;
    out->resize(n);
    memcpy(out->data(), rp.imgdata.image, n * 2);
  }
  return true;
}

static bool write_file(const char *path, const std::vector<uint16_t> &data)
{
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  fwrite(data.data(), 2, data.size(), f);
  fclose(f);
  return true;
}

static bool read_file(const char *path, std::vector<uint16_t> &data)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  data.resize(sz / 2);
  size_t got = fread(data.data(), 2, data.size(), f);
  fclose(f);
  return got == data.size();
}

static int compare_pixels(const char *tag, const std::vector<uint16_t> &got,
                          const std::vector<uint16_t> &ref)
{
  if (got.size() != ref.size())
  {
    printf("  %-14s FAIL size mismatch: got %zu ref %zu\n", tag, got.size(),
           ref.size());
    return 1;
  }
  size_t ndiff = 0;
  int maxdiff = 0;
  size_t first = 0;
  for (size_t i = 0; i < got.size(); i++)
    if (got[i] != ref[i])
    {
      int d = got[i] > ref[i] ? got[i] - ref[i] : ref[i] - got[i];
      if (d > maxdiff)
        maxdiff = d;
      if (!ndiff)
        first = i;
      ndiff++;
    }
  if (ndiff)
  {
    printf("  %-14s FAIL diff-pixels=%zu/%zu maxdiff=%d first@%zu\n", tag,
           ndiff, got.size(), maxdiff, first);
    return 1;
  }
  printf("  %-14s OK identical (%zu values)\n", tag, got.size());
  return 0;
}

struct Stat
{
  double mn = 1e30, sum = 0;
  int n = 0;
  void add(double v)
  {
    if (v < mn)
      mn = v;
    sum += v;
    n++;
  }
  double avg() const { return n ? sum / n : 0; }
};

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    printf("usage: %s ref|cmp [reps]\n", argv[0]);
    return 1;
  }
  bool cmp = !strcmp(argv[1], "cmp");
  int reps = argc > 2 ? atoi(argv[2]) : 3;
  const int sizes[3][2] = {{4000, 3000}, {6000, 4000}, {8000, 6000}};
  const char *names[3] = {"12MP", "24MP", "48MP"};
  int fails = 0;

  for (int s = 0; s < 3; s++)
  {
    int w = sizes[s][0], h = sizes[s][1];
    std::vector<uint16_t> raw = gen_raw(w, h);
    Stat unpack, raw2image, scale_colors, pre_interp, lin_interp, stages,
        process;
    std::vector<uint16_t> img;
    for (int r = 0; r < reps; r++)
    {
      Times t;
      if (!run_stages(raw.data(), w, h, t))
        return 2;
      unpack.add(t.unpack);
      raw2image.add(t.raw2image);
      scale_colors.add(t.scale_colors);
      pre_interp.add(t.pre_interp);
      lin_interp.add(t.lin_interp);
      stages.add(t.raw2image + t.scale_colors + t.pre_interp + t.lin_interp);
      Times tf;
      img.clear();
      if (!run_full(raw.data(), w, h, tf, &img, 0))
        return 3;
      process.add(tf.process);
    }
    printf("== %s %dx%d ==\n", names[s], w, h);
    printf("  %-16s %9s %9s\n", "stage", "min(ms)", "mean(ms)");
    printf("  %-16s %9.2f %9.2f\n", "unpack", unpack.mn, unpack.avg());
    printf("  %-16s %9.2f %9.2f\n", "raw2image", raw2image.mn, raw2image.avg());
    printf("  %-16s %9.2f %9.2f\n", "scale_colors", scale_colors.mn,
           scale_colors.avg());
    printf("  %-16s %9.2f %9.2f\n", "pre_interpolate", pre_interp.mn,
           pre_interp.avg());
    printf("  %-16s %9.2f %9.2f\n", "lin_interpolate", lin_interp.mn,
           lin_interp.avg());
    printf("  %-16s %9.2f %9.2f\n", "stages_total", stages.mn, stages.avg());
    printf("  %-16s %9.2f %9.2f\n", "dcraw_process", process.mn, process.avg());

    char path[256];
    snprintf(path, sizeof path, "artifacts/ref_%dx%d.bin", w, h);
    if (cmp)
    {
      std::vector<uint16_t> ref;
      if (!read_file(path, ref))
        return 4;
      char tag[64];
      snprintf(tag, sizeof tag, "pixels_%s", names[s]);
      fails += compare_pixels(tag, img, ref);
    }
    else if (!write_file(path, img))
      return 5;
  }

  // Variant coverage at 24MP: 2D black pattern (branch 1) and per-channel
  // cblack (branch 2) of scale_colors_loop.
  {
    int w = 6000, h = 4000;
    std::vector<uint16_t> raw = gen_raw(w, h);
    std::vector<uint16_t> img;
    Times t;
    if (!run_full(raw.data(), w, h, t, &img, 1))
      return 6;
    const char *path = "artifacts/ref_variant2d.bin";
    if (cmp)
    {
      std::vector<uint16_t> ref;
      if (!read_file(path, ref))
        return 4;
      fails += compare_pixels("variant_2dblk", img, ref);
    }
    else if (!write_file(path, img))
      return 5;
  }
  {
    int w = 6000, h = 4000;
    std::vector<uint16_t> raw = gen_raw(w, h);
    std::vector<uint16_t> img;
    if (!run_cblack_branch(raw.data(), w, h, &img))
      return 7;
    const char *path = "artifacts/ref_variantcblack.bin";
    if (cmp)
    {
      std::vector<uint16_t> ref;
      if (!read_file(path, ref))
        return 4;
      fails += compare_pixels("variant_cblack", img, ref);
    }
    else if (!write_file(path, img))
      return 5;
  }

  printf(cmp ? (fails ? "RESULT: FAIL\n" : "RESULT: PASS (pixel-identical)\n")
             : "reference written\n");
  return fails ? 1 : 0;
}
