/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * This file:
 * Copyright (C) 2003 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2010 David Schleef <ds@schleef.org>
 * Copyright (C) 2014-2020 Renesas Electronics Corporation
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

/**
 * SECTION:element-vspmfilter
 *
 * Convert video frames between a great variety of video formats.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc ! video/x-raw,format=\(string\)YUY2 ! vspmfilter ! ximagesink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gstvspmfilter.h"

#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <gst/gstquery.h>

#include <string.h>
#include <stdio.h>

#include "vspm_public.h"
#include "mmngr_user_public.h"
#include "mmngr_buf_user_public.h"

GST_DEBUG_CATEGORY (vspmfilter_debug);
#define GST_CAT_DEFAULT vspmfilter_debug
GST_DEBUG_CATEGORY_EXTERN (GST_CAT_PERFORMANCE);

#define VSP_FORMAT_PIXEL_MASK	(0x0f00)
#define VSP_FORMAT_PIXEL_BIT	(8)
#define VSPM_BUFFERS	3

GType gst_vspm_filter_get_type (void);

static GQuark _colorspace_quark;

#define gst_vspm_filter_parent_class parent_class
G_DEFINE_TYPE (GstVspmFilter, gst_vspm_filter, GST_TYPE_VIDEO_FILTER);
G_DEFINE_TYPE (GstVspmFilterBufferPool, gst_vspmfilter_buffer_pool, GST_TYPE_BUFFER_POOL);
#define CLEAR(x) memset (&(x), 0, sizeof (x))

static void gst_vspm_filter_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_vspm_filter_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static GstFlowReturn
gst_vspm_filter_transform_buffer (GstVideoFilter * filter,
                                    GstBuffer * inbuf,
                                    GstBuffer * outbuf);

static gboolean gst_vspm_filter_set_info (GstVideoFilter * filter,
    GstCaps * incaps, GstVideoInfo * in_info, GstCaps * outcaps,
    GstVideoInfo * out_info);
static GstFlowReturn gst_vspm_filter_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame);

static void gst_vspm_filter_finalize (GObject * obj);

static void gst_vspm_filter_import_fd (GstMemory *mem, gpointer *out, GQueue *import_list);
static void gst_vspm_filter_release_fd (GQueue *import_list);

struct _GstBaseTransformPrivate
{
  /* Set by sub-class */
  gboolean passthrough;
  gboolean always_in_place;

  GstCaps *cache_caps1;
  gsize cache_caps1_size;
  GstCaps *cache_caps2;
  gsize cache_caps2_size;
  gboolean have_same_caps;

  gboolean negotiated;

  /* QoS *//* with LOCK */
  gboolean qos_enabled;
  gdouble proportion;
  GstClockTime earliest_time;
  /* previous buffer had a discont */
  gboolean discont;

  GstPadMode pad_mode;

  gboolean gap_aware;
  gboolean prefer_passthrough;

  /* QoS stats */
  guint64 processed;
  guint64 dropped;

  GstClockTime position_out;

  GstBufferPool *pool;
  gboolean pool_active;
  GstAllocator *allocator;
  GstAllocationParams params;
  GstQuery *query;
};

/* Properties */
enum
{
  PROP_0,
  PROP_VSPM_OUTBUF,
  PROP_VSPM_DMABUF
};

static gboolean
gst_vspmfilter_buffer_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstVspmFilterBufferPool *vspmfltpool = GST_VSPMFILTER_BUFFER_POOL_CAST (pool);
  GstCaps *caps;
  gint width, height;
  guint size = 0;
  guint min_buffers;
  guint max_buffers;
  GstVideoInfo info;
  gint stride[GST_VIDEO_MAX_PLANES] = { 0 };
  guint i;

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, &min_buffers,
          &max_buffers)) {
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }

  if (!gst_video_info_from_caps (&info, caps)) {
    GST_WARNING_OBJECT (pool, "failed getting video info from caps %"
        GST_PTR_FORMAT, caps);
    return FALSE;
  }

  width = GST_VIDEO_INFO_WIDTH (&info);
  height = GST_VIDEO_INFO_HEIGHT (&info);

  for (i=0; i < GST_VIDEO_FORMAT_INFO_N_PLANES(info.finfo); i++) {
    stride[i] = GST_VIDEO_FORMAT_INFO_PSTRIDE(info.finfo, i) *
                  GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (info.finfo, i, width);
    size += stride[i] *
      GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (info.finfo, i, height);
  }

  gst_buffer_pool_config_set_params (config, caps, size, min_buffers,
      max_buffers);
  vspmfltpool->caps = gst_caps_ref (caps);

  return GST_BUFFER_POOL_CLASS (gst_vspmfilter_buffer_pool_parent_class)->set_config
      (pool, config);
}

static void
gst_vspmfilter_buffer_pool_free_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
  /* No processing */
}

