/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * This file:
 * Copyright (C) 2003 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2010 David Schleef <ds@schleef.org>
 * Copyright (C) 2014 Renesas Corporation
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

volatile unsigned char end_flag = 0;  /* wait vspm-callback flag */

#define gst_vspm_filter_parent_class parent_class
G_DEFINE_TYPE (GstVspmFilter, gst_vspm_filter, GST_TYPE_VIDEO_FILTER);
#define CLEAR(x) memset (&(x), 0, sizeof (x))

#define CSP_VIDEO_CAPS GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL)

static GstStaticPadTemplate gst_vspm_filter_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (CSP_VIDEO_CAPS)
    );

static GstStaticPadTemplate gst_vspm_filter_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (CSP_VIDEO_CAPS)
    );

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
  PROP_VSPM_DMABUF,
  PROP_VSPM_OUTBUF
};

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

/* Note that below swap information will be reversed later (in set_colorspace)
 *     because current system use Little Endian */
static const struct extensions_t exts[] = {
  {GST_VIDEO_FORMAT_BGRA,  VSP_IN_ARGB8888,          VSP_SWAP_B | VSP_SWAP_W},  /* Not supported in VSP. Use ARGB8888, and swap ARGB -> RABG -> BGRA */
  {GST_VIDEO_FORMAT_ARGB,  VSP_IN_ARGB8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_xRGB,  VSP_IN_ARGB8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGBx,  VSP_IN_RGBA8888,          VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_RGB,   VSP_IN_RGB888,            VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV21,  VSP_IN_YUV420_SEMI_NV21,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV12,  VSP_IN_YUV420_SEMI_NV12,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_YUY2,  VSP_IN_YUV422_INT0_YUY2,  VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_UYVY,  VSP_IN_YUV422_INT0_UYVY,  VSP_SWAP_NO},
};

