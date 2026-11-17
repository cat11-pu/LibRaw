/* -*- C++ -*-
 * Copyright 2019-2026 LibRaw LLC (info@libraw.org)
 *
 LibRaw is free software; you can redistribute it and/or modify
 it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

#include "../../internal/libraw_cxx_defs.h"

namespace
{
static inline INT64 preview_overlap(INT64 a0, INT64 a1, INT64 b0, INT64 b1)
{
  INT64 lo = a0 > b0 ? a0 : b0;
  INT64 hi = a1 < b1 ? a1 : b1;
  return hi > lo ? hi - lo : 0;
}

template <typename Sample>
static void preview_resample_row(const Sample *src, int src_width,
                                 unsigned preview_width, unsigned left,
                                 unsigned width, int colors, INT64 *dst)
{
  for (unsigned x = 0; x < width; x++)
  {
    const unsigned px = left + x;
    const INT64 x0 = INT64(px) * src_width;
    const INT64 x1 = INT64(px + 1) * src_width;
    const unsigned first = unsigned(x0 / preview_width);
    const unsigned last = unsigned((x1 + preview_width - 1) / preview_width);
    INT64 *out = dst + (INT64(x) * colors);
    for (int c = 0; c < colors; c++)
      out[c] = 0;

    for (unsigned sx = first; sx < last && sx < unsigned(src_width); sx++)
    {
      const INT64 sx0 = INT64(sx) * preview_width;
      const INT64 sx1 = sx0 + preview_width;
      const INT64 weight = preview_overlap(x0, x1, sx0, sx1);
      if (!weight)
        continue;
      const Sample *sample = src + INT64(sx) * colors;
      for (int c = 0; c < colors; c++)
        out[c] += INT64(sample[c]) * weight;
    }
  }
}

template <typename Sample>
static bool preview_resample(const Sample *src, int src_width, int src_height,
                             Sample *dst, unsigned preview_width,
                             unsigned preview_height,
                             const libraw_preview_t &preview, int colors)
{
  const unsigned crop_width =
      preview.crop_width ? preview.crop_width
                         : preview_width - preview.crop_left;
  const unsigned crop_height =
      preview.crop_height ? preview.crop_height
                          : preview_height - preview.crop_top;
  const size_t scalar_count = size_t(crop_width) * colors;
  INT64 *accum = (INT64 *)malloc(scalar_count * sizeof(INT64));
  INT64 *row = (INT64 *)malloc(scalar_count * sizeof(INT64));
  if (!accum || !row)
  {
    free(accum);
    free(row);
    return false;
  }
  const INT64 total_weight = INT64(src_width) * src_height;

  for (unsigned y = 0; y < crop_height; y++)
  {
    const unsigned py = preview.crop_top + y;
    const INT64 sy0 = INT64(py) * src_height;
    const INT64 sy1 = INT64(py + 1) * src_height;
    const unsigned sy_first = unsigned(sy0 / preview_height);
    const unsigned sy_last =
        unsigned((sy1 + preview_height - 1) / preview_height);
    memset(accum, 0, scalar_count * sizeof(INT64));

    for (unsigned sy = sy_first; sy < sy_last && sy < unsigned(src_height);
         sy++)
    {
      const INT64 py0 = INT64(sy) * preview_height;
      const INT64 py1 = py0 + preview_height;
      const INT64 weight = preview_overlap(sy0, sy1, py0, py1);
      if (!weight)
        continue;

      preview_resample_row(src + INT64(sy) * src_width * colors, src_width,
                           preview_width, preview.crop_left, crop_width, colors,
                           row);
      for (size_t i = 0; i < scalar_count; i++)
        accum[i] += row[i] * weight;
    }

    for (unsigned x = 0; x < crop_width; x++)
    {
      Sample *out = dst + (INT64(y) * crop_width + x) * colors;
      for (int c = 0; c < colors; c++)
      {
        INT64 value =
            (accum[INT64(x) * colors + c] + total_weight / 2) / total_weight;
        const INT64 maximum =
            sizeof(Sample) == 1 ? INT64((1U << 8) - 1)
                                : INT64((1U << 16) - 1);
        if (value > maximum)
          value = maximum;
        out[c] = Sample(value);
      }
    }
  }

  free(accum);
  free(row);
  return true;
}
} // namespace

libraw_processed_image_t *LibRaw::dcraw_make_mem_thumb(int *errcode)
{
  if (!T.thumb)
  {
    if (!ID.toffset && !(imgdata.thumbnail.tlength > 0 &&
                         load_raw == &LibRaw::broadcom_load_raw) // RPi
    )
    {
      if (errcode)
        *errcode = LIBRAW_NO_THUMBNAIL;
    }
    else
    {
      if (errcode)
        *errcode = LIBRAW_OUT_OF_ORDER_CALL;
    }
    return NULL;
  }

  if (T.tlength < 64u)
  {
      if (errcode)
          *errcode = EINVAL;
      return NULL;
  }

  if (INT64(T.tlength) > 1024ULL * 1024ULL * LIBRAW_MAX_THUMBNAIL_MB)
  {
      if (errcode)
          *errcode = LIBRAW_TOO_BIG;
      return NULL;
  }

  if (T.tformat == LIBRAW_THUMBNAIL_BITMAP)
  {
    libraw_processed_image_t *ret = (libraw_processed_image_t *)::malloc(
        sizeof(libraw_processed_image_t) + T.tlength);

    if (!ret)
    {
      if (errcode)
        *errcode = ENOMEM;
      return NULL;
    }

    memset(ret, 0, sizeof(libraw_processed_image_t));
    ret->type = LIBRAW_IMAGE_BITMAP;
    ret->height = T.theight;
    ret->width = T.twidth;
    if (T.tcolors > 0 && T.tcolors < 4)
        ret->colors = T.tcolors;
    else
        ret->colors = 3; // defaults
    ret->bits = 8;
    ret->data_size = T.tlength;
    memmove(ret->data, T.thumb, T.tlength);
    if (errcode)
      *errcode = 0;
    return ret;
  }
  else if (T.tformat == LIBRAW_THUMBNAIL_JPEG)
  {
    ushort exif[5];
    int mk_exif = 0;
    if (memcmp(T.thumb + 6, "Exif\0",5))
      mk_exif = 1;

    int dsize = T.tlength + mk_exif * (sizeof(exif) + sizeof(tiff_hdr));

    libraw_processed_image_t *ret = (libraw_processed_image_t *)::malloc(
        sizeof(libraw_processed_image_t) + dsize);

    if (!ret)
    {
      if (errcode)
        *errcode = ENOMEM;
      return NULL;
    }

    memset(ret, 0, sizeof(libraw_processed_image_t));

    ret->type = LIBRAW_IMAGE_JPEG;
    ret->data_size = dsize;

    ret->data[0] = 0xff;
    ret->data[1] = 0xd8;
    if (mk_exif)
    {
      struct tiff_hdr th;
      memcpy(exif, "\xff\xe1  Exif\0\0", 10);
      exif[1] = htons(8 + sizeof th);
      memmove(ret->data + 2, exif, sizeof(exif));
      tiff_head(&th, 0);
      memmove(ret->data + (2 + sizeof(exif)), &th, sizeof(th));
      memmove(ret->data + (2 + sizeof(exif) + sizeof(th)), T.thumb + 2,
              T.tlength - 2);
    }
    else
    {
      memmove(ret->data + 2, T.thumb + 2, T.tlength - 2);
    }
    if (errcode)
      *errcode = 0;
    return ret;
  }
  else if (T.tformat == LIBRAW_THUMBNAIL_H265 || T.tformat == LIBRAW_THUMBNAIL_JPEGXL)
  {
    int dsize = T.tlength;
    libraw_processed_image_t *ret = (libraw_processed_image_t *)::malloc(sizeof(libraw_processed_image_t) + dsize);
    if (!ret)
    {
      if (errcode)
        *errcode = ENOMEM;
      return NULL;
    }
    memset(ret, 0, sizeof(libraw_processed_image_t));
    ret->type = T.tformat == LIBRAW_THUMBNAIL_H265 ? LIBRAW_IMAGE_H265 : LIBRAW_IMAGE_JPEGXL;
    ret->data_size = dsize;
    memmove(ret->data, T.thumb, dsize);
    if (errcode)
      *errcode = 0;
    return ret;
  }
  else
  {
    if (errcode)
      *errcode = LIBRAW_UNSUPPORTED_THUMBNAIL;
    return NULL;
  }
}

// jlb
// macros for copying pixels to either BGR or RGB formats
#define FORBGR for (c = P1.colors - 1; c >= 0; c--)
#define FORRGB for (c = 0; c < P1.colors; c++)

void LibRaw::get_mem_image_format(int *width, int *height, int *colors,
                                  int *bps) const

{
  *width = S.width;
  *height = S.height;
  if (imgdata.progress_flags < LIBRAW_PROGRESS_FUJI_ROTATE)
  {
    if (O.use_fuji_rotate)
    {
      if (IO.fuji_width)
      {
        int fuji_width = (IO.fuji_width - 1 + IO.shrink) >> IO.shrink;
        *width = (ushort)(fuji_width / sqrt(0.5));
        *height = (ushort)((*height - fuji_width) / sqrt(0.5));
      }
      else
      {
        if (S.pixel_aspect < 0.995)
          *height = (ushort)(*height / S.pixel_aspect + 0.5);
        if (S.pixel_aspect > 1.005)
          *width = (ushort)(*width * S.pixel_aspect + 0.5);
      }
    }
  }
  if (S.flip & 4)
  {
    std::swap(*width, *height);
  }
  *colors = P1.colors;
  *bps = O.output_bps;
}

int LibRaw::copy_mem_image(void *scan0, int stride, int bgr)

{
  // the image memory pointed to by scan0 is assumed to be in the format
  // returned by get_mem_image_format
  if ((imgdata.progress_flags & LIBRAW_PROGRESS_THUMB_MASK) <
      LIBRAW_PROGRESS_PRE_INTERPOLATE)
    return LIBRAW_OUT_OF_ORDER_CALL;

  if (libraw_internal_data.output_data.histogram)
  {
    int perc, val, total, t_white = 0x2000, c;
    perc = int(S.width * S.height * O.auto_bright_thr);
    if (IO.fuji_width)
      perc /= 2;
    if (!((O.highlight & ~2) || O.no_auto_bright))
      for (t_white = c = 0; c < P1.colors; c++)
      {
        for (val = 0x2000, total = 0; --val > 32;)
          if ((total += libraw_internal_data.output_data.histogram[c][val]) >
              perc)
            break;
        if (t_white < val)
          t_white = val;
      }
    gamma_curve(O.gamm[0], O.gamm[1], 2, int((t_white << 3) / O.bright));
  }

  int s_iheight = S.iheight;
  int s_iwidth = S.iwidth;
  int s_width = S.width;
  int s_hwight = S.height;

  S.iheight = S.height;
  S.iwidth = S.width;

  if (S.flip & 4)
    SWAP(S.height, S.width);
  uchar *ppm;
  ushort *ppm2;
  int c, row, col, soff, rstep, cstep;

  soff = flip_index(0, 0);
  cstep = flip_index(0, 1) - soff;
  rstep = flip_index(1, 0) - flip_index(0, S.width);

  for (row = 0; row < S.height; row++, soff += rstep)
  {
    uchar *bufp = ((uchar *)scan0) + size_t(row) * size_t(stride);
    ppm2 = (ushort *)(ppm = bufp);
    // keep trivial decisions in the outer loop for speed
    if (bgr)
    {
      if (O.output_bps == 8)
      {
        for (col = 0; col < S.width; col++, soff += cstep)
          FORBGR *ppm++ = imgdata.color.curve[imgdata.image[soff][c]] >> 8;
      }
      else
      {
        for (col = 0; col < S.width; col++, soff += cstep)
          FORBGR *ppm2++ = imgdata.color.curve[imgdata.image[soff][c]];
      }
    }
    else
    {
      if (O.output_bps == 8)
      {
        for (col = 0; col < S.width; col++, soff += cstep)
          FORRGB *ppm++ = imgdata.color.curve[imgdata.image[soff][c]] >> 8;
      }
      else
      {
        for (col = 0; col < S.width; col++, soff += cstep)
          FORRGB *ppm2++ = imgdata.color.curve[imgdata.image[soff][c]];
      }
    }

    //            bufp += stride;           // go to the next line
  }

  S.iheight = s_iheight;
  S.iwidth = s_iwidth;
  S.width = s_width;
  S.height = s_hwight;

  return 0;
}
#undef FORBGR
#undef FORRGB

libraw_processed_image_t *LibRaw::dcraw_make_mem_image(int *errcode)

{
  int width, height, colors, bps;
  get_mem_image_format(&width, &height, &colors, &bps);
  int stride = width * (bps / 8) * colors;
  INT64 ds = INT64(height) * INT64(stride);
  libraw_processed_image_t *ret = (libraw_processed_image_t *)::malloc(
      sizeof(libraw_processed_image_t) + ds);
  if (!ret)
  {
    if (errcode)
      *errcode = ENOMEM;
    return NULL;
  }
  memset(ret, 0, sizeof(libraw_processed_image_t));

  // metadata init
  ret->type = LIBRAW_IMAGE_BITMAP;
  ret->height = height;
  ret->width = width;
  ret->colors = colors;
  ret->bits = bps;
  ret->data_size = ds;
  copy_mem_image(ret->data, stride, 0);

  return ret;
}

libraw_processed_image_t *
LibRaw::dcraw_make_mem_preview(const libraw_preview_t *preview, int *errcode)
{
  if (!preview || !preview->width || !preview->height)
  {
    if (errcode)
      *errcode = EINVAL;
    return NULL;
  }

  if (preview->crop_left >= preview->width ||
      preview->crop_top >= preview->height)
  {
    if (errcode)
      *errcode = EINVAL;
    return NULL;
  }
  const unsigned crop_width =
      preview->crop_width ? preview->crop_width
                          : preview->width - preview->crop_left;
  const unsigned crop_height =
      preview->crop_height ? preview->crop_height
                           : preview->height - preview->crop_top;
  if (!crop_width || !crop_height ||
      crop_width > preview->width - preview->crop_left ||
      crop_height > preview->height - preview->crop_top)
  {
    if (errcode)
      *errcode = EINVAL;
    return NULL;
  }

  const int saved_half_size = O.half_size;
  unsigned saved_cropbox[4];
  memmove(saved_cropbox, O.cropbox, sizeof(saved_cropbox));
  O.half_size = 0;
  O.cropbox[0] = O.cropbox[1] = 0;
  O.cropbox[2] = O.cropbox[3] = UINT_MAX;

  int rc = dcraw_process();

  O.half_size = saved_half_size;
  memmove(O.cropbox, saved_cropbox, sizeof(saved_cropbox));
  if (rc != LIBRAW_SUCCESS)
  {
    if (errcode)
      *errcode = rc;
    return NULL;
  }

  int src_width = 0, src_height = 0, colors = 0, bps = 0;
  get_mem_image_format(&src_width, &src_height, &colors, &bps);
  if (!src_width || !src_height ||
      preview->width > unsigned(src_width) ||
      preview->height > unsigned(src_height) || colors < 1 || colors > 4 ||
      (bps != 8 && bps != 16))
  {
    if (errcode)
      *errcode = EINVAL;
    return NULL;
  }

  const int sample_bytes = bps / 8;
  const INT64 src_stride = INT64(src_width) * colors * sample_bytes;
  const INT64 src_size = INT64(src_height) * src_stride;
  const INT64 dst_stride = INT64(crop_width) * colors * sample_bytes;
  const INT64 dst_size = INT64(crop_height) * dst_stride;
  if (src_size < 0 || (uint64_t)src_size > uint64_t(SIZE_MAX) ||
      dst_size < 0 || dst_size > INT64(UINT_MAX) ||
      uint64_t(dst_size) + sizeof(libraw_processed_image_t) >
          uint64_t(SIZE_MAX))
  {
    if (errcode)
      *errcode = LIBRAW_TOO_BIG;
    return NULL;
  }

  unsigned char *src = (unsigned char *)malloc(size_t(src_size));
  if (!src)
  {
    if (errcode)
      *errcode = ENOMEM;
    return NULL;
  }
  rc = copy_mem_image(src, int(src_stride), 0);
  if (rc != LIBRAW_SUCCESS)
  {
    free(src);
    if (errcode)
      *errcode = rc;
    return NULL;
  }

  libraw_processed_image_t *ret = (libraw_processed_image_t *)::malloc(
      sizeof(libraw_processed_image_t) + size_t(dst_size));
  if (!ret)
  {
    free(src);
    if (errcode)
      *errcode = ENOMEM;
    return NULL;
  }
  memset(ret, 0, sizeof(libraw_processed_image_t));
  ret->type = LIBRAW_IMAGE_BITMAP;
  ret->height = ushort(crop_height);
  ret->width = ushort(crop_width);
  ret->colors = ushort(colors);
  ret->bits = ushort(bps);
  ret->data_size = unsigned(dst_size);

  bool resampled;
  if (bps == 8)
    resampled = preview_resample(src, src_width, src_height, ret->data,
                                 preview->width, preview->height, *preview,
                                 colors);
  else
    resampled = preview_resample((const ushort *)src, src_width, src_height,
                                 (ushort *)ret->data, preview->width,
                                 preview->height, *preview, colors);

  free(src);
  if (!resampled)
  {
    ::free(ret);
    if (errcode)
      *errcode = ENOMEM;
    return NULL;
  }
  if (errcode)
    *errcode = 0;
  return ret;
}

void LibRaw::dcraw_clear_mem(libraw_processed_image_t *p)
{
  if (p)
    ::free(p);
}
