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
/**
 * SECTION:element-autoselect
 * @title: autoselect
 *
 * The #autoselect element has one sink and one source pad. It will look for
 * other elements that also have one sink and one source pad.
 *
 * The #autoselect make single or multiple elements to perform conversion
 * and add only the selected element. If the caps change, it may change the
 * selected element and remove thecurrent one from autoselect if the current
 * one no longer matches the caps.
 *
 * Firstly, it will pick an element that matches the caps on both sides whose
 * rank is greater than the composite rank level.
 *
 * Secondly, if none of the those elements match the caps whose rank is higher
 * than composite rank, it will then try to pick and combine two elements (first
 * element and last element) from the previous elements which have been tried
 * before according to the rank and check whether they match and add them.
 *
 * Lastly, if none of the composite elements match the caps, it will try to
 * pick the remain elements whose rank are equal to or less than composite rank.
 *
 * If single element can perform conversion, this single element is the first
 * element in the diagram and there is no last element. The first element src pad
 * connects #internal_sinkpad directly.
 * +---------------------------------------------------------------------------+
 * | autoselect                                                                |
 * |                   +---------------+   +---------------+                   |
 * |                   | first element |   | last element  |                   |
 * | internal_srcpad-sink             src-sink            src-internal_sinkpad |
 * |                   +---------------+   +---------------+                   |
 * sink-+                                                                   +-src
 * +---------------------------------------------------------------------------+
 *
 * The list of element it will look into can be specified in the
 * #GstAutoSelect:factories property, otherwise it will look at all available
 * elements.
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstautoselect.h"

#include <string.h>
#define DEFAULT_COMPOSITE_RANK (GST_RANK_PRIMARY)
#define DEFAULT_LOWEST_SELECT_RANK 0

GST_DEBUG_CATEGORY (autoselect_debug);
#define GST_CAT_DEFAULT (autoselect_debug)

#define GST_AUTOSELECT_LOCK(ac) GST_OBJECT_LOCK (ac)
#define GST_AUTOSELECT_UNLOCK(ac) GST_OBJECT_UNLOCK (ac)

/* elementfactory information */
static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate sink_internal_template =
GST_STATIC_PAD_TEMPLATE ("sink_internal",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate src_internal_template =
GST_STATIC_PAD_TEMPLATE ("src_internal",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

/* GstAutoSelect signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_FACTORIES,
  PROP_COMPOSITE_RANK,
  PROP_SELECT_ELEMENT,
  PROP_LOWEST_RANK
};

static void gst_auto_select_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_auto_select_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_auto_select_dispose (GObject * object);

static GstElement *gst_auto_select_get_subelement (GstAutoSelect *
    autoselect, GstPadDirection dir);
static GstPad *gst_auto_select_get_internal_sinkpad (GstAutoSelect *
    autoselect);
static GstPad *gst_auto_select_get_internal_srcpad (GstAutoSelect * autoselect);

static GstIterator *gst_auto_select_iterate_internal_links (GstPad * pad,
    GstObject * parent);
static gboolean
gst_auto_select_check_caps_negotiation (GstAutoSelect * autoselect,
    GstCaps * caps, GstEvent * event);
static gboolean gst_auto_select_sink_setcaps (GstAutoSelect * autoselect,
    GstCaps * caps, GstEvent * event);
static GstCaps *gst_auto_select_getcaps (GstAutoSelect * autoselect,
    GstCaps * filter, GstPadDirection dir);
static GstFlowReturn gst_auto_select_sink_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buffer);
static GstFlowReturn gst_auto_select_sink_chain_list (GstPad * pad,
    GstObject * parent, GstBufferList * list);
static gboolean gst_auto_select_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_auto_select_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query);

static gboolean gst_auto_select_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_auto_select_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query);

static GstFlowReturn gst_auto_select_internal_sink_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buffer);
static GstFlowReturn gst_auto_select_internal_sink_chain_list (GstPad * pad,
    GstObject * parent, GstBufferList * list);
static gboolean gst_auto_select_internal_sink_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static gboolean gst_auto_select_internal_sink_query (GstPad * pad,
    GstObject * parent, GstQuery * query);

static gboolean gst_auto_select_internal_src_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static gboolean gst_auto_select_internal_src_query (GstPad * pad,
    GstObject * parent, GstQuery * query);

static void gst_auto_select_load_factories (GstAutoSelect * autoselect);

static GList *gst_auto_select_copy_factories_list (GstAutoSelect * autoselect);
static gboolean
gst_auto_select_update_composite_factories_list (GstAutoSelect * autoselect,
    GList ** factories_list, GstElementFactory * factory);
static gboolean
gst_auto_select_construct_composite_elements (GstAutoSelect * autoselect,
    GList * first_factories, GList * last_factories,
    GstElementFactory * current_factory);
static gboolean gst_auto_select_check_current_factory (GstAutoSelect *
    autoselect, GstElementFactory * factory);
static gboolean gst_auto_select_construct_single_element (GstAutoSelect *
    autoselect, GstElementFactory * current_factory);
static gboolean gst_auto_select_add_element (GstAutoSelect * autoselect,
    GList * elem_list);
static gboolean gst_auto_select_activate_element (GstAutoSelect * autoselect,
    GList * elem_list);

static GQuark internal_srcpad_quark = 0;
static GQuark internal_sinkpad_quark = 0;
static GQuark parent_quark = 0;

G_DEFINE_TYPE (GstAutoSelect, gst_auto_select, GST_TYPE_BIN);
GST_ELEMENT_REGISTER_DEFINE (autoselect, "autoselect",
    GST_RANK_NONE, GST_TYPE_AUTO_SELECT);

static void
gst_auto_select_class_init (GstAutoSelectClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;

  GST_DEBUG_CATEGORY_INIT (autoselect_debug, "autoselect", 0,
      "Auto select based on caps");

  internal_srcpad_quark = g_quark_from_static_string ("internal_srcpad");
  internal_sinkpad_quark = g_quark_from_static_string ("internal_sinkpad");
  parent_quark = g_quark_from_static_string ("parent");


  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  gst_element_class_set_static_metadata (gstelement_class,
      "Auto select element(s) based on caps", "Generic/Bin",
      "Selects the right element(s) based on the caps",
      "Elliot Chen <elliot.chen@nxp.com>");

  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_auto_select_dispose);

  gobject_class->set_property = gst_auto_select_set_property;
  gobject_class->get_property = gst_auto_select_get_property;
  klass->update_composite_factories_list =
      GST_DEBUG_FUNCPTR (gst_auto_select_update_composite_factories_list);
  klass->construct_composite_elements =
      GST_DEBUG_FUNCPTR (gst_auto_select_construct_composite_elements);
  klass->check_current_factory =
      GST_DEBUG_FUNCPTR (gst_auto_select_check_current_factory);
  klass->construct_single_element =
      GST_DEBUG_FUNCPTR (gst_auto_select_construct_single_element);
  klass->add_element = GST_DEBUG_FUNCPTR (gst_auto_select_add_element);
  klass->activate_element =
      GST_DEBUG_FUNCPTR (gst_auto_select_activate_element);

  g_object_class_install_property (gobject_class, PROP_FACTORIES,
      g_param_spec_pointer ("factories",
          "GList of GstElementFactory",
          "GList of GstElementFactory objects to pick from (the element takes"
          " ownership of the list (NULL means it will go through all possible"
          " elements), can only be set once",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_COMPOSITE_RANK,
      g_param_spec_int ("composite-rank", "composite rank",
          "combine multiple elements if none of the single elements"
          " match the caps whose rank is above the composite rank",
          0, G_MAXINT, DEFAULT_COMPOSITE_RANK,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SELECT_ELEMENT,
      g_param_spec_pointer ("select-element-list",
          "GList of the current selected element(s)",
          "GList of the current selected element(s)",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_LOWEST_RANK,
      g_param_spec_int ("lowest-rank", "lowest rank",
          "the factory can be selected in GList of GstElementFactory "
          " whose rank need be equal to or greater than the lowest rank",
          0, G_MAXINT, DEFAULT_LOWEST_SELECT_RANK,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_auto_select_init (GstAutoSelect * autoselect)
{
  autoselect->sinkpad =
      gst_pad_new_from_static_template (&sinktemplate, "sink");
  autoselect->srcpad = gst_pad_new_from_static_template (&srctemplate, "src");

  gst_pad_set_chain_function (autoselect->sinkpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_sink_chain));
  gst_pad_set_chain_list_function (autoselect->sinkpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_sink_chain_list));
  gst_pad_set_event_function (autoselect->sinkpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_sink_event));
  gst_pad_set_query_function (autoselect->sinkpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_sink_query));
  gst_pad_set_iterate_internal_links_function (autoselect->sinkpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_iterate_internal_links));

  gst_pad_set_event_function (autoselect->srcpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_src_event));
  gst_pad_set_query_function (autoselect->srcpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_src_query));
  gst_pad_set_iterate_internal_links_function (autoselect->srcpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_iterate_internal_links));

  gst_element_add_pad (GST_ELEMENT (autoselect), autoselect->sinkpad);
  gst_element_add_pad (GST_ELEMENT (autoselect), autoselect->srcpad);

  autoselect->composite_rank = DEFAULT_COMPOSITE_RANK;
  autoselect->lowest_rank = DEFAULT_LOWEST_SELECT_RANK;
  autoselect->is_composite = FALSE;
}

static void
gst_auto_select_dispose (GObject * object)
{
  GstAutoSelect *autoselect = GST_AUTO_SELECT (object);

  if (autoselect->first_subelement)
    g_clear_object (&autoselect->first_subelement);

  if (autoselect->last_subelement)
    g_clear_object (&autoselect->last_subelement);

  g_clear_object (&autoselect->current_internal_sinkpad);
  g_clear_object (&autoselect->current_internal_srcpad);
  g_list_free (autoselect->elem_list);

  for (;;) {
    GList *factories = g_atomic_pointer_get (&autoselect->factories);

    if (g_atomic_pointer_compare_and_exchange (&autoselect->factories,
            factories, NULL)) {
      gst_plugin_feature_list_free (factories);
      break;
    }
  }

  G_OBJECT_CLASS (gst_auto_select_parent_class)->dispose (object);
}

static void
gst_auto_select_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstAutoSelect *autoselect = GST_AUTO_SELECT (object);
  GstState cur_state;

  gst_element_get_state (GST_ELEMENT (autoselect), &cur_state, NULL, 0);
  if (cur_state == GST_STATE_NULL) {
    switch (prop_id) {
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
      case PROP_FACTORIES:
        GList * old_factories = g_atomic_pointer_get (&autoselect->factories);
        GList *new_factories = g_value_get_pointer (value);

        new_factories = g_list_copy (new_factories);
        if (g_atomic_pointer_compare_and_exchange (&autoselect->factories,
                old_factories, new_factories)) {
          if (old_factories) {
            GST_AUTOSELECT_LOCK (autoselect);
            g_list_free (old_factories);
            GST_AUTOSELECT_UNLOCK (autoselect);
          }
        } else {
          g_list_free (new_factories);
        }
        break;
      case PROP_COMPOSITE_RANK:
        autoselect->composite_rank = g_value_get_int (value);
        GST_DEBUG_OBJECT (object, "composite rank set to %d",
            autoselect->composite_rank);
        break;
      case PROP_LOWEST_RANK:
        autoselect->lowest_rank = g_value_get_int (value);
        GST_DEBUG_OBJECT (object, "lowest select rank set to %d",
            autoselect->lowest_rank);
        break;
    }
  } else {
    GST_WARNING_OBJECT (object, "Can not set property because the element"
        "is not in the NULL state or initial state");
  }
}

static void
gst_auto_select_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstAutoSelect *autoselect = GST_AUTO_SELECT (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    case PROP_FACTORIES:
      g_value_set_pointer (value,
          g_atomic_pointer_get (&autoselect->factories));
      break;
    case PROP_COMPOSITE_RANK:
      g_value_set_int (value, autoselect->composite_rank);
      break;
    case PROP_SELECT_ELEMENT:
      g_value_set_pointer (value,
          g_atomic_pointer_get (&autoselect->elem_list));
      break;
    case PROP_LOWEST_RANK:
      g_value_set_int (value, autoselect->lowest_rank);
      break;
  }
}


static GstElement *
gst_auto_select_get_element_by_type (GstAutoSelect * autoselect, GType type)
{
  GList *item;
  GstBin *bin = GST_BIN (autoselect);
  GstElement *element = NULL;

  g_return_val_if_fail (type != 0, NULL);

  GST_OBJECT_LOCK (autoselect);

  for (item = bin->children; item; item = item->next) {
    if (G_TYPE_CHECK_INSTANCE_TYPE (item->data, type)) {
      element = gst_object_ref (item->data);
      break;
    }
  }

  GST_OBJECT_UNLOCK (autoselect);

  return element;
}

/**
 * get_pad_by_direction:
 * @element: The Element
 * @direction: The direction
 *
 * Gets a #GstPad that goes in the requested direction. I will return NULL
 * if there is no pad or if there is more than one pad in this direction
 */

static GstPad *
get_pad_by_direction (GstElement * element, GstPadDirection direction)
{
  GstIterator *iter = gst_element_iterate_pads (element);
  GstPad *selected_pad = NULL;
  gboolean done;
  GValue item = { 0, };

  if (!iter)
    return NULL;

  done = FALSE;
  while (!done) {
    switch (gst_iterator_next (iter, &item)) {
      case GST_ITERATOR_OK:
      {
        GstPad *pad = g_value_get_object (&item);

        if (gst_pad_get_direction (pad) == direction) {
          /* We check if there is more than one pad in this direction,
           * if there is, we return NULL so that the element is refused
           */
          if (selected_pad) {
            done = TRUE;
            gst_object_unref (selected_pad);
            selected_pad = NULL;
          } else {
            selected_pad = g_object_ref (pad);
          }
        }
        g_value_unset (&item);
      }
        break;
      case GST_ITERATOR_RESYNC:
        if (selected_pad) {
          gst_object_unref (selected_pad);
          selected_pad = NULL;
        }
        gst_iterator_resync (iter);
        break;
      case GST_ITERATOR_ERROR:
        GST_ERROR ("Error iterating pads of element %s",
            GST_OBJECT_NAME (element));
        gst_object_unref (selected_pad);
        selected_pad = NULL;
        done = TRUE;
        break;
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (iter);

  if (!selected_pad)
    GST_ERROR ("Did not find pad of direction %d in %s",
        direction, GST_OBJECT_NAME (element));

  return selected_pad;
}

static GstElement *
gst_auto_select_get_subelement (GstAutoSelect * autoselect, GstPadDirection dir)
{
  GstElement *element = NULL;

  GST_AUTOSELECT_LOCK (autoselect);
  if (autoselect->is_composite) {
    if (dir == GST_PAD_SINK) {
      if (autoselect->first_subelement)
        element = gst_object_ref (autoselect->first_subelement);
    } else {
      if (autoselect->last_subelement)
        element = gst_object_ref (autoselect->last_subelement);
    }
  } else {
    if (autoselect->first_subelement)
      element = gst_object_ref (autoselect->first_subelement);
  }
  GST_AUTOSELECT_UNLOCK (autoselect);

  return element;
}

static GstPad *
gst_auto_select_get_internal_sinkpad (GstAutoSelect * autoselect)
{
  GstPad *pad = NULL;

  GST_AUTOSELECT_LOCK (autoselect);
  if (autoselect->current_internal_sinkpad)
    pad = gst_object_ref (autoselect->current_internal_sinkpad);
  GST_AUTOSELECT_UNLOCK (autoselect);

  return pad;
}

static GstPad *
gst_auto_select_get_internal_srcpad (GstAutoSelect * autoselect)
{
  GstPad *pad = NULL;

  GST_AUTOSELECT_LOCK (autoselect);
  if (autoselect->current_internal_srcpad)
    pad = gst_object_ref (autoselect->current_internal_srcpad);
  GST_AUTOSELECT_UNLOCK (autoselect);

  return pad;
}

static void
gst_auto_select_remove_elements (GstBin * bin)
{
  GstIterator *iter = NULL;
  GValue value = { 0 };
  GstElement *elem = NULL;
  gboolean done = FALSE;

  iter = gst_bin_iterate_elements (bin);
  while (!done) {
    switch (gst_iterator_next (iter, &value)) {
      case GST_ITERATOR_OK:
        elem = (GstElement *) g_value_get_object (&value);
        GST_DEBUG_OBJECT (bin, "remove element %s from bin",
            GST_OBJECT_NAME (elem));
        gst_bin_remove (bin, elem);
        gst_element_set_state (GST_ELEMENT (elem), GST_STATE_NULL);
        /* Iterator increased the element refcount, so unref */
        g_value_unset (&value);
        break;
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (iter);
        break;
      case GST_ITERATOR_ERROR:
        GST_WARNING_OBJECT (bin, "error in iterating elements");
        done = TRUE;
        break;
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }
  gst_iterator_free (iter);
}

static gboolean
gst_auto_select_add_element (GstAutoSelect * autoselect, GList * elem_list)
{
  GstElement *current_elem = NULL;
  GstElement *first_elem = NULL;
  GstElement *last_elem = NULL;
  GstPad *internal_sinkpad = NULL;
  GstPad *internal_srcpad = NULL;
  GPtrArray *pad_array = NULL;
  GstPadLinkReturn padlinkret;
  GList *select_list = NULL;

  if (!elem_list) {
    GST_DEBUG_OBJECT (autoselect, "No valid element list");
    return FALSE;
  }

  /* Add the element to bin and get the pad */
  GST_DEBUG_OBJECT (autoselect, "Start trying to add element");
  for (select_list = elem_list; select_list;
      select_list = g_list_next (select_list)) {
    GstPad *sink_pad, *src_pad;
    current_elem = GST_ELEMENT (select_list->data);

    if (!gst_bin_add (GST_BIN (autoselect), current_elem)) {
      GST_WARNING_OBJECT (autoselect, "Could not add element %s to the bin",
          GST_OBJECT_NAME (current_elem));
      gst_object_unref (current_elem);
      goto add_element_failed;
    }
    GST_DEBUG_OBJECT (autoselect, "Trying to add element %s",
        GST_OBJECT_NAME (current_elem));

    /* Check and get pad from the selected element */
    sink_pad = get_pad_by_direction (current_elem, GST_PAD_SINK);
    src_pad = get_pad_by_direction (current_elem, GST_PAD_SRC);
    if (!sink_pad || !src_pad) {
      GST_WARNING_OBJECT (autoselect,
          "Could not find matched sink or src pad in %s",
          GST_OBJECT_NAME (current_elem));
      if (sink_pad)
        gst_object_unref (sink_pad);
      if (src_pad)
        gst_object_unref (src_pad);
      goto link_pad_failed;
    }

    if (!pad_array) {
      pad_array =
          g_ptr_array_new_with_free_func ((GDestroyNotify) gst_object_unref);
    }
    g_ptr_array_add (pad_array, sink_pad);
    g_ptr_array_add (pad_array, src_pad);

    if (first_elem == NULL) {
      first_elem = current_elem;
    }
    last_elem = current_elem;
  }

  /* Create internal pad */
  internal_sinkpad =
      gst_pad_new_from_static_template (&sink_internal_template,
      "sink_internal");
  internal_srcpad =
      gst_pad_new_from_static_template (&src_internal_template, "src_internal");

  if (!internal_sinkpad || !internal_srcpad) {
    GST_ERROR_OBJECT (autoselect, "Could not create internal pads");
    if (internal_sinkpad)
      gst_object_unref (internal_sinkpad);
    if (internal_srcpad)
      gst_object_unref (internal_srcpad);
    goto link_pad_failed;
  }

  g_object_weak_ref (G_OBJECT (first_elem), (GWeakNotify) gst_object_unref,
      internal_srcpad);
  g_object_weak_ref (G_OBJECT (last_elem), (GWeakNotify) gst_object_unref,
      internal_sinkpad);

  gst_pad_set_active (internal_sinkpad, TRUE);
  gst_pad_set_active (internal_srcpad, TRUE);

  g_object_set_qdata (G_OBJECT (internal_srcpad), parent_quark, autoselect);
  g_object_set_qdata (G_OBJECT (internal_sinkpad), parent_quark, autoselect);

  gst_pad_set_chain_function (internal_sinkpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_internal_sink_chain));
  gst_pad_set_chain_list_function (internal_sinkpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_internal_sink_chain_list));
  gst_pad_set_event_function (internal_sinkpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_internal_sink_event));
  gst_pad_set_query_function (internal_sinkpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_internal_sink_query));

  gst_pad_set_event_function (internal_srcpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_internal_src_event));
  gst_pad_set_query_function (internal_srcpad,
      GST_DEBUG_FUNCPTR (gst_auto_select_internal_src_query));

  /* Try to link the pad */
  GstPad *first_elem_sink_pad = g_ptr_array_index (pad_array, 0);
  padlinkret = gst_pad_link_full (internal_srcpad, first_elem_sink_pad,
      GST_PAD_LINK_CHECK_NOTHING);
  if (GST_PAD_LINK_FAILED (padlinkret)) {
    GST_WARNING_OBJECT (autoselect, "Could not links pad %s:%s to %s:%s"
        " for reason %d",
        GST_DEBUG_PAD_NAME (internal_srcpad),
        GST_DEBUG_PAD_NAME (first_elem_sink_pad), padlinkret);
    goto link_pad_failed;
  }

  GstPad *last_elem_src_pad = g_ptr_array_index (pad_array, pad_array->len - 1);
  padlinkret = gst_pad_link_full (last_elem_src_pad, internal_sinkpad,
      GST_PAD_LINK_CHECK_NOTHING);
  if (GST_PAD_LINK_FAILED (padlinkret)) {
    GST_WARNING_OBJECT (autoselect, "Could not links pad %s:%s to %s:%s"
        " for reason %d",
        GST_DEBUG_PAD_NAME (last_elem_src_pad),
        GST_DEBUG_PAD_NAME (internal_sinkpad), padlinkret);
    goto link_pad_failed;
  }

  /* Connect all the elements */
  if (first_elem != last_elem) {
    for (guint i = 1; i < (pad_array->len - 1); i = i + 2) {
      GstPad *src_pad = g_ptr_array_index (pad_array, i);
      GstPad *sink_pad = g_ptr_array_index (pad_array, i + 1);
      padlinkret = gst_pad_link (src_pad, sink_pad);
      if (GST_PAD_LINK_FAILED (padlinkret)) {
        GST_WARNING_OBJECT (autoselect, "Could not links pad %s:%s to %s:%s"
            " for reason %d",
            GST_DEBUG_PAD_NAME (src_pad),
            GST_DEBUG_PAD_NAME (sink_pad), padlinkret);
        goto link_pad_failed;
      }
    }
  }
  g_ptr_array_free (pad_array, TRUE);

  /* Record the internal pad */
  g_object_set_qdata (G_OBJECT (first_elem),
      internal_srcpad_quark, internal_srcpad);
  g_object_set_qdata (G_OBJECT (last_elem),
      internal_sinkpad_quark, internal_sinkpad);

  for (select_list = elem_list; select_list;
      select_list = g_list_next (select_list)) {
    current_elem = GST_ELEMENT (select_list->data);
    gst_element_sync_state_with_parent (current_elem);
  }

  return TRUE;

link_pad_failed:
  g_ptr_array_free (pad_array, TRUE);
add_element_failed:
  g_list_free (elem_list);
  gst_auto_select_remove_elements (GST_BIN (autoselect));
  return FALSE;
}

