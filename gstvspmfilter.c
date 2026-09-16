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

GType gst_vspm_filter_get_type (void);

static GQuark _colorspace_quark;

#define gst_vspm_filter_parent_class parent_class
G_DEFINE_TYPE (GstVspmFilter, gst_vspm_filter, GST_TYPE_VIDEO_FILTER);
G_DEFINE_TYPE (GstVspmFilterBufferPool, gst_vspmfilter_buffer_pool, GST_TYPE_BUFFER_POOL);
#define CLEAR(x) memset (&(x), 0, sizeof (x))

static void gst_vspm_filter_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static gboolean gst_vspm_filter_parse_cropsize (GObject * object,
    const GValue * value, guint32 * left, guint32 * right, guint32 * top,
    guint32 * bottom);
static void append_caps_from_table (GstCaps * caps, const extensions_t * table);
static GstCaps *gst_vspm_filter_get_hw_caps (GstVspmFilter * space,
    GstPadDirection direction);
static gboolean gst_vspm_filter_accept_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps);
static GstFlowReturn gst_vspm_filter_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame);
static void gst_vspm_filter_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
GstFlowReturn
gst_vspm_filter_transform_buffer (GstVideoFilter * filter,
                                    GstBuffer * inbuf,
                                    GstBuffer * outbuf);

static void gst_vspm_filter_free_buffer_pools (GstVspmFilter * space);

static gboolean gst_vspm_filter_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query);

static gboolean gst_vspm_filter_set_info (GstVideoFilter * filter,
    GstCaps * incaps, GstVideoInfo * in_info, GstCaps * outcaps,
    GstVideoInfo * out_info);
static GstFlowReturn gst_vspm_filter_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame);

static void gst_vspm_filter_finalize (GObject * obj);

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

GstFlowReturn
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

GstFlowReturn
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

GstFlowReturn
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

/* Round each crop border DOWN to align with YUV specification, using whichever of
 * the input and output formats needs the larger unit; an unaligned border makes
 * the driver reject the job. A NULL format info is a format that is not fixed
 * yet. TRUE if a border changed. */
