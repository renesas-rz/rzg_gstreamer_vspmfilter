/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * This file:
 * Copyright (C) 2003 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2010 David Schleef <ds@schleef.org>
 * Copyright (C) 2014 - 2023 Renesas Electronics Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/* ISU platform implementation -- auto-detected at init, dispatched via ops */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gstvspmfilter.h"

#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>

#include <string.h>
#include <stdio.h>

#include "vspm_public.h"

GST_DEBUG_CATEGORY_EXTERN (vspmfilter_debug);
GST_DEBUG_CATEGORY_EXTERN (GST_CAT_PERFORMANCE);
#define GST_CAT_DEFAULT vspmfilter_debug

#define ISU_STRIDE_ALIGN (32)
#define ISU_ADDR_ALIGN   (512)

static unsigned int fp_to_fixed (gdouble fval);
static void compute_csc (guint src_fmt, GstVideoColorRange in_range,
    guint dst_fmt, GstVideoColorRange out_range, gdouble Kr, gdouble Kb,
    T_ISU_CSC *csc_par);

/* Note that below swap information will be REVERSED later (in function
 *     set_colorspace) because current system use Little Endian */
static const extensions_t exts_isu[] = {
  {GST_VIDEO_FORMAT_NV12,       ISU_YUV420_NV12,ISU_SWAP_NO},    /* NV12 format is highest priority as most modules support this */
  {GST_VIDEO_FORMAT_RGB16,      ISU_RGB565,     ISU_SWAP_B},
  {GST_VIDEO_FORMAT_RGB,        ISU_RGB888,     ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_BGR,        ISU_BGR888,     ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_RGBx,       ISU_RGBA8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_BGRx,       ISU_ARGB8888,   ISU_SWAP_B | ISU_SWAP_W},
  {GST_VIDEO_FORMAT_xRGB,       ISU_ARGB8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_xBGR,       ISU_ABGR8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_RGBA,       ISU_RGBA8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_BGRA,       ISU_ARGB8888,   ISU_SWAP_B | ISU_SWAP_W},
  {GST_VIDEO_FORMAT_ARGB,       ISU_ARGB8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_ABGR,       ISU_ABGR8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_UYVY,       ISU_YUV422_UYVY,ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_YUY2,       ISU_YUV422_YUY2,ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_NV16,       ISU_YUV422_NV16,ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_GRAY8,      ISU_RAW8,       ISU_SWAP_NO},
#ifdef HAS_GRAY10_LE64
  {GST_VIDEO_FORMAT_GRAY10_LE64,ISU_RAW10,      ISU_SWAP_NO},
#endif
};

static const extensions_t exts_isu_out[] = {
  {GST_VIDEO_FORMAT_NV12,       ISU_YUV420_NV12,ISU_SWAP_NO},    /* NV12 format is highest priority as most modules support this */
  {GST_VIDEO_FORMAT_RGB16,      ISU_RGB565,     ISU_SWAP_B},
  {GST_VIDEO_FORMAT_RGB,        ISU_RGB888,     ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_BGR,        ISU_BGR888,     ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_RGBx,       ISU_RGBA8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_BGRx,       ISU_ARGB8888,   ISU_SWAP_B | ISU_SWAP_W},
  {GST_VIDEO_FORMAT_xRGB,       ISU_ARGB8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_xBGR,       ISU_ABGR8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_RGBA,       ISU_RGBA8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_BGRA,       ISU_ARGB8888,   ISU_SWAP_B | ISU_SWAP_W},
  {GST_VIDEO_FORMAT_ARGB,       ISU_ARGB8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_ABGR,       ISU_ABGR8888,   ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_UYVY,       ISU_YUV422_UYVY,ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_YUY2,       ISU_YUV422_YUY2,ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_NV16,       ISU_YUV422_NV16,ISU_SWAP_NO},
  {GST_VIDEO_FORMAT_GRAY8,      ISU_RAW8,       ISU_SWAP_NO},
#ifdef HAS_GRAY10_LE64
  {GST_VIDEO_FORMAT_GRAY10_LE64,ISU_RAW10,      ISU_SWAP_NO},
#endif
};