static gboolean
gst_auto_select_make_composite_elements (GstAutoSelect * autoselect,
    GList * first_factories, GList * last_factories)
{
  GList *first_list;
  GList *last_list;
  GList *elem_list = NULL;
  GstElement *first_elem = NULL;
  GstElement *last_elem = NULL;
  GstElementFactory *first_loaded_factory = NULL;
  GstElementFactory *last_loaded_factory = NULL;
  gboolean ret = FALSE;
  GstAutoSelectClass *klass = GST_AUTO_SELECT_GET_CLASS (autoselect);

  if (!first_factories || !last_factories) {
    GST_DEBUG_OBJECT (autoselect,
        "No valid factories to make composite element");
    return FALSE;
  }

  for (first_list = first_factories; first_list;
      first_list = g_list_next (first_list)) {
    GstElementFactory *first_factory = GST_ELEMENT_FACTORY (first_list->data);
    first_loaded_factory =
        GST_ELEMENT_FACTORY (gst_plugin_feature_load (GST_PLUGIN_FEATURE
            (first_factory)));
    if (!first_loaded_factory)
      continue;

    for (last_list = last_factories; last_list;
        last_list = g_list_next (last_list)) {
      GstElementFactory *last_factory = GST_ELEMENT_FACTORY (last_list->data);
      last_loaded_factory =
          GST_ELEMENT_FACTORY (gst_plugin_feature_load (GST_PLUGIN_FEATURE
              (last_factory)));

      if (!last_loaded_factory || first_loaded_factory == last_loaded_factory) {
        /* If the first and last factory are the same, need ignore it */
        continue;
      }

      /* Create first element */
      first_elem = gst_auto_select_get_element_by_type (autoselect,
          gst_element_factory_get_element_type (first_loaded_factory));
      if (first_elem) {
        GST_WARNING_OBJECT (autoselect,
            "first element %s has been added to the bin",
            GST_OBJECT_NAME (first_elem));
      } else {

        first_elem = gst_element_factory_create (first_loaded_factory, NULL);
        if (!first_elem) {
          GST_WARNING_OBJECT (autoselect, "Failed to create first element");
          gst_object_unref (first_loaded_factory);
          continue;
        }
      }

      /* Create last element */
      last_elem = gst_auto_select_get_element_by_type (autoselect,
          gst_element_factory_get_element_type (last_loaded_factory));

      if (last_elem) {
        GST_WARNING_OBJECT (autoselect,
            "last element %s has been added to the bin",
            GST_OBJECT_NAME (last_elem));
      } else {
        last_elem = gst_element_factory_create (last_loaded_factory, NULL);
        if (!last_elem) {
          GST_WARNING_OBJECT (autoselect, "Failed to create last element");
          gst_object_unref (last_loaded_factory);
          continue;
        }
      }
      gst_object_unref (last_loaded_factory);

      /* add the elements */
      elem_list = NULL;
      elem_list = g_list_append (elem_list, first_elem);
      elem_list = g_list_append (elem_list, last_elem);
      ret = klass->add_element (autoselect, elem_list);
      if (ret) {
        ret = klass->activate_element (autoselect, elem_list);
        if (ret)
          break;
      }
    }

    gst_object_unref (first_loaded_factory);
    if (ret)
      break;
  }
  return ret;
}