static const struct extensions_t exts_out[] = {
  /* VSP_OUT_PRGB8888 is ARGB8888, maybe a mistake in define name */
  {GST_VIDEO_FORMAT_BGRA,  VSP_OUT_PRGB8888,         VSP_SWAP_B | VSP_SWAP_W},  /* Not supported in VSP. Use ARGB8888, and swap ARGB -> RABG -> BGRA */
  {GST_VIDEO_FORMAT_ARGB,  VSP_OUT_PRGB8888,         VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_xRGB,  VSP_OUT_PRGB8888,         VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV21,  VSP_OUT_YUV420_SEMI_NV21, VSP_SWAP_NO},
  {GST_VIDEO_FORMAT_NV12,  VSP_OUT_YUV420_SEMI_NV12, VSP_SWAP_NO},
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
  gint i;

  space = GST_VIDEO_CONVERT_CAST (filter);
  vspm_out = space->vspm_out;
  vspm_outbuf = space->vspm_outbuf;
  /* these must match */
  if (in_info->fps_n != out_info->fps_n || in_info->fps_d != out_info->fps_d)
    goto format_mismatch;

  /* if present, these must match too */
  if (in_info->interlace_mode != out_info->interlace_mode)
    goto format_mismatch;

  GST_DEBUG ("reconfigured %d %d", GST_VIDEO_INFO_FORMAT (in_info),
      GST_VIDEO_INFO_FORMAT (out_info));
  if(space->use_dmabuf){
    switch (GST_VIDEO_INFO_FORMAT (out_info)) {
      case GST_VIDEO_FORMAT_NV12:
        for (i = 0; i < VSPM_BUFFERS; i++){
          GstBuffer *buf;
          gsize offset[4] = { 0, };
          gint stride[4] = { 0, };
          gsize plane_size[4] = { 0, };
          buf = gst_buffer_new();
          g_ptr_array_add (vspm_outbuf->buf_array, buf);
          offset[0] = 0;
          offset[1] = out_info->width * out_info->height;
          stride[0] = out_info->width;
          stride[1] = out_info->width;
          gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_FORMAT_NV12,
            out_info->width,
            out_info->height,
            2, offset, stride);
        }
        break;
      default :
              printf("ERROR : output format not supported\n");
          break;
    }
  } else if(space->outbuf_allocate) {
    switch (GST_VIDEO_INFO_FORMAT (out_info)) {
      case GST_VIDEO_FORMAT_NV12:
      {
        GstQuery *query;
        GValue val = { 0, };
        GstStructure *structure;
        gint vspm_used ;
        GArray *outbuf_paddr_array;
        GArray *outbuf_vaddr_array;

        outbuf_paddr_array = g_array_new (FALSE, FALSE, sizeof (gulong));
        outbuf_vaddr_array = g_array_new (FALSE, FALSE, sizeof (gulong));

        for (i = 0; i < 5; i++){
          GstBuffer *buf;
          guint size;
          gsize offset[4] = { 0, };
          gint stride[4] = { 0, };
          gsize plane_size[4] = { 0, };

          size = out_info->width * out_info->height * 3/2;
          vspm_used = vspm_out->used;
          if (R_MM_OK == mmngr_alloc_in_user(&vspm_out->vspm[vspm_used].mmng_pid,
                                    size,
                                    &vspm_out->vspm[vspm_used].pphy_addr,
                                    &vspm_out->vspm[vspm_used].phard_addr,
                                    &vspm_out->vspm[vspm_used].puser_virt_addr,
                                    MMNGR_VA_SUPPORT)) {
            vspm_out->used++;
            vspm_out->vspm[vspm_used].dmabuf_pid = -1;
            g_array_append_val (outbuf_paddr_array, 
                                vspm_out->vspm[vspm_used].phard_addr);
            g_array_append_val (outbuf_vaddr_array, 
                                vspm_out->vspm[vspm_used].puser_virt_addr);

          } else {
            printf("MMNGR: allocation error\n");
          }

          buf = gst_buffer_new_wrapped (
                  (gpointer)vspm_out->vspm[vspm_used].puser_virt_addr,
                  (gsize)size);

          g_ptr_array_add (vspm_outbuf->buf_array, buf);
          offset[0] = 0;
          offset[1] = out_info->width * out_info->height;
          stride[0] = out_info->width;
          stride[1] = out_info->width;
          gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_FORMAT_NV12,
            out_info->width,
            out_info->height,
            2, offset, stride);
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
        break;
      }
      default :
        printf("ERROR : output format not supported\n");
        break;
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

static int buffer_get_dmabuf_fd(Vspm_mmng_ar* vspm_buff_array, GstBuffer * buffer)
{
  int i = vspm_buff_array->used;
  while (i) {
    i--;
      if (buffer == vspm_buff_array->vspm[i].buf)
        return vspm_buff_array->vspm[i].dmabuf_fd;
  }

  return 0;
}

static unsigned long buffer_get_dmabuf_hard_addr(Vspm_mmng_ar* vspm_buff_array, GstBuffer * buffer)
{
  int i = vspm_buff_array->used;
  while (i) {
    i--;
    if (buffer == vspm_buff_array->vspm[i].buf)
      return vspm_buff_array->vspm[i].phard_addr;
  }

    return 0;
}

static GstFlowReturn
gst_vspm_filter_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
    GstVideoFilter *filter = GST_VIDEO_FILTER_CAST (trans);
    GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (filter);
    GstMemory *in_gmem[GST_VIDEO_MAX_PLANES], *out_gmem[GST_VIDEO_MAX_PLANES];
    GstVideoFrame in_frame, out_frame;
    GstVideoMeta *meta_in,*meta_out;
    gint out_stride;
    gint in_n_mem, out_n_mem;
    gint i;
    GstFlowReturn res = GST_FLOW_OK;
    if(space->use_dmabuf) {
        gst_buffer_ref(inbuf);
        if (G_UNLIKELY (!filter->negotiated))
            goto unknown_format;

        res = gst_vspm_filter_transform_buffer(trans,inbuf,outbuf);
    } else {
        res = GST_BASE_TRANSFORM_CLASS(parent_class)->transform(trans, inbuf,outbuf);
    }

    return res;

    /* ERRORS */
unknown_format:
  {
    GST_ELEMENT_ERROR (filter, CORE, NOT_IMPLEMENTED, (NULL),
        ("unknown format"));
    return GST_FLOW_NOT_NEGOTIATED;
  }
}

static GstBuffer *
gst_vspmfilter_buffer_pool_create_buffer_from_dmabuf (GstVspmFilter *space,
    gint dmabuf[GST_VIDEO_MAX_PLANES], GstAllocator * allocator, gint width,
    gint height, gint in_stride[GST_VIDEO_MAX_PLANES], GstVideoFormat format,
    gint n_planes)
{
  GstBuffer *buffer;
  gsize offset[GST_VIDEO_MAX_PLANES] = { 0 };
  gint stride[GST_VIDEO_MAX_PLANES] = { 0 };
  gint i;
  Vspm_mmng_ar *vspm_in;
  Vspm_dmabuff vspm_buf;

  vspm_in = space->vspm_in;

  buffer = gst_buffer_new ();
  unsigned long psize;
  vspm_buf.buf = buffer;
  vspm_buf.dmabuf_fd = dmabuf[0];
  if (R_MM_OK == mmngr_import_start_in_user(&vspm_buf.dmabuf_pid,
			&psize,
			&vspm_buf.phard_addr,
			vspm_buf.dmabuf_fd))
  {
    int dmabf_used = vspm_in->used;
    vspm_in->vspm[dmabf_used] = vspm_buf;
    vspm_in->used++;
  }

  for (i = 0; i < n_planes; i++) {
    gst_buffer_append_memory (buffer,
        gst_dmabuf_allocator_alloc (allocator, dmabuf[i], 0));

    stride[i] = in_stride[i];
  }

  gst_buffer_add_video_meta_full (buffer, GST_VIDEO_FRAME_FLAG_NONE, format,
      width, height, n_planes, offset, stride);

  return buffer;
}

static gboolean
gst_vspm_filter_propose_allocation (GstBaseTransform *trans, 
                                    GstQuery *decide_query,
                                    GstQuery *query)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (trans);
  GstBufferPool *pool;
  GstStructure *config;
  GstCaps *caps;
  guint size;
  gboolean need_pool;
  gboolean ret = TRUE;

  if(space->use_dmabuf) {
    gst_query_parse_allocation (query, &caps, &need_pool);

    if (caps == NULL)
        goto no_caps;

    if (need_pool) {
        GstVideoInfo info;

        if (!gst_video_info_from_caps (&info, caps))
          goto invalid_caps;
        GST_DEBUG_OBJECT (trans, "create new pool");
        pool = gst_video_buffer_pool_new ();

        /* the normal size of a frame */
        size = info.size;

        config = gst_buffer_pool_get_config (pool);
        gst_buffer_pool_config_set_params (config, caps, size,
            VSPM_BUFFERS, VSPM_BUFFERS);
        gst_structure_set (config, "videosink_buffer_creation_request_supported",
            G_TYPE_BOOLEAN, TRUE, NULL);

        gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
        if (!gst_buffer_pool_set_config (pool, config))
          goto config_failed;
    }

    if (pool) {
        gst_query_add_allocation_pool (query, pool, size,
            VSPM_BUFFERS, VSPM_BUFFERS);
        space->in_port_pool = pool;
        gst_object_unref (pool);
    }

  } else {
    ret = GST_BASE_TRANSFORM_CLASS(parent_class)->propose_allocation(trans, 
                                                        decide_query,query);
  }

  return ret;

  /* ERRORS */
no_caps:
  {
    GST_ERROR_OBJECT (trans, "no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_ERROR_OBJECT (trans, "invalid caps specified");
    return FALSE;
  }
config_failed:
  {
    GST_ERROR_OBJECT (trans, "failed setting config");
    gst_object_unref (pool);
    return FALSE;
  }
}

static gboolean
gst_vspmfiler_query (GstBaseTransform *trans, GstPadDirection direction,
                                   GstQuery *query)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (trans);
  gboolean ret = FALSE;

  if(space->use_dmabuf) {
    switch (GST_QUERY_TYPE (query)) {
        case GST_QUERY_CUSTOM:
        {
          const GstStructure *structure;
          GstStructure *str_writable;
          gint dmabuf[GST_VIDEO_MAX_PLANES] = { 0 };
          GstAllocator *allocator;
          gint width, height;
          gint stride[GST_VIDEO_MAX_PLANES] = { 0 };
          const gchar *str;
          const GValue *p_val;
          GValue val = { 0, };
          GstVideoFormat format;
          GstBuffer *buffer;
          GArray *dmabuf_array;
          GArray *stride_array;
          gint n_planes;
          gint i;

          structure = gst_query_get_structure (query);
          if (structure == NULL
              || !gst_structure_has_name (structure,
                  "videosink_buffer_creation_request")) {
            GST_LOG_OBJECT (trans, "not a vsink_buffer_creation_request query");
            break;
          }

          GST_DEBUG_OBJECT (trans,
              "received a videosink_buffer_creation_request query");

          gst_structure_get (structure, "width", G_TYPE_INT, &width,
              "height", G_TYPE_INT, &height, "stride", G_TYPE_ARRAY, &stride_array,
              "dmabuf", G_TYPE_ARRAY, &dmabuf_array,
              "n_planes", G_TYPE_INT, &n_planes,
              "allocator", G_TYPE_POINTER, &p_val,
              "format", G_TYPE_STRING, &str, NULL);

          allocator = (GstAllocator *) g_value_get_pointer (p_val);
          if (allocator == NULL) {
            GST_WARNING_OBJECT (trans,
                "an invalid allocator in videosink_buffer_creation_request query");
            break;
          }

          format = gst_video_format_from_string (str);
          if (format == GST_VIDEO_FORMAT_UNKNOWN) {
            GST_WARNING_OBJECT (trans,
                "invalid color format in videosink_buffer_creation_request query");
            break;
          }

          for (i = 0; i < n_planes; i++) {
            dmabuf[i] = g_array_index (dmabuf_array, gint, i);
            stride[i] = g_array_index (stride_array, gint, i);
            GST_DEBUG_OBJECT (trans, "plane:%d dmabuf:%d stride:%d\n", i, dmabuf[i],
                stride[i]);
          }

          GST_DEBUG_OBJECT (trans,
              "videosink_buffer_creation_request query param: width:%d height:%d allocator:%p format:%s",
              width, height, allocator, str);

          buffer = gst_vspmfilter_buffer_pool_create_buffer_from_dmabuf (
              space, dmabuf, allocator, width, height, stride, format, n_planes);
          if (buffer == NULL) {
            GST_WARNING_OBJECT (trans,
                "failed to create a buffer from videosink_buffer_creation_request query");
            break;
          }

          g_value_init (&val, GST_TYPE_BUFFER);
          gst_value_set_buffer (&val, buffer);
          gst_buffer_unref (buffer);
          str_writable = gst_query_writable_structure (query);
          gst_structure_set_value (str_writable, "buffer", &val);

          ret = TRUE;
          break;
        }

        default:
          ret = GST_BASE_TRANSFORM_CLASS(parent_class)->query(trans,direction,query);
          break;
    }
  } else {
    ret = GST_BASE_TRANSFORM_CLASS(parent_class)->query(trans,direction,query);
  }

  return ret;
}

