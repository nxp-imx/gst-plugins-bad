/* GStreamer Wayland Library
 *
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstwldisplay.h"

#include "fullscreen-shell-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "alpha-compositing-unstable-v1-client-protocol.h"
#include "linux-explicit-synchronization-unstable-v1-client-protocol.h"
#include "hdr10-metadata-unstable-v1-client-protocol.h"

#include <errno.h>

#define GST_CAT_DEFAULT gst_wl_display_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

typedef struct _GstWlDisplayPrivate
{
  /* public objects */
  struct wl_display *display;
  struct wl_display *display_wrapper;
  struct wl_event_queue *queue;

  /* globals */
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_subcompositor *subcompositor;
  struct xdg_wm_base *xdg_wm_base;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_touch *touch;
  struct zwp_fullscreen_shell_v1 *fullscreen_shell;
  struct wl_shm *shm;
  struct wp_viewporter *viewporter;
  struct zwp_linux_dmabuf_v1 *dmabuf;
  struct zwp_alpha_compositing_v1 *alpha_compositing;
  struct zwp_linux_explicit_synchronization_v1 *explicit_sync;
  struct zwp_hdr10_metadata_v1 *hdr10_metadata;
  GArray *shm_formats;
  GArray *dmabuf_formats;
  GArray *outputs;
  GHashTable *dmabuf_modifiers;

  /* real display resolution */
  gint width, height;

  /* preferred window resolution */
  gint preferred_width, preferred_height;

  /* private */
  gboolean own_display;
  GThread *thread;
  GstPoll *wl_fd_poll;

  GMutex buffers_mutex;
  GHashTable *buffers;
  gboolean shutting_down;
} GstWlDisplayPrivate;

G_DEFINE_TYPE_WITH_CODE (GstWlDisplay, gst_wl_display, G_TYPE_OBJECT,
    G_ADD_PRIVATE (GstWlDisplay)
    GST_DEBUG_CATEGORY_INIT (gst_wl_display_debug,
        "wldisplay", 0, "wldisplay library");
    );

static void gst_wl_display_finalize (GObject * gobject);

static void
gst_wl_display_class_init (GstWlDisplayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gst_wl_display_finalize;
}

static void
gst_wl_display_init (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  priv->shm_formats = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  priv->dmabuf_modifiers = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) g_array_unref);
  priv->dmabuf_formats = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  priv->outputs = g_array_new (FALSE, FALSE, sizeof (struct wl_output *));
  priv->wl_fd_poll = gst_poll_new (TRUE);
  priv->buffers = g_hash_table_new (g_direct_hash, g_direct_equal);
  priv->width = -1;
  priv->height = -1;
  priv->preferred_width = -1;
  priv->preferred_height = -1;
  g_mutex_init (&priv->buffers_mutex);

  gst_wl_linux_dmabuf_init_once ();
  gst_wl_shm_allocator_init_once ();
  gst_wl_videoformat_init_once ();
}

static void
gst_wl_ref_wl_buffer (gpointer key, gpointer value, gpointer user_data)
{
  g_object_ref (value);
}

