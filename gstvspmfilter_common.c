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

/* Common helpers shared by the core (gstvspmfilter.c) and the platform
 * files (gstvspmfilter_isu.c / gstvspmfilter_vsp.c): MMNGR import/release
 * of dmabuf buffers, physical address lookup and the crop border accessor. */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gstvspmfilter.h"

#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "mmngr_user_public.h"
#include "mmngr_buf_user_public.h"

GST_DEBUG_CATEGORY_EXTERN (vspmfilter_debug);
#define GST_CAT_DEFAULT vspmfilter_debug

/* Read the crop borders under the object lock. TRUE if they amount to a crop. */
gboolean
gst_vspm_filter_get_crop_value (GstVspmFilter * space, guint * left, guint * right,
    guint * top, guint * bottom)
{
  GST_OBJECT_LOCK (space);
  *left   = space->crop.left;
  *right  = space->crop.right;
  *top    = space->crop.top;
  *bottom = space->crop.bottom;
  GST_OBJECT_UNLOCK (space);

  return (*left || *right || *top || *bottom);
}

GstFlowReturn
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

void
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

void
gst_vspm_filter_release_fd (GQueue *import_list)
{
  int fd;
  while (!g_queue_is_empty(import_list)) {
    fd = GPOINTER_TO_INT(g_queue_pop_tail (import_list));
    if (fd >= 0 )
      mmngr_import_end_in_user_ext (fd);
  }
}

GstFlowReturn
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