static GstBuffer *
gst_vspm_filter_buffer_pool_request_videosink_buffer_creation (GstVspmFilter * space,
    gint dmabuf_fd[GST_VIDEO_MAX_PLANES], gint stride[GST_VIDEO_MAX_PLANES], 
    gint width, gint height)
{
  GstQuery *query;
  GValue val = { 0, };
  GstStructure *structure;
  const GValue *value;
  GstBuffer *buffer;
  GArray *dmabuf_array;
  GArray *stride_array;
  gint n_planes;
  gint i;

  g_value_init (&val, G_TYPE_POINTER);
  g_value_set_pointer (&val, (gpointer) space->allocator);

  dmabuf_array = g_array_new (FALSE, FALSE, sizeof (gint));
  stride_array = g_array_new (FALSE, FALSE, sizeof (gint));

  n_planes = space->vsp_info->out_nplane;
  for (i = 0; i < n_planes; i++) {
    g_array_append_val (dmabuf_array, dmabuf_fd[i]);
    g_array_append_val (stride_array, stride[i]);
  }

    structure = gst_structure_new ("videosink_buffer_creation_request",
      "width", G_TYPE_INT, width,
      "height", G_TYPE_INT, height,
      "stride", G_TYPE_ARRAY, stride_array,
      "dmabuf", G_TYPE_ARRAY, dmabuf_array,
      "allocator", G_TYPE_POINTER, &val,
      "format", G_TYPE_STRING,
      gst_video_format_to_string (GST_VIDEO_FORMAT_NV12),
      "n_planes", G_TYPE_INT, n_planes, NULL);

  query = gst_query_new_custom (GST_QUERY_CUSTOM, structure);

  GST_DEBUG_OBJECT (space, "send a videosink_buffer_creation_request query");

  if (!gst_pad_peer_query (GST_BASE_TRANSFORM_SRC_PAD (GST_ELEMENT_CAST(space)), query)) {
    GST_ERROR_OBJECT (space, "videosink_buffer_creation_request query failed");
    return NULL;
  }

  value = gst_structure_get_value (structure, "buffer");
  buffer = gst_value_get_buffer (value);
  if (buffer == NULL) {
    GST_ERROR_OBJECT (space,
        "could not get a buffer from videosink_buffer_creation query");
    return NULL;
  }

  gst_query_unref (query);

  g_array_free (dmabuf_array, TRUE);
  g_array_free (stride_array, TRUE);

  return buffer;
}

