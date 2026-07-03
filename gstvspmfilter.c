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

/* isu hardware limitation */
#define ISU_STRIDE_ALIGN (32)
#define ISU_ADDR_ALIGN   (512)

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

static void gst_vspm_filter_free_buffer_pools (GstVspmFilter * space);
static void gst_vspm_filter_set_buffer_info (GstVspmFilter * space, VspmBufferInfo * buf_info,
    GstVideoInfo * info, GstVideoAlignment * align);

static GstFlowReturn gst_vspm_filter_get_mem_phys_addr (GstVspmFilter * space,
    GstBuffer * buf, gpointer vir_addr, guint plane, gsize plane_offset,
    gpointer * out_phy);

static gboolean gst_vspm_filter_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query);

static gboolean gst_vspm_filter_set_info (GstVideoFilter * filter,
    GstCaps * incaps, GstVideoInfo * in_info, GstCaps * outcaps,
    GstVideoInfo * out_info);
static GstFlowReturn gst_vspm_filter_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame);

static void gst_vspm_filter_finalize (GObject * obj);

static void gst_vspm_filter_import_fd (GstMemory *mem, gsize plane_offset, gpointer *out, GQueue *import_list);
static void gst_vspm_filter_release_fd (GQueue *import_list);
static void gst_vspm_filter_compute_csc (guint              src_fmt,
                                         GstVideoColorRange in_range,
                                         guint              dst_fmt,
                                         GstVideoColorRange out_range,
                                         gdouble            Kr,
                                         gdouble            Kb,
                                         T_ISU_CSC         *csc_par);
static gboolean
gst_vspm_filter_buffer_can_passthrough (GstVspmFilter * space, GstBuffer * buf);

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
  PROP_VSPM_INBUF,
  PROP_VSPM_DMABUF,
  PROP_VSPM_CROP
};

static void
gst_vspm_filter_free_buffer (VspmBuffer * buf)
{
  gint plane;

  if (buf == NULL)
    return;

  for (plane = 0; plane < GST_VIDEO_MAX_PLANES; plane++) {
    if (buf->dmabuf_pid[plane] >= 0) {
      mmngr_export_end_in_user (buf->dmabuf_pid[plane]);
      buf->dmabuf_pid[plane] = -1;
    }
  }
  if (buf->mmng_pid >= 0) {
    mmngr_free_in_user (buf->mmng_pid);
    buf->mmng_pid = -1;
  }
}

static void
gst_vspm_filter_free_vspm_buffers (VspmBufferPool * vspm_pool)
{
  while (vspm_pool->used_count) {
    gst_vspm_filter_free_buffer (&vspm_pool->buffers[vspm_pool->used_count - 1]);
    vspm_pool->used_count--;
  }
}

static void
gst_vspm_filter_free_pool (GstBufferPool ** gst_pool)
{
  if (gst_pool == NULL || *gst_pool == NULL)
    return;

  gst_vspm_filter_free_vspm_buffers (
      &GST_VSPMFILTER_BUFFER_POOL_CAST (*gst_pool)->vspm_pool);

  if (gst_buffer_pool_is_active (*gst_pool))
    gst_buffer_pool_set_active (*gst_pool, FALSE);
  gst_object_unref (*gst_pool);
  *gst_pool = NULL;
}

