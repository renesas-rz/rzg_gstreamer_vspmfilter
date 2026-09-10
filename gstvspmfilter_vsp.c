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

/* VSP platform implementation -- auto-detected at init, dispatched via ops */

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

static gint set_colorspace (GstVideoFormat vid_fmt, guint * format, guint * fswap);
static gint set_colorspace_output (GstVideoFormat vid_fmt, guint * format, guint * fswap);

/* Note that below swap information will be REVERSED later (in function
 *     set_colorspace) because current system use Little Endian */
static const extensions_t exts_vsp[] = {
  {GST_VIDEO_FORMAT_NV12,  VSP_IN_YUV420_SEMI_NV12,  VSP_SWAP_NO},    /* NV12 format is highest priority as most modules support this */
  {GST_VIDEO_FORMAT_I420,  VSP_IN_YUV420_PLANAR,     VSP_SWAP_NO},    /* I420 is second priority */
  {GST_VIDEO_FORMAT_Y42B,  VSP_IN_YUV422_PLANAR,     VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_YUY2,  VSP_IN_YUV422_INT0_YUY2,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_UYVY,  VSP_IN_YUV422_INT0_UYVY,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGBx,  VSP_IN_RGBA8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_BGRx,  VSP_IN_ARGB8888,          VSP_SWAP_B | VSP_SWAP_W},  /* Not supported in VSP. Use ARGB8888, and swap ARGB -> RABG -> BGRA */
  {GST_VIDEO_FORMAT_xRGB,  VSP_IN_ARGB8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_xBGR,  VSP_IN_ABGR8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGBA,  VSP_IN_RGBA8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_BGRA,  VSP_IN_ARGB8888,          VSP_SWAP_B | VSP_SWAP_W},  /* Same as BGRA */
  {GST_VIDEO_FORMAT_ARGB,  VSP_IN_ARGB8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_ABGR,  VSP_IN_ABGR8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGB ,  VSP_IN_RGB888,            VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_BGR ,  VSP_IN_BGR888,            VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_YVYU,  VSP_IN_YUV422_INT0_YVYU,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_Y444,  VSP_IN_YUV444_PLANAR,     VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV21,  VSP_IN_YUV420_SEMI_NV21,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_v308,  VSP_IN_YUV444_INTERLEAVED,VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGB16, VSP_IN_RGB565,            VSP_SWAP_B},
  {GST_VIDEO_FORMAT_NV16,  VSP_IN_YUV422_SEMI_NV16,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV24,  VSP_IN_YUV444_SEMI_PLANAR,VSP_SWAP_NO},
};


static const extensions_t exts_vsp_out[] = {
  {GST_VIDEO_FORMAT_NV12,  VSP_OUT_YUV420_SEMI_NV12,  VSP_SWAP_NO},    /* NV12 format is highest priority as most modules support this */
  {GST_VIDEO_FORMAT_I420,  VSP_OUT_YUV420_PLANAR,     VSP_SWAP_NO},    /* I420 is second priority */
  {GST_VIDEO_FORMAT_Y42B,  VSP_OUT_YUV422_PLANAR,     VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_YUY2,  VSP_OUT_YUV422_INT0_YUY2,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_UYVY,  VSP_OUT_YUV422_INT0_UYVY,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGBx,  VSP_OUT_RGBP8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_BGRx,  VSP_OUT_PRGB8888,          VSP_SWAP_B | VSP_SWAP_W},  /* Not supported in VSP. Use ARGB8888, and swap ARGB -> RABG -> BGRA */
  {GST_VIDEO_FORMAT_xRGB,  VSP_OUT_PRGB8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_xBGR,  VSP_OUT_PRGB8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGBA,  VSP_OUT_RGBP8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_BGRA,  VSP_OUT_PRGB8888,          VSP_SWAP_B | VSP_SWAP_W},  /* Same as BGRA */
  {GST_VIDEO_FORMAT_ARGB,  VSP_OUT_PRGB8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_ABGR,  VSP_OUT_PBGR8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGB ,  VSP_OUT_RGB888,            VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_BGR ,  VSP_OUT_BGR888,            VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_YVYU,  VSP_OUT_YUV422_INT0_YVYU,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_Y444,  VSP_OUT_YUV444_PLANAR,     VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV21,  VSP_OUT_YUV420_SEMI_NV21,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_v308,  VSP_OUT_YUV444_INTERLEAVED,VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGB16, VSP_OUT_RGB565,            VSP_SWAP_B},
  {GST_VIDEO_FORMAT_NV16,  VSP_OUT_YUV422_SEMI_NV16,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV24,  VSP_OUT_YUV444_SEMI_PLANAR,VSP_SWAP_NO},
};