static GstFlowReturn gst_vspm_filter_prepare_output_buffer (GstBaseTransform * trans,
                                          GstBuffer *inbuf, GstBuffer **outbuf)
{
    GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (trans);
    GstBuffer *buf;
    guint n_mem,i;
    GstVideoMeta *meta_in;
    GstFlowReturn ret = GST_FLOW_OK;
    Vspm_mmng_ar *vspm_out;
    VspmbufArray *vspm_outbuf;

    vspm_out = space->vspm_out;
    vspm_outbuf = space->vspm_outbuf;

    if(space->use_dmabuf) {
        while(TRUE) {
            buf = g_ptr_array_index (vspm_outbuf->buf_array, vspm_outbuf->current_buffer_index);
            n_mem = gst_buffer_n_memory (buf);
            if (n_mem == 0) {
              GstBuffer *new_buf;
              GstVideoMeta *vmeta;
              gint n_planes;
              gint size;
              gint vspm_used = vspm_out->used;
              gint dmabuf_fd[GST_VIDEO_MAX_PLANES];
              gint plane_size[GST_VIDEO_MAX_PLANES];

              vmeta = gst_buffer_get_video_meta (buf);
              switch(vmeta->format) {
                case GST_VIDEO_FORMAT_NV12:
                    size = (vmeta->width * vmeta->height ) * 2;

                    if (R_MM_OK == mmngr_alloc_in_user(&vspm_out->vspm[vspm_used].mmng_pid,
                                        size,
                                        &vspm_out->vspm[vspm_used].pphy_addr,
                                        &vspm_out->vspm[vspm_used].phard_addr,
                                        &vspm_out->vspm[vspm_used].puser_virt_addr,
                                        MMNGR_VA_SUPPORT)) {
                        vspm_out->used++;
                      } else {
                        printf("MMNGR: allocation error\n");
                      }
                    break;
                default : 
                    printf("ERROR: Format not supported\n");
                    break;
              }

              n_planes = vmeta->n_planes; 
              space->vsp_info->out_nplane = n_planes;

              for (i = 0; i < n_planes; i++) {
                gint res;
                guint phys_addr;

                phys_addr = (guint) vspm_out->vspm[vspm_used].phard_addr + vmeta->offset[i];
                plane_size[i] = vmeta->stride[i] * vmeta->height;

                res =
                    mmngr_export_start_in_user (&vspm_out->vspm[vspm_used].dmabuf_pid,
                    plane_size[i], (unsigned long) phys_addr, &dmabuf_fd[i]);
                if (res != R_MM_OK) {
                  GST_ERROR_OBJECT (trans,
                      "mmngr_export_start_in_user failed (phys_addr:0x%08x)",
                      phys_addr);
                  return GST_FLOW_ERROR;
                }
              }
              new_buf = gst_vspm_filter_buffer_pool_request_videosink_buffer_creation (space,
                    dmabuf_fd, vmeta->stride, vmeta->width, vmeta->height);
              if(!new_buf)
                new_buf = gst_buffer_new();

              vspm_out->vspm[vspm_used].buf = new_buf;

              if (!space->first_buff) {
                gst_buffer_unref (buf);
                g_ptr_array_remove_index (vspm_outbuf->buf_array, 
                                            vspm_outbuf->current_buffer_index);
                g_ptr_array_add (vspm_outbuf->buf_array, new_buf);
              }

              space->first_buff = 0;
              *outbuf = new_buf;
            } else {
              *outbuf = buf;
              vspm_outbuf->current_buffer_index ++;
              if(vspm_outbuf->current_buffer_index >= vspm_outbuf->buf_array->len)
                    vspm_outbuf->current_buffer_index = 0;
            }

            if(gst_buffer_is_writable(*outbuf)) {
                if (!GST_BASE_TRANSFORM_CLASS(parent_class)->copy_metadata (trans,
                                                            inbuf, *outbuf)) {
                    /* something failed, post a warning */
                    GST_ELEMENT_WARNING (trans, STREAM, NOT_IMPLEMENTED,
                    ("could not copy metadata"), (NULL));
                }
                break;
            }
        }
    } else if(space->outbuf_allocate) {
      trans->priv->passthrough = 0; //disable pass-through mode

      while(TRUE) {
        buf = g_ptr_array_index (vspm_outbuf->buf_array, vspm_outbuf->current_buffer_index);
        *outbuf = buf;

        vspm_outbuf->current_buffer_index ++;
        if(vspm_outbuf->current_buffer_index >= vspm_outbuf->buf_array->len)
              vspm_outbuf->current_buffer_index = 0;

        if(gst_buffer_is_writable(*outbuf)) {
          if (!GST_BASE_TRANSFORM_CLASS(parent_class)->copy_metadata (trans,
                                                      inbuf, *outbuf)) {
            /* something failed, post a warning */
            GST_ELEMENT_WARNING (trans, STREAM, NOT_IMPLEMENTED,
            ("could not copy metadata"), (NULL));
          }
          break;
        }
      }
    } else {
      ret = GST_BASE_TRANSFORM_CLASS(parent_class)->prepare_output_buffer(trans,
                                                                        inbuf,outbuf);
    }

    return ret;
}
                                          
