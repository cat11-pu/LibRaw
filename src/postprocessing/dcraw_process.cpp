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

int LibRaw::dcraw_process(void)
{
  int quality, i;

  int iterations = -1, dcb_enhance = 1, noiserd = 0;
  float preser = 0;
  float expos = 1.0;

  CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);
  //    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_PRE_INTERPOLATE);

  try
  {

    int no_crop = 1;

    if (~O.cropbox[2] && ~O.cropbox[3])
      no_crop = 0;

	for(int c = 0; c < 4; c++)
		if (O.aber[c]< 0.001 || O.aber[c] > 1000.f)
			O.aber[c] = 1.0;

    libraw_decoder_info_t di;
    get_decoder_info(&di);

    bool is_bayer = (imgdata.idata.filters || P1.colors == 1);
    int subtract_inline =
        !O.bad_pixels && !O.dark_frame && is_bayer && !IO.zero_is_bad;

    int rc = raw2image_ex(subtract_inline); // allocate imgdata.image and copy data!
	if (rc != LIBRAW_SUCCESS)
		return rc;

    // Adjust sizes

    int save_4color = O.four_color_rgb;

    if (IO.zero_is_bad)
    {
      remove_zeroes();
      SET_PROC_FLAG(LIBRAW_PROGRESS_REMOVE_ZEROES);
    }

    if (O.bad_pixels && no_crop)
    {
      bad_pixels(O.bad_pixels);
      SET_PROC_FLAG(LIBRAW_PROGRESS_BAD_PIXELS);
    }

    if (O.dark_frame && no_crop)
    {
      subtract(O.dark_frame);
      SET_PROC_FLAG(LIBRAW_PROGRESS_DARK_FRAME);
    }
    /* pre subtract black callback: check for it above to disable subtract
     * inline */

    if (callbacks.pre_subtractblack_cb)
      (callbacks.pre_subtractblack_cb)(this);

    quality = 2 + !IO.fuji_width;

    if (O.user_qual >= 0)
      quality = O.user_qual;

    if (!subtract_inline || !C.data_maximum)
    {
      adjust_bl();
      subtract_black_internal();
    }

    if (!(di.decoder_flags & LIBRAW_DECODER_FIXEDMAXC))
    {
      if (libraw_internal_data.unpacker_data.roi_mode)
        C.data_maximum = libraw_internal_data.unpacker_data.roi_data_maximum;
      adjust_maximum();
    }

    if (O.user_sat > 0)
      C.maximum = O.user_sat;

    if (P1.is_foveon)
    {
      if (load_raw == &LibRaw::x3f_load_raw)
      {
        // Filter out zeroes
        for (int q = 0; q < S.height * S.width; q++)
        {
          for (int c = 0; c < 4; c++)
            if ((short)imgdata.image[q][c] < 0)
              imgdata.image[q][c] = 0;
        }
      }
      SET_PROC_FLAG(LIBRAW_PROGRESS_FOVEON_INTERPOLATE);
    }

    if (O.green_matching && !O.half_size)
    {
      green_matching();
    }

    if (callbacks.pre_scalecolors_cb)
      (callbacks.pre_scalecolors_cb)(this);

    if (!O.no_auto_scale)
    {
      scale_colors();
      SET_PROC_FLAG(LIBRAW_PROGRESS_SCALE_COLORS);
    }

    if (callbacks.pre_preinterpolate_cb)
      (callbacks.pre_preinterpolate_cb)(this);

    pre_interpolate();

    SET_PROC_FLAG(LIBRAW_PROGRESS_PRE_INTERPOLATE);

    if (O.dcb_iterations >= 0)
      iterations = O.dcb_iterations;
    if (O.dcb_enhance_fl >= 0)
      dcb_enhance = O.dcb_enhance_fl;
    if (O.fbdd_noiserd >= 0)
      noiserd = O.fbdd_noiserd;

    /* pre-exposure correction callback */

    if (O.exp_correc > 0)
    {
      expos = O.exp_shift;
      preser = O.exp_preser;
      exp_bef(expos, preser);
    }

    if (callbacks.pre_interpolate_cb)
      (callbacks.pre_interpolate_cb)(this);

    /* post-exposure correction fallback */
    if (P1.filters && !O.no_interpolation)
    {
	  int real_colors = P1.colors;
	  int bad_bayer = 0;
	  if (P1.filters > 1000)
		  for (int r = 0; r < 4; r++)
			  for (int c = 0; c < 8; c++)
			  {
				  real_colors = MAX(COLOR(r, c) + 1, real_colors);
				  if (P1.filters > 1000)
				  {
					  // Normal bayer: adjacent pixels horizontally/vertically have different colors
					  bad_bayer += (FC(r, c) == FC(r + 1, c));
					  bad_bayer += (FC(r, c) == FC(r, c + 1));
				  }
			  }

      if (noiserd > 0 && P1.colors == 3 && real_colors == 3 && P1.filters > 1000)
        fbdd(noiserd);

      if (P1.filters > 1000 && callbacks.interpolate_bayer_cb)
        (callbacks.interpolate_bayer_cb)(this);
      else if (P1.filters == 9 && callbacks.interpolate_xtrans_cb)
        (callbacks.interpolate_xtrans_cb)(this);
      else if (quality == 0)
        lin_interpolate();
      else if (quality == 1 || P1.colors > 3 || real_colors > 3 || bad_bayer || (P1.filters != LIBRAW_XTRANS && P1.filters <= 1000))
        vng_interpolate();
      else if (quality == 2 && P1.filters > 1000)
        ppg_interpolate();
      else if (P1.filters == LIBRAW_XTRANS)
      {
        // Fuji X-Trans
        xtrans_interpolate(quality > 2 ? 3 : 1);
      }
      else if (quality == 3)
        ahd_interpolate(); // really don't need it here due to fallback op
      else if (quality == 4)
        dcb(iterations, dcb_enhance);

      else if (quality == 11)
        dht_interpolate();
      else if (quality == 12)
        aahd_interpolate();
      // fallback to AHD
      else
      {
        ahd_interpolate();
        imgdata.process_warnings |= LIBRAW_WARN_FALLBACK_TO_AHD;
      }

      SET_PROC_FLAG(LIBRAW_PROGRESS_INTERPOLATE);
    }
    if (IO.mix_green)
    {
      for (P1.colors = 3, i = 0; i < S.height * S.width; i++)
        imgdata.image[i][1] = (imgdata.image[i][1] + imgdata.image[i][3]) >> 1;
      SET_PROC_FLAG(LIBRAW_PROGRESS_MIX_GREEN);
    }

    if (callbacks.post_interpolate_cb)
      (callbacks.post_interpolate_cb)(this);
    else if (!P1.is_foveon && P1.colors == 3 && O.med_passes > 0)
    {
      median_filter();
      SET_PROC_FLAG(LIBRAW_PROGRESS_MEDIAN_FILTER);
    }

    if (O.highlight == 2)
    {
      blend_highlights();
      SET_PROC_FLAG(LIBRAW_PROGRESS_HIGHLIGHTS);
    }

    if (O.highlight > 2)
    {
      recover_highlights();
      SET_PROC_FLAG(LIBRAW_PROGRESS_HIGHLIGHTS);
    }

    if (O.use_fuji_rotate)
    {
      fuji_rotate();
      SET_PROC_FLAG(LIBRAW_PROGRESS_FUJI_ROTATE);
    }

    if (!libraw_internal_data.output_data.histogram)
    {
      libraw_internal_data.output_data.histogram =
          (int(*)[LIBRAW_HISTOGRAM_SIZE])calloc(1,
              sizeof(*libraw_internal_data.output_data.histogram) * 4);
    }
#ifndef NO_LCMS
    if (O.camera_profile)
    {
      apply_profile(O.camera_profile, O.output_profile);
      SET_PROC_FLAG(LIBRAW_PROGRESS_APPLY_PROFILE);
    }
#endif

    if (callbacks.pre_converttorgb_cb)
      (callbacks.pre_converttorgb_cb)(this);

    convert_to_rgb();
    SET_PROC_FLAG(LIBRAW_PROGRESS_CONVERT_RGB);

    if (callbacks.post_converttorgb_cb)
      (callbacks.post_converttorgb_cb)(this);

    if (O.use_fuji_rotate)
    {
      stretch();
      SET_PROC_FLAG(LIBRAW_PROGRESS_STRETCH);
    }
    O.four_color_rgb = save_4color; // also, restore

    return 0;
  }
  catch (const std::bad_alloc&)
  {
      recycle();
      return LIBRAW_UNSUFFICIENT_MEMORY;
  }
  catch (const LibRaw_exceptions& err)
  {
    EXCEPTION_HANDLER(err);
  }
}