static GstFlowReturn
gst_vspmfilter_buffer_pool_alloc_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstVspmFilterBufferPool *vspmfltpool = GST_VSPMFILTER_BUFFER_POOL_CAST (bpool);
  GstVspmFilter * vspmfilter = vspmfltpool->vspmfilter;
  GstBuffer *tmp;
  GstFlowReturn result = GST_FLOW_OK;
  VspmbufArray *vspm_outbuf;

  vspm_outbuf = vspmfilter->vspm_outbuf;
  tmp = g_ptr_array_index (vspm_outbuf->buf_array, vspm_outbuf->current_buffer_index);
  *buffer = tmp;

  vspm_outbuf->current_buffer_index ++;
  if(vspm_outbuf->current_buffer_index >= vspm_outbuf->buf_array->len)
      vspm_outbuf->current_buffer_index = 0;

  return result;
}

static GstBufferPool *
gst_vspmfilter_buffer_pool_new (GstVspmFilter * vspmfilter)
{
  GstVspmFilterBufferPool *pool;

  g_return_val_if_fail (GST_IS_VIDEO_CONVERT(vspmfilter), NULL);
  pool = g_object_new (GST_TYPE_VSPMFILTER_BUFFER_POOL, NULL);
  pool->vspmfilter = gst_object_ref (vspmfilter);

  GST_LOG_OBJECT (pool, "new vspmfilter buffer pool %p", pool);

  return GST_BUFFER_POOL_CAST (pool);
}

static void
gst_vspmfilter_buffer_pool_finalize (GObject * object)
{
  GstVspmFilterBufferPool *pool = GST_VSPMFILTER_BUFFER_POOL_CAST (object);

  if (pool->caps)
    gst_caps_unref (pool->caps);
  gst_object_unref (pool->vspmfilter);

  G_OBJECT_CLASS (gst_vspmfilter_buffer_pool_parent_class)->finalize (object);
}

static void
gst_vspmfilter_buffer_pool_init (GstVspmFilterBufferPool * pool)
{
  /* No processing */
}

static void
gst_vspmfilter_buffer_pool_class_init (GstVspmFilterBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gobject_class->finalize = gst_vspmfilter_buffer_pool_finalize;
  gstbufferpool_class->alloc_buffer = gst_vspmfilter_buffer_pool_alloc_buffer;
  gstbufferpool_class->set_config = gst_vspmfilter_buffer_pool_set_config;
  gstbufferpool_class->free_buffer = gst_vspmfilter_buffer_pool_free_buffer;
}

/* copies the given caps */
static GstCaps *
gst_vspm_filter_caps_remove_format_info (GstCaps * caps)
{
  GstStructure *st;
  gint i, n;
  GstCaps *res;

  res = gst_caps_new_empty ();

  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    st = gst_caps_get_structure (caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure (res, st))
      continue;

    st = gst_structure_copy (st);
    gst_structure_remove_fields (st, "format",
        "colorimetry", "chroma-site", NULL);

    gst_caps_append_structure (res, st);
  }

  return res;
}

static GstCaps *
gst_vspm_filter_fixate_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstCaps *result;
  gint from_w, from_h;
  gint w = 0, h = 0;
  GstStructure *ins, *outs;

  GST_DEBUG_OBJECT (trans, "caps %" GST_PTR_FORMAT, caps);
  GST_DEBUG_OBJECT (trans, "othercaps %" GST_PTR_FORMAT, othercaps);

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);

  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);

  gst_structure_get_int (ins, "width", &from_w);
  gst_structure_get_int (ins, "height", &from_h);

  gst_structure_get_int (outs, "width", &w);
  gst_structure_get_int (outs, "height", &h);

  if (!w || !h) {
    gst_structure_fixate_field_nearest_int (outs, "height", from_h);
    gst_structure_fixate_field_nearest_int (outs, "width", from_w);
  }

  result = gst_caps_intersect (othercaps, caps);
  if (gst_caps_is_empty (result)) {
    gst_caps_unref (result);
    result = othercaps;
  } else {
    gst_caps_unref (othercaps);
  }

  /* fixate remaining fields */
  result = gst_caps_fixate (result);

  GST_DEBUG_OBJECT (trans, "result caps %" GST_PTR_FORMAT, result);

  return result;
}

static gboolean
gst_vspm_filter_filter_meta (GstBaseTransform * trans, GstQuery * query,
    GType api, const GstStructure * params)
{
  /* propose all metadata upstream */
  return TRUE;
}

/* The caps can be transformed into any other caps with format info removed.
 * However, we should prefer passthrough, so if passthrough is possible,
 * put it first in the list. */
static GstCaps *
gst_vspm_filter_transform_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *tmp, *tmp2;
  GstCaps *result;
  GstCaps *caps_full_range_sizes;
  GstStructure *structure;
  gint i, n;

  /* Get all possible caps that we can transform to */
  tmp = gst_vspm_filter_caps_remove_format_info (caps);

  caps_full_range_sizes = gst_caps_new_empty ();
  n = gst_caps_get_size (tmp);
  for (i = 0; i < n; i++) {
    structure = gst_caps_get_structure (tmp, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure (caps_full_range_sizes,
            structure))
      continue;

    /* make copy */
    structure = gst_structure_copy (structure);
    gst_structure_set (structure,
        "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);

    gst_caps_append_structure (caps_full_range_sizes, structure);
  }

  gst_caps_unref (tmp);

  if (filter) {
    tmp2 = gst_caps_intersect_full (filter, caps_full_range_sizes,
        GST_CAPS_INTERSECT_FIRST);

    gst_caps_unref (caps_full_range_sizes);
    tmp = tmp2;
  } else
    tmp = caps_full_range_sizes;

  result = tmp;

  GST_DEBUG_OBJECT (btrans, "transformed %" GST_PTR_FORMAT " into %"
      GST_PTR_FORMAT, caps, result);

  return result;
}