static void
gst_wl_display_finalize (GObject * gobject)
{
  guint i;
  struct wl_output *output = NULL;
  GstWlDisplay *self = GST_WL_DISPLAY (gobject);
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  gst_poll_set_flushing (priv->wl_fd_poll, TRUE);
  if (priv->thread)
    g_thread_join (priv->thread);

  /* to avoid buffers being unregistered from another thread
   * at the same time, take their ownership */
  g_mutex_lock (&priv->buffers_mutex);
  priv->shutting_down = TRUE;
  g_hash_table_foreach (priv->buffers, gst_wl_ref_wl_buffer, NULL);
  g_mutex_unlock (&priv->buffers_mutex);

  g_hash_table_foreach (priv->buffers,
      (GHFunc) gst_wl_buffer_force_release_and_unref, NULL);
  g_hash_table_remove_all (priv->buffers);

  g_hash_table_remove_all (priv->dmabuf_modifiers);
  g_hash_table_unref (priv->dmabuf_modifiers);
  g_array_unref (priv->dmabuf_formats);

  g_array_unref (priv->shm_formats);
  gst_poll_free (priv->wl_fd_poll);
  g_hash_table_unref (priv->buffers);
  g_mutex_clear (&priv->buffers_mutex);

  if (priv->viewporter)
    wp_viewporter_destroy (priv->viewporter);

  if (priv->shm)
    wl_shm_destroy (priv->shm);

  if (priv->dmabuf)
    zwp_linux_dmabuf_v1_destroy (priv->dmabuf);

  if (priv->xdg_wm_base)
    xdg_wm_base_destroy (priv->xdg_wm_base);

  if (priv->seat)
    wl_seat_destroy (priv->seat);

  if (priv->pointer)
    wl_pointer_destroy (priv->pointer);

  if (priv->touch)
    wl_touch_destroy (priv->touch);

  if (priv->fullscreen_shell)
    zwp_fullscreen_shell_v1_release (priv->fullscreen_shell);

  if (priv->alpha_compositing)
    zwp_alpha_compositing_v1_destroy (priv->alpha_compositing);

  if (priv->compositor)
    wl_compositor_destroy (priv->compositor);

  if (priv->subcompositor)
    wl_subcompositor_destroy (priv->subcompositor);

  if (priv->explicit_sync)
    zwp_linux_explicit_synchronization_v1_destroy (priv->explicit_sync);

  if (priv->hdr10_metadata)
    zwp_hdr10_metadata_v1_destroy (priv->hdr10_metadata);

  if (priv->outputs) {
    for (i = 0; i < priv->outputs->len; i++) {
      output = g_array_index (priv->outputs, struct wl_output *, i);
      if (output)
        wl_output_destroy (output);
    }
    g_array_unref (priv->outputs);
  }

  if (priv->registry)
    wl_registry_destroy (priv->registry);

  if (priv->display_wrapper)
    wl_proxy_wrapper_destroy (priv->display_wrapper);

  if (priv->queue)
    wl_event_queue_destroy (priv->queue);

  if (priv->own_display) {
    wl_display_flush (priv->display);
    wl_display_disconnect (priv->display);
  }

  G_OBJECT_CLASS (gst_wl_display_parent_class)->finalize (gobject);
}

static void
shm_format (void *data, struct wl_shm *wl_shm, uint32_t format)
{
  GstWlDisplay *self = data;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  g_array_append_val (priv->shm_formats, format);
}

static const struct wl_shm_listener shm_listener = {
  shm_format
};

static void
dmabuf_format (void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
    uint32_t format)
{
  /* this event has been deprecated */
}

static void
dmabuf_modifier (void *data,
    struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
    uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
  GstWlDisplay *self = data;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  uint64_t modifier = ((uint64_t) modifier_hi << 32) | modifier_lo;

  if (gst_wl_dmabuf_format_to_video_format (format) != GST_VIDEO_FORMAT_UNKNOWN) {
    if (!g_hash_table_contains (priv->dmabuf_modifiers,
            GUINT_TO_POINTER (format))) {
      GArray *modifiers = g_array_new (FALSE, FALSE, sizeof (uint64_t));
      g_array_append_val (modifiers, modifier);
      g_hash_table_insert (priv->dmabuf_modifiers, GUINT_TO_POINTER (format),
          modifiers);

      g_array_append_val (priv->dmabuf_formats, format);
    } else {
      int i;
      GArray *modifiers = g_hash_table_lookup (priv->dmabuf_modifiers,
          GUINT_TO_POINTER (format));
      for (i = 0; i < modifiers->len; i++) {
        uint64_t mod = g_array_index (modifiers, uint64_t, i);
        if (mod == modifier)
          break;
      }
      if (i == modifiers->len)
        g_array_append_val (modifiers, modifier);
    }
  }
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
  dmabuf_format,
  dmabuf_modifier
};

gboolean
gst_wl_display_check_format_for_shm (GstWlDisplay * self, GstVideoFormat format)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  enum wl_shm_format shm_fmt;
  GArray *formats;
  guint i;

  shm_fmt = gst_video_format_to_wl_shm_format (format);
  if (shm_fmt == (enum wl_shm_format) -1)
    return FALSE;

  formats = priv->shm_formats;
  for (i = 0; i < formats->len; i++) {
    if (g_array_index (formats, uint32_t, i) == shm_fmt)
      return TRUE;
  }

  return FALSE;
}

gboolean
gst_wl_display_check_format_for_dmabuf (GstWlDisplay * self,
    GstVideoFormat format)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  GArray *formats;
  guint i, dmabuf_fmt;

  if (!priv->dmabuf)
    return FALSE;

  dmabuf_fmt = gst_video_format_to_wl_dmabuf_format (format);
  if (dmabuf_fmt == (guint) - 1)
    return FALSE;

  formats = priv->dmabuf_formats;
  for (i = 0; i < formats->len; i++) {
    if (g_array_index (formats, uint32_t, i) == dmabuf_fmt)
      return TRUE;
  }

  return FALSE;
}

