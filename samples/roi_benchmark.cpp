/* -*- C++ -*-
 * Synthetic Bayer benchmark for dcraw_process_crop().
 */

#include "libraw/libraw.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

static double now_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static long peak_rss_kb()
{
#if defined(__linux__)
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
#else
  return 0;
#endif
}

static int open_synthetic(LibRaw &r, const std::vector<unsigned short> &raw,
                          int w, int h)
{
  int rc = r.open_bayer((const unsigned char *)raw.data(),
                        (unsigned)raw.size() * 2, w, h, 0, 0, 0, 0, 0,
                        LIBRAW_OPENBAYER_RGGB, 0, 0, 0);
  if (rc == LIBRAW_SUCCESS)
    rc = r.unpack();
  if (rc == LIBRAW_SUCCESS)
  {
    r.imgdata.params.no_auto_bright = 1;
    r.imgdata.params.output_bps = 8;
    r.imgdata.params.output_color = 1;
  }
  return rc;
}

int main(int argc, char **argv)
{
  if (argc != 6)
  {
    fprintf(stderr,
            "usage: %s width height crop_w crop_h full|roi\n"
            "       %s --file path crop_w crop_h full|roi\n",
            argv[0], argv[0]);
    return 2;
  }
  LibRaw r;
  int w = 0, h = 0, cw = 0, ch = 0, rc = LIBRAW_SUCCESS;
  bool full = false, preview = false;
  if (!strcmp(argv[1], "--file"))
  {
    cw = atoi(argv[3]);
    ch = atoi(argv[4]);
    full = !strcmp(argv[5], "full");
    preview = !strcmp(argv[5], "preview");
    rc = r.open_file(argv[2]);
    if (rc == LIBRAW_SUCCESS && !preview)
      rc = r.unpack();
    if (rc == LIBRAW_SUCCESS && !preview)
    {
      w = r.imgdata.sizes.width;
      h = r.imgdata.sizes.height;
      r.imgdata.params.no_auto_bright = 1;
      r.imgdata.params.output_bps = 8;
      r.imgdata.params.output_color = 1;
    }
  }
  else
  {
    w = atoi(argv[1]);
    h = atoi(argv[2]);
    cw = atoi(argv[3]);
    ch = atoi(argv[4]);
    full = !strcmp(argv[5], "full");
    if (w < 32 || h < 32 || cw <= 0 || ch <= 0 || cw > w || ch > h)
      return 2;
    std::vector<unsigned short> raw(size_t(w) * h);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        raw[size_t(y) * w + x] =
            (unsigned short)(1024 + (size_t(x) * 37 + size_t(y) * 53) % 52000);
    rc = open_synthetic(r, raw, w, h);
  }
  if (rc != LIBRAW_SUCCESS ||
      (!preview && (cw <= 0 || ch <= 0 || cw > w || ch > h)))
  {
    fprintf(stderr, "open/unpack: %d %s\n", rc, libraw_strerror(rc));
    return 1;
  }

  if (preview)
  {
    double t0 = now_ms();
    int err = 0;
    libraw_processed_image_t *img = r.dcraw_make_mem_thumb_bitmap(&err);
    double elapsed = now_ms() - t0;
    if (!img || err != LIBRAW_SUCCESS)
    {
      fprintf(stderr, "preview: %d %s\n", err, libraw_strerror(err));
      return 1;
    }
    printf("preview %ux%u colors=%u time_ms=%.2f rss_mb=%.2f\n", img->width,
           img->height, img->colors, elapsed,
           double(peak_rss_kb()) / 1024.0);
    LibRaw::dcraw_clear_mem(img);
    return 0;
  }

  const int x0 = (w - cw) / 2;
  const int y0 = (h - ch) / 2;
  std::vector<unsigned char> out;
  if (!full)
    out.resize(size_t(cw) * ch * 3);

  double t0 = now_ms();
  if (full)
  {
    rc = r.dcraw_process();
    if (rc == LIBRAW_SUCCESS)
    {
      libraw_processed_image_t *img = r.dcraw_make_mem_image(&rc);
      if (img)
        LibRaw::dcraw_clear_mem(img);
    }
  }
  else
  {
    rc = r.dcraw_process_crop(x0, y0, cw, ch, out.data(), size_t(cw) * 3,
                              out.size(), NULL, NULL);
  }
  double elapsed = now_ms() - t0;
  if (rc != LIBRAW_SUCCESS)
  {
    fprintf(stderr, "process: %d %s\n", rc, libraw_strerror(rc));
    return 1;
  }

  const int halo = 8;
  const size_t full_work =
      size_t(w + 2) * size_t(h + 2) * sizeof(ushort[4]) + size_t(w) * h * 3;
  int strip_core_h = std::min(128, std::max(8, ch / 5 - 2 * halo));
  if (strip_core_h > ch)
    strip_core_h = ch;
  const int proc_x0 = std::max(0, x0 - halo);
  const int proc_x1 = std::min(w, x0 + cw + halo);
  const int proc_y0 = std::max(0, y0 - halo);
  const int proc_y1 = std::min(h, y0 + std::min(ch, strip_core_h) + halo);
  const size_t roi_work =
      size_t(proc_x1 - proc_x0 + 2) * size_t(proc_y1 - proc_y0 + 2) *
          sizeof(ushort[4]) +
      size_t(cw) * ch * 3;
  printf("%s %dx%d crop=%dx%d time_ms=%.2f work_mb=%.2f rss_mb=%.2f\n",
         full ? "full" : "roi", w, h, cw, ch, elapsed,
         double(full ? full_work : roi_work) / (1024.0 * 1024.0),
         double(peak_rss_kb()) / 1024.0);
  return 0;
}