/*
 * The public dcraw cropbox is applied after raw2image(), but demosaic
 * algorithms need pixels outside the requested rectangle.  This entry point
 * expands the rectangle by a small halo, runs the normal pipeline on that
 * expanded buffer, and copies only the requested rectangle to the caller.
 *
 * The output rectangle is expressed in the same (possibly rotated/flipped)
 * coordinate system as dcraw_make_mem_image().
 */
int LibRaw::dcraw_process_crop(int x, int y, int width, int height, void *dst,
                               size_t dst_stride, size_t dst_size, int *colors,
                               int *bps)
{
  CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);

  if (width <= 0 || height <= 0 || !dst)
    return LIBRAW_BAD_CROP;
  if (INT64(x) + INT64(width) > INT64(UINT_MAX) ||
      INT64(y) + INT64(height) > INT64(UINT_MAX))
    return LIBRAW_BAD_CROP;

  if (!imgdata.rawdata.raw_image || imgdata.rawdata.color3_image ||
      imgdata.rawdata.color4_image || imgdata.rawdata.float3_image ||
      imgdata.rawdata.float4_image)
    return LIBRAW_NOT_IMPLEMENTED;
  if (!imgdata.idata.filters || imgdata.idata.filters == 1 ||
      imgdata.idata.filters == LIBRAW_XTRANS ||
      imgdata.idata.colors != 3 || imgdata.idata.is_foveon)
    return LIBRAW_NOT_IMPLEMENTED;
  if (is_phaseone_compressed() || is_canon_600() ||
      load_raw == &LibRaw::canon_600_load_raw)
    return LIBRAW_NOT_IMPLEMENTED;
  if (libraw_internal_data.internal_output_params.fuji_width ||
      imgdata.sizes.pixel_aspect < 0.9995f ||
      imgdata.sizes.pixel_aspect > 1.0005f)
    return LIBRAW_NOT_IMPLEMENTED;

  /*
   * These options either need the complete image (auto WB/auto bright,
   * wavelet denoise), change the sampling grid (half size), or are outside
   * the narrow Bayer ROI contract.
   */
  if (O.use_auto_wb || !O.no_auto_bright || O.threshold ||
      O.half_size || O.four_color_rgb || O.aber[0] != 1.0 ||
      O.aber[2] != 1.0 || O.med_passes > 0 || O.highlight ||
      O.fbdd_noiserd > 0 || O.exp_correc > 0 || O.no_interpolation ||
      O.bad_pixels || O.dark_frame ||
      libraw_internal_data.internal_output_params.zero_is_bad)
    return LIBRAW_NOT_IMPLEMENTED;
  if (callbacks.pre_subtractblack_cb || callbacks.pre_scalecolors_cb ||
      callbacks.pre_preinterpolate_cb || callbacks.pre_interpolate_cb ||
      callbacks.interpolate_bayer_cb || callbacks.interpolate_xtrans_cb ||
      callbacks.post_interpolate_cb || callbacks.pre_converttorgb_cb ||
      callbacks.post_converttorgb_cb)
    return LIBRAW_NOT_IMPLEMENTED;
  if (O.output_bps != 8 && O.output_bps != 16)
    return LIBRAW_NOT_IMPLEMENTED;
  if ((~O.cropbox[2] && ~O.cropbox[3]))
    return LIBRAW_NOT_IMPLEMENTED;
  if (O.bright <= 0)
    return LIBRAW_NOT_IMPLEMENTED;

  int quality = 2 + !libraw_internal_data.internal_output_params.fuji_width;
  if (O.user_qual >= 0)
    quality = O.user_qual;
  if (quality < 0 || quality > 3)
    return LIBRAW_NOT_IMPLEMENTED;

  raw2image_start();
  if (O.use_camera_wb && !O.use_auto_wb &&
      (C.cam_mul[0] < -0.5f ||
       (C.cam_mul[0] <= 0.00001f &&
        !(imgdata.rawparams.options &
          LIBRAW_RAWOPTIONS_CAMERAWB_FALLBACK_TO_DAYLIGHT))))
    return LIBRAW_NOT_IMPLEMENTED;
  const int full_width = S.width;
  const int full_height = S.height;
  const int flip = S.flip;
  const int out_width = (flip & 4) ? full_height : full_width;
  const int out_height = (flip & 4) ? full_width : full_height;

  if (x < 0 || y < 0 || INT64(x) + width > out_width ||
      INT64(y) + height > out_height)
    return LIBRAW_BAD_CROP;

  const int bytes = O.output_bps == 8 ? 1 : 2;
  const int out_colors = P1.colors;
  if (out_colors < 1 || out_colors > 4)
    return LIBRAW_NOT_IMPLEMENTED;
  if (size_t(width) > size_t(-1) / (size_t(out_colors) * bytes))
    return LIBRAW_TOO_BIG;
  size_t rowbytes = size_t(width) * out_colors * bytes;
  if (dst_stride < rowbytes ||
      (height > 1 &&
       dst_stride > (size_t(-1) - rowbytes) / size_t(height - 1)) ||
      dst_size < dst_stride * size_t(height - 1) + rowbytes)
    return EINVAL;

  /*
   * map an output-space coordinate back to the unrotated image.  This is the
   * same transform used by flip_index() in copy_mem_image().
   */
