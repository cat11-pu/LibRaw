#include "libraw/libraw.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct RawImage
{
  int width;
  int height;
  int bps;
  std::vector<unsigned char> bytes;
  std::vector<unsigned short> words;

  const unsigned char *data() const
  {
    return bps == 8 ? bytes.data()
                    : reinterpret_cast<const unsigned char *>(words.data());
  }

  size_t size() const { return bps == 8 ? bytes.size() : words.size() * 2; }
};

struct Bitmap
{
  int width = 0;
  int height = 0;
  int colors = 0;
  int bps = 0;
  std::vector<unsigned char> bytes;
};

static RawImage make_raw(int width, int height, int bps)
{
  RawImage raw;
  raw.width = width;
  raw.height = height;
  raw.bps = bps;
  if (bps == 8)
  {
    raw.bytes.resize((size_t)width * height);
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++)
        raw.bytes[(size_t)y * width + x] =
            (unsigned char)((x * 13 + y * 7 + ((x ^ y) & 15) + 32) & 255);
  }
  else
  {
    raw.words.resize((size_t)width * height);
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++)
        raw.words[(size_t)y * width + x] =
            (unsigned short)((x * 257 + y * 131 + ((x * y) & 63) + 512) &
                             4095);
  }
  return raw;
}

static void configure(LibRaw &processor, int bps)
{
  processor.imgdata.params.output_bps = bps;
  processor.imgdata.params.output_color = 1;
  processor.imgdata.params.use_camera_wb = 0;
  processor.imgdata.params.use_auto_wb = 0;
  processor.imgdata.params.no_auto_bright = 1;
  processor.imgdata.params.highlight = 0;
  processor.imgdata.params.user_mul[0] = 1.9f;
  processor.imgdata.params.user_mul[1] = 1.0f;
  processor.imgdata.params.user_mul[2] = 1.4f;
  processor.imgdata.params.user_mul[3] = 1.0f;
}

static int open_raw(LibRaw &processor, const RawImage &raw)
{
  configure(processor, raw.bps);
  return processor.open_bayer(raw.data(), (unsigned)raw.size(), raw.width,
                              raw.height, 0, 0, 0, 0, 0,
                              LIBRAW_OPENBAYER_RGGB, 0, 0, 0);
}

static Bitmap render_full(const RawImage &raw)
{
  LibRaw processor;
  int rc = open_raw(processor, raw);
  if (rc == LIBRAW_SUCCESS)
    rc = processor.unpack();
  if (rc == LIBRAW_SUCCESS)
    rc = processor.dcraw_process();
  if (rc != LIBRAW_SUCCESS)
  {
    std::printf("reference render failed: %d (%s)\n", rc,
                libraw_strerror(rc));
    std::exit(2);
  }

  int err = 0;
  libraw_processed_image_t *image = processor.dcraw_make_mem_image(&err);
  if (!image)
  {
    std::printf("reference copy failed: %d (%s)\n", err,
                libraw_strerror(err));
    std::exit(2);
  }

  Bitmap result;
  result.width = image->width;
  result.height = image->height;
  result.colors = image->colors;
  result.bps = image->bits;
  result.bytes.assign(image->data, image->data + image->data_size);
  processor.dcraw_clear_mem(image);
  return result;
}

template <typename T>
static INT64 overlap(INT64 a0, INT64 a1, INT64 b0, INT64 b1)
{
  INT64 lo = std::max(a0, b0);
  INT64 hi = std::min(a1, b1);
  return hi > lo ? hi - lo : 0;
}

template <typename T>
static void reference_resample(const T *src, int src_width, int src_height,
                               T *dst, unsigned preview_width,
                               unsigned preview_height, unsigned left,
                               unsigned top, unsigned crop_width,
                               unsigned crop_height, int colors)
{
  for (unsigned y = 0; y < crop_height; y++)
  {
    const unsigned py = top + y;
    const INT64 y0 = INT64(py) * src_height;
    const INT64 y1 = INT64(py + 1) * src_height;
    for (unsigned x = 0; x < crop_width; x++)
    {
      const unsigned px = left + x;
      const INT64 x0 = INT64(px) * src_width;
      const INT64 x1 = INT64(px + 1) * src_width;
      INT64 sums[4] = {0, 0, 0, 0};
      INT64 total_weight = 0;

      for (unsigned sy = unsigned(y0 / preview_height);
           sy < unsigned((y1 + preview_height - 1) / preview_height) &&
           sy < unsigned(src_height);
           sy++)
      {
        const INT64 oy = overlap<INT64>(y0, y1, INT64(sy) * preview_height,
                                        INT64(sy + 1) * preview_height);
        if (!oy)
          continue;
        for (unsigned sx = unsigned(x0 / preview_width);
             sx < unsigned((x1 + preview_width - 1) / preview_width) &&
             sx < unsigned(src_width);
             sx++)
        {
          const INT64 ox = overlap<INT64>(x0, x1, INT64(sx) * preview_width,
                                          INT64(sx + 1) * preview_width);
          if (!ox)
            continue;
          const INT64 weight = ox * oy;
          const T *sample = src + (INT64(sy) * src_width + sx) * colors;
          for (int c = 0; c < colors; c++)
            sums[c] += INT64(sample[c]) * weight;
          total_weight += weight;
        }
      }

      T *out = dst + (INT64(y) * crop_width + x) * colors;
      for (int c = 0; c < colors; c++)
        out[c] = T(total_weight ? (sums[c] + total_weight / 2) / total_weight
                                : 0);
    }
  }
}