static void
gst_vspm_filter_class_init (GstVspmFilterClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstBaseTransformClass *gstbasetransform_class =
      (GstBaseTransformClass *) klass;
  GstVideoFilterClass *gstvideofilter_class = (GstVideoFilterClass *) klass;

  gobject_class->set_property = gst_vspm_filter_set_property;
  gobject_class->get_property = gst_vspm_filter_get_property;
  gobject_class->finalize = gst_vspm_filter_finalize;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_vspm_filter_src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_vspm_filter_sink_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "Colorspace and Video Size Converter with VSPM",
      "Filter/Converter/Video",
      "Converts colorspace and video size from one to another",
      "Renesas Corporation");
  g_object_class_install_property (gobject_class, PROP_VSPM_DMABUF,
      g_param_spec_boolean ("dmabuf-use", "Use DMABUF mode",
        "Whether or not to use dmabuf",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_VSPM_OUTBUF,
      g_param_spec_boolean ("outbuf-alloc", "Use outbuf-alloc mode",
        "Whether or not to self-allocate output buffer",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstbasetransform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_transform_caps);
  gstbasetransform_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_fixate_caps);
  gstbasetransform_class->filter_meta =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_filter_meta);
  gstbasetransform_class->transform_meta =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_transform_meta);

  gstbasetransform_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_propose_allocation);
  gstbasetransform_class->prepare_output_buffer = 
      GST_DEBUG_FUNCPTR (gst_vspm_filter_prepare_output_buffer);
  gstbasetransform_class->transform =
      GST_DEBUG_FUNCPTR (gst_vspm_filter_transform);
    gstbasetransform_class->query = 
        GST_DEBUG_FUNCPTR (gst_vspmfiler_query);
  gstbasetransform_class->passthrough_on_same_caps = TRUE;

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
    vspm_in->used--;
    if(vspm_in->vspm[i].dmabuf_pid >= 0)
      mmngr_import_end_in_user(vspm_in->vspm[i].dmabuf_pid);
  }
  
  while (vspm_out->used)
  {
    int i = vspm_out->used - 1;
    vspm_out->used--;
    if (vspm_out->vspm[i].dmabuf_pid >= 0)
      mmngr_export_end_in_user(vspm_out->vspm[i].dmabuf_pid );
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

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}


static void
gst_vspm_filter_init (GstVspmFilter * space)
{
  GstVspmFilterVspInfo *vsp_info;
  int i;
  Vspm_mmng_ar *vspm_in;
  Vspm_mmng_ar *vspm_out;
  VspmbufArray *vspm_outbuf;

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
    printf("MMNGR: open error. \n");
  }
  
  if (VSPM_lib_DriverInitialize(&vsp_info->vspm_handle) == R_VSPM_OK) {
    vsp_info->is_init_vspm = TRUE;
  } else {
    printf("VSPM: Error Initialized. \n");
  }

  vspm_in->used = 0;
  vspm_out->used = 0;
  vspm_outbuf->buf_array = g_ptr_array_new ();  
  vspm_outbuf->current_buffer_index = 0;
  space->allocator = gst_dmabuf_allocator_new ();
  space->use_dmabuf = FALSE;
  space->outbuf_allocate = FALSE;
  space->first_buff = 1;
}

void
gst_vspm_filter_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVspmFilter *space = GST_VIDEO_CONVERT_CAST (object);
  GstBaseTransform *trans;

  trans = GST_BASE_TRANSFORM (object);
  switch (property_id) {
    case PROP_VSPM_DMABUF:
      space->use_dmabuf = g_value_get_boolean (value);
      if(space->use_dmabuf) {
        gst_base_transform_set_qos_enabled (trans, FALSE);
        space->outbuf_allocate = FALSE;
      }
      break;
    case PROP_VSPM_OUTBUF:
      space->outbuf_allocate = g_value_get_boolean (value);
      if(space->use_dmabuf) {
        space->use_dmabuf = FALSE;
      }
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
    case PROP_VSPM_DMABUF:
      g_value_set_boolean (value, space->use_dmabuf);
      break;
    case PROP_VSPM_OUTBUF:
      g_value_set_boolean (value, space->outbuf_allocate);
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
  if (wResult != 0) {
    printf("VSPM: error end. (%ld)\n", wResult);
  }
  end_flag = 1;
}