#define ROI_MAP_OUTPUT(oy, ox, sr, sc)                                        \
  do                                                                           \
  {                                                                            \
    int rr_ = (oy), cc_ = (ox);                                                \
    if (flip & 4)                                                              \
    {                                                                          \
      int t_ = rr_;                                                            \
      rr_ = cc_;                                                               \
      cc_ = t_;                                                                \
    }                                                                          \
    if (flip & 2)                                                              \
      rr_ = full_height - 1 - rr_;                                             \
    if (flip & 1)                                                              \
      cc_ = full_width - 1 - cc_;                                              \
    sr = rr_;                                                                  \
    sc = cc_;                                                                  \
  } while (0)

  int sx[4], sy[4];
  ROI_MAP_OUTPUT(y, x, sy[0], sx[0]);
  ROI_MAP_OUTPUT(y, x + width - 1, sy[1], sx[1]);
  ROI_MAP_OUTPUT(y + height - 1, x, sy[2], sx[2]);
  ROI_MAP_OUTPUT(y + height - 1, x + width - 1, sy[3], sx[3]);
  int min_x = sx[0], max_x = sx[0], min_y = sy[0], max_y = sy[0];
  for (int i = 1; i < 4; i++)
  {
    min_x = MIN(min_x, sx[i]);
    max_x = MAX(max_x, sx[i]);
    min_y = MIN(min_y, sy[i]);
    max_y = MAX(max_y, sy[i]);
  }