static gboolean
gst_vspm_filter_transform_meta (GstBaseTransform * trans, GstBuffer * outbuf,
    GstMeta * meta, GstBuffer * inbuf)
{
  const GstMetaInfo *info = meta->info;
  gboolean ret;

  if (gst_meta_api_type_has_tag (info->api, _colorspace_quark)) {
    /* don't copy colorspace specific metadata, FIXME, we need a MetaTransform
     * for the colorspace metadata. */
    ret = FALSE;
  } else {
    /* copy other metadata */
    ret = TRUE;
  }
  return ret;
}

struct extensions_t
{
  GstVideoFormat gst_format;
  guint vsp_format;
  guint vsp_swap;
};

/* Note that below swap information will be REVERSED later (in function
 *     set_colorspace) because current system use Little Endian */
static const struct extensions_t exts[] = {
  {GST_VIDEO_FORMAT_NV12,  VSP_IN_YUV420_SEMI_NV12,  VSP_SWAP_NO},    /* NV12 format is highest priority as most modules support this */
  {GST_VIDEO_FORMAT_I420,  VSP_IN_YUV420_PLANAR,     VSP_SWAP_NO},    /* I420 is second priority */
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
  {GST_VIDEO_FORMAT_RGB16, VSP_IN_RGB565,            VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV16,  VSP_IN_YUV422_SEMI_NV16,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV24,  VSP_IN_YUV444_SEMI_PLANAR,VSP_SWAP_NO},
};

static const struct extensions_t exts_out[] = {
  {GST_VIDEO_FORMAT_NV12,  VSP_OUT_YUV420_SEMI_NV12,  VSP_SWAP_NO},    /* NV12 format is highest priority as most modules support this */
  {GST_VIDEO_FORMAT_I420,  VSP_OUT_YUV420_PLANAR,     VSP_SWAP_NO},    /* I420 is second priority */
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
  {GST_VIDEO_FORMAT_RGB16, VSP_OUT_RGB565,            VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV16,  VSP_OUT_YUV422_SEMI_NV16,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV24,  VSP_OUT_YUV444_SEMI_PLANAR,VSP_SWAP_NO},
};

static gint
set_colorspace (GstVideoFormat vid_fmt, guint * format, guint * fswap)
{
  int nr_exts = sizeof (exts) / sizeof (exts[0]);
  int i;

  for (i = 0; i < nr_exts; i++) {
    if (vid_fmt == exts[i].gst_format) {
      *format = exts[i].vsp_format;

      /* Need to reverse swap information for Little Endian */
      *fswap  = (VSP_SWAP_B | VSP_SWAP_W | VSP_SWAP_L | VSP_SWAP_LL) ^ exts[i].vsp_swap;
      return 0;
    }
  }
  return -1;
}

static gint
set_colorspace_output (GstVideoFormat vid_fmt, guint * format, guint * fswap)
{
  int nr_exts = sizeof (exts_out) / sizeof (exts_out[0]);
  int i;

  for (i = 0; i < nr_exts; i++) {
    if (vid_fmt == exts_out[i].gst_format) {
      *format = exts_out[i].vsp_format;

      /* Need to reverse swap information for Little Endian */
      *fswap  = (VSP_SWAP_B | VSP_SWAP_W | VSP_SWAP_L | VSP_SWAP_LL) ^ exts_out[i].vsp_swap;
      return 0;
    }
  }
  return -1;
}

