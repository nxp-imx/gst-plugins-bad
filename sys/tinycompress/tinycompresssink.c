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

/**
 * SECTION:element-tinycompresssink
 *
 * This element outputs audio to tinycompress.
 *
 * <refsect2>
 * <title>Example pipelines</title>
 * |[
 * gst-launch-1.0 -v filesrc location=example.mp3 ! mpegaudioparse ! tinycompresssink
 * ]| Play an mp3 file.
 * |[
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <gst/audio/audio.h>
#include "sound/compress_params.h"
#include "tinycompresssink.h"

GST_DEBUG_CATEGORY_STATIC (tinycompresssink_debug);
#define GST_CAT_DEFAULT tinycompresssink_debug

#define DEFAULT_DEVICE          "hw:0,0"
#define DEFAULT_TIMESTAMP       FALSE
#define DEFAULT_ENABLE_LPA      FALSE
#define DEFAULT_TLENGTH         -1
#define DEFAULT_MINREQ          -1
#define DEFAULT_MAXLENGTH       -1
#define DEFAULT_PREBUF          -1
#define DEFAULT_PROVIDE_CLOCK   FALSE

enum
{
  PROP_0,
  PROP_DEVICE,
  PROP_TIMESTAMP,
  PROP_ENABLE_LPA,
  PROP_TLENGTH,
  PROP_MINREQ,
  PROP_MAXLENGTH,
  PROP_PREBUF,
  PROP_PROVIDE_CLOCK,
  PROP_LAST
};

/* the capabilities of the sink pad */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (TINY_COMPRESS_SINK_TEMPLATE_CAPS));

static gboolean gst_tinycompresssink_start (GstBaseSink * basesink);
static gboolean gst_tinycompresssink_stop (GstBaseSink * basesink);
static gboolean gst_tinycompresssink_set_caps (GstBaseSink * sink,
    GstCaps * caps);
static void gst_tinycompresssink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_tinycompresssink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_tinycompresssink_finalize (GObject * obj);
static GstStateChangeReturn gst_tinycompresssink_change_state (GstElement *
    element, GstStateChange transition);
static GstClock *gst_tinycompresssink_provide_clock (GstElement * element);
static GstFlowReturn gst_tinycompresssink_render (GstBaseSink * bsink,
    GstBuffer * buffer);
static GstFlowReturn gst_tinycompresssink_unlock (GstBaseSink * bsink);
static GstFlowReturn gst_tinycompresssink_unlock_stop (GstBaseSink * bsink);
static gboolean gst_tinycompresssink_query (GstBaseSink * bsink,
    GstQuery * query);
static gboolean gst_tinycompresssink_event (GstBaseSink * bsink,
    GstEvent * event);
static GstFlowReturn gst_tinycompresssink_wait_event (GstBaseSink * bsink,
    GstEvent * event);
static void gst_tinycompresssink_get_times (GstBaseSink * bsink,
    GstBuffer * buffer, GstClockTime * start, GstClockTime * end);
static GstClockTime gst_tinycompresssink_get_time (GstClock * clock,
    void *userdata);

#define parent_class gst_tinycompresssink_parent_class
G_DEFINE_TYPE (GstTinyCompressSink, gst_tinycompresssink, GST_TYPE_BASE_SINK);