static gboolean
gst_auto_select_construct_single_element (GstAutoSelect * autoselect,
    GstElementFactory * factory)
{
  GstElement *element = NULL;
  GList *elem_list = NULL;
  GstElementFactory *loaded_factory =
      GST_ELEMENT_FACTORY (gst_plugin_feature_load (GST_PLUGIN_FEATURE
          (factory)));
  GstAutoSelectClass *klass = GST_AUTO_SELECT_GET_CLASS (autoselect);

  if (!loaded_factory)
    return FALSE;

  element = gst_auto_select_get_element_by_type (autoselect,
      gst_element_factory_get_element_type (loaded_factory));
  gst_object_unref (loaded_factory);

  if (element) {
    GST_WARNING_OBJECT (autoselect, "element %s has been added to the bin",
        GST_OBJECT_NAME (element));
  } else {
    element = gst_element_factory_create (factory, NULL);
  }

  if (!element)
    return FALSE;

  elem_list = g_list_append (elem_list, element);
  if (!klass->add_element (autoselect, elem_list)) {
    return FALSE;
  }

  return klass->activate_element (autoselect, elem_list);
}

/*
 * This function checks if there is one and only one pad template on the
 * factory that can accept the given caps. If there is one and only one,
 * it returns TRUE, otherwise, its FALSE
 */

