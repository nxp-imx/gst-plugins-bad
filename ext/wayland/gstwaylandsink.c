/* GStreamer Wayland video sink
 *
 * Copyright (C) 2011 Intel Corporation
 * Copyright (C) 2011 Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 * Copyright (C) 2012 Wim Taymans <wim.taymans@gmail.com>
 * Copyright (C) 2014 Collabora Ltd.
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
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

/**
 * SECTION:element-waylandsink
 * @title: waylandsink
 *
 *  The waylandsink is creating its own window and render the decoded video frames to that.
 *  Setup the Wayland environment as described in
 *  [Wayland](http://wayland.freedesktop.org/building.html) home page.
 *
 *  The current implementation is based on weston compositor.
 *
 * ## Example pipelines
 * |[
 * gst-launch-1.0 -v videotestsrc ! waylandsink
 * ]| test the video rendering in wayland
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstwaylandsink.h"
#include <gst/allocators/allocators.h>

#include "gstimxcommon.h"

#include <gst/video/videooverlay.h>

#ifdef HAVE_DMABUFHEAPS_ALLOCATOR
#include <gst/allocators/gstdmabufheaps.h>
#endif
#ifdef HAVE_ION_ALLOCATOR
#include <gst/allocators/gstionmemory.h>
#endif

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/version.h>

#include "hdr10-metadata-unstable-v1-client-protocol.h"

/* signals */
enum
{
  SIGNAL_0,
  LAST_SIGNAL
};

/* Properties */
enum
{
  PROP_0,
  PROP_WINDOW_WIDTH,
  PROP_WINDOW_HEIGHT,
  PROP_DISPLAY,
  PROP_FULLSCREEN,
  PROP_ALPHA,
  PROP_ENABLE_TILE,
  PROP_ROTATE_METHOD,
  PROP_LAST
};

GST_DEBUG_CATEGORY (gstwayland_debug);
#define GST_CAT_DEFAULT gstwayland_debug

#define WL_VIDEO_FORMATS \
    "{ BGRx, BGRA, RGBx, xBGR, xRGB, RGBA, ABGR, ARGB, RGB, BGR, " \
    "RGB16, BGR16, YUY2, YVYU, UYVY, AYUV, NV12, NV21, NV16, NV61, " \
    "YUV9, YVU9, Y41B, I420, YV12, Y42B, v308 }"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (WL_VIDEO_FORMATS) ";"
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES (GST_CAPS_FEATURE_MEMORY_DMABUF,
            WL_VIDEO_FORMATS))
    );

static void gst_wayland_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_wayland_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_wayland_sink_finalize (GObject * object);

static GstStateChangeReturn gst_wayland_sink_change_state (GstElement * element,
    GstStateChange transition);
static void gst_wayland_sink_set_context (GstElement * element,
    GstContext * context);

static gboolean gst_wayland_sink_event (GstBaseSink * bsink, GstEvent * event);
static GstCaps *gst_wayland_sink_get_caps (GstBaseSink * bsink,
    GstCaps * filter);
static gboolean gst_wayland_sink_set_caps (GstBaseSink * bsink, GstCaps * caps);
static gboolean
gst_wayland_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query);
static GstFlowReturn gst_wayland_sink_show_frame (GstVideoSink * vsink,
    GstBuffer * buffer);
static void
gst_wayland_sink_config_hdr10 (GstWaylandSink * self, const GstCaps * caps);

/* VideoOverlay interface */
static void gst_wayland_sink_videooverlay_init (GstVideoOverlayInterface *
    iface);
static void gst_wayland_sink_set_window_handle (GstVideoOverlay * overlay,
    guintptr handle);
static void gst_wayland_sink_set_render_rectangle (GstVideoOverlay * overlay,
    gint x, gint y, gint w, gint h);
static void gst_wayland_sink_expose (GstVideoOverlay * overlay);

#define gst_wayland_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstWaylandSink, gst_wayland_sink, GST_TYPE_VIDEO_SINK,
    G_IMPLEMENT_INTERFACE (GST_TYPE_VIDEO_OVERLAY,
        gst_wayland_sink_videooverlay_init));