/* initialize the tinycompresssink's class */
static void
gst_tinycompresssink_class_init (GstTinyCompressSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_tinycompresssink_set_property;
  gobject_class->get_property = gst_tinycompresssink_get_property;
  gobject_class->finalize = gst_tinycompresssink_finalize;

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_tinycompresssink_change_state);
  gstelement_class->provide_clock =
      GST_DEBUG_FUNCPTR (gst_tinycompresssink_provide_clock);

  gstbasesink_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_tinycompresssink_set_caps);
  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_tinycompresssink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_tinycompresssink_stop);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_tinycompresssink_render);
  gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_tinycompresssink_unlock);
  gstbasesink_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_tinycompresssink_unlock_stop);
  gstbasesink_class->query = GST_DEBUG_FUNCPTR (gst_tinycompresssink_query);
  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_tinycompresssink_event);
  gstbasesink_class->wait_event =
      GST_DEBUG_FUNCPTR (gst_tinycompresssink_wait_event);
  gstbasesink_class->get_times =
      GST_DEBUG_FUNCPTR (gst_tinycompresssink_get_times);

  /* Properties of tinycompresssink */
  g_object_class_install_property (gobject_class, PROP_DEVICE,
      g_param_spec_string ("device", "Device",
          "The Tinycompress sink device to connect to", DEFAULT_DEVICE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_TIMESTAMP,
      g_param_spec_boolean ("timestamp", "Timestamp",
          "Provide buffers with timestamp", DEFAULT_TIMESTAMP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ENABLE_LPA,
      g_param_spec_boolean ("enable-lpa", "Enable lpa",
          "Enable LPA", DEFAULT_ENABLE_LPA,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_TLENGTH,
      g_param_spec_uint ("tlength", "Target length",
          "The target buffer level (total latency) to request (in bytes)",
          0, G_MAXUINT32, DEFAULT_TLENGTH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MINREQ,
      g_param_spec_uint ("minreq", "Minmum request size",
          "The minmum amount of data that server will request (in bytes)",
          0, G_MAXUINT32, DEFAULT_MINREQ,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAXLENGTH,
      g_param_spec_uint ("maxlength", "Maximum buffer length",
          "Maximum stream buffer size that the server should hold (in bytes)",
          0, G_MAXUINT32, DEFAULT_MAXLENGTH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PREBUF,
      g_param_spec_uint ("prebuf", "Prebuffering length",
          "Minimum amount of data required for playback to start (in bytes)",
          0, G_MAXUINT32, DEFAULT_PREBUF,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PROVIDE_CLOCK,
      g_param_spec_boolean ("provide-clock", "Provide clock",
          "Provide a clock that can be used as the pipeline clock",
          DEFAULT_PROVIDE_CLOCK, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (gstelement_class,
      "Tinycompress Audio Direct Sink",
      "Sink/Audio", "Plays audio to tinycompress",
      "Bing Song <bing.song@nxp.com>");

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);

  GST_DEBUG_CATEGORY_INIT (tinycompresssink_debug, "tinycompresssink", 0,
      "tinycompress Sink");
}

static void
gst_tinycompresssink_set_provide_clock (GstTinyCompressSink * csink,
    gboolean provide_clock)
{
  GST_OBJECT_LOCK (csink);

  csink->provide_clock = provide_clock;

  if (csink->provide_clock)
    GST_OBJECT_FLAG_SET (csink, GST_ELEMENT_FLAG_PROVIDE_CLOCK);
  else
    GST_OBJECT_FLAG_UNSET (csink, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  GST_OBJECT_UNLOCK (csink);
}

static void
gst_tinycompresssink_init (GstTinyCompressSink * csink)
{
  csink->device = g_strdup (DEFAULT_DEVICE);

  csink->provide_clock = DEFAULT_PROVIDE_CLOCK;
  csink->timestamp = DEFAULT_TIMESTAMP;
  csink->enable_lpa = DEFAULT_ENABLE_LPA;
  csink->tlength = DEFAULT_TLENGTH;
  csink->minreq = DEFAULT_MINREQ;
  csink->maxlength = DEFAULT_MAXLENGTH;
  csink->prebuf = DEFAULT_PREBUF;

  csink->clock = gst_audio_clock_new ("TinyCompressSinkClock",
      gst_tinycompresssink_get_time, csink, NULL);

  gst_tinycompresssink_set_provide_clock (csink, DEFAULT_PROVIDE_CLOCK);

  g_atomic_int_set (&csink->unlocked, 0);
}

static void
gst_tinycompresssink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK_CAST (object);

  switch (prop_id) {
    case PROP_DEVICE:
      g_free (csink->device);
      csink->device = g_value_dup_string (value);
      break;

    case PROP_TIMESTAMP:
      csink->timestamp = g_value_get_boolean (value);
      break;

    case PROP_ENABLE_LPA:
      csink->enable_lpa = g_value_get_boolean (value);
      break;

    case PROP_TLENGTH:
      csink->tlength = g_value_get_uint (value);
      break;

    case PROP_MINREQ:
      csink->minreq = g_value_get_uint (value);
      break;

    case PROP_MAXLENGTH:
      csink->maxlength = g_value_get_uint (value);
      break;

    case PROP_PREBUF:
      csink->prebuf = g_value_get_uint (value);
      break;

    case PROP_PROVIDE_CLOCK:
      gst_tinycompresssink_set_provide_clock (csink,
          g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_tinycompresssink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK_CAST (object);

  switch (prop_id) {
    case PROP_DEVICE:
      g_value_set_string (value, csink->device);
      break;

    case PROP_TIMESTAMP:
      g_value_set_boolean (value, csink->timestamp);
      break;

    case PROP_ENABLE_LPA:
      g_value_set_boolean (value, csink->enable_lpa);
      break;

    case PROP_TLENGTH:
      g_value_set_uint (value, csink->tlength);
      break;

    case PROP_MINREQ:
      g_value_set_uint (value, csink->minreq);
      break;

    case PROP_MAXLENGTH:
      g_value_set_uint (value, csink->maxlength);
      break;

    case PROP_PREBUF:
      g_value_set_uint (value, csink->prebuf);
      break;

    case PROP_PROVIDE_CLOCK:
      g_value_set_boolean (value, csink->provide_clock);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_tinycompresssink_finalize (GObject * obj)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK_CAST (obj);

  g_free (csink->device);

  gst_object_unref (csink->clock);

  /* TODO: free any properties or proplist */
  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static GstStateChangeReturn
gst_tinycompresssink_change_state (GstElement * element,
    GstStateChange transition)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;

    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_element_post_message (element,
          gst_message_new_clock_provide (GST_OBJECT_CAST (element),
              csink->clock, TRUE));
      break;

    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      compress_resume(csink->compress);
      break;

    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto state_failure;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      compress_pause(csink->compress);
      break;

    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_caps_unref (csink->caps);
      csink->caps = NULL;

      gst_element_post_message (element,
          gst_message_new_clock_lost (GST_OBJECT_CAST (element), csink->clock));
      break;

    case GST_STATE_CHANGE_READY_TO_NULL:
      break;

    default:
      break;
  }

  return ret;

  /* ERRORS */
state_failure:
  {
    return ret;
  }
}

static GstClock *
gst_tinycompresssink_provide_clock (GstElement * element)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (element);
  GstClock *ret = NULL;

  GST_OBJECT_LOCK (csink);

  if (csink->compress && csink->provide_clock)
    ret = gst_object_ref (csink->clock);
  else
    GST_DEBUG_OBJECT (csink,
        "No stream or clock disabled, cannot provide clock");

  GST_OBJECT_UNLOCK (csink);

  return ret;
}

static GstFlowReturn
gst_tinycompresssink_render (GstBaseSink * bsink, GstBuffer * buf)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK_CAST (bsink);
  GstMapInfo info;
  GstFlowReturn ret;
  unsigned int available;
  struct timespec tstamp;
  int wrote;

  gst_buffer_map (buf, &info, GST_MAP_READ);

  GST_LOG_OBJECT (csink, "Writing %" G_GSIZE_FORMAT " bytes", info.size);

  for (;;) {
    if (bsink->flushing) {
      GST_LOG_OBJECT (csink, "In flushing");
      ret = GST_FLOW_FLUSHING;
      goto done;
    }

    if (compress_get_hpointer(csink->compress, &available, &tstamp) != 0)
      goto writable_size_failed;

    /* We have space to write now, let's do it */
    if (available >= info.size)
      break;

    GST_LOG_OBJECT (csink, "Waiting for space, available = %" G_GSIZE_FORMAT,
        available);

    /* The buffer is full, let's wait till we're asked for more data */
    if (csink->enable_lpa)
      system ("echo mem > /sys/power/state");
    else
      compress_wait(csink->compress, 10);

    if (g_atomic_int_get (&csink->unlocked)) {
      /* We've been asked to unlock, wait until we can proceed */
      ret = gst_base_sink_wait_preroll (bsink);

      if (ret != GST_FLOW_OK)
        goto unlock_and_fail;
    }
  }

  /* FIXME: we skip basesink sync, is there anything we need to do here? */

  /* FIXME: perform segment clipping */

  if (info.size > 0) {
    wrote = compress_write(csink->compress, info.data, info.size);
    if (wrote < 0) {
      GST_ERROR_OBJECT (csink, "Error playing sample\n");
      GST_ERROR_OBJECT (csink, "ERR: %s\n", compress_get_error(csink->compress));
      goto writable_size_failed;
    }
    if (wrote != info.size) {
      GST_ERROR_OBJECT (csink, "We wrote %d, DSP accepted %d\n", info.size, wrote);
    }
    GST_DEBUG_OBJECT (csink, "%s: wrote %d\n", __func__, wrote);

    if (csink->pauseed) {
      /* We were pauseed, but the buffer is now full, so let's unpause */
      compress_start(csink->compress);
      csink->pauseed = FALSE;
    }
  }


  ret = GST_FLOW_OK;

done:
  gst_buffer_unmap (buf, &info);
  return ret;

  /* ERRORS */
unlock_and_fail:
  {
    goto done;
  }
writable_size_failed:
  {
    GST_ELEMENT_ERROR (csink, RESOURCE, FAILED,
        ("compress_get_hpointer() failed: %s",
            compress_get_error(csink->compress)), (NULL));
    ret = GST_FLOW_ERROR;
    goto unlock_and_fail;
  }
}

static GstFlowReturn
gst_tinycompresssink_unlock (GstBaseSink * bsink)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (bsink);

  GST_LOG_OBJECT (csink, "triggering unlock");

  g_atomic_int_set (&csink->unlocked, 1);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_tinycompresssink_unlock_stop (GstBaseSink * bsink)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (bsink);

  GST_LOG_OBJECT (csink, "stopping unlock");

  g_atomic_int_set (&csink->unlocked, 0);

  return GST_FLOW_OK;
}

static gboolean
gst_tinycompresssink_open_device (GstTinyCompressSink * csink)
{
  struct compr_config config = {0};
  struct snd_codec codec = {0};
  unsigned int card = 0, device = 0;

  config.codec = &codec;
  codec.id = csink->codec_id;
  codec.ch_in = csink->channels;
  codec.ch_out = csink->channels;
  codec.sample_rate = csink->rate;

  sscanf (csink->device, "hw:%d,%d", &card, &device);
  GST_INFO_OBJECT (csink, "device: %s card: %d device: %d", csink->device, card, device);

  csink->compress = compress_open(card, device, COMPRESS_IN, &config);
  if (!csink->compress || !is_compress_ready(csink->compress)) {
    GST_ERROR_OBJECT (csink, "Unable to open Compress device %d:%d\n",
        card, device);
    GST_ERROR_OBJECT (csink, "ERR: %s\n", compress_get_error(csink->compress));
    return FALSE;
  };

  compress_nonblock(csink->compress, 1);

  csink->pauseed = TRUE;

  return TRUE;
}

static gboolean
gst_tinycompresssink_start (GstBaseSink * basesink)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (basesink);

}

static gboolean
gst_tinycompresssink_close_device (GstTinyCompressSink * csink)
{
  GST_LOG_OBJECT (csink, "closing device");

  if (csink->compress)
    compress_stop(csink->compress);

  if (csink->compress)
    compress_close(csink->compress);

  csink->compress = NULL;

  return TRUE;
}

static gboolean
gst_tinycompresssink_stop (GstBaseSink * basesink)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (basesink);

  return gst_tinycompresssink_close_device (csink);
}

static GstClockTime
gst_tinycompresssink_get_time (GstClock * clock, void *userdata)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (userdata);
  unsigned int available;
  struct timespec tstamp;

  if (!csink->compress)
    return GST_CLOCK_TIME_NONE;

  if (compress_get_hpointer(csink->compress, &available, &tstamp) != 0) {
    GST_ERROR_OBJECT (csink, "Error querying timestamp\n");
    GST_ERROR_OBJECT (csink, "ERR: %s\n", compress_get_error(csink->compress));
    GST_DEBUG_OBJECT (csink, "could not get time");
    return GST_CLOCK_TIME_NONE;
  } else {
    GST_ERROR_OBJECT (csink, "DSP played %jd.%jd\n", tstamp.tv_sec, tstamp.tv_nsec*1000);
    GST_LOG_OBJECT (csink, "got time: %" GST_TIME_FORMAT, GST_TIMESPEC_TO_TIME (tstamp));
    return GST_TIMESPEC_TO_TIME (tstamp);
  }
}

static GstCaps *
gst_tinycompresssink_query_getcaps (GstTinyCompressSink * csink, GstCaps * filter)
{
  GstCaps *ret = NULL;

  if (!csink->compress) {
    ret = gst_pad_get_pad_template_caps (GST_BASE_SINK_PAD (csink));
    goto out;
  }

  ret = gst_caps_ref (csink->caps);

out:
  if (filter) {
    GstCaps *tmp = gst_caps_intersect_full (filter, ret,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (ret);
    ret = tmp;
  }

  GST_LOG_OBJECT (csink, "returning caps caps %" GST_PTR_FORMAT, ret);

  return ret;
}

static gboolean
gst_tinycompresssink_query (GstBaseSink * bsink, GstQuery * query)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK_CAST (bsink);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *caps, *filter;

      gst_query_parse_caps (query, &filter);
      caps = gst_tinycompresssink_query_getcaps (csink, filter);

      if (caps) {
        gst_query_set_caps_result (query, caps);
        gst_caps_unref (caps);
        ret = TRUE;
      }

      break;
    }

    default:
      ret =
          GST_BASE_SINK_CLASS (parent_class)->query (GST_BASE_SINK (csink),
          query);
      break;
  }

  return ret;
}

static gboolean
gst_tinycompresssink_event (GstBaseSink * bsink, GstEvent * event)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (bsink);
  gboolean ret = TRUE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:
      GST_DEBUG_OBJECT (csink, "Flushing stream");
      if (csink->compress)
        compress_stop(csink->compress);
      csink->pauseed = TRUE;
      break;

    default:
      break;
  }

  if (ret)
    ret = GST_BASE_SINK_CLASS (parent_class)->event (bsink, event);

  return ret;
}