static gboolean
gst_vspm_filter_align_crop (const GstVideoFormatInfo * in_fmt_info,
    const GstVideoFormatInfo * out_fmt_info, guint * left, guint * right,
    guint * top, guint * bottom)
{
  guint h_unit   = 1;
  guint v_unit   = 1;
  guint o_left   = *left;
  guint o_right  = *right;
  guint o_top    = *top;
  guint o_bottom = *bottom;

  /* Component 1 is chroma; its subsampling gives the unit:
   *   YCbCr 4:2:0: 2-pixel units for both horizontal and vertical settings
   *   YCbCr 4:2:2: 2-pixel units for horizontal and 1-pixel units for vertical
   *   other formats: 1-pixel units for all dimensions
   * A non-YUV format keeps the unit at 1, so only the YUV side of a conversion
   * constrains it. */
  if (in_fmt_info && GST_VIDEO_FORMAT_INFO_IS_YUV (in_fmt_info)) {
    h_unit = 1U << GST_VIDEO_FORMAT_INFO_W_SUB (in_fmt_info, 1);
    v_unit = 1U << GST_VIDEO_FORMAT_INFO_H_SUB (in_fmt_info, 1);
  }
  if (out_fmt_info && GST_VIDEO_FORMAT_INFO_IS_YUV (out_fmt_info)) {
    h_unit = MAX (h_unit, 1U << GST_VIDEO_FORMAT_INFO_W_SUB (out_fmt_info, 1));
    v_unit = MAX (v_unit, 1U << GST_VIDEO_FORMAT_INFO_H_SUB (out_fmt_info, 1));
  }

  *left   -= *left   % h_unit;
  *right  -= *right  % h_unit;
  *top    -= *top    % v_unit;
  *bottom -= *bottom % v_unit;

  return ((o_left != *left) || (o_right  != *right) ||
          (o_top  != *top)  || (o_bottom != *bottom));
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
    guint c_left, c_right, c_top, c_bottom;

    if (gst_vspm_filter_get_crop_value (space, &c_left, &c_right, &c_top, &c_bottom)
        && direction == GST_PAD_SINK) {
      /* Going SINK->SRC with a crop set: propose the cropped size */
      gint crop_w, crop_h;

      const gchar *in_fmt  = gst_structure_get_string (ins, "format");
      const gchar *out_fmt = gst_structure_get_string (outs, "format");

      /* Align first so this size tracks the crop transform_frame() applies.
       * Best-effort: the out format may still be unfixed here. */
      (void) gst_vspm_filter_align_crop (
          in_fmt ?
          gst_video_format_get_info (gst_video_format_from_string (in_fmt)) : NULL,
          out_fmt ?
          gst_video_format_get_info (gst_video_format_from_string (out_fmt)) : NULL,
          &c_left, &c_right, &c_top, &c_bottom);

      crop_w = from_w - (gint) (c_left + c_right);
      crop_h = from_h - (gint) (c_top  + c_bottom);
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
 * The formats are then restricted to the table of the detected hardware on the
 * pad the result is for: the pad templates are the union of the ISU and VSP
 * tables, so they cannot express that per-instance limitation. */
static GstCaps *
gst_vspm_filter_transform_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *tmp, *tmp2;
  GstCaps *result;
  GstCaps *caps_full_range_sizes;
  GstCaps *hw_caps;
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

  /* Keep only the formats the detected hardware offers on the other pad. */
  hw_caps = gst_vspm_filter_get_hw_caps (GST_VIDEO_CONVERT_CAST (btrans),
      (direction == GST_PAD_SINK) ? GST_PAD_SRC : GST_PAD_SINK);
  tmp = gst_caps_intersect_full (hw_caps, caps_full_range_sizes,
      GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (hw_caps);
  gst_caps_unref (caps_full_range_sizes);

  if (filter) {
    tmp2 = gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);

    gst_caps_unref (tmp);
    tmp = tmp2;
  }

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

static void
gst_vspm_filter_free_buffer_pools (GstVspmFilter * space)
{
  /* Release the importing to avoid leak FD */
  gst_vspm_filter_release_fd (space->mmngr_import_list);

  gst_vspm_filter_free_pool (&space->in_gst_pool);
  gst_vspm_filter_free_pool (&space->out_gst_pool);
}

/* Clear crop_dirty, validate the crop for the negotiated caps (warning if it is
 * out of range), align the borders for them, and set passthrough to match. */
static void
gst_vspm_filter_configure_crop (GstVspmFilter * space, GstVideoInfo * in_info,
    GstVideoInfo * out_info)
{
  guint in_w = GST_VIDEO_INFO_WIDTH (in_info);
  guint in_h = GST_VIDEO_INFO_HEIGHT (in_info);
  gboolean crop_needed  = FALSE;
  gboolean out_of_range = FALSE;
  gboolean was_aligned  = FALSE;

  GST_OBJECT_LOCK (space);
  out_of_range = ((space->crop.left + space->crop.right)  >= in_w ||
                  (space->crop.top  + space->crop.bottom) >= in_h);
  if (!out_of_range) {
    was_aligned = gst_vspm_filter_align_crop (in_info->finfo, out_info->finfo,
        &space->crop.left, &space->crop.right, &space->crop.top,
        &space->crop.bottom);

    /* The aligned borders decide whether anything is left to crop. */
    crop_needed = (space->crop.left || space->crop.right ||
                   space->crop.top  || space->crop.bottom);
  }
  space->crop_dirty = FALSE;
  GST_OBJECT_UNLOCK (space);

  if (out_of_range) {
    /* Too big to fit: warn and skip, but keep the borders for a larger input. */
    GST_ELEMENT_WARNING (space, STREAM, FORMAT,
        ("Crop out of range for %ux%u input, ignoring it", in_w, in_h),
        ("crop left:right:top:bottom %u:%u:%u:%u does not fit "
         "(left+right=%u vs width=%u, top+bottom=%u vs height=%u)",
         space->crop.left, space->crop.right, space->crop.top,
         space->crop.bottom, space->crop.left + space->crop.right, in_w,
         space->crop.top + space->crop.bottom, in_h));
  } else if (was_aligned) {
    GST_DEBUG_OBJECT (space, "crop left:right:top:bottom aligned down to "
        "%u:%u:%u:%u for the YUV specification of the negotiated format",
        space->crop.left, space->crop.right, space->crop.top,
        space->crop.bottom);
  }

  /* Set both ways, not just off: on the prepare_output_buffer() path nothing
   * else restores it. Pass through only with no crop and equal caps. */
  gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (space),
      !crop_needed && gst_video_info_is_equal (in_info, out_info));
}

static gboolean
gst_vspm_filter_set_info (GstVideoFilter * filter,
    GstCaps * incaps, GstVideoInfo * in_info, GstCaps * outcaps,
    GstVideoInfo * out_info)
{
  GstVspmFilter *space;
  GstStructure *structure;

  space = GST_VIDEO_CONVERT_CAST (filter);
  /* these must match */
  if (in_info->fps_n != out_info->fps_n || in_info->fps_d != out_info->fps_d)
    goto format_mismatch;

  /* if present, these must match too */
  if (in_info->interlace_mode != out_info->interlace_mode)
    goto format_mismatch;

  gst_vspm_filter_configure_crop (space, in_info, out_info);

  GST_DEBUG ("reconfigured %d %d", GST_VIDEO_INFO_FORMAT (in_info),
      GST_VIDEO_INFO_FORMAT (out_info));

  if (!space->ops->set_info (filter, incaps, in_info, outcaps, out_info))
    goto format_mismatch;

  if(space->outbuf_allocate) {
    VspmBufferInfo *out_buf_info;

    /* Drop any pool from a previous negotiation so we never hand out
     * buffers sized for stale caps after a resolution change. */
    gst_vspm_filter_free_pool (&space->out_gst_pool);

    /* create a new buffer pool; its buf_info is filled in below */
    space->out_gst_pool = gst_vspmfilter_buffer_pool_new (space);
    out_buf_info =
        &GST_VSPMFILTER_BUFFER_POOL_CAST (space->out_gst_pool)->buf_info;

    space->ops->set_buffer_info (space, out_buf_info, out_info, NULL);

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

  /* Rebuild only when the input changed; filter->in_info still holds the
   * previous caps, GstVideoFilter assigns the new one after set_info(). */
  if (space->inbuf_allocate &&
      ((space->in_gst_pool == NULL) ||
       (!gst_video_info_is_equal (&filter->in_info, in_info)))) {
    VspmBufferInfo *in_buf_info;

    /* Drop any pool from a previous negotiation so we never hand out
     * buffers sized for stale caps after a resolution change. */
    gst_vspm_filter_free_pool (&space->in_gst_pool);

    space->in_gst_pool = gst_vspmfilter_buffer_pool_new (space);
    in_buf_info =
        &GST_VSPMFILTER_BUFFER_POOL_CAST (space->in_gst_pool)->buf_info;

    space->ops->set_buffer_info (space, in_buf_info, in_info, NULL);

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
  } else if (!space->inbuf_allocate) {
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

      space->ops->set_buffer_info (space, out_buf_info, NULL, &align);

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
    GstVideoFilter *filter = GST_VIDEO_FILTER_CAST (trans);
    GstFlowReturn ret = GST_FLOW_OK;
    gboolean crop_dirty;

    /* Apply a pending crop that set_info() was skipped for, still ahead of the
     * passthrough check so it counts for this buffer. */
    GST_OBJECT_LOCK (space);
    crop_dirty = space->crop_dirty;
    GST_OBJECT_UNLOCK (space);
    if (G_UNLIKELY (crop_dirty))
      gst_vspm_filter_configure_crop (space, &filter->in_info,
          &filter->out_info);

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
append_caps_from_table (GstCaps * caps, const extensions_t * table)
{
  gint i;
  gint nr = 0;

  while (table[nr].gst_format != GST_VIDEO_FORMAT_UNKNOWN)
    nr++;

  for (i = 0; i < nr; i++) {
    GstCaps *tmpcaps = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING, gst_video_format_to_string (table[i].gst_format),
        "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
    gst_caps_append (caps, tmpcaps);
  }
}

/* Build the caps the detected hardware can handle on this pad. The pad
 * template advertises the union of the ISU and VSP tables because the platform
 * is only known per instance; transform_caps() and accept_caps() narrow it down
 * to the table of the detected platform at runtime. */
static GstCaps *
gst_vspm_filter_build_hw_caps (GstVspmFilter * space,
    GstPadDirection direction)
{
  GstCaps *caps = gst_caps_new_empty ();

  append_caps_from_table (caps, (direction == GST_PAD_SINK) ?
      space->ops->exts : space->ops->exts_out);

  return caps;
}

/* The cached hardware caps of this instance (built in _init); returns a ref. */
static GstCaps *
gst_vspm_filter_get_hw_caps (GstVspmFilter * space, GstPadDirection direction)
{
  return gst_caps_ref ((direction == GST_PAD_SINK) ?
      space->hw_caps_sink : space->hw_caps_src);
}

static gboolean
gst_vspm_filter_accept_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (trans);
  GstCaps *hw_caps = gst_vspm_filter_get_hw_caps (space, direction);
  gboolean ret = gst_caps_is_subset (caps, hw_caps);

  gst_caps_unref (hw_caps);

  return ret;
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

GstFlowReturn
gst_vspm_filter_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (filter);
  GstVspmFilterVspInfo *vsp_info = space->vsp_info;
  VSPM_IP_PAR ip_par;
  GstFlowReturn ret;
  gboolean submit = TRUE;
  long ercd;

  /* The platform file builds the per-frame IP parameters into ip_par; the
   * Entry, the completion wait and the fd drain live here, next to the other
   * VSPM_lib_* calls (DriverInitialize/GetHwType/DriverQuit). */
  ret = space->ops->transform_frame_options (filter, in_frame, out_frame,
      &ip_par, &submit);

  if (ret == GST_FLOW_OK && submit) {
    ercd = VSPM_lib_Entry (vsp_info->vspm_handle, &vsp_info->jobid, 126,
        &ip_par, (unsigned long)&space->smp_wait, cb_func);
    if (ercd) {
      GST_ERROR ("VSPM_lib_Entry() Failed!! ercd=%ld\n", ercd);
      ret = GST_FLOW_ERROR;
    } else {
      /* Wait for callback */
      do {
        ercd = sem_wait (&space->smp_wait);
      } while (ercd != 0 && errno == EINTR);
      if (ercd != 0) {
        GST_ERROR ("sem_wait() Failed!! ercd=%ld\n", (long)ercd);
        ret = GST_FLOW_ERROR;
      }
    }
  }

  /* Release the importing done by get_mem_phys_addr() to avoid leak FD.
   * The drain lives here (common), not in the platform files, so the
   * import/release pair stays in one layer. */
  gst_vspm_filter_release_fd (space->mmngr_import_list);

  return ret;
}

static void
gst_vspm_filter_class_init (GstVspmFilterClass * klass)
{
  GstCaps* incaps;
  GstCaps* outcaps;
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

  gstbasetransform_class->accept_caps =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_accept_caps);

  incaps  = gst_caps_new_empty();
  outcaps = gst_caps_new_empty();

  /* The template advertises the union of the ISU and VSP format tables; the
   * platform is only known per instance, so transform_caps() and accept_caps()
   * narrow it down to the table of the detected hardware at runtime. */
  {
    static const GstVspmFilterOps *const ops_variants[] =
        { &vsp_ops, &isu_ops };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (ops_variants); i++) {
      append_caps_from_table (incaps, ops_variants[i]->exts);
      append_caps_from_table (outcaps, ops_variants[i]->exts_out);
    }
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
        "Crop input frames. Format: \"left:right:top:bottom\" in pixels. Reads "
        "back the borders in effect, rounded down to align with YUV "
        "specification.",
        NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
        GST_PARAM_MUTABLE_PLAYING));
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

  gst_caps_replace (&space->hw_caps_sink, NULL);
  gst_caps_replace (&space->hw_caps_src, NULL);

  g_clear_pointer (&vsp_info->cached_csc, g_free);
  if (space->vsp_info)
    g_free (space->vsp_info);
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
  /* mmngr dev open */
  vsp_info->mmngr_fd = open(DEVFILE, O_RDWR);
  if (vsp_info->mmngr_fd == -1) {
    GST_ERROR ("MMNGR: open error. \n");
  }

  /* Single-phase init: merged libvspm handles detect + init in one call.
   * VSPM_lib_DriverInitialize trials VSP then ISU internally, returns
   * a handle with cb_list/mutex ready. Then GetHwType tells us which
   * ops table to use. */
  if (VSPM_lib_DriverInitialize(&vsp_info->vspm_handle) == R_VSPM_OK) {
    int hw_type = 0;
    vsp_info->is_init_vspm = TRUE;

    if (VSPM_lib_GetHwType(vsp_info->vspm_handle, &hw_type) == R_VSPM_OK) {
      if (hw_type == VSPM_TYPE_VSP_AUTO) {
        space->ops = &vsp_ops;
        GST_INFO ("VSPM: Detected VSP hardware");
      } else if (hw_type == VSPM_TYPE_ISU_AUTO) {
        space->ops = &isu_ops;
        GST_INFO ("VSPM: Detected ISU hardware");
      } else {
        GST_ERROR ("VSPM: Unknown hw type %d\n", hw_type);
        space->ops = &isu_ops; /* fallback */
      }
    } else {
      GST_ERROR ("VSPM: GetHwType failed\n");
      space->ops = &isu_ops; /* fallback */
    }
  } else {
    GST_ERROR ("VSPM: Error Initialized. \n");
    space->ops = &isu_ops; /* fallback */
  }

  /* Cache the caps of the detected platform: the pad templates are the union
   * of both platforms and cannot be narrowed down per instance. */
  space->hw_caps_sink = gst_vspm_filter_build_hw_caps (space, GST_PAD_SINK);
  space->hw_caps_src  = gst_vspm_filter_build_hw_caps (space, GST_PAD_SRC);

  space->allocator          = gst_dmabuf_allocator_new ();
  space->outbuf_allocate    = FALSE;
  space->inbuf_allocate     = FALSE;
  space->in_gst_pool        = NULL;
  space->out_gst_pool       = NULL;
  space->use_dmabuf         = FALSE;
  space->mmngr_import_list  = g_queue_new ();

  vsp_info->cached_csc = NULL;

  /* Initialize crop to disabled */
  space->crop.left   = 0;
  space->crop.right  = 0;
  space->crop.top    = 0;
  space->crop.bottom = 0;
  space->crop_dirty  = FALSE;

  sem_init (&space->smp_wait, 0, 0);
}

