/* GStreamer
 * Copyright 2021-2022 NXP
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
#ifndef __GST_MATROSKA_H__
#define __GST_MATROSKA_H__
#include <gst/gst.h>
#include <gst/gstinfo.h>
#include <gst/base/gstadapter.h>

G_BEGIN_DECLS
#ifndef GST_MATROSKA_API
# ifdef BUILDING_GST_MATROSKA
#  define GST_MATROSKA_API GST_API_EXPORT       /* from config.h */
# else
#  define GST_MATROSKA_API GST_API_IMPORT
# endif
#endif
    typedef struct
{
  guint32 id;
  guint64 size;
  gint64 data_offset;
  guint8 *data_buf;
} GstMatroskaEbmlInfo;

typedef enum
{
  GST_MATROSKA_PARSER_OK,
  GST_MATROSKA_PARSER_DONE,
  GST_MATROSKA_PARSER_NOT_SUPPORTED,
  GST_MATROSKA_PARSER_ERROR_PARAM,
  GST_MATROSKA_PARSER_INSUFFICIENT_DATA,
  GST_MATROSKA_PARSER_ERROR
} GstMatroskaParserResult;

typedef enum
{
  GST_MATROSKA_PARSER_STATUS_INIT,
  GST_MATROSKA_PARSER_STATUS_HEADER,
  GST_MATROSKA_PARSER_STATUS_DATA,
  GST_MATROSKA_PARSER_STATUS_FINISHED
} GstMatroskaParserStatus;

typedef struct
{
  guint64 track;
  guint64 cluster_pos;
} GstMatroskaTrackPosType;

typedef struct
{
  guint64 cue_time;
  GstMatroskaTrackPosType track_pos;
} GstMatroskaPointData;

typedef struct _GstMatroskaParser
{
  /* total length before first cluster */
  guint64 len;
  /* segment offset */
  guint64 segment_offset;
  /* cluster address = segment_head_offset + cue cluster position */
  guint64 segment_head_offset;
  /* unit of time scale is nanosecond */
  guint64 time_scale;
  guint64 duration;
  /* parsed length in adapter */
  guint64 offset;
  /* current consume length */
  guint64 consume;
  /* number of cue point */
  guint64 cue_point_num;
  /* cue point data list */
  GArray *array;
  GstMatroskaParserStatus status;
  gboolean is_discard_ebml_header;
  guint64 total_length;
} GstMatroskaParser;

GST_MATROSKA_API void gst_matroska_parser_init (GstMatroskaParser * parser);

GST_MATROSKA_API void gst_matroska_parser_clear (GstMatroskaParser * parser);

GST_MATROSKA_API
    GstMatroskaParserResult gst_matroska_parser_entry (GstMatroskaParser *
    parser, GstAdapter * adapter);

G_END_DECLS
#endif /* __GST_MATROSKA_H__ */