#undef ROI_MAP_OUTPUT

  /*
   * AHD uses a 5px border, VNG/PPG use 3px, green matching uses 2px.
   * 8px also leaves room for algorithm-internal border interpolation.
   */
  const int halo = 8;
  const int crop_x0 = MAX(0, min_x - halo);
  const int crop_y0 = MAX(0, min_y - halo);
  const int crop_x1 = MIN(full_width, max_x + halo + 1);
  const int crop_y1 = MIN(full_height, max_y + halo + 1);
  if (crop_x1 <= crop_x0 || crop_y1 <= crop_y0)
    return LIBRAW_BAD_CROP;

  /*
   * adjust_maximum() is image-global in a normal dcraw_process().  Compute
   * the same maximum from the full Bayer plane before running the ROI.
   * This scan does not allocate a second image.
   */
  adjust_bl();
  unsigned roi_dmax = 0;
  const int max_height =
      MAX(0, MIN(int(S.height), int(S.raw_height) - int(S.top_margin)));
  const int max_width =
      MAX(0, MIN(int(S.width), int(S.raw_width) - int(S.left_margin)));
  for (int row = 0; row < max_height; row++)
  {
    for (int col = 0; col < max_width; col++)
    {
      unsigned val =
          imgdata.rawdata
              .raw_image[(row + S.top_margin) * S.raw_pitch / 2 +
                         (col + S.left_margin)];
      const int cc = fcol(row, col);
      if (val > C.cblack[cc])
      {
        val -= C.cblack[cc];
        if (roi_dmax < val)
          roi_dmax = val;
      }
    }
  }

  unsigned saved_cropbox[4];
  memmove(saved_cropbox, O.cropbox, sizeof(saved_cropbox));
  libraw_internal_data.unpacker_data.roi_data_maximum = roi_dmax;