static gint
set_colorspace (GstVideoFormat vid_fmt, guint * format, guint * fswap)
{
  int nr_exts = sizeof (exts_vsp) / sizeof (exts_vsp[0]);
  int i;

  for (i = 0; i < nr_exts; i++) {
    if (vid_fmt == exts_vsp[i].gst_format) {
      *format = exts_vsp[i].hw_format;

      /* Need to reverse swap information for Little Endian */
      *fswap  = (VSP_SWAP_B | VSP_SWAP_W | VSP_SWAP_L | VSP_SWAP_LL) ^ exts_vsp[i].hw_swap;
      return 0;
    }
  }
  return -1;
}

static gint
set_colorspace_output (GstVideoFormat vid_fmt, guint * format, guint * fswap)
{
  int nr_exts = sizeof (exts_vsp_out) / sizeof (exts_vsp_out[0]);
  int i;

  for (i = 0; i < nr_exts; i++) {
    if (vid_fmt == exts_vsp_out[i].gst_format) {
      *format = exts_vsp_out[i].hw_format;

      /* Need to reverse swap information for Little Endian */
      *fswap  = (VSP_SWAP_B | VSP_SWAP_W | VSP_SWAP_L | VSP_SWAP_LL) ^ exts_vsp_out[i].hw_swap;
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

    buf_info->plane_offset[i] = buf_info->buf_size;

    /* If we have alignment requirement from downstream */
    if (align) {
      /* FIXME: Currently, we ignore padding and only update stride */

      /* According to the implementation of Gstreamer, stride_align, logically,
       * must be a number equal to 2^N-1 instead of 2^N. Downstream proposes
       * alignment as 2^N in the older versions and 2^N-1 in the new version.
       * So, we should round the alignment up before using to get the same
       * result for both cases */
      stride = GST_ROUND_UP_N(stride, GST_ROUND_UP_2(align->stride_align[i]));
    } else {
      GST_DEBUG_OBJECT (space, "No stride alignment requirement from downstream");
    }
    buf_info->plane_stride[i] = stride;
    buf_info->plane_size[i] = stride * sliceheight;

    buf_info->buf_size += buf_info->plane_size[i];
  }
  return;
}
static GstFlowReturn
transform_frame_options (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame,
    VSPM_IP_PAR * ip_par, gboolean * submit)
{
  GstVspmFilter *space;
  GstVspmFilterVspInfo *vsp_info;
  GstVspmFilterVspParams *params;

  VSPM_VSP_PAR vsp_par;

  T_VSP_IN src_par;
  T_VSP_ALPHA src_alpha_par;
  T_VSP_OUT dst_par;
  T_VSP_CTRL ctrl_par;
  T_VSP_UDS uds_par;

  gint in_width, in_height;
  gint out_width, out_height;
  gint irc;
  unsigned long use_module;

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
  params = &space->ip_params.vsp;

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

  memset(&ctrl_par, 0, sizeof(T_VSP_CTRL));

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

  guint crop_start_x   = 0;
  guint crop_start_y   = 0;
  guint crop_in_width  = in_width;
  guint crop_in_height = in_height;
  guint c_left, c_right, c_top, c_bottom;

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

  if (((gint) crop_in_width == out_width) &&
      ((gint) crop_in_height == out_height)) {
    use_module = 0;
  } else {
    /* UDS scaling */
    use_module = VSP_UDS_USE;
  }

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

  if (in_n_planes >= 3) {
    if (gst_vspm_filter_get_mem_phys_addr (space, in_frame->buffer,
            in_frame->data[2], 2, in_frame->info.offset[2], &src_addr[2]) != GST_FLOW_OK) {
      GST_ERROR_OBJECT (space,
          "no physical address for input plane 2");
      ret = GST_FLOW_ERROR;
      goto err;
    }
  }

  if (out_n_planes >= 3) {
    if (gst_vspm_filter_get_mem_phys_addr (space, out_frame->buffer,
            out_frame->data[2], 2, out_frame->info.offset[2], &dst_addr[2]) != GST_FLOW_OK) {
      GST_ERROR_OBJECT (space,
          "no physical address for output plane 2");
      ret = GST_FLOW_ERROR;
      goto err;
    }
  }

  if (!src_addr[0] || !dst_addr[0] ||
      ((in_n_planes >= 2 && !src_addr[1]) || (out_n_planes >= 2 && !dst_addr[1])) ||
      ((in_n_planes >= 3 && !src_addr[2]) || (out_n_planes >= 3 && !dst_addr[2]))) {
    /* W/A: Sometimes we can not convert virtual address to physical address,
     * we should skip this frame to avoid issue with HW processor.
     */
    *submit = FALSE;
    ret = GST_FLOW_OK;
    goto err;
  }

  {
    /* Setting input parameters */
    src_alpha_par.addr_a   = NULL;
    src_alpha_par.alphan   = VSP_ALPHA_NO;
    src_alpha_par.alpha1   = 0;
    src_alpha_par.alpha2   = 0;
    src_alpha_par.astride  = 0;
    src_alpha_par.aswap    = VSP_SWAP_NO;
    src_alpha_par.asel     = VSP_ALPHA_NUM5;
    src_alpha_par.aext     = VSP_AEXT_EXPAN;
    src_alpha_par.anum0    = 0;
    src_alpha_par.anum1    = 0;
    src_alpha_par.afix     = 0xff;
    src_alpha_par.irop     = VSP_IROP_NOP;
    src_alpha_par.msken    = VSP_MSKEN_ALPHA;
    src_alpha_par.bsel     = 0;
    src_alpha_par.mgcolor  = 0;
    src_alpha_par.mscolor0 = 0;
    src_alpha_par.mscolor1 = 0;

    src_par.addr           = src_addr[0];
    src_par.addr_c0        = src_addr[1];
    src_par.addr_c1        = src_addr[2];
    src_par.stride         = in_frame->info.stride[0];
    src_par.stride_c       = in_frame->info.stride[1];
    src_par.csc            = VSP_CSC_OFF;  /* do not convert colorspace */
    src_par.width          = crop_in_width;
    src_par.height         = crop_in_height;
    src_par.width_ex       = 0;
    src_par.height_ex      = 0;
    src_par.x_offset       = crop_start_x;
    src_par.y_offset       = crop_start_y;
    src_par.format         = vsp_info->in_format;
    src_par.swap           = vsp_info->in_swapbit;
    src_par.x_position     = 0;
    src_par.y_position     = 0;
    src_par.pwd            = VSP_LAYER_PARENT;
    src_par.cipm           = VSP_CIPM_0_HOLD;
    src_par.cext           = VSP_CEXT_EXPAN;
    src_par.iturbt         = VSP_ITURBT_709;
    src_par.clrcng         = VSP_ITU_COLOR;
    src_par.vir            = VSP_NO_VIR;
    src_par.vircolor       = 0x00000000;
    src_par.osd_lut        = NULL;
    src_par.alpha_blend    = &src_alpha_par;
    src_par.clrcnv         = NULL;
    src_par.connect        = use_module;
  }

  {
    /* Setting output parameters */
    dst_par.addr           = dst_addr[0];
    dst_par.addr_c0        = dst_addr[1];
    dst_par.addr_c1        = dst_addr[2];
    dst_par.stride         = out_frame->info.stride[0];
    dst_par.stride_c       = out_frame->info.stride[1];

    /* convert if format in and out different in color space */
    if (!GST_VIDEO_FORMAT_INFO_IS_YUV(vspm_in_vinfo) != !GST_VIDEO_FORMAT_INFO_IS_YUV(vspm_out_vinfo)) {
      dst_par.csc          = VSP_CSC_ON;
    } else {
      dst_par.csc          = VSP_CSC_OFF;
    }

    dst_par.width          = out_width;
    dst_par.height         = out_height;
    dst_par.x_offset       = 0;
    dst_par.y_offset       = 0;
    dst_par.format         = vsp_info->out_format;
    dst_par.pxa            = VSP_PAD_P;
    dst_par.pad            = 0xff;
    dst_par.x_coffset      = 0;
    dst_par.y_coffset      = 0;
    dst_par.iturbt         = VSP_ITURBT_709;
    dst_par.clrcng         = VSP_ITU_COLOR;
    dst_par.cbrm           = VSP_CSC_ROUND_DOWN;
    dst_par.abrm           = VSP_CONVERSION_ROUNDDOWN;
    dst_par.athres         = 0;
    dst_par.clmd           = VSP_CLMD_NO;
    dst_par.dith           = VSP_NO_DITHER;
    dst_par.swap           = vsp_info->out_swapbit;
  }

  {
    /* Setting resize parameters */
    if (use_module == VSP_UDS_USE) {
      /* Set T_VSP_UDS. */
      ctrl_par.uds         = &uds_par;

      memset(&uds_par, 0, sizeof(T_VSP_UDS));
      uds_par.fmd          = VSP_FMD_NO;
      uds_par.filcolor     = 0x0000FF00; /* green */
      uds_par.amd          = VSP_AMD;
      uds_par.clip         = VSP_CLIP_OFF;
      uds_par.alpha        = VSP_ALPHA_ON;
      uds_par.complement   = VSP_COMPLEMENT_BIL;
      uds_par.athres0      = 0;
      uds_par.athres1      = 0;
      uds_par.anum0        = 0;
      uds_par.anum1        = 0;
      uds_par.anum2        = 0;
      uds_par.x_ratio      = (unsigned short)( (crop_in_width << 12) / out_width );
      uds_par.y_ratio      = (unsigned short)( (crop_in_height << 12) / out_height );
      uds_par.out_cwidth   = (unsigned short)out_width;
      uds_par.out_cheight  = (unsigned short)out_height;
      uds_par.connect      = 0;
    }
  }

  {
    /* Update all settings */
    vsp_par.rpf_num        = 1;
    vsp_par.use_module     = use_module;
    vsp_par.src1_par       = &src_par;
    vsp_par.src2_par       = NULL;
    vsp_par.src3_par       = NULL;
    vsp_par.src4_par       = NULL;
    vsp_par.dst_par        = &dst_par;
    vsp_par.ctrl_par       = &ctrl_par;
  }

  /* Store the parameters in the instance union and re-point the internal
   * references: the core submits ip_par to VSPM_lib_Entry() after this
   * call returns, so the parameter blocks must outlive this stack. */
  params->start     = vsp_par;
  params->src       = src_par;
  params->src_alpha = src_alpha_par;
  params->dst       = dst_par;
  params->ctrl      = ctrl_par;
  if (use_module == VSP_UDS_USE)
    params->uds     = uds_par;

  params->start.src1_par  = &params->src;
  params->start.dst_par   = &params->dst;
  params->start.ctrl_par  = &params->ctrl;
  params->src.alpha_blend = &params->src_alpha;
  if (ctrl_par.uds)
    params->ctrl.uds      = &params->uds;

  memset(ip_par, 0, sizeof(VSPM_IP_PAR));
  ip_par->uhType             = VSPM_TYPE_VSP_AUTO;
  ip_par->unionIpParam.ptVsp = &params->start;

  ret = GST_FLOW_OK;
err:
  return ret;
}

static gboolean
set_info (GstVideoFilter * filter,
    GstCaps * incaps, GstVideoInfo * in_info, GstCaps * outcaps,
    GstVideoInfo * out_info)
{
  /* VSP needs no platform-specific set_info work */
  return TRUE;
}

const GstVspmFilterOps vsp_ops = {
  transform_frame_options,
  set_info,
  set_colorspace,
  set_colorspace_output,
  set_buffer_info,
  exts_vsp,
  exts_vsp_out,
};