static GstFlowReturn
gst_vspm_filter_alloc_buffer (GstVspmFilter * space, VspmBufferInfo * buf_info,
    VspmBuffer * vspm_buf, GstBuffer ** gst_buf)
{
  GstBuffer *buf;
  gint page_size = getpagesize();
  guint plane;

  if (buf_info->buf_size == 0) {
    GST_ERROR_OBJECT (space, "buffer info not initialized");
    return GST_FLOW_ERROR;
  }

  vspm_buf->mmng_pid = -1;
  for (plane = 0; plane < GST_VIDEO_MAX_PLANES; plane++) {
    vspm_buf->dmabuf_pid[plane] = -1;
  }

  if (R_MM_OK != mmngr_alloc_in_user (&vspm_buf->mmng_pid, buf_info->buf_size,
          &vspm_buf->pphy_addr, &vspm_buf->phard_addr, &vspm_buf->puser_virt_addr,
          MMNGR_VA_SUPPORT_CACHED)) {
    GST_ERROR_OBJECT (space, "mmngr_alloc_in_user failed (%u bytes)",
        buf_info->buf_size);
    vspm_buf->mmng_pid = -1;
    return GST_FLOW_ERROR;
  }

  buf = gst_buffer_new ();

  for (plane = 0; plane < buf_info->n_planes; plane++) {
    GstMemory *mem;

    if (space->use_dmabuf) {
      unsigned long plane_phys_addr = vspm_buf->phard_addr + buf_info->plane_offset[plane];
      gint phys_page_offset  = plane_phys_addr & (page_size - 1);
      gint page_aligned_size = GST_ROUND_UP_N (buf_info->plane_size[plane] + phys_page_offset,
                                      page_size);
      gint dmabuf_fd = -1;

      if ((mmngr_export_start_in_user (&vspm_buf->dmabuf_pid[plane], page_aligned_size,
              (unsigned long) GST_ROUND_DOWN_N (plane_phys_addr, page_size),
              &dmabuf_fd) != R_MM_OK) || (dmabuf_fd < 0)) {
        GST_ERROR_OBJECT (space,
            "mmngr_export_start_in_user failed (plane %u, phys 0x%lx)",
            plane, plane_phys_addr);
        vspm_buf->dmabuf_pid[plane] = -1;
        gst_buffer_unref (buf);
        gst_vspm_filter_free_buffer (vspm_buf);
        return GST_FLOW_ERROR;
      }

      mem = gst_dmabuf_allocator_alloc_with_flags (space->allocator, dmabuf_fd,
          page_aligned_size, GST_FD_MEMORY_FLAG_DONT_CLOSE);
      if (mem == NULL) {
        GST_ERROR_OBJECT (space, "gst_dmabuf_allocator_alloc failed");
        gst_buffer_unref (buf);
        gst_vspm_filter_free_buffer (vspm_buf);
        return GST_FLOW_ERROR;
      }
      mem->offset = phys_page_offset;
      mem->size = buf_info->plane_size[plane];
    } else {
      mem = gst_memory_new_wrapped (0,
          (gpointer) (vspm_buf->puser_virt_addr + buf_info->plane_offset[plane]),
          buf_info->plane_size[plane], 0, buf_info->plane_size[plane], NULL, NULL);
      if (mem == NULL) {
        GST_ERROR_OBJECT (space, "gst_memory_new_wrapped failed");
        gst_buffer_unref (buf);
        gst_vspm_filter_free_buffer (vspm_buf);
        return GST_FLOW_ERROR;
      }
    }
    gst_buffer_append_memory (buf, mem);
  }

  gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
      buf_info->format, buf_info->width, buf_info->height,
      buf_info->n_planes, buf_info->plane_offset, buf_info->plane_stride);

  *gst_buf = buf;
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_vspm_filter_allocate_port_buffer (GstVspmFilter * space, VspmBufferPool * vspm_pool,
    VspmBufferInfo * buf_info, GstBuffer ** buffer)
{
  GstFlowReturn ret;

  if (vspm_pool->used_count >=
          (gint) (sizeof (vspm_pool->buffers) / sizeof (vspm_pool->buffers[0]))) {
    GST_ERROR_OBJECT (space, "buffer pool exhausted (max %u)",
        (guint) (sizeof (vspm_pool->buffers) / sizeof (vspm_pool->buffers[0])));
    return GST_FLOW_ERROR;
  }

  ret = gst_vspm_filter_alloc_buffer (space, buf_info,
      &vspm_pool->buffers[vspm_pool->used_count], buffer);
  if (ret == GST_FLOW_OK)
    vspm_pool->used_count++;

  return ret;
}

static void
gst_vspmfilter_buffer_pool_free_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
  gst_buffer_unref (buffer);
}