static gint
set_colorspace (GstVideoFormat vid_fmt, guint * format, guint * fswap)
{
  int nr_exts = sizeof (exts_isu) / sizeof (exts_isu[0]);
  int i;

  for (i = 0; i < nr_exts; i++) {
    if (vid_fmt == exts_isu[i].gst_format) {
      *format = exts_isu[i].hw_format;

      /* Need to reverse swap information for Little Endian */
      *fswap  = exts_isu[i].hw_swap;
      return 0;
    }
  }
  return -1;
}

static gint
set_colorspace_output (GstVideoFormat vid_fmt, guint * format, guint * fswap)
{
  int nr_exts = sizeof (exts_isu_out) / sizeof (exts_isu_out[0]);
  int i;

  for (i = 0; i < nr_exts; i++) {
    if (vid_fmt == exts_isu_out[i].gst_format) {
      *format = exts_isu_out[i].hw_format;

      /* Need to reverse swap information for Little Endian */
      *fswap  = exts_isu_out[i].hw_swap;
      return 0;
    }
  }
  return -1;
}

static void
set_buffer_info (GstVspmFilter * space, VspmBufferInfo * buf_info,
    GstVideoInfo * info, GstVideoAlignment * align)
{
  gint i;

  if (!buf_info) {
    GST_ERROR_OBJECT (space, "buf_info is NULL");
    return;
  }

  if (info != NULL) {
    buf_info->width = GST_VIDEO_INFO_WIDTH (info);
    buf_info->height = GST_VIDEO_INFO_HEIGHT (info);
    buf_info->format = GST_VIDEO_FORMAT_INFO_FORMAT (info->finfo);
    buf_info->n_planes = GST_VIDEO_FORMAT_INFO_N_PLANES (info->finfo);
    for (i = 0; i < buf_info->n_planes; i++) {
      buf_info->plane_width[i] =
          GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (info->finfo, i, info->width);
      buf_info->plane_height[i] =
          GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (info->finfo, i, info->height);
      buf_info->plane_pixel_stride[i] =
          GST_VIDEO_FORMAT_INFO_PSTRIDE (info->finfo, i);
    }
  }

  buf_info->buf_size = 0;
  memset (buf_info->plane_stride, 0, sizeof (buf_info->plane_stride));
  memset (buf_info->plane_size  , 0, sizeof (buf_info->plane_size));
  memset (buf_info->plane_offset, 0, sizeof (buf_info->plane_offset));

  for (i = 0; i < buf_info->n_planes; i++) {
    gint stride = buf_info->plane_width[i] * buf_info->plane_pixel_stride[i];
    gint sliceheight = buf_info->plane_height[i];

#ifdef HAS_GRAY10_LE64
    /* INFO: Renesas defined format. 10-bit grayscale, packed into 64bit words
     *       with 6 pixels and 4 bits padding. */
    if (buf_info->format == GST_VIDEO_FORMAT_GRAY10_LE64) {
      stride = (buf_info->plane_width[i] + 5) / 6 * 8;
    }
#endif

    buf_info->plane_offset[i] = buf_info->buf_size;

    if (i == GST_VIDEO_COMP_Y) {
      /* If we have alignment requirement from downstream */
      if (align != NULL) {
        /* FIXME: Currently, we ignore padding and only update stride */

        /* According to the implementation of Gstreamer, stride_align, logically,
         * must be a number equal to 2^N-1 instead of 2^N. Downstream proposes
         * alignment as 2^N in the older versions and 2^N-1 in the new version.
         * So, we should round the alignment up before using to get the same
         * result for both cases */
        stride = GST_ROUND_UP_N(stride, GST_ROUND_UP_2(align->stride_align[i]));
      }

      /* Check output stride whether 32 pixels alignment or not */
      if (stride % ISU_STRIDE_ALIGN) {
        stride = GST_ROUND_UP_N(stride, ISU_STRIDE_ALIGN);
      }

      /* Check output address whether 512 bytes alignment or not */
      if ((stride * sliceheight) % ISU_ADDR_ALIGN) {
        if (!(sliceheight % 8)) {
          stride = GST_ROUND_UP_N(stride, 64);
        } else if (!(sliceheight % 4)) {
          stride = GST_ROUND_UP_N(stride, 128);
        } else if (!(sliceheight % 2)) {
          stride = GST_ROUND_UP_N(stride, 256);
        } else {
          /* do nothing */
        }
      }
    } else {
      /* Update stride of plane UV following the stride of plane Y */
      stride = buf_info->plane_stride[0];
    }

    buf_info->plane_stride[i] = stride;
    buf_info->plane_size[i] = stride * sliceheight;

    buf_info->buf_size += buf_info->plane_size[i];
  }
  return;
}