static void
gst_wayland_sink_class_init (GstWaylandSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;
  GstVideoSinkClass *gstvideosink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;
  gstvideosink_class = (GstVideoSinkClass *) klass;

  gobject_class->set_property = gst_wayland_sink_set_property;
  gobject_class->get_property = gst_wayland_sink_get_property;
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_wayland_sink_finalize);

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);

  gst_element_class_set_static_metadata (gstelement_class,
      "wayland video sink", "Sink/Video",
      "Output to wayland surface",
      "Sreerenj Balachandran <sreerenj.balachandran@intel.com>, "
      "George Kiagiadakis <george.kiagiadakis@collabora.com>");

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_change_state);
  gstelement_class->set_context =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_set_context);

  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_wayland_sink_event);
  gstbasesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_wayland_sink_get_caps);
  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_wayland_sink_set_caps);
  gstbasesink_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_propose_allocation);

  gstvideosink_class->show_frame =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_show_frame);

  g_object_class_install_property (gobject_class, PROP_WINDOW_WIDTH,
      g_param_spec_int ("window-width", "Wayland sink window width", "Wayland "
          "sink preferred window width in pixel",
          -1, G_MAXINT, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_WINDOW_HEIGHT,
      g_param_spec_int ("window-height", "Wayland sink window height",
          "Wayland " "sink preferred window height in pixel", -1, G_MAXINT, -1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DISPLAY,
      g_param_spec_string ("display", "Wayland Display name", "Wayland "
          "display name to connect to, if not supplied via the GstContext",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FULLSCREEN,
      g_param_spec_boolean ("fullscreen", "Fullscreen",
          "Whether the surface should be made fullscreen ", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ALPHA,
      g_param_spec_float ("alpha", "Wayland surface alpha", "Wayland "
          "surface alpha value, apply custom alpha value to wayland surface",
          0.0f, 1.0f, 0.0f, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ENABLE_TILE,
      g_param_spec_boolean ("enable-tile", "enable hantro tile",
          "When enabled, the sink propose VSI tile modifier to VPU", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));

  /**
   * waylandsink:rotate-method:
   *
   * Since: 1.22
   */
  g_object_class_install_property (gobject_class, PROP_ROTATE_METHOD,
      g_param_spec_enum ("rotate-method",
          "rotate method",
          "rotate method",
          GST_TYPE_VIDEO_ORIENTATION_METHOD, GST_VIDEO_ORIENTATION_IDENTITY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

 /**
  * waylandsink:render-rectangle:
  *
  * This helper installs the "render-rectangle" property into the
  * class.
  *
  * Since: 1.22
  */
  gst_video_overlay_install_properties (gobject_class, PROP_LAST);
}

static void
gst_wayland_sink_init (GstWaylandSink * self)
{
  self->alpha = 0.0f;
  self->preferred_width = -1;
  self->preferred_height = -1;
  g_mutex_init (&self->display_lock);
  g_mutex_init (&self->render_lock);
  g_cond_init (&self->redraw_wait);
  self->frame_showed = 0;
  self->run_time = 0;
  self->enable_tile = FALSE;
}

static void
gst_wayland_sink_set_fullscreen (GstWaylandSink * self, gboolean fullscreen)
{
  if (fullscreen == self->fullscreen)
    return;

  g_mutex_lock (&self->render_lock);
  self->fullscreen = fullscreen;
  gst_wl_window_ensure_fullscreen (self->window, fullscreen);
  g_mutex_unlock (&self->render_lock);
}

static void
gst_wayland_sink_set_rotate_method (GstWaylandSink * self,
    GstVideoOrientationMethod method, gboolean from_tag)
{
  GstVideoOrientationMethod new_method;

  if (method == GST_VIDEO_ORIENTATION_CUSTOM) {
    GST_WARNING_OBJECT (self, "unsupported custom orientation");
    return;
  }

  GST_OBJECT_LOCK (self);
  if (from_tag)
    self->tag_rotate_method = method;
  else
    self->sink_rotate_method = method;

  if (self->sink_rotate_method == GST_VIDEO_ORIENTATION_AUTO)
    new_method = self->tag_rotate_method;
  else
    new_method = self->sink_rotate_method;

  if (new_method != self->current_rotate_method) {
    GST_DEBUG_OBJECT (self, "Changing method from %d to %d",
        self->current_rotate_method, new_method);

    if (self->window) {
      g_mutex_lock (&self->render_lock);
      gst_wl_window_set_rotate_method (self->window, new_method);
      g_mutex_unlock (&self->render_lock);
    }

    self->current_rotate_method = new_method;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_wayland_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (object);

  switch (prop_id) {
    case PROP_DISPLAY:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->display_name);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_FULLSCREEN:
      GST_OBJECT_LOCK (self);
      g_value_set_boolean (value, self->fullscreen);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_WINDOW_WIDTH:
      g_value_set_int (value, self->preferred_width);
      break;
    case PROP_WINDOW_HEIGHT:
      g_value_set_int (value, self->preferred_height);
      break;
    case PROP_ALPHA:
      g_value_set_float (value, self->alpha);
      break;
    case PROP_ROTATE_METHOD:
      GST_OBJECT_LOCK (self);
      g_value_set_enum (value, self->current_rotate_method);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_ENABLE_TILE:
      g_value_set_boolean (value, self->enable_tile);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_wayland_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (object);

  switch (prop_id) {
    case PROP_DISPLAY:
      GST_OBJECT_LOCK (self);
      self->display_name = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_FULLSCREEN:
      GST_OBJECT_LOCK (self);
      gst_wayland_sink_set_fullscreen (self, g_value_get_boolean (value));
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_WINDOW_WIDTH:
      self->preferred_width = g_value_get_int (value);
      break;
    case PROP_WINDOW_HEIGHT:
      self->preferred_height = g_value_get_int (value);
      break;
    case PROP_ALPHA:
      self->alpha = g_value_get_float (value);
      break;
    case PROP_ROTATE_METHOD:
      gst_wayland_sink_set_rotate_method (self, g_value_get_enum (value),
          FALSE);
      break;
    case PROP_ENABLE_TILE:
      self->enable_tile = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_wayland_sink_finalize (GObject * object)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (object);

  GST_DEBUG_OBJECT (self, "Finalizing the sink..");

  if (self->last_buffer)
    gst_buffer_unref (self->last_buffer);
  if (self->display)
    g_object_unref (self->display);
  if (self->window)
    g_object_unref (self->window);
  if (self->pool)
    gst_object_unref (self->pool);

  g_free (self->display_name);

  g_mutex_clear (&self->display_lock);
  g_mutex_clear (&self->render_lock);
  g_cond_clear (&self->redraw_wait);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* must be called with the display_lock */
static void
gst_wayland_sink_set_display_from_context (GstWaylandSink * self,
    GstContext * context)
{
  struct wl_display *display;
  GError *error = NULL;

  display = gst_wl_display_handle_context_get_handle (context);
  self->display = gst_wl_display_new_existing (display, FALSE, &error);

  if (error) {
    GST_ELEMENT_WARNING (self, RESOURCE, OPEN_READ_WRITE,
        ("Could not set display handle"),
        ("Failed to use the external wayland display: '%s'", error->message));
    g_error_free (error);
  }
}

static gboolean
gst_wayland_sink_find_display (GstWaylandSink * self)
{
  GstQuery *query;
  GstMessage *msg;
  GstContext *context = NULL;
  GError *error = NULL;
  gboolean ret = TRUE;

  g_mutex_lock (&self->display_lock);

  if (!self->display) {
    /* first query upstream for the needed display handle */
    query = gst_query_new_context (GST_WL_DISPLAY_HANDLE_CONTEXT_TYPE);
    if (gst_pad_peer_query (GST_VIDEO_SINK_PAD (self), query)) {
      gst_query_parse_context (query, &context);
      gst_wayland_sink_set_display_from_context (self, context);
    }
    gst_query_unref (query);

    if (G_LIKELY (!self->display)) {
      /* now ask the application to set the display handle */
      msg = gst_message_new_need_context (GST_OBJECT_CAST (self),
          GST_WL_DISPLAY_HANDLE_CONTEXT_TYPE);

      g_mutex_unlock (&self->display_lock);
      gst_element_post_message (GST_ELEMENT_CAST (self), msg);
      /* at this point we expect gst_wayland_sink_set_context
       * to get called and fill self->display */
      g_mutex_lock (&self->display_lock);

      if (!self->display) {
        /* if the application didn't set a display, let's create it ourselves */
        GST_OBJECT_LOCK (self);
        self->display = gst_wl_display_new (self->display_name, &error);
        GST_OBJECT_UNLOCK (self);

        if (error) {
          GST_ELEMENT_WARNING (self, RESOURCE, OPEN_READ_WRITE,
              ("Could not initialise Wayland output"),
              ("Failed to create GstWlDisplay: '%s'", error->message));
          g_error_free (error);
          ret = FALSE;
        }
      }
    }
  }

  g_mutex_unlock (&self->display_lock);

  return ret;
}

static GstStateChangeReturn
gst_wayland_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (element);
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  struct wl_subsurface *area_surface;
  struct wl_display *display_display;
  gint render_rectangle_w, render_rectangle_h;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_wayland_sink_find_display (self))
        return GST_STATE_CHANGE_FAILURE;
      gst_wl_display_set_preferred_width (self->display, self->preferred_width);
      gst_wl_display_set_preferred_height (self->display, self->preferred_height);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      self->run_time = gst_element_get_start_time (element);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_buffer_replace (&self->last_buffer, NULL);
      gst_wayland_sink_config_hdr10 (self, NULL);
      if (self->window) {
        area_surface = gst_wl_window_get_area_surface (self->window);
        render_rectangle_w = gst_wl_window_get_rectangle_w (self->window);
        render_rectangle_h = gst_wl_window_get_rectangle_h (self->window);
        display_display = gst_wl_display_get_display (self->display);
        gst_wl_window_set_alpha (self->window, 1.0);
        wl_surface_damage (area_surface, 0, 0,
            render_rectangle_w, render_rectangle_h);
        wl_surface_commit (area_surface);
        wl_display_roundtrip (display_display);
        if (gst_wl_window_is_toplevel (self->window)) {
          g_clear_object (&self->window);
        } else {
          /* remove buffer from surface, show nothing */
          gst_wl_window_render (self->window, NULL, NULL);
        }
      }

      g_mutex_lock (&self->render_lock);
      if (self->callback) {
        wl_callback_destroy (self->callback);
        self->callback = NULL;
      }
      self->redraw_pending = FALSE;
      g_mutex_unlock (&self->render_lock);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      g_mutex_lock (&self->display_lock);
      /* If we had a toplevel window, we most likely have our own connection
       * to the display too, and it is a good idea to disconnect and allow
       * potentially the application to embed us with GstVideoOverlay
       * (which requires to re-use the same display connection as the parent
       * surface). If we didn't have a toplevel window, then the display
       * connection that we have is definitely shared with the application
       * and it's better to keep it around (together with the window handle)
       * to avoid requesting them again from the application if/when we are
       * restarted (GstVideoOverlay behaves like that in other sinks)
       */
      if (self->display && !self->window)       /* -> the window was toplevel */
        g_clear_object (&self->display);

      g_mutex_unlock (&self->display_lock);
      g_clear_object (&self->pool);

      if (self->run_time > 0) {
        g_print ("Total showed frames (%lld), playing for (%" GST_TIME_FORMAT
            "), fps (%.3f).\n", self->frame_showed,
            GST_TIME_ARGS (self->run_time),
            (gfloat) GST_SECOND * self->frame_showed / self->run_time);
      }
      self->frame_showed = 0;
      self->run_time = 0;
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_wayland_sink_set_context (GstElement * element, GstContext * context)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (element);

  if (gst_context_has_context_type (context,
          GST_WL_DISPLAY_HANDLE_CONTEXT_TYPE)) {
    g_mutex_lock (&self->display_lock);
    if (G_LIKELY (!self->display)) {
      gst_wayland_sink_set_display_from_context (self, context);
    } else {
      GST_WARNING_OBJECT (element, "changing display handle is not supported");
      g_mutex_unlock (&self->display_lock);
      return;
    }
    g_mutex_unlock (&self->display_lock);
  }

  if (GST_ELEMENT_CLASS (parent_class)->set_context)
    GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static gboolean
gst_wayland_sink_event (GstBaseSink * bsink, GstEvent * event)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (bsink);
  GstTagList *taglist;
  GstVideoOrientationMethod method;
  gboolean ret;

  GST_DEBUG_OBJECT (self, "handling %s event", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_TAG:
      gst_event_parse_tag (event, &taglist);

      if (gst_video_orientation_from_tag (taglist, &method)) {
        gst_wayland_sink_set_rotate_method (self, method, TRUE);
      }

      break;
    default:
      break;
  }

  ret = GST_BASE_SINK_CLASS (parent_class)->event (bsink, event);

  return ret;
}

static GstCaps *
gst_wayland_sink_get_caps (GstBaseSink * bsink, GstCaps * filter)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (bsink);;
  GstCaps *caps;

  caps = gst_pad_get_pad_template_caps (GST_VIDEO_SINK_PAD (self));
  caps = gst_caps_make_writable (caps);

  g_mutex_lock (&self->display_lock);

  if (self->display) {
    GValue shm_list = G_VALUE_INIT, dmabuf_list = G_VALUE_INIT;
    GValue value = G_VALUE_INIT;
    GArray *formats;
    gint i;
    guint fmt;
    GstVideoFormat gfmt;

    g_value_init (&shm_list, GST_TYPE_LIST);
    g_value_init (&dmabuf_list, GST_TYPE_LIST);

    /* Add corresponding shm formats */
    formats = gst_wl_display_get_shm_formats (self->display);
    for (i = 0; i < formats->len; i++) {
      fmt = g_array_index (formats, uint32_t, i);
      gfmt = gst_wl_shm_format_to_video_format (fmt);
      if (gfmt != GST_VIDEO_FORMAT_UNKNOWN) {
        g_value_init (&value, G_TYPE_STRING);
        g_value_set_static_string (&value, gst_video_format_to_string (gfmt));
        gst_value_list_append_and_take_value (&shm_list, &value);
      }
    }

    /** FIXME:work around for 10bit format not in the none capsfeature list
     * need vpu add memory:DMABuf capsfeature when output dmabuf
    */
    if (HAS_DCSS ()) {
      g_value_init (&value, G_TYPE_STRING);
      g_value_set_static_string (&value,
          gst_video_format_to_string (GST_VIDEO_FORMAT_NV12_10LE40));
      gst_value_list_append_and_take_value (&shm_list, &value);
    }

    gst_structure_take_value (gst_caps_get_structure (caps, 0), "format",
        &shm_list);

    /* Add corresponding dmabuf formats */
    formats = gst_wl_display_get_dmabuf_formats (self->display);
    for (i = 0; i < formats->len; i++) {
      fmt = g_array_index (formats, uint32_t, i);
      gfmt = gst_wl_dmabuf_format_to_video_format (fmt);
      if (gfmt != GST_VIDEO_FORMAT_UNKNOWN) {
        g_value_init (&value, G_TYPE_STRING);
        g_value_set_static_string (&value, gst_video_format_to_string (gfmt));
        gst_value_list_append_and_take_value (&dmabuf_list, &value);
      }
    }

    gst_structure_take_value (gst_caps_get_structure (caps, 1), "format",
        &dmabuf_list);

    GST_DEBUG_OBJECT (self, "display caps: %" GST_PTR_FORMAT, caps);
  }

  g_mutex_unlock (&self->display_lock);

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = intersection;
  }

  return caps;
}

#define ISALIGNED(a, b) (!(a & (b-1)))

static GstBufferPool *
gst_wayland_create_pool (GstWaylandSink * self, GstCaps * caps)
{
  GstBufferPool *pool = NULL;
  GstStructure *structure;
  gsize size = self->video_info.size;
  GstAllocator *alloc = NULL;

  pool = gst_wl_video_buffer_pool_new ();

  GstVideoInfo info;
  gst_video_info_from_caps (&info, caps);
  GstVideoFormat format = GST_VIDEO_INFO_FORMAT (&info);
  if (gst_wl_display_check_format_for_dmabuf (self->display, format)) {
#ifdef HAVE_DMABUFHEAPS_ALLOCATOR
    alloc = gst_dmabufheaps_allocator_obtain ();
#endif
#ifdef HAVE_ION_ALLOCATOR
    if (!alloc)
      alloc = gst_ion_allocator_obtain ();
#endif
  }

  structure = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (structure, caps, size, 2, 0);
  if (!alloc)
    alloc = gst_wl_shm_allocator_get ();
  else {
    gint w = GST_VIDEO_INFO_WIDTH (&self->video_info);
    gint h = GST_VIDEO_INFO_HEIGHT (&self->video_info);
    if (!ISALIGNED (w, 16) || !ISALIGNED (h, 16)) {
      GstVideoAlignment alignment;

      memset (&alignment, 0, sizeof (GstVideoAlignment));
      alignment.padding_right = GST_ROUND_UP_N (w, 16) - w;
      alignment.padding_bottom = GST_ROUND_UP_N (h, 16) - h;

      GST_DEBUG
          ("align buffer pool, w(%d) h(%d), padding_right (%d), padding_bottom (%d)",
          w, h, alignment.padding_right, alignment.padding_bottom);

      gst_buffer_pool_config_add_option (structure,
          GST_BUFFER_POOL_OPTION_VIDEO_META);
      gst_buffer_pool_config_add_option (structure,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
      gst_buffer_pool_config_set_video_alignment (structure, &alignment);
    }
  }
  gst_buffer_pool_config_set_allocator (structure, alloc, NULL);
  if (!gst_buffer_pool_set_config (pool, structure)) {
    g_object_unref (pool);
    pool = NULL;
  }
  g_object_unref (alloc);

  return pool;
}

static gboolean
gst_wayland_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (bsink);;
  gboolean use_dmabuf;
  GstVideoFormat format;

  GST_DEBUG_OBJECT (self, "set caps %" GST_PTR_FORMAT, caps);

  /* extract info from caps */
  if (!gst_video_info_from_caps (&self->video_info, caps))
    goto invalid_format;

  self->src_info = self->video_info;

  format = GST_VIDEO_INFO_FORMAT (&self->video_info);
  self->video_info_changed = TRUE;

  /* create a new pool for the new caps */
  if (self->pool)
    gst_object_unref (self->pool);
  self->pool = gst_wayland_create_pool (self, caps);

  use_dmabuf = gst_caps_features_contains (gst_caps_get_features (caps, 0),
      GST_CAPS_FEATURE_MEMORY_DMABUF);

  /* validate the format base on the memory type. */
  if (use_dmabuf) {
    if (!gst_wl_display_check_format_for_dmabuf (self->display, format))
      goto unsupported_format;
  } else if (!gst_wl_display_check_format_for_shm (self->display, format)) {
    goto unsupported_format;
  }

  self->use_dmabuf = use_dmabuf;

  gst_wayland_sink_config_hdr10 (self, caps);

  return TRUE;

invalid_format:
  {
    GST_ERROR_OBJECT (self,
        "Could not locate image format from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
unsupported_format:
  {
    GST_ERROR_OBJECT (self, "Format %s is not available on the display",
        gst_video_format_to_string (format));
    return FALSE;
  }
}

static gboolean
gst_wayland_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (bsink);
  GstCaps *caps;
  GstBufferPool *pool = NULL;
  gboolean need_pool;
  GstAllocator *alloc = NULL;
  guint64 drm_modifier;

  gst_query_parse_allocation (query, &caps, &need_pool);

  drm_modifier = DRM_FORMAT_MOD_AMPHION_TILED;
  gst_query_add_allocation_dmabuf_meta (query, drm_modifier);

  if (self->enable_tile && HAS_DCSS ()) {
    drm_modifier = DRM_FORMAT_MOD_VSI_G1_TILED;
    gst_query_add_allocation_dmabuf_meta (query, drm_modifier);
    drm_modifier = DRM_FORMAT_MOD_VSI_G2_TILED;
    gst_query_add_allocation_dmabuf_meta (query, drm_modifier);
    drm_modifier = DRM_FORMAT_MOD_VSI_G2_TILED_COMPRESSED;
    gst_query_add_allocation_dmabuf_meta (query, drm_modifier);
  }

  if (need_pool)
    pool = gst_wayland_create_pool (self, caps);

  if (pool) {
    GstStructure *config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_allocator (config, &alloc, NULL);
    g_object_ref (alloc);
    gst_structure_free (config);
  }

  gst_query_add_allocation_pool (query, pool, self->video_info.size, 2, 0);
  if (pool)
    g_object_unref (pool);

  if (!alloc)
    alloc = gst_wl_shm_allocator_get ();
  gst_query_add_allocation_param (query, alloc, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_CROP_META_API_TYPE, NULL);
  g_object_unref (alloc);

  return TRUE;
}

static void
frame_redraw_callback (void *data, struct wl_callback *callback, uint32_t time)
{
  GstWaylandSink *self = data;

  GST_LOG_OBJECT (self, "frame_redraw_cb");

  g_mutex_lock (&self->render_lock);
  self->redraw_pending = FALSE;
  g_cond_signal (&self->redraw_wait);

  if (self->callback) {
    wl_callback_destroy (callback);
    self->callback = NULL;
  }
  g_mutex_unlock (&self->render_lock);
}

static const struct wl_callback_listener frame_callback_listener = {
  frame_redraw_callback
};

/* must be called with the render lock */
static void
render_last_buffer (GstWaylandSink * self, gboolean redraw)
{
  GstWlBuffer *wlbuffer;
  const GstVideoInfo *info = NULL;
  struct wl_surface *surface;
  struct wl_callback *callback;

  wlbuffer = gst_buffer_get_wl_buffer (self->display, self->last_buffer);
  surface = gst_wl_window_get_wl_surface (self->window);

  self->redraw_pending = TRUE;
  callback = wl_surface_frame (surface);
  self->callback = callback;
  wl_callback_add_listener (callback, &frame_callback_listener, self);

  if (G_UNLIKELY (self->video_info_changed && !redraw)) {
    info = &self->video_info;
    self->video_info_changed = FALSE;
  }
  gst_wl_window_render (self->window, wlbuffer, info);
}

static void
on_window_closed (GstWlWindow * window, gpointer user_data)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (user_data);

  /* Handle window closure by posting an error on the bus */
  GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
      ("Output window was closed"), (NULL));
}

static void
gst_wayland_sink_config_hdr10 (GstWaylandSink * self, const GstCaps * caps)
{
  GstWlDisplay *display = self->display;
  struct zwp_hdr10_metadata_v1 *hdr10_metadata;
  struct wl_display *display_display;
  guint32 eotf = 0;
  guint32 type = 0;
  guint32 display_primaries_red = 0;
  guint32 display_primaries_green = 0;
  guint32 display_primaries_blue = 0;
  guint32 white_point = 0;
  guint32 mastering_display_luminance = 0;
  guint32 max_cll = 0;
  guint32 max_fall = 0;

  /* buf could be NULL when resize */

  if (caps) {
    GstVideoMasteringDisplayInfo minfo;
    GstVideoContentLightLevel cll;
    if (!gst_video_mastering_display_info_from_caps (&minfo, caps)
        || !gst_video_content_light_level_from_caps (&cll, caps)) {
      GST_INFO_OBJECT (self, "no HDR metadata present in caps");
      return;
    }

    GST_INFO_OBJECT (self, "redPrimary x=%d y=%d", minfo.display_primaries[0].x,
        minfo.display_primaries[0].y);
    GST_INFO_OBJECT (self, "greenPrimary x=%d y=%d",
        minfo.display_primaries[1].x, minfo.display_primaries[1].y);
    GST_INFO_OBJECT (self, "bluePrimary x=%d y=%d",
        minfo.display_primaries[2].x, minfo.display_primaries[2].y);
    GST_INFO_OBJECT (self, "whitePoint x=%d y=%d", minfo.white_point.x,
        minfo.white_point.y);
    GST_INFO_OBJECT (self, "maxMasteringLuminance %d",
        minfo.max_display_mastering_luminance);
    GST_INFO_OBJECT (self, "minMasteringLuminance %d",
        minfo.min_display_mastering_luminance);
    GST_INFO_OBJECT (self, "maxContentLightLevel %d",
        cll.max_content_light_level);
    GST_INFO_OBJECT (self, "maxFrameAverageLightLevel %d",
        cll.max_frame_average_light_level);

    eotf = SMPTE_ST2084;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
    type = 0;
#else
    type = 1;
#endif
    display_primaries_red =
        (guint) (minfo.display_primaries[0].x << 16 | minfo.
        display_primaries[0].y);
    display_primaries_green =
        (guint) (minfo.display_primaries[1].x << 16 | minfo.
        display_primaries[1].y);
    display_primaries_blue =
        (guint) (minfo.display_primaries[2].x << 16 | minfo.
        display_primaries[2].y);
    white_point = (guint) (minfo.white_point.x << 16 | minfo.white_point.y);
    mastering_display_luminance =
        (guint) (((minfo.max_display_mastering_luminance /
                10000) & 0xffff) << 16 | (minfo.min_display_mastering_luminance
            & 0xffff));
    max_cll = cll.max_content_light_level;
    max_fall = cll.max_frame_average_light_level;
  }

  hdr10_metadata = gst_wl_display_get_hdr10_metadata (display);
  display_display = gst_wl_display_get_display (display);
  if (hdr10_metadata) {
    zwp_hdr10_metadata_v1_set_metadata (hdr10_metadata,
        eotf,
        type,
        display_primaries_red,
        display_primaries_green,
        display_primaries_blue,
        white_point, mastering_display_luminance, max_cll, max_fall);
    wl_display_roundtrip (display_display);
  }
}

static GstFlowReturn
gst_wayland_sink_show_frame (GstVideoSink * vsink, GstBuffer * buffer)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (vsink);
  GstBuffer *to_render;
  GstWlBuffer *wlbuffer;
  GstVideoMeta *vmeta;
  GstVideoFormat format;
  GstVideoInfo old_vinfo;
  GstMemory *mem;
  struct wl_buffer *wbuf = NULL;

  GstFlowReturn ret = GST_FLOW_OK;

  g_mutex_lock (&self->render_lock);

  GST_LOG_OBJECT (self, "render buffer %" GST_PTR_FORMAT "", buffer);

  if (G_UNLIKELY (!self->window)) {
    /* ask for window handle. Unlock render_lock while doing that because
     * set_window_handle & friends will lock it in this context */
    g_mutex_unlock (&self->render_lock);
    gst_video_overlay_prepare_window_handle (GST_VIDEO_OVERLAY (self));
    g_mutex_lock (&self->render_lock);

    if (!self->window) {
      /* if we were not provided a window, create one ourselves */
      self->window = gst_wl_window_new_toplevel (self->display,
          &self->video_info, self->fullscreen, &self->render_lock);
      g_signal_connect_object (self->window, "closed",
          G_CALLBACK (on_window_closed), self, 0);
      gst_wl_window_set_rotate_method (self->window,
          self->current_rotate_method);
    }
    gst_wl_window_set_alpha (self->window, self->alpha);
  }

  while (self->redraw_pending)
    g_cond_wait (&self->redraw_wait, &self->render_lock);

  /* make sure that the application has called set_render_rectangle() */
  if (G_UNLIKELY (gst_wl_window_get_render_rectangle (self->window)->w == 0))
    goto no_window_size;

  gst_wl_window_set_source_crop (self->window, buffer);

  wlbuffer = gst_buffer_get_wl_buffer (self->display, buffer);

  if (G_LIKELY (wlbuffer &&
          gst_wl_buffer_get_display (wlbuffer) == self->display)) {
    GST_LOG_OBJECT (self,
        "buffer %" GST_PTR_FORMAT " has a wl_buffer from our display, "
        "writing directly", buffer);
    to_render = buffer;
    goto render;
  }

  /* update video info from video meta */
  mem = gst_buffer_peek_memory (buffer, 0);

  old_vinfo = self->video_info;
  vmeta = gst_buffer_get_video_meta (buffer);
  if (vmeta) {
    gint i;

    for (i = 0; i < vmeta->n_planes; i++) {
      self->video_info.offset[i] = vmeta->offset[i];
      self->video_info.stride[i] = vmeta->stride[i];
    }
    self->video_info.size = gst_buffer_get_size (buffer);
  }

  GST_LOG_OBJECT (self,
      "buffer %" GST_PTR_FORMAT " does not have a wl_buffer from our "
      "display, creating it", buffer);

  format = GST_VIDEO_INFO_FORMAT (&self->video_info);
  if (gst_wl_display_check_format_for_dmabuf (self->display, format)) {
    guint i, nb_dmabuf = 0;

    for (i = 0; i < gst_buffer_n_memory (buffer); i++)
      if (gst_is_dmabuf_memory (gst_buffer_peek_memory (buffer, i)))
        nb_dmabuf++;

    if (nb_dmabuf && (nb_dmabuf == gst_buffer_n_memory (buffer)))
      wbuf = gst_wl_linux_dmabuf_construct_wl_buffer (buffer, self->display,
          &self->video_info);
  }

  if (!wbuf && gst_wl_display_check_format_for_shm (self->display, format)) {
    if (gst_buffer_n_memory (buffer) == 1 && gst_is_fd_memory (mem))
      wbuf = gst_wl_shm_memory_construct_wl_buffer (mem, self->display,
          &self->video_info);
  }

  /* If nothing worked, copy into our internal pool */
  if (!wbuf) {
    GstVideoFrame src, dst;

    /* rollback video info changes */
    self->video_info = old_vinfo;

    GST_LOG_OBJECT (self, "buffer %p cannot have a wl_buffer, "
        "copying to internal memory", buffer);

    /* self->pool always exists (created in set_caps), but it may not
     * be active if upstream is not using it */
    if (!gst_buffer_pool_is_active (self->pool)) {
      GstStructure *config;
      GstCaps *caps;

      config = gst_buffer_pool_get_config (self->pool);
      gst_buffer_pool_config_get_params (config, &caps, NULL, NULL, NULL);

      /* revert back to default strides and offsets */
      gst_video_info_from_caps (&self->video_info, caps);
      gst_buffer_pool_config_set_params (config, caps, self->video_info.size,
          2, 0);

      /* This is a video pool, it should not fail with basic setings */
      if (!gst_buffer_pool_set_config (self->pool, config) ||
          !gst_buffer_pool_set_active (self->pool, TRUE))
        goto activate_failed;
    }

    ret = gst_buffer_pool_acquire_buffer (self->pool, &to_render, NULL);
    if (ret != GST_FLOW_OK)
      goto no_buffer;

    wlbuffer = gst_buffer_get_wl_buffer (self->display, to_render);

    /* attach a wl_buffer if there isn't one yet */
    if (G_UNLIKELY (!wlbuffer)) {
      mem = gst_buffer_peek_memory (to_render, 0);
      if (gst_wl_display_check_format_for_dmabuf (self->display, format)
          && gst_is_dmabuf_memory (mem)) {
        GstVideoInfo info = self->video_info;

        if (self->pool && gst_buffer_pool_is_active (self->pool)) {
          GstStructure *config;
          GstVideoAlignment video_align;
          memset (&video_align, 0, sizeof (GstVideoAlignment));
          config = gst_buffer_pool_get_config (self->pool);

          if (gst_buffer_pool_config_has_option (config,
                  GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT)) {
            gst_buffer_pool_config_get_video_alignment (config, &video_align);

            GST_DEBUG_OBJECT (self, "pool has alignment (%d, %d) , (%d, %d)",
                video_align.padding_left, video_align.padding_top,
                video_align.padding_right, video_align.padding_bottom);

            gst_video_info_align (&info, &video_align);
          }
          gst_structure_free (config);
        }

        wbuf =
            gst_wl_linux_dmabuf_construct_wl_buffer (to_render, self->display,
            &info);
      }

      if (!wbuf) {
        GST_DEBUG_OBJECT (self, "no dmabuf available, try shm");
        wbuf = gst_wl_shm_memory_construct_wl_buffer (mem, self->display,
            &self->video_info);
      }

      if (G_UNLIKELY (!wbuf))
        goto no_wl_buffer_shm;

      wlbuffer = gst_buffer_add_wl_buffer (to_render, wbuf, self->display);
    }

    if (!gst_video_frame_map (&dst, &self->video_info, to_render,
            GST_MAP_WRITE))
      goto dst_map_failed;

    if (!gst_video_frame_map (&src, &self->src_info, buffer, GST_MAP_READ)) {
      gst_video_frame_unmap (&dst);
      goto src_map_failed;
    }

    gst_video_frame_copy (&dst, &src);

    gst_video_frame_unmap (&src);
    gst_video_frame_unmap (&dst);

    goto render;
  }

  if (!wbuf)
    goto no_wl_buffer;

  wlbuffer = gst_buffer_add_wl_buffer (buffer, wbuf, self->display);
  to_render = buffer;

render:
  /* drop double rendering */
  if (G_UNLIKELY (wlbuffer ==
          gst_buffer_get_wl_buffer (self->display, self->last_buffer))) {
    GST_LOG_OBJECT (self, "Buffer already being rendered");
    goto done;
  }

  gst_buffer_replace (&self->last_buffer, to_render);
  render_last_buffer (self, FALSE);

  if (buffer != to_render)
    gst_buffer_unref (to_render);
  goto done;

no_window_size:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, WRITE,
        ("Window has no size set"),
        ("Make sure you set the size after calling set_window_handle"));
    ret = GST_FLOW_ERROR;
    goto done;
  }
no_buffer:
  {
    GST_WARNING_OBJECT (self, "could not create buffer");
    goto done;
  }
no_wl_buffer_shm:
  {
    GST_ERROR_OBJECT (self, "could not create wl_buffer out of wl_shm memory");
    ret = GST_FLOW_ERROR;
    goto done;
  }
no_wl_buffer:
  {
    GST_ERROR_OBJECT (self,
        "buffer %" GST_PTR_FORMAT " cannot have a wl_buffer", buffer);
    ret = GST_FLOW_ERROR;
    goto done;
  }
activate_failed:
  {
    GST_ERROR_OBJECT (self, "failed to activate bufferpool.");
    ret = GST_FLOW_ERROR;
    goto done;
  }
src_map_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, READ,
        ("Video memory can not be read from userspace."), (NULL));
    ret = GST_FLOW_ERROR;
    goto done;
  }