static gboolean
gst_vspm_filter_set_info (GstVideoFilter * filter,
    GstCaps * incaps, GstVideoInfo * in_info, GstCaps * outcaps,
    GstVideoInfo * out_info)
{
  GstVspmFilter *space;
  Vspm_mmng_ar *vspm_out;
  VspmbufArray *vspm_outbuf;
  GstStructure *structure;
  gint i;

  space = GST_VIDEO_CONVERT_CAST (filter);
  vspm_out = space->vspm_out;
  vspm_outbuf = space->vspm_outbuf;
  structure = gst_caps_get_structure (outcaps, 0);
  /* these must match */
  if (in_info->fps_n != out_info->fps_n || in_info->fps_d != out_info->fps_d)
    goto format_mismatch;

  /* if present, these must match too */
  if (in_info->interlace_mode != out_info->interlace_mode)
    goto format_mismatch;

  GST_DEBUG ("reconfigured %d %d", GST_VIDEO_INFO_FORMAT (in_info),
      GST_VIDEO_INFO_FORMAT (out_info));
  if(space->outbuf_allocate) {
    GstQuery *query;
    gint vspm_used ;
    GArray *outbuf_paddr_array;
    GArray *outbuf_vaddr_array;
    guint i, j, size;
    gsize offset[GST_VIDEO_MAX_PLANES] = { 0, };
    gint stride[GST_VIDEO_MAX_PLANES] = { 0, };
    guint plane_size[GST_VIDEO_MAX_PLANES] = { 0, };
    gint dmabuf_fd[GST_VIDEO_MAX_PLANES] = { 0, };
    gint page_offset[GST_VIDEO_MAX_PLANES];
    gint plane_size_ext[GST_VIDEO_MAX_PLANES];;
    gint page_size;
    guint n_planes = 0;

    outbuf_paddr_array = g_array_new (FALSE, FALSE, sizeof (gulong));
    outbuf_vaddr_array = g_array_new (FALSE, FALSE, sizeof (gulong));
    size = 0;
    page_size = getpagesize ();
    n_planes = GST_VIDEO_FORMAT_INFO_N_PLANES(out_info->finfo);

    for (i = 0; i < n_planes; i++) {
      offset[i] = size;
      stride[i] = GST_VIDEO_FORMAT_INFO_PSTRIDE(out_info->finfo, i) *
          GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (out_info->finfo, i,
              out_info->width);

      plane_size[i] = stride[i] *
        GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (out_info->finfo, i, out_info->height);

      size += plane_size[i];
    }

    for (i = 0; i < 5; i++){
      GstBuffer *buf;
      vspm_used = vspm_out->used;
      if (R_MM_OK == mmngr_alloc_in_user(&vspm_out->vspm[vspm_used].mmng_pid,
                                size,
                                &vspm_out->vspm[vspm_used].pphy_addr,
                                &vspm_out->vspm[vspm_used].phard_addr,
                                &vspm_out->vspm[vspm_used].puser_virt_addr,
                                MMNGR_VA_SUPPORT)) {
        vspm_out->used++;
        g_array_append_val (outbuf_paddr_array,
                            vspm_out->vspm[vspm_used].phard_addr);
        g_array_append_val (outbuf_vaddr_array,
                            vspm_out->vspm[vspm_used].puser_virt_addr);
        if (space->use_dmabuf) {
          buf = gst_buffer_new ();
          for (j = 0; j < n_planes; j++) {
            gint res;
            guint phys_addr;
            GstMemory *mem;
            phys_addr = (guint) vspm_out->vspm[vspm_used].phard_addr + offset[j];
            /* Calculate offset between physical address and page boundary */
            page_offset[j] = phys_addr & (page_size - 1);
            /* When downstream plugins do mapping from dmabuf fd it requires
            * mapping from boundary page and size align for page size so
            * memory for plane must increase to handle for this case */
            plane_size_ext[j] = GST_ROUND_UP_N (plane_size[j] + page_offset[j],
                      page_size);
            res =
              mmngr_export_start_in_user (&vspm_out->vspm[vspm_used].dmabuf_pid[j],
            plane_size_ext[j], (unsigned long) phys_addr, &dmabuf_fd[j]);
            if (res != R_MM_OK) {
              GST_ERROR_OBJECT (space,
                "mmngr_export_start_in_user failed (phys_addr:0x%08x)",
                phys_addr);
              return GST_FLOW_ERROR;
            }

            /* Set offset's information */
            mem = gst_dmabuf_allocator_alloc (space->allocator, dmabuf_fd[j],
                     plane_size_ext[j]);
            mem->offset = page_offset[j];
            /* Only allow to access plane size */
            mem->size = plane_size[j];
            gst_buffer_append_memory (buf, mem);
          }
        } else {
          buf = gst_buffer_new_wrapped (
                  (gpointer)vspm_out->vspm[vspm_used].puser_virt_addr,
                  (gsize)size);
        }
      } else {
        GST_ERROR_OBJECT (space,
              "mmngr_alloc_in_user failed to allocate memory (%d)",
              size);
        return GST_FLOW_ERROR;
      }

      g_ptr_array_add (vspm_outbuf->buf_array, buf);
      gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_INFO_FORMAT (out_info),
        out_info->width,
        out_info->height,
        n_planes, offset, stride);
    }

    structure = gst_structure_new ("vspm_allocation_request",
                                  "paddr_array", G_TYPE_ARRAY, outbuf_paddr_array,
                                  "vaddr_array", G_TYPE_ARRAY, outbuf_vaddr_array,
                                   NULL);
    query = gst_query_new_custom (GST_QUERY_CUSTOM, structure);
    GST_DEBUG_OBJECT (space, "send a vspm_allocation_request query");

    if (!gst_pad_peer_query (GST_BASE_TRANSFORM_SRC_PAD (GST_ELEMENT_CAST(space)), query)) {
      GST_WARNING_OBJECT (space, "vspm_allocation_request query failed");
    }

    gst_query_unref (query);
    g_array_free (outbuf_paddr_array, TRUE);
    g_array_free (outbuf_vaddr_array, TRUE);

    if (space->out_port_pool) {
      if (gst_buffer_pool_is_active (space->out_port_pool))
        gst_buffer_pool_set_active (space->out_port_pool, FALSE);
      gst_object_unref (space->out_port_pool);
    }

    /* create a new buffer pool*/
    space->out_port_pool = gst_vspmfilter_buffer_pool_new (space);

    structure = gst_buffer_pool_get_config (space->out_port_pool);
    gst_buffer_pool_config_set_params (structure, outcaps, out_info->size, 5, 5);
    if (!gst_buffer_pool_set_config (space->out_port_pool, structure)) {
        GST_WARNING_OBJECT (space, "failed to set buffer pool configuration");
    }
    if (!gst_buffer_pool_set_active (space->out_port_pool, TRUE)) {
        GST_WARNING_OBJECT (space, "failed to activate buffer pool");
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

static GstFlowReturn gst_vspm_filter_prepare_output_buffer (GstBaseTransform * trans,
                                          GstBuffer *inbuf, GstBuffer **outbuf)
{
    GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (trans);
    GstBuffer *buf;
    guint n_mem,i;
    GstFlowReturn ret = GST_FLOW_OK;
    Vspm_mmng_ar *vspm_out;
    VspmbufArray *vspm_outbuf;

    vspm_out    = space->vspm_out;
    vspm_outbuf = space->vspm_outbuf;

    if(space->outbuf_allocate) {
      trans->priv->passthrough = 0; //disable pass-through mode

      ret = gst_buffer_pool_acquire_buffer(space->out_port_pool, outbuf, NULL);

      if(gst_buffer_is_writable(*outbuf)) {
        if (!GST_BASE_TRANSFORM_CLASS(parent_class)->copy_metadata (trans,
                                                    inbuf, *outbuf)) {
          /* something failed, post a warning */
          GST_ELEMENT_WARNING (trans, STREAM, NOT_IMPLEMENTED,
          ("could not copy metadata"), (NULL));
        }
      }
    } else {
      ret = GST_BASE_TRANSFORM_CLASS(parent_class)->prepare_output_buffer(trans,
                                                                        inbuf,outbuf);
    }

    return ret;
}

static GstStateChangeReturn
gst_vspmfilter_change_state (GstElement * element, GstStateChange transition)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (space->out_port_pool)
        gst_buffer_pool_set_active (space->out_port_pool, FALSE);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      if (space->out_port_pool) {
        gst_object_unref (space->out_port_pool);
        space->out_port_pool = NULL;
      }
      /* Release the importing to avoid leak FD */
      gst_vspm_filter_release_fd (space->mmngr_import_list);
      break;
    default:
      break;
  }
  return   GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_vspm_filter_class_init (GstVspmFilterClass * klass)
{
  int nr_exts;
  int i;
  GstCaps* incaps;
  GstCaps* outcaps;
  GstCaps* tmpcaps;
  GstPadTemplate* gst_vspm_filter_src_template;
  GstPadTemplate* gst_vspm_filter_sink_template;

  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstBaseTransformClass *gstbasetransform_class =
      (GstBaseTransformClass *) klass;
  GstVideoFilterClass *gstvideofilter_class = (GstVideoFilterClass *) klass;

  gobject_class->set_property = gst_vspm_filter_set_property;
  gobject_class->get_property = gst_vspm_filter_get_property;
  gobject_class->finalize = gst_vspm_filter_finalize;

  incaps  = gst_caps_new_empty();
  outcaps = gst_caps_new_empty();

  nr_exts = sizeof (exts) / sizeof (exts[0]);
  for (i = 0; i < nr_exts; i++) {
	tmpcaps = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, gst_video_format_to_string (exts[i].gst_format),
            "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

    gst_caps_append (incaps, tmpcaps);
  }

  nr_exts = sizeof (exts_out) / sizeof (exts_out[0]);
  for (i = 0; i < nr_exts; i++) {
	tmpcaps = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, gst_video_format_to_string (exts_out[i].gst_format),
            "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

    gst_caps_append (outcaps, tmpcaps);
  }

  gst_vspm_filter_src_template = gst_pad_template_new ("src",
		GST_PAD_SRC, GST_PAD_ALWAYS, incaps);
  gst_vspm_filter_sink_template = gst_pad_template_new ("sink",
		GST_PAD_SINK, GST_PAD_ALWAYS, outcaps);

  gst_element_class_add_pad_template (gstelement_class,
      gst_vspm_filter_src_template);
  gst_element_class_add_pad_template (gstelement_class,
      gst_vspm_filter_sink_template);

  gst_caps_unref (incaps);
  gst_caps_unref (outcaps);

  gst_element_class_set_static_metadata (gstelement_class,
      "Colorspace and Video Size Converter with VSPM",
      "Filter/Converter/Video",
      "Converts colorspace and video size from one to another",
      "Renesas Corporation");
  g_object_class_install_property (gobject_class, PROP_VSPM_OUTBUF,
      g_param_spec_boolean ("outbuf-alloc", "Use outbuf-alloc mode",
        "Whether or not to self-allocate output buffer",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_VSPM_DMABUF,
      g_param_spec_boolean ("dmabuf-use", "Use DMABUF mode",
        "Whether or not to use dmabuf for output buffer",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  gstelement_class->change_state = gst_vspmfilter_change_state;
  gstbasetransform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_transform_caps);
  gstbasetransform_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_fixate_caps);
  gstbasetransform_class->filter_meta =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_filter_meta);
  gstbasetransform_class->transform_meta =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_transform_meta);

  gstbasetransform_class->passthrough_on_same_caps = TRUE;

  gstbasetransform_class->prepare_output_buffer = 
      GST_DEBUG_FUNCPTR (gst_vspm_filter_prepare_output_buffer);
  gstvideofilter_class->set_info =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_set_info);
  gstvideofilter_class->transform_frame =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_transform_frame);
}