static gboolean
gst_vspm_filter_parse_cropsize (GObject * object, const GValue * value,
    guint32 * left, guint32 * right, guint32 * top, guint32 * bottom)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (object);
  const gchar *str_value = g_value_get_string (value);

  gchar **str_arr = NULL, *end_char;
  gint length = 4; /* fixed-size array */
  gint64 crop_arr[4] = { 0, };
  gint i;

  if (str_value == NULL)
    goto error;

  str_arr = g_strsplit (str_value, ":", length);
  if (str_arr == NULL)
    goto error;

  for (i = 0; i < length; i++) {
    if (str_arr[i] == NULL || *str_arr[i] == '\0') /* Empty string */
      goto error;
    else
      crop_arr[i] = g_ascii_strtoll (str_arr[i], &end_char, 10);

    if (*end_char != '\0') /* Invalid end character */
      goto error;
    if (crop_arr[i] < 0 || crop_arr[i] > G_MAXINT)
      goto error;
  }
  g_strfreev (str_arr);

  /* Hand the parsed borders back to the caller. */
  *left   = crop_arr[0];
  *right  = crop_arr[1];
  *top    = crop_arr[2];
  *bottom = crop_arr[3];

  return TRUE;

error:
  GST_ERROR_OBJECT (space, "Failed to parse crop size: %s. Keeping the crop "
                    "already in effect", str_value);
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
    {
      GstVideoFilter *filter = GST_VIDEO_FILTER_CAST (object);
      guint32 left, right, top, bottom;

      if (!gst_vspm_filter_parse_cropsize (object, value, &left, &right, &top,
              &bottom))
        break;

      /* Reject a crop too big for the current input and keep the running one,
       * so a bad value never drops a working crop, like one that fails to
       * parse. Only checkable once negotiated. in_info is read unlocked on
       * purpose: transform_frame() re-checks it on the streaming thread, so a
       * stale size here at worst rejects a good crop or defers a bad one. */
      if (filter->negotiated &&
          ((left + right) >= (guint) GST_VIDEO_INFO_WIDTH (&filter->in_info) ||
           (top + bottom) >= (guint) GST_VIDEO_INFO_HEIGHT (&filter->in_info))) {
        GST_WARNING_OBJECT (space, "Crop %u:%u:%u:%u out of range for %dx%d "
            "input, keeping the crop already in effect", left, right, top,
            bottom, GST_VIDEO_INFO_WIDTH (&filter->in_info),
            GST_VIDEO_INFO_HEIGHT (&filter->in_info));
        break;
      }

      GST_DEBUG_OBJECT (space, "crop left:right:top:bottom set to %u:%u:%u:%u",
          left, right, top, bottom);

      if (filter->negotiated &&
          gst_vspm_filter_align_crop (filter->in_info.finfo,
              filter->out_info.finfo, &left, &right, &top, &bottom))
        GST_DEBUG_OBJECT (space, "crop left:right:top:bottom aligned down to "
            "%u:%u:%u:%u for the YUV specification of the negotiated format",
            left, right, top, bottom);

      /* Store the borders and the flag together, the streaming thread reads
       * them under this lock. */
      GST_OBJECT_LOCK (space);
      space->crop.left   = left;
      space->crop.right  = right;
      space->crop.top    = top;
      space->crop.bottom = bottom;
      space->crop_dirty  = TRUE;
      GST_OBJECT_UNLOCK (space);

      /* Renegotiate so the borders can shape the output size. */
      gst_base_transform_reconfigure_src (trans);
      break;
    }
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
      guint c_left, c_right, c_top, c_bottom;
      gchar *str_value;

      (void) gst_vspm_filter_get_crop_value (space, &c_left, &c_right, &c_top, &c_bottom);
      str_value = g_strdup_printf ("%u:%u:%u:%u",
                                   c_left, c_right, c_top, c_bottom);
      g_value_set_string (value, str_value);
      g_free (str_value);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
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