#define ROI_FINISH(code)                                                       \
  do                                                                           \
  {                                                                            \
    libraw_internal_data.unpacker_data.roi_mode = 0;                           \
    memmove(O.cropbox, saved_cropbox, sizeof(saved_cropbox));                  \
    return (code);                                                             \
  } while (0)

  /*
   * Process in horizontal strips.  A full-width strip plus halo keeps the
   * demosaic working set small even when the requested crop is large.
   */
  const int target_h = max_y - min_y + 1;
  int strip_core_h = MIN(128, MAX(8, target_h / 5 - 2 * halo));
  if (strip_core_h > target_h)
    strip_core_h = target_h;

  for (int core_y0 = min_y; core_y0 <= max_y; core_y0 += strip_core_h)
  {
    const int core_y1 = MIN(max_y + 1, core_y0 + strip_core_h);
    const int proc_y0 = MAX(0, core_y0 - halo);
    const int proc_y1 = MIN(full_height, core_y1 + halo);
    if (proc_y1 <= proc_y0)
      ROI_FINISH(LIBRAW_BAD_CROP);

    O.cropbox[0] = crop_x0;
    O.cropbox[1] = proc_y0;
    O.cropbox[2] = crop_x1 - crop_x0;
    O.cropbox[3] = proc_y1 - proc_y0;
    libraw_internal_data.unpacker_data.roi_mode = 1;
    int rc = dcraw_process();
    libraw_internal_data.unpacker_data.roi_mode = 0;
    if (rc != LIBRAW_SUCCESS)
      ROI_FINISH(rc);
    if (int(S.width) != crop_x1 - crop_x0 ||
        int(S.height) != proc_y1 - proc_y0 || P1.colors != out_colors)
      ROI_FINISH(LIBRAW_DATA_ERROR);

    /*
     * dcraw_make_mem_image() applies this LUT in copy_mem_image().  Auto
     * bright is disabled above, so t_white is the same constant for every
     * strip and for the full image.
     */
    gamma_curve(O.gamm[0], O.gamm[1], 2, int((0x2000 << 3) / O.bright));

    auto write_pixel = [&](int oy, int ox, int src_y, int src_x) -> int {
      const int wr = src_y - proc_y0;
      const int wc = src_x - crop_x0;
      if (wr < 0 || wr >= int(S.height) || wc < 0 || wc >= int(S.width))
        return LIBRAW_DATA_ERROR;
      unsigned char *orow = (unsigned char *)dst + size_t(oy) * dst_stride;
      ushort *orow16 = (ushort *)orow;
      ushort *pix = imgdata.image[size_t(wr) * S.width + wc];
      for (int c = 0; c < out_colors; c++)
      {
        const ushort v = C.curve[pix[c]];
        if (O.output_bps == 8)
          orow[size_t(ox) * out_colors + c] = (unsigned char)(v >> 8);
        else
          orow16[size_t(ox) * out_colors + c] = v;
      }
      return LIBRAW_SUCCESS;
    };

    int prc = LIBRAW_SUCCESS;
    if (!(flip & 4))
    {
      for (int oy = 0; oy < height && prc == LIBRAW_SUCCESS; oy++)
      {
        const int src_y =
            (flip & 2) ? full_height - 1 - (y + oy) : (y + oy);
        if (src_y < core_y0 || src_y >= core_y1)
          continue;
        for (int ox = 0; ox < width; ox++)
        {
          const int src_x =
              (flip & 1) ? full_width - 1 - (x + ox) : (x + ox);
          prc = write_pixel(oy, ox, src_y, src_x);
          if (prc != LIBRAW_SUCCESS)
            break;
        }
      }
    }
    else
    {
      for (int ox = 0; ox < width && prc == LIBRAW_SUCCESS; ox++)
      {
        const int src_y =
            (flip & 2) ? full_height - 1 - (x + ox) : (x + ox);
        if (src_y < core_y0 || src_y >= core_y1)
          continue;
        for (int oy = 0; oy < height; oy++)
        {
          const int src_x =
              (flip & 1) ? full_width - 1 - (y + oy) : (y + oy);
          prc = write_pixel(oy, ox, src_y, src_x);
          if (prc != LIBRAW_SUCCESS)
            break;
        }
      }
    }
    if (prc != LIBRAW_SUCCESS)
      ROI_FINISH(prc);
  }

#undef ROI_FINISH
  memmove(O.cropbox, saved_cropbox, sizeof(saved_cropbox));

  if (colors)
    *colors = out_colors;
  if (bps)
    *bps = O.output_bps;
  return LIBRAW_SUCCESS;
}