static gboolean
set_info (GstVideoFilter * filter,
    GstCaps * incaps, GstVideoInfo * in_info, GstCaps * outcaps,
    GstVideoInfo * out_info)
{
  GstVspmFilter *space;
  GstVspmFilterVspInfo *vsp_info;
  GstStructure *structure;

  space = GST_VIDEO_CONVERT_CAST (filter);
  vsp_info = space->vsp_info;

  /* these must match */
  if (in_info->fps_n != out_info->fps_n || in_info->fps_d != out_info->fps_d)
    goto format_mismatch;

  /* if present, these must match too */
  if (in_info->interlace_mode != out_info->interlace_mode)
    goto format_mismatch;

  GST_DEBUG ("reconfigured %d %d", GST_VIDEO_INFO_FORMAT (in_info),
      GST_VIDEO_INFO_FORMAT (out_info));

  /* Pre-compute ISU CSC */
  {
    guint in_fmt, out_fmt, in_swap, out_swap;
    GstVideoColorRange in_range, out_range;
    g_clear_pointer (&vsp_info->cached_csc, g_free);

    if (set_colorspace (GST_VIDEO_INFO_FORMAT (in_info),
                        &in_fmt, &in_swap) != 0 ||
        set_colorspace_output (GST_VIDEO_INFO_FORMAT (out_info),
                               &out_fmt, &out_swap) != 0) {
      GST_ERROR_OBJECT (space, "input/output format is not supported.");
      return FALSE;
    }

    in_range  = in_info->colorimetry.range;
    out_range = out_info->colorimetry.range;

    if (((in_fmt & 0xF0) == (out_fmt & 0xF0)) &&
        ((in_range  == GST_VIDEO_COLOR_RANGE_UNKNOWN) ||
         (out_range == GST_VIDEO_COLOR_RANGE_UNKNOWN) ||
         (in_range  == out_range))) {
      /* Do nothing; skip color space conversion */
    } else {
      GstVideoColorMatrix matrix;
      gdouble Kr, Kb;

      vsp_info->cached_csc = g_malloc0 (sizeof (T_ISU_CSC));

      /* Pick Kr/Kb from the YUV side. Cross-matrix YUV-to-YUV (e.g. BT.601
       * input + BT.709 output) is not supported.
       */
      if ((in_fmt & 0xF0) == YUV_FORMAT)
        matrix = in_info->colorimetry.matrix;
      else if ((out_fmt & 0xF0) == YUV_FORMAT)
        matrix = out_info->colorimetry.matrix;
      else
        matrix = GST_VIDEO_COLOR_MATRIX_UNKNOWN;

      if (!gst_video_color_matrix_get_Kr_Kb (matrix, &Kr, &Kb)) {
        GST_DEBUG_OBJECT (space,
            "No Kr/Kb for matrix=%d, fallback BT.601", matrix);
        Kr = 0.299;
        Kb = 0.114;
      }
      compute_csc (in_fmt, in_range,
                                   out_fmt, out_range,
                                   Kr, Kb, vsp_info->cached_csc);

      GST_DEBUG_OBJECT (space,
          "CSC pre-computed (in_fmt=0x%02x out_fmt=0x%02x "
          "in_range=%d out_range=%d)",
          in_fmt, out_fmt, in_range, out_range);
    }
  }

  return TRUE;

  /* ERRORS */
format_mismatch:
  {
    GST_ERROR_OBJECT (space, "input and output formats do not match");
    return FALSE;
  }
}

/* Convert a floating-point coefficient to ISU 14-bit two's complement
 * fixed-point (x1024). */
static unsigned int
fp_to_fixed (gdouble val)
{
  gint ival = (gint) ((val * 1024.0) + (val < 0 ? -0.5 : 0.5));
  if (ival < 0)
    ival = 0x4000 + ival;  /* 14-bit two's complement */
  return (unsigned int) (ival & 0x3FFF);
}

