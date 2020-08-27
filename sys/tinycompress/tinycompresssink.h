/*
 * GStreamer
 * Copyright (C) 2020 Linux Foundation. All rights reserved.
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

#ifndef __GST_TINYCOMPRESSSINK_H__
#define __GST_TINYCOMPRESSSINK_H__

#include <gst/base/gstbasesink.h>
#include <tinycompress/tinycompress.h>

G_BEGIN_DECLS
#define GST_TYPE_TINYCOMPRESSSINK \
    (gst_tinycompresssink_get_type())
#define GST_TINYCOMPRESSSINK(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_TINYCOMPRESSSINK,GstTinyCompressSink))
#define GST_TINYCOMPRESSSINK_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_TINYCOMPRESSSINK,GstTinyCompressSinkClass))
#define GST_TINYCOMPRESSSINK_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_TINYCOMPRESSSINK,GstTinyCompressSinkClass))
#define GST_IS_TINYCOMPRESSSINK(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_TINYCOMPRESSSINK))
#define GST_IS_TINYCOMPRESSSINK_CLASS(obj) \
    (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_TINYCOMPRESSSINK))
#define GST_TINYCOMPRESSSINK_CAST(obj) \
    ((GstTinyCompressSink *)(obj))


#define _TINYCOMPRESS_CAPS_PCM "audio/x-raw, " \
      "format = (string) { S16LE, S32LE }, " \
      "layout = (string) interleaved, " \
      "rate = (int) [ 8000, 48000 ], " \
      "channels = (int) [ 1, 2 ]; "
#define _TINYCOMPRESS_CAPS_MP3 "audio/mpeg, mpegversion = (int) 1, " \
      "mpegaudioversion = (int) [ 1, 3 ]; "
#define _TINYCOMPRESS_CAPS_AAC "audio/mpeg, mpegversion = (int) { 2, 4 }, " \
      "stream-format = (string) { adts, raw };"

typedef struct _GstTinyCompressSink GstTinyCompressSink;
typedef struct _GstTinyCompressSinkClass GstTinyCompressSinkClass;

struct _GstTinyCompressSink
{
  GstBaseSink base_sink;
  struct compress *compress;
  gchar *device;
  gboolean provide_clock;
  gboolean timestamp;
  gboolean enable_lpa;
  gboolean pauseed;

  /* buffer attributes */
  guint32 tlength;
  guint32 minreq;
  guint32 maxlength;
  guint32 prebuf;

  guint codec_id;
  guint format;
  guint channels;
  guint rate;

  GstCaps *caps;
  GstClock *clock;

  volatile gint unlocked;
};

struct _GstTinyCompressSinkClass
{
  GstBaseSinkClass parent_class;
};

GType gst_tinycompresssink_get_type (void);

#define TINY_COMPRESS_SINK_TEMPLATE_CAPS \
  _TINYCOMPRESS_CAPS_PCM \
  _TINYCOMPRESS_CAPS_MP3

G_END_DECLS
#endif /* __GST_TINYCOMPRESSSINK_H__ */