static gboolean
factory_can_intersect (GstAutoSelect * autoselect,
    GstElementFactory * factory, GstPadDirection direction, GstCaps * caps)
{
  const GList *templates;
  gint has_direction = FALSE;
  gboolean ret = FALSE;

  g_return_val_if_fail (factory != NULL, FALSE);
  g_return_val_if_fail (caps != NULL, FALSE);

  templates = gst_element_factory_get_static_pad_templates (factory);

  while (templates) {
    GstStaticPadTemplate *template = (GstStaticPadTemplate *) templates->data;

    if (template->direction == direction) {
      GstCaps *tmpl_caps = NULL;
      gboolean intersect;

      /* If there is more than one pad in this direction, we return FALSE
       * Only transform elements (with one sink and one source pad)
       * are accepted
       */
      if (has_direction) {
        GST_DEBUG_OBJECT (autoselect, "Factory %p"
            " has more than one static template with dir %d",
            template, direction);
        return FALSE;
      }
      has_direction = TRUE;

      tmpl_caps = gst_static_caps_get (&template->static_caps);
      intersect = gst_caps_can_intersect (tmpl_caps, caps);
      GST_DEBUG_OBJECT (autoselect, "Factories %" GST_PTR_FORMAT
          " static caps %" GST_PTR_FORMAT " and caps %" GST_PTR_FORMAT
          " can%s intersect", factory, tmpl_caps, caps,
          intersect ? "" : " not");
      gst_caps_unref (tmpl_caps);

      ret |= intersect;
    }
    templates = g_list_next (templates);
  }

  return ret;
}

