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


#ifndef __GST_AUTO_SELECT_H__
#define __GST_AUTO_SELECT_H__

#include <gst/gst.h>
#include <gst/gstbin.h>

G_BEGIN_DECLS
#define GST_TYPE_AUTO_SELECT            (gst_auto_select_get_type())
#define GST_AUTO_SELECT(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AUTO_SELECT,GstAutoSelect))
#define GST_AUTO_SELECT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AUTO_SELECT,GstAutoSelectClass))
#define GST_AUTO_SELECT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_AUTO_SELECT,GstAutoSelectClass))
#define GST_IS_AUTO_SELECT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AUTO_SELECT))
#define GST_IS_AUTO_SELECT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AUTO_SELECT))
typedef struct _GstAutoSelect GstAutoSelect;
typedef struct _GstAutoSelectClass GstAutoSelectClass;

struct _GstAutoSelect
{
  /*< private > */
  GstBin bin;

  GList *factories;

  GstPad *sinkpad;
  GstPad *srcpad;

  /* Have to be set all at once
   * Protected by the object lock and the stream lock
   * Both must be held to modify these
   */
  GstPad *current_internal_srcpad;
  GstPad *current_internal_sinkpad;

  /* record all the elements which
   * have beed added
   */
  GList *elem_list;
  /* whether there are multiple elements */
  gboolean is_composite;
  /* Used to trigger trying to combine
   * multiple elements
   */
  guint composite_rank;
  /* The first element in the bin */
  GstElement *first_subelement;
  /* The last element in the bin */
  GstElement *last_subelement;
  /* Only select the factory whose rank is
   * equal to or greater than this value,
   * otherwise the factory will be ignored.
   */
  guint lowest_rank;

  GstCaps *caps;
  GstEvent *event;
};

struct _GstAutoSelectClass
{
  GstBinClass parent_class;

  gboolean (*check_current_factory) (GstAutoSelect * autoselect,
                                     GstElementFactory * factory);

  gboolean (*construct_single_element) (GstAutoSelect * autoselect,
                                        GstElementFactory * current_factory);

  gboolean (*update_composite_factories_list) (GstAutoSelect * autoselect,
                                               GList ** factories_list, GstElementFactory * factory);

  gboolean (*construct_composite_elements) (GstAutoSelect * autoselect,
                                            GList * first_factories, GList * last_factories,
                                            GstElementFactory * current_factory);

  gboolean (*add_element) (GstAutoSelect * autoselect, GList * elem_list);

  gboolean (*activate_element) (GstAutoSelect * autoselect,
                                GList * elem_list);
};

GType gst_auto_select_get_type (void);
GST_ELEMENT_REGISTER_DECLARE (autoselect);

G_END_DECLS
#endif /* __GST_AUTO_SELECT_H__ */