static Bitmap reference_preview(const Bitmap &full,
                                const libraw_preview_t &preview)
{
  Bitmap result;
  result.width = preview.crop_width;
  result.height = preview.crop_height;
  result.colors = full.colors;
  result.bps = full.bps;
  const size_t sample_bytes = full.bps / 8;
  result.bytes.resize((size_t)result.width * result.height * result.colors *
                      sample_bytes);
  if (full.bps == 8)
    reference_resample(full.bytes.data(), full.width, full.height,
                       result.bytes.data(), preview.width, preview.height,
                       preview.crop_left, preview.crop_top, preview.crop_width,
                       preview.crop_height, full.colors);
  else
    reference_resample(reinterpret_cast<const unsigned short *>(
                           full.bytes.data()),
                       full.width, full.height,
                       reinterpret_cast<unsigned short *>(result.bytes.data()),
                       preview.width, preview.height, preview.crop_left,
                       preview.crop_top, preview.crop_width,
                       preview.crop_height, full.colors);
  return result;
}

static Bitmap direct_crop(const Bitmap &full, unsigned left, unsigned top,
                          unsigned width, unsigned height)
{
  Bitmap result;
  result.width = width;
  result.height = height;
  result.colors = full.colors;
  result.bps = full.bps;
  const size_t sample_bytes = full.bps / 8;
  const size_t src_row = (size_t)full.width * full.colors * sample_bytes;
  const size_t dst_row = (size_t)width * full.colors * sample_bytes;
  result.bytes.resize(dst_row * height);
  for (unsigned y = 0; y < height; y++)
    std::memcpy(&result.bytes[(size_t)y * dst_row],
                &full.bytes[(size_t)(top + y) * src_row +
                            (size_t)left * full.colors * sample_bytes],
                dst_row);
  return result;
}

static Bitmap api_preview(const RawImage &raw,
                          const libraw_preview_t &preview)
{
  LibRaw processor;
  int rc = open_raw(processor, raw);
  if (rc == LIBRAW_SUCCESS)
    rc = processor.unpack();
  if (rc != LIBRAW_SUCCESS)
  {
    std::printf("API open/unpack failed: %d (%s)\n", rc, libraw_strerror(rc));
    std::exit(2);
  }

  int err = 0;
  libraw_processed_image_t *image =
      processor.dcraw_make_mem_preview(&preview, &err);
  if (!image)
  {
    std::printf("API preview failed: %d (%s)\n", err, libraw_strerror(err));
    std::exit(2);
  }

  Bitmap result;
  result.width = image->width;
  result.height = image->height;
  result.colors = image->colors;
  result.bps = image->bits;
  result.bytes.assign(image->data, image->data + image->data_size);
  processor.dcraw_clear_mem(image);
  return result;
}

static Bitmap c_api_preview(const RawImage &raw,
                            const libraw_preview_t &preview)
{
  libraw_data_t *processor = libraw_init(0);
  if (!processor)
  {
    std::printf("libraw_init failed\n");
    std::exit(2);
  }
  int rc = libraw_open_bayer(
      processor, const_cast<unsigned char *>(raw.data()), (unsigned)raw.size(),
      raw.width, raw.height, 0, 0, 0, 0, 0, LIBRAW_OPENBAYER_RGGB, 0, 0, 0);
  if (rc == LIBRAW_SUCCESS)
    rc = libraw_unpack(processor);
  if (rc != LIBRAW_SUCCESS)
  {
    std::printf("C API open/unpack failed: %d (%s)\n", rc,
                libraw_strerror(rc));
    libraw_close(processor);
    std::exit(2);
  }

  LibRaw *cpp_processor = (LibRaw *)processor->parent_class;
  configure(*cpp_processor, raw.bps);
  int err = 0;
  libraw_processed_image_t *image =
      libraw_dcraw_make_mem_preview(processor, &preview, &err);
  if (!image)
  {
    std::printf("C API preview failed: %d (%s)\n", err, libraw_strerror(err));
    libraw_close(processor);
    std::exit(2);
  }

  Bitmap result;
  result.width = image->width;
  result.height = image->height;
  result.colors = image->colors;
  result.bps = image->bits;
  result.bytes.assign(image->data, image->data + image->data_size);
  libraw_dcraw_clear_mem(image);
  libraw_close(processor);
  return result;
}