/*
 * gst_vspm_filter_compute_csc:
 * @src_fmt:    input format
 * @in_range:   input color range; checked against GST_VIDEO_COLOR_RANGE_16_235
 * to treat as limited range; otherwise treated as full range.
 * @dst_fmt:    output format
 * @out_range:  output color range
 * @Kr:         luma red coefficient   (BT.601: 0.299,  BT.709: 0.2126, BT.2020: 0.2627)
 * @Kb:         luma blue coefficient  (BT.601: 0.114,  BT.709: 0.0722, BT.2020: 0.0593)
 * @csc_par:    [out] filled T_ISU_CSC with k_matrix, offset, clip, csc=ISU_CSC_CUSTOM
 */
static void
compute_csc (guint              src_fmt,
                 GstVideoColorRange in_range,
                 guint              dst_fmt,
                 GstVideoColorRange out_range,
                 gdouble            Kr,
                 gdouble            Kb,
                 T_ISU_CSC         *csc_par)
{
  gdouble Kg = 1.0 - Kr - Kb;

  gboolean in_limited  = (in_range  == GST_VIDEO_COLOR_RANGE_16_235);
  gboolean out_limited = (out_range == GST_VIDEO_COLOR_RANGE_16_235);

  /* Scale factors for range conversion.
   *
   * Y scale  : 219 levels (16-235)
   * C scale  : 224 levels (16-240)
   *
   * For RGB, Y scale is reused because RGB limited range is also
   * represented as [16,235]. Therefore:
   *   - RGB channels use *_y_scale
   *   - *_c_scale is only used for YUV chroma (Cb/Cr)
   */
  gdouble in_y_scale  = in_limited  ? (255.0 / 219.0) : 1.0;
  gdouble in_c_scale  = in_limited  ? (255.0 / 224.0) : 1.0;
  gdouble out_y_scale = out_limited ? (219.0 / 255.0) : 1.0;
  gdouble out_c_scale = out_limited ? (224.0 / 255.0) : 1.0;

  /* 3×3 matrix in mathematical channel order (before ISU column swizzle) */
  gdouble math_m[3][3] = {{0}};

  /* Per-channel offsets in ISU channel order */
  unsigned int in_off[3]  = {0, 0, 0};
  unsigned int out_off[3] = {0, 0, 0};

  guint src_fam = src_fmt & 0xF0;
  guint dst_fam = dst_fmt & 0xF0;

  if (src_fam == YUV_FORMAT && dst_fam != YUV_FORMAT && dst_fam != RAW_FORMAT) {
    /* ── YUV to RGB ── */
    /* Math cols: [Y, Cb, Cr], Math rows: [R, G, B] */
    gdouble base[3][3] = {
      { 1.0,  0.0,                 2.0 * (1.0 - Kr)   },
      { 1.0, -2.0*Kb*(1.0-Kb)/Kg, -2.0*Kr*(1.0-Kr)/Kg },
      { 1.0,  2.0 * (1.0 - Kb),    0.0                },
    };
    gdouble col_s[3] = { in_y_scale, in_c_scale, in_c_scale };
    gdouble row_s[3] = { out_y_scale, out_y_scale, out_y_scale };

    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        math_m[r][c] = base[r][c] * col_s[c] * row_s[r];

    in_off[0] = in_limited ? 0x10 : 0x00;
    in_off[1] = 0x80;
    in_off[2] = 0x80;
    out_off[0] = out_limited ? 0x10 : 0x00;
    out_off[1] = out_limited ? 0x10 : 0x00;
    out_off[2] = out_limited ? 0x10 : 0x00;

  } else if (src_fam != YUV_FORMAT && src_fam != RAW_FORMAT && dst_fam == YUV_FORMAT) {
    /* ── RGB to YUV ── */
    /* Math cols: [R, G, B], Math rows: [Y, Cb, Cr] */
    gdouble base[3][3] = {
      {  Kr,                 Kg,                 Kb                },
      { -Kr/(2.0*(1.0-Kb)), -Kg/(2.0*(1.0-Kb)),  0.5               },
      {  0.5,               -Kg/(2.0*(1.0-Kr)), -Kb/(2.0*(1.0-Kr)) },
    };
    gdouble col_s[3] = { in_y_scale, in_y_scale, in_y_scale };
    gdouble row_s[3] = { out_y_scale, out_c_scale, out_c_scale };

    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        math_m[r][c] = base[r][c] * col_s[c] * row_s[r];

    in_off[0] = in_limited ? 0x10 : 0x00;
    in_off[1] = in_limited ? 0x10 : 0x00;
    in_off[2] = in_limited ? 0x10 : 0x00;
    out_off[0] = out_limited ? 0x10 : 0x00;
    out_off[1] = 0x80;
    out_off[2] = 0x80;

  } else if (src_fam == RAW_FORMAT && dst_fam != RAW_FORMAT && dst_fam != YUV_FORMAT) {
    /* ── RAW to RGB ── */
    gdouble scale = in_y_scale * out_y_scale;
    math_m[0][0] = scale;
    math_m[1][0] = scale;
    math_m[2][0] = scale;

    in_off[0]  = in_limited ? 0x10 : 0x00;
    out_off[0] = out_limited ? 0x10 : 0x00;
    out_off[1] = out_limited ? 0x10 : 0x00;
    out_off[2] = out_limited ? 0x10 : 0x00;

  } else if (src_fam == RAW_FORMAT && dst_fam == YUV_FORMAT) {
    /* ── RAW to YUV ── */
    math_m[0][0] = in_y_scale * out_y_scale;

    in_off[0]  = in_limited ? 0x10 : 0x00;
    out_off[0] = out_limited ? 0x10 : 0x00;
    out_off[1] = 0x80;
    out_off[2] = 0x80;

  } else if (src_fam == YUV_FORMAT && dst_fam == RAW_FORMAT) {
    /* ── YUV to RAW ── */
    math_m[0][0] = in_y_scale * out_y_scale;

    in_off[0] = in_limited ? 0x10 : 0x00;
    in_off[1] = 0x80;
    in_off[2] = 0x80;
    out_off[0] = out_limited ? 0x10 : 0x00;

  } else if (src_fam != YUV_FORMAT && src_fam != RAW_FORMAT && dst_fam == RAW_FORMAT) {
    /* ── RGB to RAW ── */
    gdouble s = in_y_scale * out_y_scale;
    math_m[0][0] = Kr * s;
    math_m[0][1] = Kg * s;
    math_m[0][2] = Kb * s;

    in_off[0] = in_limited ? 0x10 : 0x00;
    in_off[1] = in_limited ? 0x10 : 0x00;
    in_off[2] = in_limited ? 0x10 : 0x00;
    out_off[0] = out_limited ? 0x10 : 0x00;

  } else if (src_fam == dst_fam) {
    /* ── Same color: range-only conversion ── */
    if (src_fam == YUV_FORMAT) {
      math_m[0][0] = in_y_scale * out_y_scale;
      math_m[1][1] = in_c_scale * out_c_scale;
      math_m[2][2] = in_c_scale * out_c_scale;
      in_off[0] = in_limited ? 0x10 : 0x00;
      in_off[1] = 0x80;
      in_off[2] = 0x80;
      out_off[0] = out_limited ? 0x10 : 0x00;
      out_off[1] = 0x80;
      out_off[2] = 0x80;
    } else if (src_fam == RAW_FORMAT) {
      math_m[0][0] = in_y_scale * out_y_scale;
      in_off[0]  = in_limited ? 0x10 : 0x00;
      out_off[0] = out_limited ? 0x10 : 0x00;
    } else {
      /* RGB */
      gdouble scale = in_y_scale * out_y_scale;
      math_m[0][0] = scale;
      math_m[1][1] = scale;
      math_m[2][2] = scale;
      for (int i = 0; i < 3; i++) {
        in_off[i]  = in_limited ? 0x10 : 0x00;
        out_off[i] = out_limited ? 0x10 : 0x00;
      }
    }
  }

  /* ── Column swizzle: math order to ISU hardware column order ──
   *
   * ISU column mapping (input side):
   *   YUV input: matrix columns = [Y(layer1), Cr(layer3), Cb(layer2)]
   *   RGB input: matrix columns = [R(layer1), B(layer3), G(layer2)]
   * Both are: [col0=math0, col1=math2, col2=math1] — swap cols 1 & 2.
   */
  int col_map[3] = {0, 2, 1};
  /* Encode k_matrix to fixed-point */
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      csc_par->k_matrix[r][c] = fp_to_fixed (math_m[r][col_map[c]]);

  /* Encode offsets */
  for (int ch = 0; ch < 3; ch++) {
    csc_par->offset[ch][0] = in_off[ch];
    csc_par->offset[ch][1] = out_off[ch];
  }

  /* Encode clip based on output range and format */
  if (out_limited) {
    if (dst_fam == YUV_FORMAT) {
      /* Y: [16,235] */
      csc_par->clip[0][0] = 0x10;
      csc_par->clip[0][1] = 0xEB;
      /* Cb: [16,240] */
      csc_par->clip[1][0] = 0x10;
      csc_par->clip[1][1] = 0xF0;
      /* Cr: [16,240] */
      csc_par->clip[2][0] = 0x10;
      csc_par->clip[2][1] = 0xF0;
    } else {
      for (int ch = 0; ch < 3; ch++) {
        /* RGB/RAW limited: [16,235] */
        csc_par->clip[ch][0] = 0x10;
        csc_par->clip[ch][1] = 0xEB;
      }
    }
  } else {
    for (int ch = 0; ch < 3; ch++) {
      /* Full: [0,255] */
      csc_par->clip[ch][0] = 0x00;
      csc_par->clip[ch][1] = 0xFF;
    }
  }

  csc_par->csc = ISU_CSC_CUSTOM;
}

