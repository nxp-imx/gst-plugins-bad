/* GStreamer
 *
 *  Copyright 2024 NXP
 *   @author: Elliot Chen <elliot.chen@nxp.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_AUTO_VIDEO_CONVERT2_H__
#define __GST_AUTO_VIDEO_CONVERT2_H__

#include <gst/gst.h>
#include <gst/gstbin.h>
#include "gstautoselect.h"

G_BEGIN_DECLS
#define GST_TYPE_AUTO_VIDEO_CONVERT2 	        	(gst_auto_video_convert2_get_type())
#define GST_AUTO_VIDEO_CONVERT2(obj)	            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AUTO_VIDEO_CONVERT2,GstAutoVideoConvert2))
#define GST_AUTO_VIDEO_CONVERT2_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AUTO_VIDEO_CONVERT2,GstAutoVideoConvert2Class))
#define GST_IS_AUTO_VIDEO_CONVERT2(obj)            (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AUTO_VIDEO_CONVERT2))
#define GST_IS_AUTO_VIDEO_CONVERT2_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AUTO_VIDEO_CONVERT2))
typedef struct _GstAutoVideoConvert2 GstAutoVideoConvert2;
typedef struct _GstAutoVideoConvert2Class GstAutoVideoConvert2Class;

struct _GstAutoVideoConvert2
{
  GstAutoSelect element;
  gchar *perferred_factory_order;
};

struct _GstAutoVideoConvert2Class
{
  GstAutoSelectClass parent_class;
};

GType gst_auto_video_convert2_get_type (void);
GST_ELEMENT_REGISTER_DECLARE (autovideoconvert2);

G_END_DECLS
#endif /* __GST_AUTO_VIDEO_CONVERT2_H__ */