static GstFlowReturn
gst_vspmfilter_buffer_pool_alloc_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstVspmFilterBufferPool *vspmfltpool = GST_VSPMFILTER_BUFFER_POOL_CAST (bpool);
  GstVspmFilter * vspmfilter = vspmfltpool->vspmfilter;

  return gst_vspm_filter_allocate_port_buffer (vspmfilter,
      &vspmfltpool->vspm_pool, &vspmfltpool->buf_info, buffer);
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

  /* Guarantees no fd/hw leak (no-op if already freed) */
  gst_vspm_filter_free_vspm_buffers (&pool->vspm_pool);

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
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (trans);
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
    if (space->enable_crop && direction == GST_PAD_SINK) {
      /* When going SINK->SRC with crop enabled, propose cropped dimensions */
      gint crop_w = from_w - space->crop.left - space->crop.right;
      gint crop_h = from_h - space->crop.top  - space->crop.bottom;
      if (crop_w > 0 && crop_h > 0) {
        gst_structure_fixate_field_nearest_int (outs, "width", crop_w);
        gst_structure_fixate_field_nearest_int (outs, "height", crop_h);
      } else {
        gst_structure_fixate_field_nearest_int (outs, "width", from_w);
        gst_structure_fixate_field_nearest_int (outs, "height", from_h);
      }
    } else {
      gst_structure_fixate_field_nearest_int (outs, "height", from_h);
      gst_structure_fixate_field_nearest_int (outs, "width", from_w);
    }
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
  guint isu_format;
  guint isu_swap;
};

/* Note that below swap information will be REVERSED later (in function
 *     set_colorspace) because current system use Little Endian */
