/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * This file:
 * Copyright (C) 2003 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2014 Hitachi Solutions Corporation
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

#ifndef __GST_VSPMFILTER_H__
#define __GST_VSPMFILTER_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include <gst/allocators/gstdmabuf.h>

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <asm/types.h>          /* for videodev2.h */

#include <linux/media.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <linux/v4l2-mediabus.h>

G_BEGIN_DECLS

#define GST_TYPE_VIDEO_CONVERT	          (gst_vspm_filter_get_type())
#define GST_VIDEO_CONVERT(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_VIDEO_CONVERT,GstVspmFilter))
#define GST_VIDEO_CONVERT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_VIDEO_CONVERT,GstVspmFilterClass))
#define GST_IS_VIDEO_CONVERT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_VIDEO_CONVERT))
#define GST_IS_VIDEO_CONVERT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_VIDEO_CONVERT))
#define GST_VIDEO_CONVERT_CAST(obj)       ((GstVspmFilter *)(obj))
#define GST_TYPE_VSPMFILTER_BUFFER_POOL     (gst_vspmfilter_buffer_pool_get_type())
#define GST_VSPMFILTER_BUFFER_POOL_CAST(obj) ((GstVspmFilterBufferPool*)(obj))

#define N_BUFFERS 1

#define MAX_DEVICES 2
#define MAX_ENTITIES 4

/* mmngr dev name */
#define DEVFILE "/dev/rgnmm"

/* mmngr private structure */
struct MM_PARAM {
	unsigned long	size;
	unsigned long long	phy_addr;
	unsigned long	hard_addr;
	unsigned long	user_virt_addr;
	unsigned long	kernel_virt_addr;
	unsigned long	flag;
};

/* mmngr private define */
#define MM_IOC_MAGIC 'm'
#define MM_IOC_VTOP	_IOWR(MM_IOC_MAGIC, 7, struct MM_PARAM) 

typedef struct _GstVspmFilter GstVspmFilter;
typedef struct _GstVspmFilterClass GstVspmFilterClass;

typedef struct _GstVspmFilterVspInfo GstVspmFilterVspInfo;
typedef struct _GstVspmFilterBufferPool GstVspmFilterBufferPool;
typedef struct _GstVspmFilterBufferPoolClass GstVspmFilterBufferPoolClass;

struct _GstVspmFilterBufferPool
{
  GstBufferPool bufferpool;

  GstVspmFilter *vspmfilter;

  GstCaps *caps;
};

struct _GstVspmFilterBufferPoolClass
{
  GstBufferPoolClass parent_class;
};

struct buffer {
  void *start;
  size_t length;
};

enum {
  OUT = 0,
  CAP = 1,
  RESZ = 2,
};

typedef enum {
  IO_METHOD_READ,
  IO_METHOD_MMAP,
  IO_METHOD_USERPTR,
} io_method;

struct _GstVspmFilterVspInfo {

  unsigned long vspm_handle;
  unsigned long jobid;
  gboolean is_init_vspm;
  unsigned char format_flag;
  GstVideoFormat gst_format_in;
  guint  in_format;  
  guint  in_width;
  guint  in_height;
  guint  in_nplane;
  guint  in_swapbit;
  GstVideoFormat gst_format_out;
  guint  out_format;
  guint  out_width;
  guint  out_height;
  guint  out_nplane;
  guint  out_swapbit;
  int mmngr_fd;   /* mmngr open id */
  
};

typedef struct {
  int mmng_pid;
  int dmabuf_pid;
  unsigned long pphy_addr;
  unsigned long phard_addr;
  unsigned long puser_virt_addr;
  gint dmabuf_fd;
  GstBuffer *buf;
} Vspm_dmabuff;

typedef struct {
  Vspm_dmabuff vspm[5];
  int used;
} Vspm_mmng_ar;

typedef struct {
  GPtrArray *buf_array;
  gint current_buffer_index;
}VspmbufArray ;

/**
 * GstVspmFilter:
 *
 * Opaque object data structure.
 */
struct _GstVspmFilter {
  GstVideoFilter element;

  GstVspmFilterVspInfo *vsp_info;
  GstAllocator *allocator;
  guint use_dmabuf;
  guint outbuf_allocate;
  GstBufferPool *in_port_pool, *out_port_pool;
  Vspm_mmng_ar *vspm_in;
  Vspm_mmng_ar *vspm_out;
  VspmbufArray *vspm_outbuf;
  gint first_buff;
};

struct _GstVspmFilterClass
{
  GstVideoFilterClass parent_class;
};

GType gst_vspmfilter_buffer_pool_get_type (void);

G_END_DECLS

#endif /* __GST_VSPMFILTER_H__ */