static gint
get_uv_offset_aligned_to_page (gint page_size, gint stride, gint height)
{
  gint a, b, r;
  gint lcm;

  /*
   * The following implementation uses the Euclidean Algorithm to obtain
   * the least common multiple of stride and page size.
   */

  /* nStride is set to width, to achieve 4K aligned by adjusting
     the nSliceHeight. */
  /* (1) Calculate the GCD of stride and alignment */
  b = stride;
  a = page_size;
  while ((r = a % b) != 0) {
    a = b;
    b = r;
  }

  /* (2) Calculate the LCM of stride and alignment */
  lcm = stride * page_size / b;

  /* (3) Calculate the offset of UV plane */
  return (((stride * height) / lcm) + 1) * lcm;
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
  unsigned long	bpd = 1;
  
  int ret;
  struct MM_PARAM	p_adr[2];

  space = GST_VIDEO_CONVERT_CAST (filter);
  vsp_info = space->vsp_info;

  GST_CAT_DEBUG_OBJECT (GST_CAT_PERFORMANCE, filter,
      "doing colorspace conversion from %s -> to %s",
      GST_VIDEO_INFO_NAME (&filter->in_info),
      GST_VIDEO_INFO_NAME (&filter->out_info));


  memset(&ctrl_par, 0, sizeof(T_VSP_CTRL));

  if (vsp_info->format_flag == 0) {
    irc = set_colorspace (GST_VIDEO_FRAME_FORMAT (in_frame), &vsp_info->in_format, &vsp_info->in_swapbit);
    if (irc != 0) {
      printf("input format is non-support.\n");
      return GST_FLOW_ERROR;
    }

    irc = set_colorspace_output (GST_VIDEO_FRAME_FORMAT (out_frame), &vsp_info->out_format, &vsp_info->out_swapbit);
    if (irc != 0) {
      printf("output format is non-support.\n");
      return GST_FLOW_ERROR;
    }
    vsp_info->format_flag = 1;
  }

  in_width = GST_VIDEO_FRAME_COMP_WIDTH (in_frame, 0);
  in_height = GST_VIDEO_FRAME_COMP_HEIGHT (in_frame, 0);
  out_width = GST_VIDEO_FRAME_COMP_WIDTH (out_frame, 0);
  out_height = GST_VIDEO_FRAME_COMP_HEIGHT (out_frame, 0);

  if ((in_width == out_width) && (in_height == out_height)) {
    use_module = 0;
  } else {
    /* UDS scaling */
    use_module = VSP_UDS_USE;
  }

  /* change virtual address to physical address */
  memset(&p_adr, 0, sizeof(p_adr));
  p_adr[0].user_virt_addr = (unsigned long)in_frame->data[0];
  p_adr[1].user_virt_addr = (unsigned long)out_frame->data[0];
  ret = ioctl(vsp_info->mmngr_fd, MM_IOC_VTOP, &p_adr);
  if (ret) {
    printf("MMNGR VtoP Convert Error. \n");
    return GST_FLOW_ERROR;
  }

  {
    src_alpha_par.addr_a  = NULL;
    src_alpha_par.alphan  = VSP_ALPHA_NO;
    src_alpha_par.alpha1  = 0;
    src_alpha_par.alpha2  = 0;
    src_alpha_par.astride = 0;
    src_alpha_par.aswap   = VSP_SWAP_NO;
    src_alpha_par.asel    = VSP_ALPHA_NUM5;
    src_alpha_par.aext    = VSP_AEXT_EXPAN;
    src_alpha_par.anum0   = 0;
    src_alpha_par.anum1   = 0;
    src_alpha_par.afix    = 0xff;
    src_alpha_par.irop    = VSP_IROP_NOP;
    src_alpha_par.msken   = VSP_MSKEN_ALPHA;
    src_alpha_par.bsel    = 0;
    src_alpha_par.mgcolor = 0;
    src_alpha_par.mscolor0  = 0;
    src_alpha_par.mscolor1  = 0;

    src_par.addr      = (void *) p_adr[0].hard_addr;
    /* setting every format */
    switch (vsp_info->in_format) {
    /* YUV420 SemiPlanar */
    case VSP_IN_YUV420_SEMI_NV21:
    case VSP_IN_YUV420_SEMI_NV12:
    {
      gint uv_offset = 0;
      if ((in_width * in_height) & (4096 - 1))
        uv_offset = get_uv_offset_aligned_to_page (4096, in_width, in_height);
      else
        uv_offset = in_width * in_height;

      src_par.addr_c0		= ((guint8 *) p_adr[0].hard_addr + uv_offset);

      src_par.stride 		= in_width;
      src_par.stride_c	= in_width;
      src_par.csc       = VSP_CSC_ON;
      break;
    }
    /* YUV422 Interleaved */
    case VSP_IN_YUV422_INT0_YUY2:
    case VSP_IN_YUV422_INT0_UYVY:
      src_par.addr_c0		= NULL;
      src_par.stride 		= in_width * 2;
      src_par.stride_c	= 0;
      src_par.csc       = VSP_CSC_ON;
      break;
      
    default:
      printf("input format is non-support.\n");
      return GST_FLOW_ERROR;
    }
    src_par.addr_c1     = NULL;
    src_par.width       = in_width;
    src_par.height      = in_height;
    src_par.width_ex    = 0;
    src_par.height_ex   = 0;
    src_par.x_offset    = 0;
    src_par.y_offset    = 0;
    src_par.format      = vsp_info->in_format;
    src_par.swap        = vsp_info->in_swapbit;
    src_par.x_position  = 0;
    src_par.y_position  = 0;
    src_par.pwd         = VSP_LAYER_PARENT;
    src_par.cipm        = VSP_CIPM_0_HOLD;
    src_par.cext        = VSP_CEXT_EXPAN;
    src_par.iturbt      = VSP_ITURBT_709;
    src_par.clrcng      = VSP_ITU_COLOR;
    src_par.vir         = VSP_NO_VIR;
    src_par.vircolor    = 0x00000000;
    src_par.osd_lut     = NULL;
    src_par.alpha_blend = &src_alpha_par;
    src_par.clrcnv      = NULL;
    src_par.connect     = use_module;
  }
  
  {
    dst_par.addr      = (void *)p_adr[1].hard_addr;
    switch (vsp_info->out_format) {
    /* RGB */
    case VSP_IN_ARGB8888:
      dst_par.addr_c0   = NULL;
      bpd = (unsigned char)( (vsp_info->out_format & VSP_FORMAT_PIXEL_MASK) >> VSP_FORMAT_PIXEL_BIT );
      dst_par.stride 		= out_width * bpd;
      dst_par.stride_c	= 0;
      dst_par.csc       = VSP_CSC_OFF;
      break;
    case VSP_IN_RGBA8888:
    case VSP_IN_RGB888:
      dst_par.addr_c0   = NULL;
      bpd = (unsigned char)( (vsp_info->out_format & VSP_FORMAT_PIXEL_MASK) >> VSP_FORMAT_PIXEL_BIT );
      dst_par.stride 		= out_width * bpd;
      dst_par.stride_c	= 0;
      dst_par.csc       = VSP_CSC_OFF;
      break;
    case VSP_OUT_YUV420_SEMI_NV12:
    case VSP_OUT_YUV420_SEMI_NV21:
      dst_par.addr_c0   = ((guint8 *) p_adr[1].hard_addr + out_width * out_height);
      dst_par.stride    = out_width;
      dst_par.stride_c	= out_width;
      dst_par.csc       = VSP_CSC_ON;
      break;

    default:
      printf("output format is non-support.\n");
      return GST_FLOW_ERROR;
    }
    dst_par.addr_c1   = NULL;
    dst_par.width     = out_width;
    dst_par.height    = out_height;
    dst_par.x_offset  = 0;
    dst_par.y_offset  = 0;
    dst_par.format    = vsp_info->out_format;
    dst_par.pxa       = VSP_PAD_P;
    dst_par.pad       = 0xff;
    dst_par.x_coffset = 0;
    dst_par.y_coffset = 0;
    dst_par.iturbt    = VSP_ITURBT_709;
    dst_par.clrcng    = VSP_ITU_COLOR;
    dst_par.cbrm      = VSP_CSC_ROUND_DOWN;
    dst_par.abrm      = VSP_CONVERSION_ROUNDDOWN;
    dst_par.athres    = 0;
    dst_par.clmd      = VSP_CLMD_NO;
    dst_par.dith      = VSP_NO_DITHER;
    dst_par.swap      = vsp_info->out_swapbit;
  }

  if (use_module == VSP_UDS_USE) {
    /* Set T_VSP_UDS. */
    ctrl_par.uds     = &uds_par;
    memset(&uds_par, 0, sizeof(T_VSP_UDS));
    uds_par.fmd      = VSP_FMD;
    uds_par.filcolor = 0x0000FF00; /* green */

    uds_par.x_ratio     = (unsigned short)( (in_width << 12) / out_width );
    uds_par.y_ratio     = (unsigned short)( (in_height << 12) / out_height );
    uds_par.out_cwidth  = (unsigned short)out_width;
    uds_par.out_cheight = (unsigned short)out_height;
    uds_par.connect     = 0;
  }


  vsp_par.rpf_num   = 1;
  vsp_par.use_module  = use_module;
  vsp_par.src1_par  = &src_par;
  vsp_par.src2_par  = NULL;
  vsp_par.src3_par  = NULL;
  vsp_par.src4_par  = NULL;
  vsp_par.dst_par   = &dst_par;
  vsp_par.ctrl_par  = &ctrl_par;

  memset(&vspm_ip, 0, sizeof(VSPM_IP_PAR));
  vspm_ip.uhType    = VSPM_TYPE_VSP_AUTO;
  vspm_ip.unionIpParam.ptVsp = &vsp_par;

  end_flag = 0;
  ercd = VSPM_lib_Entry(vsp_info->vspm_handle, &vsp_info->jobid, 126, &vspm_ip, 0, cb_func); 
  if (ercd) {
    printf("VSPM_lib_Entry() Failed!! ercd=%ld\n", ercd);
    return GST_FLOW_ERROR;
  }

  while(1) {
    if (end_flag) break;
  }

  if(space->outbuf_allocate)
    gst_buffer_ref(out_frame->buffer);

  return GST_FLOW_OK;
}