dst_map_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, WRITE,
        ("Video memory can not be written from userspace."), (NULL));
    ret = GST_FLOW_ERROR;
    goto done;
  }
done:
  {
    self->frame_showed++;
    g_mutex_unlock (&self->render_lock);
    return ret;
  }
}

static void
gst_wayland_sink_videooverlay_init (GstVideoOverlayInterface * iface)
{
  iface->set_window_handle = gst_wayland_sink_set_window_handle;
  iface->set_render_rectangle = gst_wayland_sink_set_render_rectangle;
  iface->expose = gst_wayland_sink_expose;
}

static void
gst_wayland_sink_set_window_handle (GstVideoOverlay * overlay, guintptr handle)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (overlay);
  struct wl_surface *surface = (struct wl_surface *) handle;

  g_return_if_fail (self != NULL);

  if (self->window != NULL) {
    GST_WARNING_OBJECT (self, "changing window handle is not supported");
    return;
  }

  g_mutex_lock (&self->render_lock);

  GST_DEBUG_OBJECT (self, "Setting window handle %" GST_PTR_FORMAT,
      (void *) handle);

  g_clear_object (&self->window);

  if (handle) {
    if (G_LIKELY (gst_wayland_sink_find_display (self))) {
      /* we cannot use our own display with an external window handle */
      if (G_UNLIKELY (gst_wl_display_has_own_display (self->display))) {
        GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE,
            ("Application did not provide a wayland display handle"),
            ("waylandsink cannot use an externally-supplied surface without "
                "an externally-supplied display handle. Consider providing a "
                "display handle from your application with GstContext"));
      } else {
        self->window = gst_wl_window_new_in_surface (self->display, surface,
            &self->render_lock);
        gst_wl_window_set_rotate_method (self->window,
            self->current_rotate_method);
      }
    } else {
      GST_ERROR_OBJECT (self, "Failed to find display handle, "
          "ignoring window handle");
    }
  }

  g_mutex_unlock (&self->render_lock);
}