static void
seat_handle_capabilities (void *data, struct wl_seat *seat,
    enum wl_seat_capability caps)
{
  GstWlDisplay *self = data;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !priv->pointer) {
    priv->pointer = wl_seat_get_pointer (seat);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && priv->pointer) {
    wl_pointer_destroy (priv->pointer);
    priv->pointer = NULL;
  }

  if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !priv->touch) {
    priv->touch = wl_seat_get_touch (seat);
  } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && priv->touch) {
    wl_touch_destroy (priv->touch);
    priv->touch = NULL;
  }
}

static const struct wl_seat_listener seat_listener = {
  seat_handle_capabilities,
};

static void
handle_xdg_wm_base_ping (void *user_data, struct xdg_wm_base *xdg_wm_base,
    uint32_t serial)
{
  xdg_wm_base_pong (xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
  handle_xdg_wm_base_ping
};

static void
output_handle_geometry (void *data, struct wl_output *wl_output,
    int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height,
    int32_t subpixel,
    const char *make, const char *model, int32_t output_transform)
{
  /* Nothing to do now */
}

static void
output_handle_mode (void *data, struct wl_output *wl_output,
    uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
  GstWlDisplay *self = data;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  /* we only care about the current mode */
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    if (priv->width == -1 && priv->height == -1) {
      priv->width = width;
      priv->height = height;
    }

  }
}

static void
output_handle_done (void *data, struct wl_output *wl_output)
{
  /* don't bother waiting for this; there's no good reason a
   * compositor will wait more than one roundtrip before sending
   * these initial events. */
}

static void
output_handle_scale (void *data, struct wl_output *wl_output, int32_t scale)
{
  /* Nothing to do now */
}

static const struct wl_output_listener output_listener = {
  output_handle_geometry,
  output_handle_mode,
  output_handle_done,
  output_handle_scale,
};