static const struct extensions_t exts[] = {
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

static const struct extensions_t exts_out[] = {
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
  int nr_exts = sizeof (exts) / sizeof (exts[0]);
  int i;

  for (i = 0; i < nr_exts; i++) {
    if (vid_fmt == exts[i].gst_format) {
      *format = exts[i].isu_format;

      /* Need to reverse swap information for Little Endian */
      *fswap  = exts[i].isu_swap;
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
      *format = exts_out[i].isu_format;

      /* Need to reverse swap information for Little Endian */
      *fswap  = exts_out[i].isu_swap;
      return 0;
    }
  }
  return -1;
}

static void
gst_vspm_filter_set_buffer_info (GstVspmFilter * space, VspmBufferInfo * buf_info,
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

static void
gst_vspm_filter_free_buffer_pools (GstVspmFilter * space)
{
  /* Release the importing to avoid leak FD */
  gst_vspm_filter_release_fd (space->mmngr_import_list);

  gst_vspm_filter_free_pool (&space->in_gst_pool);
  gst_vspm_filter_free_pool (&space->out_gst_pool);
}

static gboolean
gst_vspm_filter_set_info (GstVideoFilter * filter,
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
      gst_vspm_filter_compute_csc (in_fmt, in_range,
                                   out_fmt, out_range,
                                   Kr, Kb, vsp_info->cached_csc);

      GST_DEBUG_OBJECT (space,
          "CSC pre-computed (in_fmt=0x%02x out_fmt=0x%02x "
          "in_range=%d out_range=%d)",
          in_fmt, out_fmt, in_range, out_range);
    }
  }

  if(space->outbuf_allocate) {
    VspmBufferInfo *out_buf_info;

    /* Drop any pool from a previous negotiation so we never hand out
     * buffers sized for stale caps after a resolution change. */
    gst_vspm_filter_free_pool (&space->out_gst_pool);

    /* create a new buffer pool; its buf_info is filled in below */
    space->out_gst_pool = gst_vspmfilter_buffer_pool_new (space);
    out_buf_info =
        &GST_VSPMFILTER_BUFFER_POOL_CAST (space->out_gst_pool)->buf_info;

    gst_vspm_filter_set_buffer_info (space, out_buf_info, out_info, NULL);

    structure = gst_buffer_pool_get_config (space->out_gst_pool);
    gst_buffer_pool_config_set_params(structure, outcaps,
                                      out_buf_info->buf_size,
                                      MIN_BUFFERS, MAX_BUFFERS);
    if (!gst_buffer_pool_set_config (space->out_gst_pool, structure)) {
      GST_WARNING_OBJECT (space, "failed to configure output buffer pool");
    }
  } else {
    /* outbuf-alloc turned off (or never on): make sure no stale pool lingers */
    gst_vspm_filter_free_pool (&space->out_gst_pool);
  }

  if (space->inbuf_allocate) {
    VspmBufferInfo *in_buf_info;

    /* Drop any pool from a previous negotiation so we never hand out
     * buffers sized for stale caps after a resolution change. */
    gst_vspm_filter_free_pool (&space->in_gst_pool);

    space->in_gst_pool = gst_vspmfilter_buffer_pool_new (space);
    in_buf_info =
        &GST_VSPMFILTER_BUFFER_POOL_CAST (space->in_gst_pool)->buf_info;

    gst_vspm_filter_set_buffer_info (space, in_buf_info, in_info, NULL);

    structure = gst_buffer_pool_get_config (space->in_gst_pool);
    /* Let upstream decide the number of buffers it needs */
    gst_buffer_pool_config_set_params (structure, incaps,
        in_buf_info->buf_size, MIN_BUFFERS, 0);
    /* VIDEO_META lets upstream accept our ISU-aligned stride/offset
     * without padding or an extra copy. */
    gst_buffer_pool_config_add_option (structure,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    if (!gst_buffer_pool_set_config (space->in_gst_pool, structure)) {
      GST_WARNING_OBJECT (space, "failed to configure input buffer pool");
    }
  } else {
    /* inbuf-alloc turned off (or never on): make sure no stale pool lingers */
    gst_vspm_filter_free_pool (&space->in_gst_pool);
  }

  return TRUE;

  /* ERRORS */
format_mismatch:
  {
    GST_ERROR_OBJECT (space, "input and output formats do not match");
    return FALSE;
  }
}

static gboolean
gst_vspm_filter_decide_allocation (GstBaseTransform * trans, GstQuery * query)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST(trans);
  gboolean update = FALSE;

  if (space->outbuf_allocate) {
    GstBufferPool *pool = NULL;
    GstStructure *config;
    GstVideoAlignment align;
    guint i;
    VspmBufferInfo *out_buf_info =
        &GST_VSPMFILTER_BUFFER_POOL_CAST (space->out_gst_pool)->buf_info;

    if (gst_query_get_n_allocation_pools (query)) {
      gst_query_parse_nth_allocation_pool(query, 0, &pool, NULL, NULL, NULL);
      if (pool) {
        config = gst_buffer_pool_get_config(pool);

        /* vspmfilter always use its own buffer pool. If downstream requires video
         * alignment, we consider to update it */
        if (gst_buffer_pool_has_option(pool,
                                       GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT)) {
          /* Get alignment */
          gst_video_alignment_reset(&align);
          gst_buffer_pool_config_get_video_alignment(config, &align);
          gst_structure_free(config);
          /* FIXME: Currently, we ignore padding and only check stride */
          GST_DEBUG_OBJECT(space, "got a stride alignment requirement from "
                                  "downstream %d:%d:%d:%d",
                                  align.stride_align[0], align.stride_align[1],
                                  align.stride_align[2], align.stride_align[3]);

          for (i = 0; i < GST_VIDEO_MAX_PLANES; i++) {
            if ((align.stride_align[i] != 0) &&
                (out_buf_info->plane_stride[i] % align.stride_align[i] != 0)) {
              update = TRUE;
              break;
            }
          }
        }
        gst_object_unref(pool);
      }
    }

    if (update) {
      GstStructure *structure;
      GstCaps *caps = NULL;

      GST_DEBUG_OBJECT(space, "update buffer info and buffer pool");

      if (space->out_gst_pool) {
        if (gst_buffer_pool_is_active (space->out_gst_pool)) {
          gst_buffer_pool_set_active (space->out_gst_pool, FALSE);
        }
      }

      gst_vspm_filter_set_buffer_info (space, out_buf_info, NULL, &align);

      structure = gst_buffer_pool_get_config (space->out_gst_pool);
      gst_buffer_pool_config_get_params(structure, &caps, NULL, NULL, NULL);

      gst_buffer_pool_config_set_params(structure, caps,
                                        out_buf_info->buf_size,
                                        MIN_BUFFERS, MAX_BUFFERS);

      if (!gst_buffer_pool_set_config (space->out_gst_pool, structure)) {
        GST_WARNING_OBJECT (space, "failed to set buffer pool configuration");
      }
    }
  }

  return TRUE;
}