static void
gst_wayland_sink_set_render_rectangle (GstVideoOverlay * overlay,
    gint x, gint y, gint w, gint h)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (overlay);

  g_return_if_fail (self != NULL);

  g_mutex_lock (&self->render_lock);
  if (!self->window) {
    g_mutex_unlock (&self->render_lock);
    GST_WARNING_OBJECT (self,
        "set_render_rectangle called without window, ignoring");
    return;
  }

  GST_DEBUG_OBJECT (self, "window geometry changed to (%d, %d) %d x %d",
      x, y, w, h);
  gst_wl_window_set_render_rectangle (self->window, x, y, w, h);

  g_mutex_unlock (&self->render_lock);
}

static void
gst_wayland_sink_expose (GstVideoOverlay * overlay)
{
  GstWaylandSink *self = GST_WAYLAND_SINK (overlay);

  g_return_if_fail (self != NULL);

  GST_DEBUG_OBJECT (self, "expose");

  g_mutex_lock (&self->render_lock);
  if (self->last_buffer && !self->redraw_pending) {
    GST_DEBUG_OBJECT (self, "redrawing last buffer");
    render_last_buffer (self, TRUE);
  }
  g_mutex_unlock (&self->render_lock);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GstRank rank = GST_RANK_MARGINAL;

  GST_DEBUG_CATEGORY_INIT (gstwayland_debug, "waylandsink", 0,
      " wayland video sink");

  if (HAS_DPU ()) {
    if (HAS_VPU ())
      rank = IMX_GST_PLUGIN_RANK + 1;
  } else if (IS_IMX8MM () || IS_IMX8MN () || IS_IMX8MP () || IS_IMX8ULP () || IS_IMX93()) {
    rank = IMX_GST_PLUGIN_RANK + 1;
  } else if (HAS_DCSS ()) {
    rank = IMX_GST_PLUGIN_RANK;
  }

  GST_ELEMENT_REGISTER_DEFINE (waylandsink, "waylandsink", rank,
    GST_TYPE_WAYLAND_SINK);

  return GST_ELEMENT_REGISTER (waylandsink, plugin);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    waylandsink,
    "Wayland Video Sink", plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
