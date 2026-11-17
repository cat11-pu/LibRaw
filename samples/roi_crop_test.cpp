/* -*- C++ -*-
 * Minimal regression check for the exact-crop interface.
 */

#include "libraw/libraw.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static void fill_bayer(std::vector<unsigned short> &raw, int width, int height)
{
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
    {
      unsigned v = 1024 + (x * 37 + y * 53) % 52000;
      if ((x ^ y) & 1)
        v = v * 3 / 4;
      raw[size_t(y) * width + x] = (unsigned short)v;
    }
}

static int open_bayer(LibRaw &r, const std::vector<unsigned short> &raw, int w,
                      int h, int procflags = 0, unsigned black_level = 0,
                      int quality = -1, int bps = 8)
{
  int rc = r.open_bayer((const unsigned char *)raw.data(),
                        (unsigned)raw.size() * 2, w, h, 0, 0, 0, 0, procflags,
                        LIBRAW_OPENBAYER_RGGB, 0, 0, black_level);
  if (rc == LIBRAW_SUCCESS)
    rc = r.unpack();
  if (rc == LIBRAW_SUCCESS)
  {
    r.imgdata.params.no_auto_bright = 1;
    r.imgdata.params.output_bps = bps;
    r.imgdata.params.output_color = 1;
    r.imgdata.params.user_qual = quality;
  }
  return rc;
}

static bool compare_crop(const std::vector<unsigned short> &raw, int w, int h,
                         int x, int y, int cw, int ch, int procflags = 0,
                         unsigned black_level = 0, int quality = -1,
                         int bps = 8)
{
  LibRaw full, roi;
  int rc = open_bayer(full, raw, w, h, procflags, black_level, quality, bps);
  if (rc != LIBRAW_SUCCESS)
  {
    printf("full open/unpack failed: %d\n", rc);
    return false;
  }
  rc = full.dcraw_process();
  if (rc != LIBRAW_SUCCESS)
  {
    printf("full process failed: %d\n", rc);
    return false;
  }
  int fw = 0, fh = 0, fc = 0, fb = 0;
  full.get_mem_image_format(&fw, &fh, &fc, &fb);
  libraw_processed_image_t *img = full.dcraw_make_mem_image(&rc);
  if (!img || rc != LIBRAW_SUCCESS)
  {
    printf("full make_mem failed: %d\n", rc);
    return false;
  }
  size_t fstride = size_t(fw) * fc * (fb / 8);
  std::vector<unsigned char> got(size_t(cw) * ch * fc * (fb / 8));
  int out_colors = 0, out_bps = 0;
  rc = open_bayer(roi, raw, w, h, procflags, black_level, quality, bps);
  if (rc == LIBRAW_SUCCESS)
    rc = roi.dcraw_process_crop(x, y, cw, ch, got.data(),
                                size_t(cw) * fc * (fb / 8), got.size(),
                                &out_colors, &out_bps);
  if (rc != LIBRAW_SUCCESS)
  {
    printf("roi process failed (%d,%d %dx%d): %d (%s)\n", x, y, cw, ch, rc,
           libraw_strerror(rc));
    LibRaw::dcraw_clear_mem(img);
    return false;
  }
  if (out_colors != fc || out_bps != fb)
  {
    printf("format mismatch: %d/%d vs %d/%d\n", out_colors, out_bps, fc, fb);
    LibRaw::dcraw_clear_mem(img);
    return false;
  }
  size_t bytes = size_t(fc) * (fb / 8);
  for (int row = 0; row < ch; row++)
  {
    const unsigned char *want =
        img->data + size_t(y + row) * fstride + size_t(x) * bytes;
    if (memcmp(want, got.data() + size_t(row) * cw * bytes,
               size_t(cw) * bytes))
    {
      printf("pixel mismatch at (%d,%d %dx%d)\n", x, y, cw, ch);
      LibRaw::dcraw_clear_mem(img);
      return false;
    }
  }
  LibRaw::dcraw_clear_mem(img);
  return true;
}