static GstFlowReturn
transform_frame_options (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame,
    VSPM_IP_PAR * ip_par, gboolean * submit)
{
  GstVspmFilter *space;
  GstVspmFilterVspInfo *vsp_info;
  GstVspmFilterIsuParams *params;

  VSPM_ISU_PAR isu_par;

  T_ISU_IN src_par;
  T_ISU_ALPHA src_alpha_par, dst_alpha_par;
  T_ISU_OUT dst_par;
  T_ISU_RS rs_par;

  gint in_width, in_height;
  gint out_width, out_height;
  gint irc;

  int i;
  GstFlowReturn ret;
  gint stride[GST_VIDEO_MAX_PLANES];
  gsize offset[GST_VIDEO_MAX_PLANES];
  gint offs, plane_size;
  const GstVideoFormatInfo * vspm_in_vinfo;
  const GstVideoFormatInfo * vspm_out_vinfo;
  void *src_addr[3] = { 0 };
  void *dst_addr[3] = { 0 };
  guint in_n_planes, out_n_planes;

  space = GST_VIDEO_CONVERT_CAST (filter);
  vsp_info = space->vsp_info;
  params = &space->ip_params.isu;

  /* The frame is submitted by the core after this call returns; the W/A
   * paths below clear it to skip the frame without touching the hardware. */
  *submit = TRUE;

  GST_CAT_DEBUG_OBJECT (GST_CAT_PERFORMANCE, filter,
      "doing colorspace conversion from %s -> to %s",
      GST_VIDEO_INFO_NAME (&filter->in_info),
      GST_VIDEO_INFO_NAME (&filter->out_info));

  vsp_info->gst_format_in = GST_VIDEO_FRAME_FORMAT (in_frame);
  vsp_info->in_width = GST_VIDEO_FRAME_COMP_WIDTH (in_frame, 0);
  vsp_info->in_height = GST_VIDEO_FRAME_COMP_HEIGHT (in_frame, 0);

  vsp_info->gst_format_out = GST_VIDEO_FRAME_FORMAT (out_frame);
  vsp_info->out_width = GST_VIDEO_FRAME_COMP_WIDTH (out_frame, 0);
  vsp_info->out_height = GST_VIDEO_FRAME_COMP_HEIGHT (out_frame, 0);

  if (vsp_info->format_flag == 0) {
    irc = set_colorspace (GST_VIDEO_FRAME_FORMAT (in_frame), &vsp_info->in_format, &vsp_info->in_swapbit);
    if (irc != 0) {
      GST_ERROR("input format is non-support.\n");
      ret = GST_FLOW_ERROR;
      goto err;
    }

    irc = set_colorspace_output (GST_VIDEO_FRAME_FORMAT (out_frame), &vsp_info->out_format, &vsp_info->out_swapbit);
    if (irc != 0) {
      GST_ERROR("output format is non-support.\n");
      ret = GST_FLOW_ERROR;
      goto err;
    }
    vsp_info->format_flag = 1;
  }

  in_width = vsp_info->in_width;
  in_height = vsp_info->in_height;
  vspm_in_vinfo = gst_video_format_get_info (vsp_info->gst_format_in);

  out_width = vsp_info->out_width;
  out_height = vsp_info->out_height;
  vspm_out_vinfo = gst_video_format_get_info (vsp_info->gst_format_out);

  in_n_planes = GST_VIDEO_FORMAT_INFO_N_PLANES(vspm_in_vinfo);
  out_n_planes = GST_VIDEO_FORMAT_INFO_N_PLANES(vspm_out_vinfo);

  if (gst_vspm_filter_get_mem_phys_addr (space, in_frame->buffer,
          in_frame->data[0], 0, in_frame->info.offset[0], &src_addr[0]) != GST_FLOW_OK) {
    GST_ERROR_OBJECT (space, "no physical address for input plane 0");
    ret = GST_FLOW_ERROR;
    goto err;
  }

  if (gst_vspm_filter_get_mem_phys_addr (space, out_frame->buffer,
          out_frame->data[0], 0, out_frame->info.offset[0], &dst_addr[0]) != GST_FLOW_OK) {
    GST_ERROR_OBJECT (space, "no physical address for output plane 0");
    ret = GST_FLOW_ERROR;
    goto err;
  }

  if (in_n_planes >= 2) {
    if (gst_vspm_filter_get_mem_phys_addr (space, in_frame->buffer,
            in_frame->data[1], 1, in_frame->info.offset[1], &src_addr[1]) != GST_FLOW_OK) {
      GST_ERROR_OBJECT (space,
          "no physical address for input plane 1");
      ret = GST_FLOW_ERROR;
      goto err;
    }
  }

  if (out_n_planes >= 2) {
    if (gst_vspm_filter_get_mem_phys_addr (space, out_frame->buffer,
            out_frame->data[1], 1, out_frame->info.offset[1], &dst_addr[1]) != GST_FLOW_OK) {
      GST_ERROR_OBJECT (space,
          "no physical address for output plane 1");
      ret = GST_FLOW_ERROR;
      goto err;
    }
  }

  if (in_n_planes >= 3 || out_n_planes >= 3) {
    GST_ERROR("ISU hardware does not support number plane > 2\n");
    ret = GST_FLOW_ERROR;
    goto err;
  }

  if (!src_addr[0] || !dst_addr[0] ||
      (in_n_planes >= 2 && !src_addr[1]) ||
      (out_n_planes >= 2 && !dst_addr[1])) {
    /* W/A: Sometimes we can not convert virtual address to physical address,
     * we should skip this frame to avoid issue with HW processor.
     */
    *submit = FALSE;
    ret = GST_FLOW_OK;
    goto err;
  }

  {
    /* Setting input parameters */
    src_alpha_par.asel    = 0;
    src_alpha_par.anum0   = 0;
    src_alpha_par.anum1   = 0;
    src_alpha_par.anum2   = 0;
    src_alpha_par.athres0 = 0;
    src_alpha_par.athres1 = 0;

    src_par.addr          = src_addr[0];
    src_par.stride        = in_frame->info.stride[0];
    if (in_n_planes >= 2) {
      src_par.addr_c      = src_addr[1];
      src_par.stride_c    = in_frame->info.stride[1];
    } else {
      src_par.addr_c      = 0;
      src_par.stride_c    = 0;
    }
    src_par.width         = in_width;
    src_par.height        = in_height;
    src_par.format        = vsp_info->in_format;
    src_par.swap          = vsp_info->in_swapbit;
    src_par.td            = NULL;
    src_par.alpha         = &src_alpha_par;
    src_par.uv_conv       = 0;
  }

  {
    /* Setting output parameters */
    dst_par.addr          = dst_addr[0];
    dst_par.stride        = out_frame->info.stride[0];
    if (out_n_planes >= 2) {
      dst_par.addr_c      = dst_addr[1];
      dst_par.stride_c    = out_frame->info.stride[1];
    } else {
      dst_par.addr_c      = 0;
      dst_par.stride_c    = 0;
    }
    dst_par.format        = vsp_info->out_format;
    dst_par.swap          = vsp_info->out_swapbit;
    /* Set csc for color convert */
    dst_par.csc           = (T_ISU_CSC *) vsp_info->cached_csc;
    dst_par.alpha         = &src_alpha_par;
  }

  {
    /* Setting resize parameters */
    guint crop_start_x   = 0;
    guint crop_start_y   = 0;
    guint crop_in_width  = in_width;
    guint crop_in_height = in_height;
    guint c_left, c_right, c_top, c_bottom;
    gdouble scale_x      = 0;
    gdouble scale_y      = 0;

    memset(&rs_par, 0, sizeof(T_ISU_RS));

    if (gst_vspm_filter_get_crop_value (space, &c_left, &c_right, &c_top, &c_bottom)) {
      /* Validate the requested borders against the input */
      if ((c_left + c_right) < (guint) in_width &&
          (c_top + c_bottom) < (guint) in_height) {
        crop_start_x   = c_left;
        crop_start_y   = c_top;
        crop_in_width  = in_width  - c_left - c_right;
        crop_in_height = in_height - c_top  - c_bottom;

        GST_DEBUG_OBJECT (space,
            "crop: start(%u,%u) cropped size(%ux%u) from input(%dx%d)",
            crop_start_x, crop_start_y, crop_in_width, crop_in_height,
            in_width, in_height);
      } else {
        /* Out of range for this input. configure_crop() already warned on the
         * bus, so log quietly here rather than repeat it for every buffer. */
        GST_DEBUG_OBJECT (space,
            "crop %u:%u:%u:%u does not fit %dx%d input, skipping",
            c_left, c_right, c_top, c_bottom, in_width, in_height);
      }
    }

    rs_par.start_x        = crop_start_x;
    rs_par.start_y        = crop_start_y;
    rs_par.tune_x         = 0;
    rs_par.tune_y         = 0;
    rs_par.crop_w         = out_width;
    rs_par.crop_h         = out_height;
    rs_par.pad_mode       = 0;
    rs_par.pad_val        = 0;
    rs_par.x_ratio        = (unsigned short)( (crop_in_width << 12) / out_width );
    rs_par.y_ratio        = (unsigned short)( (crop_in_height << 12) / out_height );
    scale_x               = (gdouble)4096 / rs_par.x_ratio;
    scale_y               = (gdouble)4096 / rs_par.y_ratio;

    GST_DEBUG_OBJECT(space,
        "resize: horizontal x %.3f, vertical x %.3f", scale_x, scale_y);
  }

  {
    /* Update all settings */
    isu_par.src_par       = &src_par;
    isu_par.dst_par       = &dst_par;
    isu_par.rs_par        = &rs_par;
  }

  /* Store the parameters in the instance union and re-point the internal
   * references: the core submits ip_par to VSPM_lib_Entry() after this
   * call returns, so the parameter blocks must outlive this stack. */
  params->start     = isu_par;
  params->src       = src_par;
  params->src_alpha = src_alpha_par;
  params->dst       = dst_par;
  params->rs        = rs_par;

  params->start.src_par = &params->src;
  params->start.dst_par = &params->dst;
  params->start.rs_par  = &params->rs;
  params->src.alpha     = &params->src_alpha;
  params->dst.alpha     = &params->src_alpha;

  memset(ip_par, 0, sizeof(VSPM_IP_PAR));
  ip_par->uhType             = VSPM_TYPE_ISU_AUTO;
  ip_par->unionIpParam.ptisu = &params->start;

  ret = GST_FLOW_OK;
err:
  return ret;
}

const GstVspmFilterOps isu_ops = {
  transform_frame_options,
  set_info,
  set_colorspace,
  set_colorspace_output,
  set_buffer_info,
  exts_isu,
  exts_isu_out,
};