static void
gst_vspm_filter_finalize (GObject * obj)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT (obj);
  GstVspmFilterVspInfo *vsp_info;
  Vspm_mmng_ar *vspm_in;
  Vspm_mmng_ar *vspm_out;

  vsp_info = space->vsp_info;
  vspm_in = space->vspm_in;
  vspm_out = space->vspm_out;

  if (vsp_info->mmngr_fd != -1) {
    /* mmngr dev close */
    close (vsp_info->mmngr_fd);
    vsp_info->mmngr_fd = -1;
  }

  if (vsp_info->is_init_vspm) {
    VSPM_lib_DriverQuit(vsp_info->vspm_handle);
  }

  if (space->vsp_info)
    g_free (space->vsp_info);

  while (vspm_in->used)
  {
    int i = vspm_in->used - 1;
    int j;

    vspm_in->used--;
    for (j = 0; j < GST_VIDEO_MAX_PLANES; j++)
      if(vspm_in->vspm[i].dmabuf_pid[j] >= 0)
        mmngr_import_end_in_user(vspm_in->vspm[i].dmabuf_pid[j]);
  }

  /* Release the importing to avoid leak FD */
  gst_vspm_filter_release_fd (space->mmngr_import_list);

  g_queue_free (space->mmngr_import_list);

  while (vspm_out->used)
  {
    int i = vspm_out->used - 1;
    int j;

    vspm_out->used--;
    for (j = 0; j < GST_VIDEO_MAX_PLANES; j++)
      if (vspm_out->vspm[i].dmabuf_pid[j] >= 0)
        mmngr_export_end_in_user(vspm_out->vspm[i].dmabuf_pid[j] );

    if (vspm_out->vspm[i].mmng_pid >= 0)
      mmngr_free_in_user(vspm_out->vspm[i].mmng_pid);
  }

  if (space->vspm_in)
    g_free (space->vspm_in);
  /* free buf_array when finalize */
  if (space->vspm_outbuf->buf_array)
    g_ptr_array_free(space->vspm_outbuf->buf_array, TRUE);
  if (space->vspm_out)
    g_free (space->vspm_out);
  if (space->vspm_outbuf)
    g_free (space->vspm_outbuf);
  /* free space->allocator when finalize */
  if (space->allocator)
    gst_object_unref(space->allocator);

  sem_destroy (&space->smp_wait);
  G_OBJECT_CLASS (parent_class)->finalize (obj);
}