int main()
{
  const int w = 640, h = 480;
  std::vector<unsigned short> raw(size_t(w) * h);
  fill_bayer(raw, w, h);

  struct
  {
    int x, y, w, h;
  } cases[] = {
      {0, 0, 1, 1},       {1, 1, 32, 24},      {37, 23, 100, 77},
      {0, 0, w, h},       {w - 1, h - 1, 1, 1}, {w - 64, h - 48, 64, 48},
      {639, 0, 1, 1},     {0, 479, 1, 1},       {2, 3, 13, 17},
  };

  bool ok = true;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    ok &= compare_crop(raw, w, h, cases[i].x, cases[i].y, cases[i].w,
                       cases[i].h);
  ok &= compare_crop(raw, w, h, 10, 20, 111, 73, 6 << 2);
  ok &= compare_crop(raw, w, h, 10, 20, 111, 73, 3 << 2);
  for (int quality = 0; quality <= 3; quality++)
    ok &= compare_crop(raw, w, h, 19, 27, 97, 61, 0, 500, quality, 8);
  ok &= compare_crop(raw, w, h, 21, 29, 83, 57, 0, 0, -1, 16);

  LibRaw bad1;
  int rc = open_bayer(bad1, raw, w, h);
  if (rc == LIBRAW_SUCCESS)
    rc = bad1.dcraw_process_crop(-1, 0, 2, 2, raw.data(), 8, 8, NULL, NULL);
  if (rc != LIBRAW_BAD_CROP)
  {
    printf("negative crop returned %d\n", rc);
    ok = false;
  }
  LibRaw bad2;
  rc = open_bayer(bad2, raw, w, h);
  if (rc == LIBRAW_SUCCESS)
    rc = bad2.dcraw_process_crop(w - 1, 0, 2, 1, raw.data(), 8, 8, NULL, NULL);
  if (rc != LIBRAW_BAD_CROP)
  {
    printf("out-of-bounds crop returned %d\n", rc);
    ok = false;
  }

  LibRaw overflow;
  rc = open_bayer(overflow, raw, w, h);
  if (rc == LIBRAW_SUCCESS)
    rc = overflow.dcraw_process_crop(2147483647, 0, 2147483647, 1, raw.data(),
                                     8, 8, NULL, NULL);
  if (rc != LIBRAW_BAD_CROP)
  {
    printf("overflowing crop returned %d\n", rc);
    ok = false;
  }

  LibRaw auto_bright;
  rc = open_bayer(auto_bright, raw, w, h);
  if (rc == LIBRAW_SUCCESS)
  {
    auto_bright.imgdata.params.no_auto_bright = 0;
    rc = auto_bright.dcraw_process_crop(0, 0, 16, 16, raw.data(), 48, 16 * 48,
                                        NULL, NULL);
  }
  if (rc != LIBRAW_NOT_IMPLEMENTED)
  {
    printf("auto-bright crop returned %d\n", rc);
    ok = false;
  }

  LibRaw auto_wb;
  rc = open_bayer(auto_wb, raw, w, h);
  if (rc == LIBRAW_SUCCESS)
  {
    auto_wb.imgdata.params.use_auto_wb = 1;
    rc = auto_wb.dcraw_process_crop(0, 0, 16, 16, raw.data(), 48, 16 * 48,
                                    NULL, NULL);
  }
  if (rc != LIBRAW_NOT_IMPLEMENTED)
  {
    printf("auto-WB crop returned %d\n", rc);
    ok = false;
  }

  LibRaw zero;
  rc = open_bayer(zero, raw, w, h);
  if (rc == LIBRAW_SUCCESS)
    rc = zero.dcraw_process_crop(0, 0, 0, 16, raw.data(), 48, 16 * 48, NULL,
                                 NULL);
  if (rc != LIBRAW_BAD_CROP)
  {
    printf("zero-width crop returned %d\n", rc);
    ok = false;
  }

  LibRaw short_buffer;
  rc = open_bayer(short_buffer, raw, w, h);
  if (rc == LIBRAW_SUCCESS)
    rc = short_buffer.dcraw_process_crop(0, 0, 16, 16, raw.data(), 48, 100,
                                          NULL, NULL);
  if (rc != EINVAL)
  {
    printf("short output buffer returned %d\n", rc);
    ok = false;
  }

  LibRaw no_thumb;
  rc = open_bayer(no_thumb, raw, w, h);
  int thumb_error = 0;
  libraw_processed_image_t *thumb =
      no_thumb.dcraw_make_mem_thumb_bitmap(&thumb_error);
  if (thumb || thumb_error != LIBRAW_NO_THUMBNAIL)
  {
    printf("missing thumbnail returned %p/%d\n", (void *)thumb, thumb_error);
    if (thumb)
      LibRaw::dcraw_clear_mem(thumb);
    ok = false;
  }

  printf("%s\n", ok ? "ROI crop tests passed" : "ROI crop tests failed");
  return ok ? 0 : 1;
}