static gboolean
sticky_event_push (GstPad * pad, GstEvent ** event, gpointer user_data)
{
  GstAutoSelect *autoselect = GST_AUTO_SELECT (user_data);

  gst_event_ref (*event);
  gst_pad_push_event (autoselect->current_internal_srcpad, *event);

  return TRUE;
}

static gboolean
gst_auto_select_activate_element (GstAutoSelect * autoselect, GList * elem_list)
{
  gboolean ret = FALSE;
  GList *select_list = NULL;
  GstElement *first_elem = NULL;
  GstElement *last_elem = NULL;
  GstPad *internal_srcpad = NULL;
  GstPad *internal_sinkpad = NULL;

  if (!elem_list) {
    GST_DEBUG_OBJECT (autoselect, "No valid element");
    return ret;
  }

  select_list = elem_list;
  first_elem = GST_ELEMENT (select_list->data);
  internal_srcpad = g_object_get_qdata (G_OBJECT (first_elem),
      internal_srcpad_quark);

  select_list = g_list_next (select_list);
  if (select_list) {
    select_list = g_list_last (elem_list);
    last_elem = GST_ELEMENT (select_list->data);
    internal_sinkpad = g_object_get_qdata (G_OBJECT (last_elem),
        internal_sinkpad_quark);
  } else {
    internal_sinkpad = g_object_get_qdata (G_OBJECT (first_elem),
        internal_sinkpad_quark);
  }

  /* Check the first elements can really accept caps */
  if (autoselect->caps) {
    if (!gst_pad_peer_query_accept_caps (internal_srcpad, autoselect->caps)) {
      GST_DEBUG_OBJECT (autoselect, "Could not set %s:%s to %"
          GST_PTR_FORMAT, GST_DEBUG_PAD_NAME (internal_srcpad),
          autoselect->caps);
      goto activate_element_failed;
    } else {
      ret = TRUE;
    }
  }

  GST_AUTOSELECT_LOCK (autoselect);
  gst_object_replace ((GstObject **) & autoselect->first_subelement,
      GST_OBJECT (first_elem));
  if (last_elem) {
    gst_object_replace ((GstObject **) & autoselect->last_subelement,
        GST_OBJECT (last_elem));
  }
  gst_object_replace ((GstObject **) & autoselect->current_internal_srcpad,
      GST_OBJECT (internal_srcpad));
  gst_object_replace ((GstObject **) & autoselect->current_internal_sinkpad,
      GST_OBJECT (internal_sinkpad));
  autoselect->elem_list = elem_list;
  GST_AUTOSELECT_UNLOCK (autoselect);

  gst_pad_sticky_events_foreach (autoselect->sinkpad, sticky_event_push,
      autoselect);

  /* If there are multiple elements in the
   * bin, we need check caps negotiation result
   */
  if (last_elem)
    ret =
        gst_auto_select_check_caps_negotiation (autoselect, autoselect->caps,
        autoselect->event);

  if (ret)
    gst_pad_push_event (autoselect->sinkpad, gst_event_new_reconfigure ());
  return ret;

activate_element_failed:
  g_list_free (elem_list);
  gst_auto_select_remove_elements (GST_BIN (autoselect));
  return ret;
}

static GstIterator *
gst_auto_select_iterate_internal_links (GstPad * pad, GstObject * parent)
{
  GstAutoSelect *autoselect = GST_AUTO_SELECT (parent);
  GstIterator *it = NULL;
  GstPad *internal;

  if (pad == autoselect->sinkpad)
    internal = gst_auto_select_get_internal_srcpad (autoselect);
  else
    internal = gst_auto_select_get_internal_sinkpad (autoselect);

  if (internal) {
    GValue val = { 0, };

    g_value_init (&val, GST_TYPE_PAD);
    g_value_take_object (&val, internal);

    it = gst_iterator_new_single (GST_TYPE_PAD, &val);
    g_value_unset (&val);
  }

  return it;
}

static gboolean
gst_auto_select_check_caps_negotiation (GstAutoSelect * autoselect,
    GstCaps * caps, GstEvent * event)
{
  GList *item;
  GstElement *curr_elem;
  GstPad *sink_pad = NULL;
  GstPad *peer = NULL;
  gboolean is_first_elem = TRUE;

  for (item = autoselect->elem_list; item; item = item->next) {
    curr_elem = item->data;

    sink_pad = get_pad_by_direction (curr_elem, GST_PAD_SINK);
    if (!sink_pad) {
      GST_WARNING_OBJECT (autoselect, "Could not find matched sink pad in %s",
          GST_OBJECT_NAME (curr_elem));
      goto check_failed;
    }
    if (is_first_elem) {
      is_first_elem = FALSE;
      peer = gst_pad_get_peer (sink_pad);
      if (!peer) {
        GST_WARNING_OBJECT (autoselect, "Could not get peer pad, %s:%s",
            GST_DEBUG_PAD_NAME (sink_pad));
        goto check_failed;
      }

      /* Send caps event to trigger caps negotiation
       * If there are multiple elements in the bin,
       * we can get all the element sink caps to
       * check caps negotiation result
       */
      gst_event_ref (event);
      if (!gst_pad_push_event (peer, event)) {
        GST_DEBUG_OBJECT (autoselect, "Could not send gap event, %s:%s",
            GST_DEBUG_PAD_NAME (peer));
        gst_object_unref (peer);
        goto check_failed;
      }
      gst_object_unref (peer);
    }

    if (!gst_pad_get_current_caps (sink_pad)) {
      GST_DEBUG_OBJECT (autoselect, "Could not get caps, %s:%s",
          GST_DEBUG_PAD_NAME (sink_pad));
      gst_object_unref (sink_pad);
      goto check_failed;
    }
    gst_object_unref (sink_pad);
  }

  gst_event_unref (event);
  return TRUE;
check_failed:
  GST_AUTOSELECT_LOCK (autoselect);
  autoselect->first_subelement = NULL;
  autoselect->last_subelement = NULL;
  autoselect->current_internal_sinkpad = NULL;
  autoselect->current_internal_srcpad = NULL;
  g_list_free (autoselect->elem_list);
  autoselect->elem_list = NULL;
  GST_AUTOSELECT_UNLOCK (autoselect);
  gst_auto_select_remove_elements (GST_BIN (autoselect));
  return FALSE;
}

static gboolean
gst_auto_select_update_composite_factories_list (GstAutoSelect * autoselect,
    GList ** factories_list, GstElementFactory * factory)
{
  guint rank = gst_plugin_feature_get_rank ((GstPluginFeature *) factory);

  if (rank > autoselect->composite_rank) {
    GST_LOG_OBJECT (autoselect,
        "Add Factory %s to list",
        gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)));
    /* Record the factory that only accept sink pad */
    *factories_list = g_list_append (*factories_list, factory);
    return TRUE;
  }
  return FALSE;
}