static void
gst_vspm_filter_init (GstVspmFilter * space)
{
  GstVspmFilterVspInfo *vsp_info;
  Vspm_mmng_ar *vspm_in;
  Vspm_mmng_ar *vspm_out;
  VspmbufArray *vspm_outbuf;
  guint i, j;

  space->vsp_info = g_malloc0 (sizeof (GstVspmFilterVspInfo));
  space->vspm_in = g_malloc0 (sizeof (Vspm_mmng_ar));
  space->vspm_out = g_malloc0 (sizeof (Vspm_mmng_ar));
  space->vspm_outbuf = g_malloc0 (sizeof (VspmbufArray));
  if (!space->vsp_info 
    || !space->vspm_in 
    || !space->vspm_out
    || !space->vspm_outbuf) {
    GST_ELEMENT_ERROR (space, RESOURCE, NO_SPACE_LEFT,
        ("Could not allocate vsp info"), ("Could not allocate vsp info"));
    return;
  }

  vsp_info = space->vsp_info;
  vspm_in = space->vspm_in;
  vspm_out = space->vspm_out;
  vspm_outbuf = space->vspm_outbuf;

  vsp_info->is_init_vspm = FALSE;
  vsp_info->format_flag = 0;

  vsp_info->mmngr_fd = -1;
  /* mmngr dev open */
  vsp_info->mmngr_fd = open(DEVFILE, O_RDWR);
  if (vsp_info->mmngr_fd == -1) {
    GST_ERROR ("MMNGR: open error. \n");
  }
  
  if (VSPM_lib_DriverInitialize(&vsp_info->vspm_handle) == R_VSPM_OK) {
    vsp_info->is_init_vspm = TRUE;
  } else {
    GST_ERROR ("VSPM: Error Initialized. \n");
  }

  vspm_in->used = 0;
  vspm_out->used = 0;
  vspm_outbuf->buf_array = g_ptr_array_new ();  
  vspm_outbuf->current_buffer_index = 0;
  space->allocator = gst_dmabuf_allocator_new ();
  space->outbuf_allocate = FALSE;
  space->use_dmabuf = FALSE;
  space->first_buff = 1;
  space->mmngr_import_list = g_queue_new ();

  for (i = 0; i < sizeof(vspm_in->vspm)/sizeof(vspm_in->vspm[0]); i++) {
    for (j = 0; j < GST_VIDEO_MAX_PLANES; j++)
      vspm_in->vspm[i].dmabuf_pid[j] = -1;
  }
  for (i = 0; i < sizeof(vspm_out->vspm)/sizeof(vspm_out->vspm[0]); i++) {
    for (j = 0; j < GST_VIDEO_MAX_PLANES; j++)
      vspm_out->vspm[i].dmabuf_pid[j] = -1;
  }

  sem_init (&space->smp_wait, 0, 0);
}