static void
registry_handle_global (void *data, struct wl_registry *registry,
    uint32_t id, const char *interface, uint32_t version)
{
  GstWlDisplay *self = data;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  if (g_strcmp0 (interface, "wl_compositor") == 0) {
    priv->compositor = wl_registry_bind (registry, id, &wl_compositor_interface,
        MIN (version, 4));
  } else if (g_strcmp0 (interface, "wl_subcompositor") == 0) {
    priv->subcompositor =
        wl_registry_bind (registry, id, &wl_subcompositor_interface, 1);
  } else if (g_strcmp0 (interface, "xdg_wm_base") == 0) {
    priv->xdg_wm_base =
        wl_registry_bind (registry, id, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener (priv->xdg_wm_base, &xdg_wm_base_listener, self);
  } else if (strcmp (interface, "wl_seat") == 0) {
    priv->seat = wl_registry_bind (registry, id, &wl_seat_interface, 1);
    wl_seat_add_listener (priv->seat, &seat_listener, self);
  } else if (g_strcmp0 (interface, "zwp_fullscreen_shell_v1") == 0) {
    priv->fullscreen_shell = wl_registry_bind (registry, id,
        &zwp_fullscreen_shell_v1_interface, 1);
  } else if (g_strcmp0 (interface, "wl_shm") == 0) {
    priv->shm = wl_registry_bind (registry, id, &wl_shm_interface, 1);
    wl_shm_add_listener (priv->shm, &shm_listener, self);
  } else if (g_strcmp0 (interface,
          "zwp_linux_explicit_synchronization_v1") == 0) {
    priv->explicit_sync =
        wl_registry_bind (registry, id,
        &zwp_linux_explicit_synchronization_v1_interface, 1);
  } else if (g_strcmp0 (interface, "wp_viewporter") == 0) {
    priv->viewporter =
        wl_registry_bind (registry, id, &wp_viewporter_interface, 1);
  } else if (g_strcmp0 (interface, "zwp_linux_dmabuf_v1") == 0) {
    priv->dmabuf =
        wl_registry_bind (registry, id, &zwp_linux_dmabuf_v1_interface,
        MIN (version, 3));
    zwp_linux_dmabuf_v1_add_listener (priv->dmabuf, &dmabuf_listener, self);
  } else if (g_strcmp0 (interface, "zwp_alpha_compositing_v1") == 0) {
    priv->alpha_compositing =
        wl_registry_bind (registry, id, &zwp_alpha_compositing_v1_interface, 1);
  } else if (g_strcmp0 (interface, "zwp_hdr10_metadata_v1") == 0) {
    priv->hdr10_metadata =
        wl_registry_bind (registry, id, &zwp_hdr10_metadata_v1_interface, 1);
  } else if (g_strcmp0 (interface, "wl_output") == 0) {
    struct wl_output *output;
    output =
	wl_registry_bind (registry, id, &wl_output_interface, MIN (version, 2));
    wl_output_add_listener (output, &output_listener, self);
    g_array_append_val (priv->outputs, output);
  }
}

static void
registry_handle_global_remove (void *data, struct wl_registry *registry,
    uint32_t name)
{
  /* temporarily do nothing */
}

static const struct wl_registry_listener registry_listener = {
  registry_handle_global,
  registry_handle_global_remove
};

static gpointer
gst_wl_display_thread_run (gpointer data)
{
  GstWlDisplay *self = data;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  GstPollFD pollfd = GST_POLL_FD_INIT;
  gint err;
  gboolean normal = FALSE;

  pollfd.fd = wl_display_get_fd (priv->display);
  gst_poll_add_fd (priv->wl_fd_poll, &pollfd);
  gst_poll_fd_ctl_read (priv->wl_fd_poll, &pollfd, TRUE);

  /* main loop */
  while (1) {
    while (wl_display_prepare_read_queue (priv->display, priv->queue) != 0)
      wl_display_dispatch_queue_pending (priv->display, priv->queue);
    wl_display_flush (priv->display);

    if (gst_poll_wait (priv->wl_fd_poll, GST_CLOCK_TIME_NONE) < 0) {
      err = errno;
      normal = (err == EBUSY);
      wl_display_cancel_read (priv->display);
      if (normal)
        break;
      else if (err == EINTR)
        continue;
      else
        goto error;
    }
    if (wl_display_read_events (priv->display) == -1)
      goto error;
    wl_display_dispatch_queue_pending (priv->display, priv->queue);
  }

  return NULL;

error:
  GST_ERROR ("Error communicating with the wayland server");
  return NULL;
}

GstWlDisplay *
gst_wl_display_new (const gchar * name, GError ** error)
{
  struct wl_display *display;

  display = wl_display_connect (name);

  if (!display) {
    *error = g_error_new (g_quark_from_static_string ("GstWlDisplay"), 0,
        "Failed to connect to the wayland display '%s'",
        name ? name : "(default)");
    return NULL;
  } else {
    return gst_wl_display_new_existing (display, TRUE, error);
  }
}

GstWlDisplay *
gst_wl_display_new_existing (struct wl_display * display,
    gboolean take_ownership, GError ** error)
{
  GstWlDisplay *self;
  GstWlDisplayPrivate *priv;
  GError *err = NULL;
  gint i;

  g_return_val_if_fail (display != NULL, NULL);

  self = g_object_new (GST_TYPE_WL_DISPLAY, NULL);
  priv = gst_wl_display_get_instance_private (self);
  priv->display = display;
  priv->display_wrapper = wl_proxy_create_wrapper (display);
  priv->own_display = take_ownership;

  priv->queue = wl_display_create_queue (priv->display);
  wl_proxy_set_queue ((struct wl_proxy *) priv->display_wrapper, priv->queue);
  priv->registry = wl_display_get_registry (priv->display_wrapper);
  wl_registry_add_listener (priv->registry, &registry_listener, self);

  /* we need exactly 2 roundtrips to discover global objects and their state */
  for (i = 0; i < 2; i++) {
    if (wl_display_roundtrip_queue (priv->display, priv->queue) < 0) {
      *error = g_error_new (g_quark_from_static_string ("GstWlDisplay"), 0,
          "Error communicating with the wayland display");
      g_object_unref (self);
      return NULL;
    }
  }

  /* verify we got all the required interfaces */
#define VERIFY_INTERFACE_EXISTS(var, interface) \
  if (!priv->var) { \
    g_set_error (error, g_quark_from_static_string ("GstWlDisplay"), 0, \
        "Could not bind to " interface ". Either it is not implemented in " \
        "the compositor, or the implemented version doesn't match"); \
    g_object_unref (self); \
    return NULL; \
  }

  VERIFY_INTERFACE_EXISTS (compositor, "wl_compositor");
  VERIFY_INTERFACE_EXISTS (subcompositor, "wl_subcompositor");
  VERIFY_INTERFACE_EXISTS (shm, "wl_shm");

#undef VERIFY_INTERFACE_EXISTS

  /* We make the viewporter optional even though it may cause bad display.
   * This is so one can test wayland display on older compositor or on
   * compositor that don't implement this extension. */
  if (!priv->viewporter) {
    g_warning ("Wayland compositor is missing the ability to scale, video "
        "display may not work properly.");
  }

  if (!priv->dmabuf) {
    g_warning ("Could not bind to zwp_linux_dmabuf_v1");
  }

  if (!priv->xdg_wm_base && !priv->fullscreen_shell) {
    /* If wl_surface and wl_display are passed via GstContext
     * xdg_shell and zwp_fullscreen_shell are not used.
     * In this case is correct to continue.
     */
    g_warning ("Could not bind to either xdg_wm_base or zwp_fullscreen_shell, "
        "video display may not work properly.");
  }

  priv->thread = g_thread_try_new ("GstWlDisplay", gst_wl_display_thread_run,
      self, &err);
  if (err) {
    g_propagate_prefixed_error (error, err,
        "Failed to start thread for the display's events");
    g_object_unref (self);
    return NULL;
  }

  return self;
}

void
gst_wl_display_register_buffer (GstWlDisplay * self, gpointer gstmem,
    gpointer wlbuffer)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  g_assert (!priv->shutting_down);

  GST_TRACE_OBJECT (self, "registering GstWlBuffer %p to GstMem %p",
      wlbuffer, gstmem);

  g_mutex_lock (&priv->buffers_mutex);
  g_hash_table_replace (priv->buffers, gstmem, wlbuffer);
  g_mutex_unlock (&priv->buffers_mutex);
}