static GstFlowReturn
gst_tinycompresssink_wait_event (GstBaseSink * bsink, GstEvent * event)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (bsink);
  GstFlowReturn ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      /* Force the stream to start */
      GST_DEBUG_OBJECT (csink, "Forcing start if needed");

      break;

    default:
      break;
  }

  ret = GST_BASE_SINK_CLASS (parent_class)->wait_event (bsink, event);
  if (ret != GST_FLOW_OK)
    return ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      GST_DEBUG_OBJECT (csink, "Draining on EOS");
      compress_write(csink->compress, NULL, 1);
      /* remove suspend as bitstream maybe returned before systen suspended*/
      /*if (csink->enable_lpa)
        system ("echo mem > /sys/power/state");*/
      compress_drain(csink->compress);
      break;

    default:
      break;
  }

  return ret;
}

static void
gst_tinycompresssink_get_times (GstBaseSink * bsink, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{
  /* We need to buffer up some data etc. so we can't let basesink do
   * synchronisation for us. */
  *start = GST_CLOCK_TIME_NONE;
  *end = GST_CLOCK_TIME_NONE;
}

static gboolean
gst_tinycompresssink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstTinyCompressSink *csink = GST_TINYCOMPRESSSINK (bsink);
  const gchar *format;
  GstStructure *structure;
  GstAudioInfo info;

  GST_INFO_OBJECT (bsink, "setting caps %" GST_PTR_FORMAT, caps);

  csink->caps = gst_caps_ref (caps);
  structure = gst_caps_get_structure (caps, 0);
  gst_audio_info_init (&info);

  format = gst_structure_get_name (structure);

  if (g_str_equal (format, "audio/x-raw")) {
    if (!gst_audio_info_from_caps (&info, caps))
      goto parse_error;
  } else if (g_str_equal (format, "audio/mpeg")) {
    gint mpegversion, rate, channels;
    const gchar *stream_format = NULL;

    if (!gst_structure_get_int (structure, "mpegversion", &mpegversion))
      goto parse_error;

    stream_format = gst_structure_get_string (structure, "stream-format");
    if (mpegversion > 1 && !stream_format)
      goto parse_error;

    switch (mpegversion) {
      case 1:
        csink->codec_id = SND_AUDIOCODEC_MP3;
        break;

      case 2:
        if (g_str_equal (stream_format, "raw"))
          csink->codec_id = SND_AUDIOCODEC_AAC;
        else
          csink->codec_id = SND_AUDIOCODEC_AAC;
        break;

      case 4:
        if (g_str_equal (stream_format, "raw"))
          csink->codec_id = SND_AUDIOCODEC_AAC;
        else
          csink->codec_id = SND_AUDIOCODEC_AAC;
        break;

      default:
        g_assert_not_reached ();
    }

    if (!gst_structure_get_int (structure, "rate", &rate))
      goto parse_error;
    if (!gst_structure_get_int (structure, "channels", &channels))
      goto parse_error;

    csink->rate = rate;
    csink->channels = channels;
  } else {
    /* There should be no other format we support as of now */
    g_assert_not_reached ();
  }

  gst_tinycompresssink_close_device (csink);
  if (!gst_tinycompresssink_open_device (csink))
    return FALSE;

  /* Reset clock as we have a new stream */
  gst_audio_clock_reset (GST_AUDIO_CLOCK (csink->clock), 0);

  return TRUE;

  /* ERRORS */
parse_error:
  {
    GST_DEBUG ("could not parse caps");
    return FALSE;
  }
}