void
gst_vspm_filter_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (object);
  GstBaseTransform *trans;

  trans = GST_BASE_TRANSFORM (object);
  switch (property_id) {
    case PROP_VSPM_OUTBUF:
      space->outbuf_allocate = g_value_get_boolean (value);
      break;
    case PROP_VSPM_DMABUF:
      space->use_dmabuf = g_value_get_boolean (value);
      if (space->use_dmabuf)
          space->outbuf_allocate = TRUE;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_vspm_filter_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (object);
  switch (property_id) {
    case PROP_VSPM_OUTBUF:
      g_value_set_boolean (value, space->outbuf_allocate);
      break;
    case PROP_VSPM_DMABUF:
      g_value_set_boolean (value, space->use_dmabuf);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


/* callback function */
static void cb_func(
  unsigned long uwJobId, long wResult, unsigned long uwUserData)
{
  sem_t *p_smpwait = (sem_t *) uwUserData;

  if (wResult != 0) {
    GST_ERROR ("VSPM: error end. (%ld)\n", wResult);
  }
  /* Inform frame finish to transform function */
  sem_post (p_smpwait);
}

static GstFlowReturn
find_physical_address (GstVspmFilter *space, gpointer in_vir1, gpointer in_vir2,
    gpointer *out_phy1, gpointer *out_phy2)
{
  struct MM_PARAM p_adr[2];
  GstFlowReturn ret;
  gint page_size, max_size_in_page;

  /* change virtual address to physical address */
  memset(&p_adr, 0, sizeof(p_adr));
  p_adr[0].user_virt_addr = (unsigned long)in_vir1;
  p_adr[1].user_virt_addr = (unsigned long)in_vir2;
  ret = ioctl(space->vsp_info->mmngr_fd, MM_IOC_VTOP, &p_adr);
  if (ret) {
    GST_ERROR ("MMNGR VtoP Convert Error. \n");
    return GST_FLOW_ERROR;
  }
  /* Note that this method to find physical address may only find the address at
   * start of page. If there is an offset from page, we need to add it here */
  page_size = getpagesize ();
  max_size_in_page = page_size - 1;
  if ((p_adr[0].hard_addr & max_size_in_page) == 0)
    p_adr[0].hard_addr += (max_size_in_page & (unsigned long)in_vir1);
  if ((p_adr[1].hard_addr & max_size_in_page) == 0)
    p_adr[1].hard_addr += (max_size_in_page & (unsigned long)in_vir2);

  if (out_phy1 != NULL) *out_phy1 = (gpointer) p_adr[0].hard_addr;
  if (out_phy2 != NULL) *out_phy2 = (gpointer) p_adr[1].hard_addr;
  return GST_FLOW_OK;
}

static void
gst_vspm_filter_import_fd (GstMemory *mem, gpointer *out, GQueue *import_list)
{
  int fd;

  if (gst_is_dmabuf_memory(mem)) {
    int import_pid;
    size_t size;

    fd = gst_dmabuf_memory_get_fd (mem);
    if (R_MM_OK == mmngr_import_start_in_user_ext (&import_pid,
                                                   &size, (unsigned int *)out,
                                                   fd, NULL)) {
      g_queue_push_tail (import_list, GINT_TO_POINTER(import_pid));
    }
  }
}

static void
gst_vspm_filter_release_fd (GQueue *import_list)
{
  int fd;
  while (!g_queue_is_empty(import_list)) {
    fd = GPOINTER_TO_INT(g_queue_pop_tail (import_list));
    if (fd >= 0 )
      mmngr_import_end_in_user_ext (fd);
  }
}

static GstFlowReturn
gst_vspm_filter_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame)
{
  GstVspmFilter *space;
  GstVspmFilterVspInfo *vsp_info;

  VSPM_IP_PAR vspm_ip;
  VSPM_VSP_PAR vsp_par;

  T_VSP_IN src_par;
  T_VSP_ALPHA src_alpha_par;
  T_VSP_OUT dst_par;
  T_VSP_CTRL ctrl_par;
  T_VSP_UDS uds_par;

  gint in_width, in_height;
  gint out_width, out_height;
  long ercd;
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
  GstBuffer *buf;
  GstMemory *mem;

  space = GST_VIDEO_CONVERT_CAST (filter);
  vsp_info = space->vsp_info;

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

  if ((in_width == out_width) && (in_height == out_height)) {
    use_module = 0;
  } else {
    /* UDS scaling */
    use_module = VSP_UDS_USE;
  }

  ret = find_physical_address (space, in_frame->data[0], out_frame->data[0],
      &src_addr[0], &dst_addr[0]);
  if (!src_addr[0] || ret) {
    buf = in_frame->buffer;
    mem = gst_buffer_peek_memory (buf, 0);
    gst_vspm_filter_import_fd (mem, &src_addr[0], space->mmngr_import_list);
  }
  if (!dst_addr[0] || ret) {
    buf = out_frame->buffer;
    mem = gst_buffer_peek_memory (buf, 0);
    gst_vspm_filter_import_fd (mem, &dst_addr[0], space->mmngr_import_list);
  }

  if (in_n_planes >= 2 || out_n_planes >= 2) {
    ret = find_physical_address (space, in_frame->data[1], out_frame->data[1],
        &src_addr[1], &dst_addr[1]);
    if (!src_addr[1] || ret) {
      buf = in_frame->buffer;
      if (gst_buffer_n_memory(buf) > 1) {
        /* Make sure we have separate GstMemory for each planar */
        mem = gst_buffer_peek_memory (buf, 1);
        gst_vspm_filter_import_fd (mem, &src_addr[1], space->mmngr_import_list);
      } else {
        /* We can not find the exactly address for plane 2, return as error */
        GST_ERROR("Can not find physical address of input buffer for planar 2\n");
        ret = GST_FLOW_ERROR;
        goto err;
      }
    }
    if (!dst_addr[1] || ret) {
      buf = out_frame->buffer;
      if (gst_buffer_n_memory(buf) > 1) {
        /* Make sure we have separate GstMemory for each planar */
        mem = gst_buffer_peek_memory (buf, 1);
        gst_vspm_filter_import_fd (mem, &dst_addr[1], space->mmngr_import_list);
      } else {
        GST_ERROR("Can not find physical address of output buffer for planar 2\n");
        /* We can not find the exactly address for plane 2, return as error */
        ret = GST_FLOW_ERROR;
        goto err;
      }
    }
  }

  if (in_n_planes >= 3 || out_n_planes >= 3) {
    ret = find_physical_address (space, in_frame->data[2], out_frame->data[2],
        &src_addr[2], &dst_addr[2]);
    if (!src_addr[2] || ret) {
      buf = in_frame->buffer;
      if (gst_buffer_n_memory(buf) > 2) {
        /* Make sure we have separate GstMemory for each planar */
        mem = gst_buffer_peek_memory (buf, 2);
        gst_vspm_filter_import_fd (mem, &src_addr[2], space->mmngr_import_list);
      } else {
        /* We can not find the exactly address for plane 3, return as error */
        GST_ERROR("Can not find physical address of input buffer for planar 3\n");
        ret = GST_FLOW_ERROR;
        goto err;
      }
    }
    if (!dst_addr[2] || ret) {
      buf = out_frame->buffer;
      if (gst_buffer_n_memory(buf) > 2) {
        /* Make sure we have separate GstMemory for each planar */
        mem = gst_buffer_peek_memory (buf, 2);
        gst_vspm_filter_import_fd (mem, &dst_addr[2], space->mmngr_import_list);
      } else {
        GST_ERROR("Can not find physical address of output buffer for planar 3\n");
        /* We can not find the exactly address for plane 3, return as error */
        ret = GST_FLOW_ERROR;
        goto err;
      }
    }

  }

  if (!src_addr[0] || !dst_addr[0] ||
      ((in_n_planes >= 2 && !src_addr[1]) || (out_n_planes >= 2 && !dst_addr[1])) ||
      ((in_n_planes >= 3 && !src_addr[2]) || (out_n_planes >= 3 && !dst_addr[2]))) {
    /* W/A: Sometimes we can not convert virtual address to physical address,
     * we should skip this frame to avoid issue with HW processor.
     */
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
    src_par.width          = in_width;
    src_par.height         = in_height;
    src_par.width_ex       = 0;
    src_par.height_ex      = 0;
    src_par.x_offset       = 0;
    src_par.y_offset       = 0;
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
      uds_par.x_ratio      = (unsigned short)( (in_width << 12) / out_width );
      uds_par.y_ratio      = (unsigned short)( (in_height << 12) / out_height );
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

  memset(&vspm_ip, 0, sizeof(VSPM_IP_PAR));
  vspm_ip.uhType             = VSPM_TYPE_VSP_AUTO;
  vspm_ip.unionIpParam.ptVsp = &vsp_par;

  ercd = VSPM_lib_Entry(vsp_info->vspm_handle, &vsp_info->jobid, 126, &vspm_ip, (unsigned long)&space->smp_wait, cb_func);
  if (ercd) {
    GST_ERROR ("VSPM_lib_Entry() Failed!! ercd=%ld\n", ercd);
    ret = GST_FLOW_ERROR;
    goto err;
  }

  /* Wait for callback */
  sem_wait (&space->smp_wait);

  ret = GST_FLOW_OK;
err:
  /* Release the importing to avoid leak FD */
  gst_vspm_filter_release_fd (space->mmngr_import_list);

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (vspmfilter_debug, "vspmfilter", 0,
      "Colorspace and Video Size Converter");

  _colorspace_quark = g_quark_from_static_string ("colorspace");

  return gst_element_register (plugin, "vspmfilter",
      GST_RANK_NONE, GST_TYPE_VIDEO_CONVERT);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vspmfilter, "Colorspace conversion and Video scaling with VSPM", plugin_init, VERSION, GST_LICENSE,
    GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