static gboolean
gst_auto_select_construct_composite_elements (GstAutoSelect * autoselect,
    GList * first_factories, GList * last_factories,
    GstElementFactory * current_factory)
{
  guint curent_rank =
      gst_plugin_feature_get_rank ((GstPluginFeature *) current_factory);

  if (curent_rank <= autoselect->composite_rank
      && first_factories && last_factories) {
    /* Try to make and combine two elements if no
     * single element currently matched whose rank
     * is greater than the composite rank level.
     */
    autoselect->is_composite =
        gst_auto_select_make_composite_elements (autoselect,
        first_factories, last_factories);

    if (!autoselect->is_composite) {
      GST_LOG_OBJECT (autoselect, "Can not make composite elements");
    }
    return TRUE;
  }

  return FALSE;
}

static gboolean
gst_auto_select_check_current_factory (GstAutoSelect * autoselect,
    GstElementFactory * factory)
{
  guint rank = gst_plugin_feature_get_rank ((GstPluginFeature *) factory);

  /* Check current factory rank and ignore it
   * if it's lower than the specified lowest rank.
   */
  if (rank < autoselect->lowest_rank) {
    GST_LOG_OBJECT (autoselect,
        "Factory %s is ignored because the rank is litter than the specified rank",
        gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)));
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_auto_select_sink_setcaps (GstAutoSelect * autoselect, GstCaps * caps,
    GstEvent * event)
{
  GList *elem;
  GstCaps *other_caps = NULL;
  GList *factories = NULL;
  GstCaps *current_caps;
  GList *first_factories = NULL;
  GList *last_factories = NULL;
  gboolean has_sent_event = FALSE;
  GstAutoSelectClass *klass = GST_AUTO_SELECT_GET_CLASS (autoselect);

  g_return_val_if_fail (autoselect != NULL, FALSE);

  /* Check the caps and return if it's the same as the current caps */
  current_caps = gst_pad_get_current_caps (autoselect->sinkpad);
  if (current_caps) {
    if (gst_caps_is_equal_fixed (caps, current_caps)) {
      GST_DEBUG_OBJECT (autoselect, "Got the same caps %" GST_PTR_FORMAT, caps);
      gst_caps_unref (current_caps);
      goto done;
    }
    gst_caps_unref (current_caps);
  }

  /* Check whether we can set the new caps with the current element(s) */
  if (caps && (autoselect->is_composite || autoselect->first_subelement)) {
    if (gst_pad_peer_query_accept_caps (autoselect->current_internal_srcpad,
            caps)) {
      /* If there is one element, it can really accept caps */
      if (!autoselect->is_composite) {
        goto done;
      }

      /* Need check multiple elements caps negotiation result */
      if (gst_auto_select_check_caps_negotiation (autoselect, caps, event)) {
        has_sent_event = TRUE;
        goto done;
      }
    }
  }

  /* Start to construct single or composite elements with factories list */
  autoselect->caps = caps;
  autoselect->event = event;
  other_caps = gst_pad_peer_query_caps (autoselect->srcpad, NULL);
  factories = gst_auto_select_copy_factories_list (autoselect);
  if (!factories)
    goto done;

  for (elem = factories; elem; elem = g_list_next (elem)) {
    GstElementFactory *factory = GST_ELEMENT_FACTORY (elem->data);

    /* If no single element currently is selected,
     * check whether we need to construct composite elements now.
     */
    if (klass->construct_composite_elements (autoselect,
            first_factories, last_factories, factory)) {
      g_list_free (first_factories);
      g_list_free (last_factories);
      first_factories = NULL;
      last_factories = NULL;

      if (autoselect->is_composite) {
        has_sent_event = TRUE;
        break;
      }
    }

    /* Check the current factory and ignore it if the
     * conditions don't match.
     */
    if (!klass->check_current_factory (autoselect, factory)) {
      continue;
    }

    /* Let's first check if these caps have any chance of success
     * according to the static pad templates on the factory
     */
    if (!factory_can_intersect (autoselect, factory, GST_PAD_SINK, caps)) {
      GST_LOG_OBJECT (autoselect, "Factory %s does not accept sink caps %"
          GST_PTR_FORMAT,
          gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)), caps);

      if (other_caps != NULL) {
        /* Record the factory that only accept src pad */
        if (factory_can_intersect (autoselect, factory, GST_PAD_SRC,
                other_caps)) {
          if (klass->update_composite_factories_list (autoselect,
                  &last_factories, factory)) {
            GST_LOG_OBJECT (autoselect,
                "Factory %s can only accept src caps %" GST_PTR_FORMAT,
                gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)),
                other_caps);
          }
        }
      }
      continue;
    }

    if (other_caps != NULL) {
      if (!factory_can_intersect (autoselect, factory, GST_PAD_SRC, other_caps)) {
        GST_LOG_OBJECT (autoselect,
            "Factory %s does not accept src caps %" GST_PTR_FORMAT,
            gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)),
            other_caps);

        /* Record the factory that only accept sink pad */
        if (klass->update_composite_factories_list (autoselect,
                &first_factories, factory)) {
          GST_LOG_OBJECT (autoselect,
              "Factory %s can only accept sink caps %" GST_PTR_FORMAT,
              gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)), caps);
        }
        continue;
      }
    }

    /* Try to construct single element */
    if (!klass->construct_single_element (autoselect, factory)) {
      /* add the factory to the two lists together and it can be
       * selected again when constructing composite elements.
       */
      first_factories = g_list_append (first_factories, factory);
      last_factories = g_list_append (last_factories, factory);
    } else {
      break;
    }
  }

done:
  if (factories)
    g_list_free (factories);

  if (other_caps)
    gst_caps_unref (other_caps);

  if (first_factories)
    g_list_free (first_factories);
  if (last_factories)
    g_list_free (last_factories);

  if (autoselect->is_composite || autoselect->first_subelement) {
    GST_DEBUG_OBJECT (autoselect, "Could set %s:%s to %" GST_PTR_FORMAT,
        GST_DEBUG_PAD_NAME (autoselect->current_internal_srcpad), caps);

    /* Need send caps event if it hasn't been sent */
    if (!has_sent_event) {
      return gst_pad_push_event (autoselect->current_internal_srcpad, event);
    }
    return TRUE;
  } else {
    gst_event_unref (event);
    GST_DEBUG_OBJECT (autoselect, "Could not find a matching element for caps");
    return FALSE;
  }
}

/*
 * This function filters the pad pad templates, taking only transform element
 * (with one sink and one src pad)
 */

static gboolean
gst_auto_select_default_filter_func (GstPluginFeature * feature,
    gpointer user_data)
{
  GstElementFactory *factory = NULL;
  const GList *static_pad_templates, *tmp;
  GstStaticPadTemplate *src = NULL, *sink = NULL;

  if (!GST_IS_ELEMENT_FACTORY (feature))
    return FALSE;

  factory = GST_ELEMENT_FACTORY (feature);

  static_pad_templates = gst_element_factory_get_static_pad_templates (factory);

  for (tmp = static_pad_templates; tmp; tmp = g_list_next (tmp)) {
    GstStaticPadTemplate *template = tmp->data;
    GstCaps *caps;

    if (template->presence == GST_PAD_SOMETIMES)
      return FALSE;

    if (template->presence != GST_PAD_ALWAYS)
      continue;

    switch (template->direction) {
      case GST_PAD_SRC:
        if (src)
          return FALSE;
        src = template;
        break;
      case GST_PAD_SINK:
        if (sink)
          return FALSE;
        sink = template;
        break;
      default:
        return FALSE;
    }

    caps = gst_static_pad_template_get_caps (template);

    if (gst_caps_is_any (caps) || gst_caps_is_empty (caps))
      return FALSE;
  }

  if (!src || !sink)
    return FALSE;

  return TRUE;
}