static uint64_t fnv1a(const std::vector<unsigned char> &bytes)
{
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < bytes.size(); i++)
  {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static bool check_exact(const char *name, const Bitmap &actual,
                        const Bitmap &expected)
{
  size_t mismatches = 0;
  int maxdiff = 0;
  const size_t compare_size =
      std::min(actual.bytes.size(), expected.bytes.size());
  for (size_t i = 0; i < compare_size; i++)
  {
    const int diff =
        std::abs(int(actual.bytes[i]) - int(expected.bytes[i]));
    if (diff)
      mismatches++;
    maxdiff = std::max(maxdiff, diff);
  }
  if (mismatches && actual.bytes.size() == expected.bytes.size())
  {
    std::printf("  first bytes actual/expected:");
    for (size_t i = 0; i < actual.bytes.size() && i < 12; i++)
      std::printf(" %u/%u", unsigned(actual.bytes[i]),
                  unsigned(expected.bytes[i]));
    std::printf("\n");
  }
  mismatches += actual.bytes.size() > expected.bytes.size()
                    ? actual.bytes.size() - expected.bytes.size()
                    : 0;
  mismatches += expected.bytes.size() > actual.bytes.size()
                    ? expected.bytes.size() - actual.bytes.size()
                    : 0;

  const bool exact = actual.width == expected.width &&
                     actual.height == expected.height &&
                     actual.colors == expected.colors &&
                     actual.bps == expected.bps && !mismatches && !maxdiff;
  std::printf("%-24s %4dx%-3d %5zu bytes  mismatch=%zu maxdiff=%d "
              "fnv=%016llx %s\n",
              name, actual.width, actual.height, actual.bytes.size(),
              mismatches, maxdiff,
              (unsigned long long)fnv1a(actual.bytes),
              exact ? "PASS" : "FAIL");
  return exact;
}

static bool check_invalid(const RawImage &raw,
                          const libraw_preview_t *preview)
{
  LibRaw processor;
  int rc = open_raw(processor, raw);
  if (rc == LIBRAW_SUCCESS)
    rc = processor.unpack();
  if (rc != LIBRAW_SUCCESS)
    return false;

  int err = 0;
  libraw_processed_image_t *image =
      processor.dcraw_make_mem_preview(preview, &err);
  const bool ok = !image && err == EINVAL;
  if (image)
    processor.dcraw_clear_mem(image);
  return ok;
}

static bool check_tiles(const RawImage &raw, const Bitmap &whole)
{
  const unsigned left = 11;
  const unsigned top = 7;
  const libraw_preview_t requests[4] = {
      {29, 23, 0, 0, left, top},
      {29, 23, left, 0, 29 - left, top},
      {29, 23, 0, top, left, 23 - top},
      {29, 23, left, top, 29 - left, 23 - top},
  };
  const size_t sample_bytes = whole.bps / 8;
  const size_t row_bytes = (size_t)29 * whole.colors * sample_bytes;
  std::vector<unsigned char> joined(whole.bytes.size(), 0);

  for (int i = 0; i < 4; i++)
  {
    Bitmap tile = api_preview(raw, requests[i]);
    const size_t tile_row_bytes =
        (size_t)tile.width * tile.colors * sample_bytes;
    for (int y = 0; y < tile.height; y++)
      std::memcpy(&joined[(size_t)(requests[i].crop_top + y) * row_bytes +
                          (size_t)requests[i].crop_left * whole.colors *
                              sample_bytes],
                  &tile.bytes[(size_t)y * tile_row_bytes], tile_row_bytes);
  }

  size_t mismatches = 0;
  for (size_t i = 0; i < joined.size(); i++)
    mismatches += joined[i] != whole.bytes[i];
  std::printf("%-24s %4dx%-3d %5zu bytes  mismatch=%zu fnv=%016llx %s\n",
              "four-tile-boundaries", whole.width, whole.height,
              joined.size(), mismatches,
              (unsigned long long)fnv1a(joined),
              mismatches ? "FAIL" : "PASS");
  return mismatches == 0;
}

static bool check_state_preserved(const RawImage &raw)
{
  LibRaw processor;
  int rc = open_raw(processor, raw);
  if (rc == LIBRAW_SUCCESS)
    rc = processor.unpack();
  if (rc != LIBRAW_SUCCESS)
    return false;

  processor.imgdata.params.half_size = 1;
  const unsigned saved_crop[4] = {3, 4, 17, 19};
  std::memcpy(processor.imgdata.params.cropbox, saved_crop,
              sizeof(saved_crop));
  const libraw_preview_t request = {29, 23, 0, 0, 29, 23};
  int err = 0;
  libraw_processed_image_t *image =
      processor.dcraw_make_mem_preview(&request, &err);
  const bool ok =
      image && !err && processor.imgdata.params.half_size == 1 &&
      !std::memcmp(processor.imgdata.params.cropbox, saved_crop,
                   sizeof(saved_crop));
  if (image)
    processor.dcraw_clear_mem(image);
  return ok;
}

int main()
{
  bool ok = true;
  const RawImage raw8 = make_raw(73, 61, 8);
  const RawImage raw16 = make_raw(73, 61, 16);
  const Bitmap full8 = render_full(raw8);
  const Bitmap full16 = render_full(raw16);

  std::printf("reference 8-bit: %dx%d, %d colors, fnv=%016llx\n", full8.width,
              full8.height, full8.colors,
              (unsigned long long)fnv1a(full8.bytes));
  std::printf("reference 16-bit: %dx%d, %d colors, fnv=%016llx\n",
              full16.width, full16.height, full16.colors,
              (unsigned long long)fnv1a(full16.bytes));

  const libraw_preview_t cases[] = {
      {73, 61, 1, 2, 71, 57},
      {37, 31, 0, 0, 37, 31},
      {29, 23, 0, 0, 29, 23},
      {1, 1, 0, 0, 1, 1},
      {29, 23, 0, 0, 3, 2},
      {29, 23, 26, 20, 3, 3},
      {29, 23, 7, 5, 13, 7},
      {29, 23, 3, 0, 11, 1},
      {29, 23, 0, 4, 1, 9},
      {29, 23, 5, 6, 0, 0},
  };
  const char *names[] = {
      "full-origin-crop", "integer-half",  "non-integer",
      "one-pixel",        "top-left-edge", "bottom-right-edge",
      "odd-crop",         "single-row",    "single-column",
      "auto-rest-crop",
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    libraw_preview_t request = cases[i];
    if (!request.crop_width)
      request.crop_width = request.width - request.crop_left;
    if (!request.crop_height)
      request.crop_height = request.height - request.crop_top;
    ok &= check_exact(names[i], api_preview(raw8, request),
                      reference_preview(full8, request));
  }

  const libraw_preview_t case16 = {29, 23, 7, 5, 13, 7};
  ok &= check_exact("16-bit-odd-crop", api_preview(raw16, case16),
                    reference_preview(full16, case16));

  const libraw_preview_t direct8 = {73, 61, 9, 10, 17, 13};
  ok &= check_exact("direct-full-crop-8", api_preview(raw8, direct8),
                    direct_crop(full8, 9, 10, 17, 13));
  const libraw_preview_t direct16 = {73, 61, 9, 10, 17, 13};
  ok &= check_exact("direct-full-crop-16", api_preview(raw16, direct16),
                    direct_crop(full16, 9, 10, 17, 13));
  ok &= check_exact("c-api-wrapper", c_api_preview(raw8, case16),
                    api_preview(raw8, case16));

  ok &= check_tiles(raw8, api_preview(raw8, {29, 23, 0, 0, 29, 23}));

  const libraw_preview_t zero_width = {0, 23, 0, 0, 1, 1};
  const libraw_preview_t out_of_bounds = {29, 23, 29, 0, 1, 1};
  const libraw_preview_t too_wide = {29, 23, 25, 0, 5, 1};
  const libraw_preview_t upscale = {74, 61, 0, 0, 1, 1};
  bool invalid_ok = true;
  invalid_ok &= check_invalid(raw8, NULL);
  invalid_ok &= check_invalid(raw8, &zero_width);
  invalid_ok &= check_invalid(raw8, &out_of_bounds);
  invalid_ok &= check_invalid(raw8, &too_wide);
  invalid_ok &= check_invalid(raw8, &upscale);
  ok &= invalid_ok;
  std::printf("%-24s %s\n", "invalid-requests",
              invalid_ok ? "PASS" : "FAIL");
  const bool state_ok = check_state_preserved(raw8);
  ok &= state_ok;
  std::printf("%-24s %s\n", "params-preserved", state_ok ? "PASS" : "FAIL");

  return ok ? 0 : 1;
}