static gboolean
gst_vspm_filter_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (trans);
  VspmBufferInfo *in_buf_info;

  if (!GST_BASE_TRANSFORM_CLASS (parent_class)->propose_allocation (trans,
          decide_query, query))
    return FALSE;

  if (space->inbuf_allocate || space->outbuf_allocate)
    gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  /* Only propose our own input pool when we actually allocate input buffers. */
  if (!space->inbuf_allocate)
    return TRUE;

  if (space->in_gst_pool == NULL) {
    GST_DEBUG_OBJECT (space,
        "input pool not ready, upstream will use its own allocator");
    return TRUE;
  }

  in_buf_info = &GST_VSPMFILTER_BUFFER_POOL_CAST (space->in_gst_pool)->buf_info;

  /* Let upstream decide the number of buffers it needs */
  if (gst_query_get_n_allocation_pools (query) > 0)
    gst_query_set_nth_allocation_pool (query, 0, space->in_gst_pool,
        in_buf_info->buf_size, MIN_BUFFERS, 0);
  else
    gst_query_add_allocation_pool (query, space->in_gst_pool,
        in_buf_info->buf_size, MIN_BUFFERS, 0);

  GST_DEBUG_OBJECT (space,
      "proposed input pool=%p size=%u min=%d max=%d",
      space->in_gst_pool, in_buf_info->buf_size,
      MIN_BUFFERS, 0);

  return TRUE;
}