/* function used to sort element features
 * Copy-pasted from decodebin */
static gint
compare_ranks (GstPluginFeature * f1, GstPluginFeature * f2)
{
  gint diff;
  const gchar *rname1, *rname2;

  diff = gst_plugin_feature_get_rank (f2) - gst_plugin_feature_get_rank (f1);
  if (diff != 0)
    return diff;

  rname1 = gst_plugin_feature_get_name (f1);
  rname2 = gst_plugin_feature_get_name (f2);

  diff = strcmp (rname2, rname1);

  return diff;
}

static void
gst_auto_select_load_factories (GstAutoSelect * autoselect)
{
  GList *all_factories;

  all_factories =
      gst_registry_feature_filter (gst_registry_get (),
      gst_auto_select_default_filter_func, FALSE, NULL);

  all_factories = g_list_sort (all_factories, (GCompareFunc) compare_ranks);

  g_assert (all_factories);

  if (!g_atomic_pointer_compare_and_exchange (&autoselect->factories,
          (GList *) NULL, all_factories)) {
    gst_plugin_feature_list_free (all_factories);
  }
}

/* In this case, we should almost always have an internal element, because
 * set_caps() should have been called first
 */

static GstFlowReturn
gst_auto_select_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstFlowReturn ret = GST_FLOW_NOT_NEGOTIATED;
  GstAutoSelect *autoselect = GST_AUTO_SELECT (parent);

  if (autoselect->current_internal_srcpad) {
    ret = gst_pad_push (autoselect->current_internal_srcpad, buffer);
    if (ret != GST_FLOW_OK)
      GST_DEBUG_OBJECT (autoselect,
          "Child element %" GST_PTR_FORMAT "returned flow %s",
          autoselect->first_subelement, gst_flow_get_name (ret));
  } else {
    GST_ERROR_OBJECT (autoselect, "Got buffer without an negotiated element,"
        " returning not-negotiated");
    gst_buffer_unref (buffer);
  }

  return ret;
}

static GstFlowReturn
gst_auto_select_sink_chain_list (GstPad * pad, GstObject * parent,
    GstBufferList * list)
{
  GstFlowReturn ret = GST_FLOW_NOT_NEGOTIATED;
  GstAutoSelect *autoselect = GST_AUTO_SELECT (parent);

  if (autoselect->current_internal_srcpad) {
    ret = gst_pad_push_list (autoselect->current_internal_srcpad, list);
    if (ret != GST_FLOW_OK)
      GST_DEBUG_OBJECT (autoselect,
          "Child element %" GST_PTR_FORMAT "returned flow %s",
          autoselect->first_subelement, gst_flow_get_name (ret));
  } else {
    GST_ERROR_OBJECT (autoselect, "Got buffer without an negotiated element,"
        " returning not-negotiated");
    gst_buffer_list_unref (list);
  }

  return ret;
}

static gboolean
gst_auto_select_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean ret = TRUE;
  GstAutoSelect *autoselect = GST_AUTO_SELECT (parent);
  GstPad *internal_srcpad;

  if (GST_EVENT_TYPE (event) == GST_EVENT_CAPS) {
    GstCaps *caps;

    gst_event_parse_caps (event, &caps);
    return gst_auto_select_sink_setcaps (autoselect, caps, event);
  }

  internal_srcpad = gst_auto_select_get_internal_srcpad (autoselect);
  if (internal_srcpad) {
    ret = gst_pad_push_event (internal_srcpad, event);
    gst_object_unref (internal_srcpad);
  } else {
    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_FLUSH_STOP:
      case GST_EVENT_FLUSH_START:
        ret = gst_pad_push_event (autoselect->srcpad, event);
        break;
      default:
        gst_event_unref (event);
        ret = TRUE;
        break;
    }
  }

  return ret;
}

/* TODO Properly test that this code works well for queries */
static gboolean
gst_auto_select_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean ret = TRUE;
  GstAutoSelect *autoselect = GST_AUTO_SELECT (parent);
  GstElement *subelement;

  if (GST_QUERY_TYPE (query) == GST_QUERY_CAPS) {
    GstCaps *filter, *caps;

    gst_query_parse_caps (query, &filter);
    caps = gst_auto_select_getcaps (autoselect, filter, GST_PAD_SINK);
    gst_query_set_caps_result (query, caps);
    gst_caps_unref (caps);

    return TRUE;
  }

  subelement = gst_auto_select_get_subelement (autoselect, GST_PAD_SINK);
  if (subelement) {
    GstPad *sub_sinkpad = get_pad_by_direction (subelement, GST_PAD_SINK);

    ret = gst_pad_query (sub_sinkpad, query);

    gst_object_unref (sub_sinkpad);
    gst_object_unref (subelement);

    if (ret && GST_QUERY_TYPE (query) == GST_QUERY_ACCEPT_CAPS) {
      gboolean res;
      gst_query_parse_accept_caps_result (query, &res);

      if (!res)
        goto ignore_acceptcaps_failure;
    }
    return ret;
  }

ignore_acceptcaps_failure:

  if (GST_QUERY_TYPE (query) == GST_QUERY_ACCEPT_CAPS) {
    GstCaps *caps;
    GstCaps *accept_caps;

    gst_query_parse_accept_caps (query, &accept_caps);

    caps = gst_auto_select_getcaps (autoselect, accept_caps, GST_PAD_SINK);
    gst_query_set_accept_caps_result (query,
        gst_caps_can_intersect (caps, accept_caps));
    gst_caps_unref (caps);

    return TRUE;
  }

  GST_WARNING_OBJECT (autoselect, "Got query %s while no element was"
      " selected, letting through",
      gst_query_type_get_name (GST_QUERY_TYPE (query)));
  return gst_pad_peer_query (autoselect->srcpad, query);
}

static GList *
gst_auto_select_copy_factories_list (GstAutoSelect * autoselect)
{
  GList *factories;

  GST_AUTOSELECT_LOCK (autoselect);
  factories = g_atomic_pointer_get (&autoselect->factories);
  if (factories)
    factories = g_list_copy (factories);
  GST_AUTOSELECT_UNLOCK (autoselect);

  if (!factories) {
    GST_WARNING_OBJECT (autoselect,
        "No factory list information, create it itself");
    gst_auto_select_load_factories (autoselect);
    GST_AUTOSELECT_LOCK (autoselect);
    factories = g_atomic_pointer_get (&autoselect->factories);
    if (factories)
      factories = g_list_copy (factories);
    GST_AUTOSELECT_UNLOCK (autoselect);
  }

  return factories;
}

/**
 * gst_auto_select_getcaps:
 * @pad: the sink #GstPad
 *
 * This function returns the union of the caps of all the possible element
 * factories, based on the static pad templates.
 * It also checks does a getcaps on the downstream element and ignores all
 * factories whose static caps can not satisfy it.
 *
 * It does not try to use each elements getcaps() function
 */