gpointer
gst_wl_display_lookup_buffer (GstWlDisplay * self, gpointer gstmem)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  gpointer wlbuffer;

  g_mutex_lock (&priv->buffers_mutex);
  wlbuffer = g_hash_table_lookup (priv->buffers, gstmem);
  g_mutex_unlock (&priv->buffers_mutex);
  return wlbuffer;
}

void
gst_wl_display_unregister_buffer (GstWlDisplay * self, gpointer gstmem)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  GST_TRACE_OBJECT (self, "unregistering GstWlBuffer owned by %p", gstmem);

  g_mutex_lock (&priv->buffers_mutex);
  if (G_LIKELY (!priv->shutting_down))
    g_hash_table_remove (priv->buffers, gstmem);
  g_mutex_unlock (&priv->buffers_mutex);
}

struct wl_display *
gst_wl_display_get_display (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->display;
}

struct wl_event_queue *
gst_wl_display_get_event_queue (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->queue;
}

struct wl_compositor *
gst_wl_display_get_compositor (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->compositor;
}

struct wl_subcompositor *
gst_wl_display_get_subcompositor (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->subcompositor;
}

struct xdg_wm_base *
gst_wl_display_get_xdg_wm_base (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->xdg_wm_base;
}

struct wl_seat *gst_wl_display_get_seat (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->seat;
}

struct zwp_fullscreen_shell_v1 *
gst_wl_display_get_fullscreen_shell_v1 (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->fullscreen_shell;
}

struct wp_viewporter *
gst_wl_display_get_viewporter (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->viewporter;
}

struct zwp_alpha_compositing_v1 *
gst_wl_display_get_alpha_compositing (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->alpha_compositing;
}

struct zwp_linux_explicit_synchronization_v1 *
gst_wl_display_get_explicit_sync (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->explicit_sync;
}

struct wl_pointer *
gst_wl_display_get_pointer (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->pointer;
}

struct wl_touch *
gst_wl_display_get_touch (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->touch;
}

struct zwp_hdr10_metadata_v1 *
gst_wl_display_get_hdr10_metadata (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->hdr10_metadata;
}

gint
gst_wl_display_get_width (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->width;
}

gint
gst_wl_display_get_height (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->height;
}

void
gst_wl_display_set_preferred_width (GstWlDisplay * self, gint preferred_width)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  priv->preferred_width = preferred_width;
}

void
gst_wl_display_set_preferred_height (GstWlDisplay * self, gint preferred_height)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  priv->preferred_height = preferred_height;
}

gint
gst_wl_display_get_preferred_width (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->preferred_width;
}

gint
gst_wl_display_get_preferred_height (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->preferred_height;
}

struct wl_shm *
gst_wl_display_get_shm (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->shm;
}

GArray *
gst_wl_display_get_shm_formats (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->shm_formats;
}

struct zwp_linux_dmabuf_v1 *
gst_wl_display_get_dmabuf_v1 (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->dmabuf;
}

GArray *
gst_wl_display_get_dmabuf_formats (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->dmabuf_formats;
}

gboolean
gst_wl_display_has_own_display (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->own_display;
}