static GstFlowReturn gst_vspm_filter_prepare_output_buffer (GstBaseTransform * trans,
                                          GstBuffer *inbuf, GstBuffer **outbuf)
{
    GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (trans);
    GstFlowReturn ret = GST_FLOW_OK;

    gboolean base_passthrough = gst_base_transform_is_passthrough (trans);
    gboolean do_passthrough   = base_passthrough &&
                                gst_vspm_filter_buffer_can_passthrough (space, inbuf);

    if (do_passthrough) {
      return GST_BASE_TRANSFORM_CLASS (parent_class)->prepare_output_buffer (
          trans, inbuf, outbuf);
    }

    if(space->outbuf_allocate) {
      gst_base_transform_set_passthrough (trans, FALSE);

      if (!gst_buffer_pool_is_active (space->out_gst_pool)) {
        if (!gst_buffer_pool_set_active (space->out_gst_pool, TRUE)) {
          GST_ERROR_OBJECT (space, "failed to activate output buffer pool");
          return GST_FLOW_ERROR;
        }
      }

      ret = gst_buffer_pool_acquire_buffer (space->out_gst_pool, outbuf, NULL);
      if (ret != GST_FLOW_OK)
        return ret;

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
      if (space->out_gst_pool)
        gst_buffer_pool_set_active (space->out_gst_pool, FALSE);
      if (space->in_gst_pool)
        gst_buffer_pool_set_active (space->in_gst_pool, FALSE);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_vspm_filter_free_buffer_pools (space);
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
  g_object_class_install_property (gobject_class, PROP_VSPM_INBUF,
      g_param_spec_boolean ("inbuf-alloc", "Use inbuf-alloc mode",
        "Whether or not to self-allocate input buffer",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_VSPM_DMABUF,
      g_param_spec_boolean ("dmabuf-use", "Use DMABUF mode",
        "Whether or not to use dmabuf for output buffer",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_VSPM_CROP,
      g_param_spec_string ("crop", "Crop information",
        "Crop input frames. Format: \"left:right:top:bottom\" in pixels.",
        NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
        GST_PARAM_MUTABLE_READY));
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
  gstbasetransform_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_decide_allocation);
  gstbasetransform_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_propose_allocation);
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

  vsp_info = space->vsp_info;

  if (vsp_info->mmngr_fd != -1) {
    /* mmngr dev close */
    close (vsp_info->mmngr_fd);
    vsp_info->mmngr_fd = -1;
  }

  if (vsp_info->is_init_vspm) {
    VSPM_lib_DriverQuit(vsp_info->vspm_handle);
  }

  /* Release any buffer resources left over if teardown did not go through
   * change_state (imported fds, both ports' buffers + pools). */
  gst_vspm_filter_free_buffer_pools (space);

  if (space->vsp_info) {
    g_free (vsp_info->cached_csc);
    g_free (space->vsp_info);
  }
  if (space->mmngr_import_list)
    g_queue_free (space->mmngr_import_list);
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

  space->vsp_info      = g_malloc0 (sizeof (GstVspmFilterVspInfo));
  if (!space->vsp_info) {
    GST_ELEMENT_ERROR (space, RESOURCE, NO_SPACE_LEFT,
        ("Could not allocate vsp info"), ("Could not allocate vsp info"));
    return;
  }

  vsp_info      = space->vsp_info;

  vsp_info->is_init_vspm = FALSE;
  vsp_info->format_flag  = 0;
  vsp_info->mmngr_fd     = -1;
  vsp_info->cached_csc   = NULL;
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

  space->allocator          = gst_dmabuf_allocator_new ();
  space->outbuf_allocate    = FALSE;
  space->inbuf_allocate     = FALSE;
  space->in_gst_pool        = NULL;
  space->out_gst_pool       = NULL;
  space->use_dmabuf         = FALSE;
  space->mmngr_import_list  = g_queue_new ();

  /* Initialize crop to disabled */
  space->crop.left   = 0;
  space->crop.right  = 0;
  space->crop.top    = 0;
  space->crop.bottom = 0;
  space->enable_crop = FALSE;

  sem_init (&space->smp_wait, 0, 0);
}

static gboolean
gst_vspm_filter_parse_cropsize (GObject * object, const GValue * value)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (object);
  const gchar *str_value = g_value_get_string (value);

  gchar **str_arr, *end_char;
  gint length = 4; /* fixed-size array */
  gint64 crop_arr[4] = { 0, };
  gint i;

  str_arr = g_strsplit (str_value, ":", length);
  if (str_arr == NULL)
    goto error;

  for (i = 0; i < length; i++) {
    if (str_arr[i] == NULL || *str_arr[i] == '\0') /* Empty string */
      goto error;
    else
      crop_arr[i] = g_ascii_strtoll(str_arr[i], &end_char, 10);

    if (*end_char != '\0') /* Invalid end character */
      goto error;
    if (crop_arr[i] < 0 || crop_arr[i] > G_MAXINT)
      goto error;
  }
  g_strfreev (str_arr);

  space->crop.left   = crop_arr[0];
  space->crop.right  = crop_arr[1];
  space->crop.top    = crop_arr[2];
  space->crop.bottom = crop_arr[3];

  return TRUE;

error:
  GST_ERROR_OBJECT (space, "Failed to parse crop size: %s. Using default instead",
                    str_value);
  if (str_arr)
    g_strfreev (str_arr);
  return FALSE;
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
    case PROP_VSPM_INBUF:
      space->inbuf_allocate = g_value_get_boolean (value);
      break;
    case PROP_VSPM_DMABUF:
      space->use_dmabuf = g_value_get_boolean (value);
      if (space->use_dmabuf)
          space->outbuf_allocate = TRUE;
      break;
    case PROP_VSPM_CROP:
      space->enable_crop = gst_vspm_filter_parse_cropsize (object, value);
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
    case PROP_VSPM_INBUF:
      g_value_set_boolean (value, space->inbuf_allocate);
      break;
    case PROP_VSPM_DMABUF:
      g_value_set_boolean (value, space->use_dmabuf);
      break;
    case PROP_VSPM_CROP:
    {
      gchar *str_value = g_strdup_printf ("%d:%d:%d:%d",
                                          space->crop.left,
                                          space->crop.right,
                                          space->crop.top,
                                          space->crop.bottom);
      g_value_set_string (value, str_value);
      g_free (str_value);
      break;
    }
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
find_physical_address (GstVspmFilter *space, gpointer in_vir, gpointer *out_phy)
{
  struct MM_PARAM p_adr;
  GstFlowReturn ret;
  gint page_size, max_size_in_page;

  /* change virtual address to physical address */
  memset(&p_adr, 0, sizeof(p_adr));
  p_adr.user_virt_addr = (unsigned long)in_vir;
  ret = ioctl(space->vsp_info->mmngr_fd, MM_IOC_VTOP, &p_adr);
  if (ret) {
    GST_ERROR ("MMNGR VtoP Convert Error. \n");
    return GST_FLOW_ERROR;
  }
  /* Note that this method to find physical address may only find the address at
   * start of page. If there is an offset from page, we need to add it here */
  page_size = getpagesize ();
  max_size_in_page = page_size - 1;
  if ((p_adr.hard_addr & max_size_in_page) == 0)
    p_adr.hard_addr += (max_size_in_page & (unsigned long)in_vir);

  if (out_phy != NULL) *out_phy = (gpointer) p_adr.hard_addr;
  return GST_FLOW_OK;
}

/* TRUE if the dmabuf is one physically-contiguous block: a physical address
 * resolves and the imported mapped_size covers the full mem size. */
static gboolean
gst_vspm_filter_dmabuf_is_contiguous (GstMemory * mem)
{
  int import_pid;
  size_t mapped_size = 0;
  gpointer phys_addr = NULL;
  gsize mem_size;

  if (!gst_is_dmabuf_memory (mem))
    return FALSE;

  if (R_MM_OK != mmngr_import_start_in_user_ext (&import_pid, &mapped_size,
          (unsigned int *) &phys_addr, gst_dmabuf_memory_get_fd (mem), NULL))
    return FALSE;

  mmngr_import_end_in_user_ext (import_pid);

  mem_size = gst_memory_get_sizes (mem, NULL, NULL);
  return (phys_addr != NULL && mapped_size >= mem_size);
}

/* TRUE if the plane is physically contiguous: a dmabuf passing the import
 * probe (dmabuf-use mode), or every page following the previous one. */
static gboolean
gst_vspm_filter_mem_is_contiguous (GstVspmFilter * space, GstBuffer * buf,
    guint plane)
{
  GstMemory *mem = gst_buffer_peek_memory (buf, plane);
  gint page_size = getpagesize ();
  gpointer base = NULL;
  GstMapInfo map;
  gsize offset;

  if (!mem)
    return FALSE;

  if (space->use_dmabuf)
    return gst_vspm_filter_dmabuf_is_contiguous (mem);

  if (!gst_memory_map (mem, &map, GST_MAP_READ))
    return FALSE;

  /* Physical address of the first page */
  if (find_physical_address (space, map.data, &base) != GST_FLOW_OK) {
    gst_memory_unmap (mem, &map);
    return FALSE;
  }

  /* Every later page must follow directly on from the one before */
  for (offset = page_size - ((guintptr) map.data & (page_size - 1));
       offset < map.size; offset += page_size) {
    gpointer phys = NULL;

    if ((find_physical_address (space, (guint8 *) map.data + offset, &phys) != GST_FLOW_OK) ||
        ((guintptr) phys != ((guintptr) base + offset))) {
      gst_memory_unmap (mem, &map);
      return FALSE;
    }
  }

  gst_memory_unmap (mem, &map);
  return TRUE;
}

/* TRUE if every input plane's stride and offset match the output. Layout is
 * taken from the video meta if present, else from the negotiated input caps. */
static gboolean
gst_vspm_filter_buffer_layout_matches (GstVspmFilter * space, GstBuffer * buf)
{
  if (!space->out_gst_pool)
    return FALSE;

  VspmBufferInfo *outbuf_info =
      &GST_VSPMFILTER_BUFFER_POOL_CAST (space->out_gst_pool)->buf_info;
  GstVideoInfo *in_info = &GST_VIDEO_FILTER (space)->in_info;
  GstVideoMeta *vmeta = gst_buffer_get_video_meta (buf);
  guint plane, n_planes = outbuf_info->n_planes;

  if ((vmeta != NULL) && (vmeta->n_planes != n_planes))
    return FALSE;

  for (plane = 0; plane < n_planes; plane++) {
    gsize in_stride, in_offset;

    if (vmeta != NULL) {
      in_stride = vmeta->stride[plane];
      in_offset = vmeta->offset[plane];
    } else {
      in_stride = GST_VIDEO_INFO_PLANE_STRIDE (in_info, plane);
      in_offset = GST_VIDEO_INFO_PLANE_OFFSET (in_info, plane);
    }

    if ((in_stride != (gsize) outbuf_info->plane_stride[plane]) ||
        (in_offset != (gsize) outbuf_info->plane_offset[plane]))
      return FALSE;
  }

  return TRUE;
}

/* TRUE if every plane is contiguous and its layout matches the output. */
static gboolean
gst_vspm_filter_buffer_can_passthrough (GstVspmFilter * space, GstBuffer * buf)
{
  guint plane, n_planes;

  if (!buf)
    return FALSE;

  n_planes = gst_buffer_n_memory (buf);

  for (plane = 0; plane < n_planes; plane++) {
    if (!gst_vspm_filter_mem_is_contiguous (space, buf, plane))
      return FALSE;
  }

  if (!gst_vspm_filter_buffer_layout_matches (space, buf))
    return FALSE;

  return TRUE;
}

static void
gst_vspm_filter_import_fd (GstMemory *mem, gsize plane_offset, gpointer *out,
    GQueue *import_list)
{
  int fd;

  if (gst_is_dmabuf_memory(mem)) {
    int import_pid;
    size_t size;
    unsigned int phys_base = 0;

    fd = gst_dmabuf_memory_get_fd (mem);
    if (R_MM_OK == mmngr_import_start_in_user_ext (&import_pid, &size,
                                                   &phys_base, fd, NULL)) {
      /* import returns the page-aligned base of the dmabuf; the plane data
       * starts at mem->offset + the plane's byte offset within the buffer
       * (non-zero for packed multi-plane formats sharing one dmabuf) */
      *out = (gpointer) ((unsigned long) phys_base + mem->offset + plane_offset);
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

/* Convert a floating-point coefficient to ISU 14-bit two's complement
 * fixed-point (×1024). */
static unsigned int
fp_to_isu_fixed (gdouble val)
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
gst_vspm_filter_compute_csc (guint              src_fmt,
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
      csc_par->k_matrix[r][c] = fp_to_isu_fixed (math_m[r][col_map[c]]);

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
gst_vspm_filter_get_mem_phys_addr (GstVspmFilter * space, GstBuffer * buf,
    gpointer vir_addr, guint plane, gsize plane_offset, gpointer * out_phy)
{
  GstMemory *mem = NULL;
  guint n_mem;
  guint mem_idx;
  gsize offs;

  *out_phy = NULL;

  if (!buf)
    return GST_FLOW_ERROR;

  find_physical_address (space, vir_addr, out_phy);
  if (*out_phy != NULL)
    return GST_FLOW_OK;

  /* A single memory packs all planes (use memory 0 at the plane's byte offset);
   * otherwise there is one memory per plane, each starting at its own base. */
  n_mem   = gst_buffer_n_memory (buf);
  mem_idx = (n_mem == 1) ? 0 : plane;
  offs    = (n_mem == 1) ? plane_offset : 0;

  if (mem_idx < n_mem)
    mem = gst_buffer_peek_memory (buf, mem_idx);
  if (mem != NULL) {
    gst_vspm_filter_import_fd (mem, offs, out_phy, space->mmngr_import_list);
    if (*out_phy != NULL)
      return GST_FLOW_OK;
  }

  return GST_FLOW_ERROR;
}

static GstFlowReturn
gst_vspm_filter_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame)
{
  GstVspmFilter *space;
  GstVspmFilterVspInfo *vsp_info;

  VSPM_IP_PAR vspm_ip;
  VSPM_ISU_PAR isu_par;

  T_ISU_IN src_par;
  T_ISU_ALPHA src_alpha_par, dst_alpha_par;
  T_ISU_OUT dst_par;
  T_ISU_RS rs_par;

  gint in_width, in_height;
  gint out_width, out_height;
  long ercd;
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
    guint crop_in_width  = 0;
    guint crop_in_height = 0;
    guint crop_start_x   = 0;
    guint crop_start_y   = 0;
    gdouble scale_x      = 0;
    gdouble scale_y      = 0;

    memset(&rs_par, 0, sizeof(T_ISU_RS));

    /* Validate crop parameters against input dimensions */
    if ((space->crop.left + space->crop.right) >= in_width) {
      GST_ERROR ("Crop left(%u) + right(%u) >= input width(%d)\n",
                 space->crop.left, space->crop.right, in_width);
      ret = GST_FLOW_ERROR;
      goto err;
    }
    if ((space->crop.top + space->crop.bottom) >= in_height) {
      GST_ERROR ("Crop top(%u) + bottom(%u) >= input height(%d)\n",
                 space->crop.top, space->crop.bottom, in_height);
      ret = GST_FLOW_ERROR;
      goto err;
    }

    crop_start_x   = space->crop.left;
    crop_start_y   = space->crop.top;
    crop_in_width  = in_width  - space->crop.left - space->crop.right;
    crop_in_height = in_height - space->crop.top  - space->crop.bottom;

    GST_DEBUG_OBJECT (space,
        "crop: start(%u,%u) cropped size(%ux%u) from input(%dx%d)",
        crop_start_x, crop_start_y, crop_in_width, crop_in_height,
        in_width, in_height);

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

  memset(&vspm_ip, 0, sizeof(VSPM_IP_PAR));
  vspm_ip.uhType             = VSPM_TYPE_ISU_AUTO;
  vspm_ip.unionIpParam.ptisu = &isu_par;

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