static GstCaps *
gst_auto_select_getcaps (GstAutoSelect * autoselect, GstCaps * filter,
    GstPadDirection dir)
{
  GstCaps *caps = NULL, *other_caps = NULL;
  GList *elem;
  GList *factories = NULL;

  caps = gst_caps_new_empty ();

  if (dir == GST_PAD_SINK)
    other_caps = gst_pad_peer_query_caps (autoselect->srcpad, NULL);
  else
    other_caps = gst_pad_peer_query_caps (autoselect->sinkpad, NULL);

  GST_DEBUG_OBJECT (autoselect,
      "Lets find all the element that can fit here with src caps %"
      GST_PTR_FORMAT, other_caps);

  if (other_caps && gst_caps_is_empty (other_caps)) {
    goto out;
  }

  factories = gst_auto_select_copy_factories_list (autoselect);
  if (!factories)
    return caps;

  for (elem = factories; elem; elem = g_list_next (elem)) {
    GstElementFactory *factory = GST_ELEMENT_FACTORY (elem->data);

    if (filter) {
      if (!factory_can_intersect (autoselect, factory, dir, filter)) {
        GST_LOG_OBJECT (autoselect,
            "Factory %s does not accept filter caps %" GST_PTR_FORMAT,
            gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)), filter);
        continue;
      }
    }
    if (other_caps != NULL) {
      if (!factory_can_intersect (autoselect, factory,
              dir == GST_PAD_SINK ? GST_PAD_SRC : GST_PAD_SINK, other_caps)) {
        GST_LOG_OBJECT (autoselect,
            "Factory %s does not accept other caps %" GST_PTR_FORMAT,
            gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)),
            other_caps);
        continue;
      }
    }

    const GList *tmp;
    for (tmp = gst_element_factory_get_static_pad_templates (factory);
        tmp; tmp = g_list_next (tmp)) {
      GstStaticPadTemplate *template = tmp->data;
      if (template->direction == dir) {
        GstCaps *static_caps = gst_static_pad_template_get_caps (template);

        if (static_caps) {
          GstCaps *intersection;
          if (filter) {
            intersection = gst_caps_intersect_full (filter, static_caps,
                GST_CAPS_INTERSECT_FIRST);
            gst_caps_unref (static_caps);
            caps = gst_caps_merge (caps, intersection);
          } else {
            caps = gst_caps_merge (caps, static_caps);
          }
        }

        /* Early out, any is absorbing */
        if (gst_caps_is_any (caps))
          goto out;
      }
    }
  }

  GST_DEBUG_OBJECT (autoselect,
      "Pad dir: %d, Returning unioned caps %" GST_PTR_FORMAT, dir, caps);

out:
  if (factories)
    g_list_free (factories);
  if (other_caps)
    gst_caps_unref (other_caps);

  return caps;
}



static gboolean
gst_auto_select_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean ret = TRUE;
  GstAutoSelect *autoselect = GST_AUTO_SELECT (parent);
  GstPad *internal_sinkpad;

  if (GST_EVENT_TYPE (event) == GST_EVENT_RECONFIGURE)
    gst_pad_push_event (autoselect->sinkpad, gst_event_ref (event));

  internal_sinkpad = gst_auto_select_get_internal_sinkpad (autoselect);
  if (internal_sinkpad) {
    ret = gst_pad_push_event (internal_sinkpad, event);
    gst_object_unref (internal_sinkpad);
  } else if (GST_EVENT_TYPE (event) != GST_EVENT_RECONFIGURE) {
    GST_WARNING_OBJECT (autoselect,
        "Got upstream event while no element was selected," "forwarding.");
    ret = gst_pad_push_event (autoselect->sinkpad, event);
  } else
    gst_event_unref (event);

  return ret;
}

/* TODO Properly test that this code works well for queries */
static gboolean
gst_auto_select_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean ret = TRUE;
  GstAutoSelect *autoselect = GST_AUTO_SELECT (parent);
  GstElement *subelement;

  if (GST_QUERY_TYPE (query) == GST_QUERY_CAPS) {
    GstCaps *filter, *caps;

    gst_query_parse_caps (query, &filter);
    caps = gst_auto_select_getcaps (autoselect, filter, GST_PAD_SRC);
    gst_query_set_caps_result (query, caps);
    gst_caps_unref (caps);

    return TRUE;
  }

  subelement = gst_auto_select_get_subelement (autoselect, GST_PAD_SRC);
  if (subelement) {
    GstPad *sub_srcpad = get_pad_by_direction (subelement, GST_PAD_SRC);

    ret = gst_pad_query (sub_srcpad, query);

    gst_object_unref (sub_srcpad);
    gst_object_unref (subelement);
  } else {
    GST_WARNING_OBJECT (autoselect,
        "Got upstream query of type %s while no element was selected,"
        " forwarding.", gst_query_type_get_name (GST_QUERY_TYPE (query)));
    ret = gst_pad_peer_query (autoselect->sinkpad, query);
  }

  return ret;
}

static GstFlowReturn
gst_auto_select_internal_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstAutoSelect *autoselect =
      GST_AUTO_SELECT (g_object_get_qdata (G_OBJECT (pad),
          parent_quark));

  return gst_pad_push (autoselect->srcpad, buffer);
}

static GstFlowReturn
gst_auto_select_internal_sink_chain_list (GstPad * pad, GstObject * parent,
    GstBufferList * list)
{
  GstAutoSelect *autoselect =
      GST_AUTO_SELECT (g_object_get_qdata (G_OBJECT (pad),
          parent_quark));

  return gst_pad_push_list (autoselect->srcpad, list);
}

static gboolean
gst_auto_select_internal_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstAutoSelect *autoselect =
      GST_AUTO_SELECT (g_object_get_qdata (G_OBJECT (pad),
          parent_quark));
  gboolean drop = FALSE;

  GST_AUTOSELECT_LOCK (autoselect);
  if (autoselect->current_internal_sinkpad != pad) {
    drop = TRUE;
  }
  GST_AUTOSELECT_UNLOCK (autoselect);

  if (drop) {
    gst_event_unref (event);
    return TRUE;
  }

  return gst_pad_push_event (autoselect->srcpad, event);
}

static gboolean
gst_auto_select_internal_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstAutoSelect *autoselect =
      GST_AUTO_SELECT (g_object_get_qdata (G_OBJECT (pad),
          parent_quark));

  if (!gst_pad_peer_query (autoselect->srcpad, query)) {
    switch (GST_QUERY_TYPE (query)) {
      case GST_QUERY_CAPS:
      {
        GstCaps *filter;

        gst_query_parse_caps (query, &filter);
        if (filter) {
          gst_query_set_caps_result (query, filter);
        } else {
          filter = gst_caps_new_any ();
          gst_query_set_caps_result (query, filter);
          gst_caps_unref (filter);
        }
        return TRUE;
      }
      case GST_QUERY_ACCEPT_CAPS:
        gst_query_set_accept_caps_result (query, TRUE);
        return TRUE;
      default:
        return FALSE;
    }
  }

  return TRUE;
}

static gboolean
gst_auto_select_internal_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstAutoSelect *autoselect =
      GST_AUTO_SELECT (g_object_get_qdata (G_OBJECT (pad),
          parent_quark));
  gboolean drop = FALSE;

  GST_AUTOSELECT_LOCK (autoselect);
  if (autoselect->current_internal_srcpad != pad) {
    drop = TRUE;
  }
  GST_AUTOSELECT_UNLOCK (autoselect);

  if (drop) {
    GST_DEBUG_OBJECT (autoselect, "Dropping event %" GST_PTR_FORMAT, event);
    gst_event_unref (event);
    return TRUE;
  }

  return gst_pad_push_event (autoselect->sinkpad, event);
}

static gboolean
gst_auto_select_internal_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstAutoSelect *autoselect =
      GST_AUTO_SELECT (g_object_get_qdata (G_OBJECT (pad),
          parent_quark));

  return gst_pad_peer_query (autoselect->sinkpad, query);
}