static GstFlowReturn
gst_vspm_filter_transform_buffer (GstVideoFilter * filter,
                                    GstBuffer * inbuf,
                                    GstBuffer * outbuf)
{
  GstVspmFilter *space;
  GstVspmFilterVspInfo *vsp_info;
  GstVideoMeta *in_vmeta;
  GstVideoMeta *out_vmeta;
  VSPM_IP_PAR vspm_ip;
  VSPM_VSP_PAR vsp_par;
  Vspm_mmng_ar *vspm_in;
  Vspm_mmng_ar *vspm_out;

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
  unsigned long	bpd = 1;

  int ret;
  struct MM_PARAM	p_adr[2];

  space = GST_VIDEO_CONVERT_CAST (filter);
  vsp_info = space->vsp_info;
  vspm_in = space->vspm_in;
  vspm_out = space->vspm_out;
  in_vmeta = gst_buffer_get_video_meta (inbuf); 
  if(in_vmeta) {
    vsp_info->gst_format_in = in_vmeta->format;
    vsp_info->in_width = in_vmeta->width;
    vsp_info->in_height = in_vmeta->height;
  }

  out_vmeta = gst_buffer_get_video_meta (outbuf);
  if(out_vmeta) {
    vsp_info->gst_format_out = out_vmeta->format;
    vsp_info->out_width = out_vmeta->width;
    vsp_info->out_height = out_vmeta->height;
  }

  GST_CAT_DEBUG_OBJECT (GST_CAT_PERFORMANCE, filter,
      "doing colorspace conversion from %s -> to %s",
      GST_VIDEO_INFO_NAME (&filter->in_info),
      GST_VIDEO_INFO_NAME (&filter->out_info));

  memset(&ctrl_par, 0, sizeof(T_VSP_CTRL));

  if (vsp_info->format_flag == 0) {
    irc = set_colorspace (vsp_info->gst_format_in, &vsp_info->in_format, &vsp_info->in_swapbit);
    if (irc != 0) {
      printf("input format is non-support.\n");
      return GST_FLOW_ERROR;
    }
    irc = set_colorspace_output (vsp_info->gst_format_out, &vsp_info->out_format, &vsp_info->out_swapbit);
    if (irc != 0) {
      printf("output format is non-support.\n");
      return GST_FLOW_ERROR;
    }
    vsp_info->format_flag = 1;
  }

  in_width = vsp_info->in_width;
  in_height = vsp_info->in_height;

  out_width = vsp_info->out_width;
  out_height = vsp_info->out_height;

  if ((in_width == out_width) && (in_height == out_height)) {
    use_module = 0;
  } else {
    /* UDS scaling */
    use_module = VSP_UDS_USE;
  }

  memset(&p_adr, 0, sizeof(p_adr));

  if(buffer_get_dmabuf_hard_addr(vspm_in,inbuf))  
    p_adr[0].hard_addr = buffer_get_dmabuf_hard_addr(vspm_in,inbuf);

  if(buffer_get_dmabuf_hard_addr(vspm_out,outbuf))  
    p_adr[1].hard_addr = buffer_get_dmabuf_hard_addr(vspm_out,outbuf);

  {
    src_alpha_par.addr_a    = NULL;
    src_alpha_par.alphan    = VSP_ALPHA_NO;
    src_alpha_par.alpha1    = 0;
    src_alpha_par.alpha2    = 0;
    src_alpha_par.astride   = 0;
    src_alpha_par.aswap     = VSP_SWAP_NO;
    src_alpha_par.asel      = VSP_ALPHA_NUM5;
    src_alpha_par.aext      = VSP_AEXT_EXPAN;
    src_alpha_par.anum0     = 0;
    src_alpha_par.anum1     = 0;
    src_alpha_par.afix      = 0xff;
    src_alpha_par.irop      = VSP_IROP_NOP;
    src_alpha_par.msken     = VSP_MSKEN_ALPHA;
    src_alpha_par.bsel      = 0;
    src_alpha_par.mgcolor   = 0;
    src_alpha_par.mscolor0  = 0;
    src_alpha_par.mscolor1  = 0;

    src_par.addr      = (void *) p_adr[0].hard_addr;
    /* setting every format */
   switch (vsp_info->in_format) {   
    /* YUV420 SemiPlanar */
    case VSP_IN_YUV420_SEMI_NV21:
    case VSP_IN_YUV420_SEMI_NV12:
    {
      gint uv_offset = 0;
      if ((in_width * in_height) & (4096 - 1))
        uv_offset = get_uv_offset_aligned_to_page (4096, in_width, in_height);
      else
        uv_offset = in_width * in_height;

      src_par.addr_c0		= ((guint8 *) p_adr[0].hard_addr + uv_offset);

      src_par.stride 		= in_width;
      src_par.stride_c	= in_width;
      src_par.csc       = VSP_CSC_ON;
      break;
    }
    /* YUV422 Interleaved */
    case VSP_IN_YUV422_INT0_YUY2:
    case VSP_IN_YUV422_INT0_UYVY:
      src_par.addr_c0		= NULL;
      src_par.stride 		= in_width * 2;
      src_par.stride_c	= 0;
      src_par.csc       = VSP_CSC_ON;
      break;
      
    default:
      printf("input format is non-support.\n");
      return GST_FLOW_ERROR;
    }
    src_par.addr_c1     = NULL;
    src_par.width       = in_width;
    src_par.height      = in_height;
    src_par.width_ex    = 0;
    src_par.height_ex   = 0;
    src_par.x_offset    = 0;
    src_par.y_offset    = 0;
    src_par.format      = vsp_info->in_format;
    src_par.swap        = vsp_info->in_swapbit;
    src_par.x_position  = 0;
    src_par.y_position  = 0;
    src_par.pwd         = VSP_LAYER_PARENT;
    src_par.cipm        = VSP_CIPM_0_HOLD;
    src_par.cext        = VSP_CEXT_EXPAN;
    src_par.iturbt      = VSP_ITURBT_709;
    src_par.clrcng      = VSP_ITU_COLOR;
    src_par.vir         = VSP_NO_VIR;
    src_par.vircolor    = 0x00000000;
    src_par.osd_lut     = NULL;
    src_par.alpha_blend = &src_alpha_par;
    src_par.clrcnv      = NULL;
    src_par.connect     = use_module;
  }

  {
    dst_par.addr      = (void *)p_adr[1].hard_addr;
    switch (vsp_info->out_format) {
    /* RGB */
    case VSP_IN_ARGB8888:
      dst_par.addr_c0   = NULL;
      bpd = (unsigned char)( (vsp_info->out_format & VSP_FORMAT_PIXEL_MASK) >> VSP_FORMAT_PIXEL_BIT );
      dst_par.stride 		= out_width * bpd;
      dst_par.stride_c	= 0;
      dst_par.csc       = VSP_CSC_OFF;
      break;
    case VSP_OUT_YUV420_SEMI_NV12:
    case VSP_OUT_YUV420_SEMI_NV21:
      dst_par.addr_c0   = ((guint8 *) p_adr[1].hard_addr + out_width * out_height);;
      dst_par.stride    = out_width;
      dst_par.stride_c	= out_width;
      dst_par.csc       = VSP_CSC_ON;
      break;
    default:
      printf("output format is non-support.\n");
      return GST_FLOW_ERROR;
    }

    dst_par.addr_c1   = NULL;
    dst_par.width     = out_width;
    dst_par.height    = out_height;
    dst_par.x_offset  = 0;
    dst_par.y_offset  = 0;
    dst_par.format    = vsp_info->out_format;
    dst_par.swap      = vsp_info->out_swapbit;
    dst_par.pxa       = VSP_PAD_P;
    dst_par.pad       = 0xff;
    dst_par.x_coffset = 0;
    dst_par.y_coffset = 0;
    dst_par.iturbt    = VSP_ITURBT_709;
    dst_par.clrcng    = VSP_ITU_COLOR;
    dst_par.cbrm      = VSP_CSC_ROUND_DOWN;
    dst_par.abrm      = VSP_CONVERSION_ROUNDDOWN;
    dst_par.athres    = 0;
    dst_par.clmd      = VSP_CLMD_NO;
    dst_par.dith      = VSP_NO_DITHER;
  }

  if (use_module == VSP_UDS_USE) {
    /* Set T_VSP_UDS. */
    ctrl_par.uds     = &uds_par;
    memset(&uds_par, 0, sizeof(T_VSP_UDS));
    uds_par.fmd      = VSP_FMD;
    uds_par.filcolor = 0x0000FF00; /* green */

    uds_par.x_ratio     = (unsigned short)( (in_width << 12) / out_width );
    uds_par.y_ratio     = (unsigned short)( (in_height << 12) / out_height );
    uds_par.out_cwidth  = (unsigned short)out_width;
    uds_par.out_cheight = (unsigned short)out_height;
    uds_par.connect     = 0;
  }

  vsp_par.rpf_num    = 1;
  vsp_par.use_module = use_module;
  vsp_par.src1_par   = &src_par;
  vsp_par.src2_par   = NULL;
  vsp_par.src3_par   = NULL;
  vsp_par.src4_par   = NULL;
  vsp_par.dst_par    = &dst_par;
  vsp_par.ctrl_par   = &ctrl_par;

  memset(&vspm_ip, 0, sizeof(VSPM_IP_PAR));
  vspm_ip.uhType    = VSPM_TYPE_VSP_AUTO;
  vspm_ip.unionIpParam.ptVsp = &vsp_par;

  end_flag = 0;
  ercd = VSPM_lib_Entry(vsp_info->vspm_handle, &vsp_info->jobid, 126, &vspm_ip, 0, cb_func); 
  if (ercd) {
    printf("VSPM_lib_Entry() Failed!! ercd=%ld\n", ercd);
    return GST_FLOW_ERROR;
  }

  while(1) {
    if (end_flag) break;
  }

  gst_buffer_ref(outbuf);
  gst_buffer_unref(inbuf);

  return GST_FLOW_OK;
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
