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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstmatroska.h"
#include "stdio.h"

GST_DEBUG_CATEGORY_STATIC (gst_matroska_debug);
#define GST_CAT_DEFAULT gst_matroska_debug

static gboolean initialized = FALSE;

#define INITIALIZE_DEBUG_CATEGORY \
  if (!initialized) { \
  GST_DEBUG_CATEGORY_INIT (gst_matroska_debug, "matroska", 0, \
      "Matroska Format parsing library"); \
    initialized = TRUE; \
  }

/* log definition */
#define GST_MATROSKA_PARSER_DEBUG                  GST_LOG

/* L0: EMBL header */
#define GST_EBML_ID_HEADER                         0x1A45DFA3
/* L0: toplevel Segment */
#define GST_MATROSKA_ID_SEGMENT                    0x18538067
#define GST_MATROSKA_ID_CLUSTER                    0x1F43B675

/* L1: matroska top-level master IDs, childs of Segment */
#define GST_MATROSKA_ID_SEEKHEAD                   0x114D9B74
#define GST_MATROSKA_ID_SEGMENTINFO                0x1549A966
#define GST_MATROSKA_ID_TRACKS                     0x1654AE6B
#define GST_MATROSKA_ID_CUES                       0x1C53BB6B
#define GST_MATROSKA_ID_TAGS                       0x1254C367
#define GST_MATROSKA_ID_ATTACHMENTS                0x1941A469
#define GST_MATROSKA_ID_CHAPTERS                   0x1043A770

/* L2: time scale, child of segment information  */
#define GST_MATROSKA_ID_TIMECODESCALE              0x2AD7B1
#define GST_MATROSKA_ID_DURATION                   0x4489
/* L2: cue point, child of cues */
#define GST_MATROSKA_ID_POINTENTRY                 0xBB
/* L3: cue time, child of cue point */
#define GST_MATROSKA_ID_CUETIME                    0xB3
/* L3: cue track position , child of cue point */
#define GST_MATROSKA_ID_CUETRACKPOSITION           0xB7
/* L4: cue track, child of track position */
#define GST_MATROSKA_ID_CUETRACK                   0xF7
/* L4: cue cluster position, child of track position */
#define GST_MATROSKA_ID_CUECLUSTERPOSITION         0xF1
#define GST_MATROSKA_ID_CUEBLOCKNUMBER             0x5378

void
gst_matroska_parser_init (GstMatroskaParser * parser)
{
  if (parser->array) {
    GST_ERROR ("array list has not free yet, addr = %p", parser->array);
  }
  memset (parser, 0, sizeof (GstMatroskaParser));
}

void
gst_matroska_parser_clear (GstMatroskaParser * parser)
{
  if (parser->array) {
    GST_ERROR ("free array list, addr = %p", parser->array);
    g_array_free (parser->array, TRUE);
  }
  memset (parser, 0, sizeof (GstMatroskaParser));
}

static gint32
gst_matroska_parser_read_len (guint8 data)
{
  guint i = 0;

  for (i = 0; i < 8; i++) {
    if ((data & (0x80 >> i)) != 0)
      break;
  }
  return (i + 1);
}

static guint64
gst_matroska_parser_read_data (guint8 * p, gint64 len)
{
  guint64 data = 0;

  if (!p || !len || (len > 8)) {
    GST_ERROR ("param error, p = %p, len = %" G_GINT64_FORMAT, p, len);
    return 0;
  }

  while (len--) {
    data <<= 8;
    data |= *p++;
  }

  return data;
}

static guint64
gst_matroska_parser_read_data_len (guint8 * p_buf, guint64 len)
{
  guint64 data = 0;
  guint8 *p8 = p_buf;

  if (!p8 || !len || len > 8) {
    GST_ERROR ("param error, p8 = %p , len = %" G_GUINT64_FORMAT, p8, len);
    return 0;
  }

  data = *p8++;
  data -= (guint64) 1 << (8 - len);
  while (--len) {
    data = (data << 8) | (*p8++);
  }

  return data;
}

static guint64
gst_matroska_read_one_ebml_info (guint8 * buf, gint64 size,
    GstMatroskaEbmlInfo * p_info)
{
  gint64 len = 0;
  gint64 bytes = 0;
  guint64 rem_size = 0;
  guint8 *p8 = NULL;
  guint64 unknown_length[8] = { 0x7F, 0x3FFF, 0x1FFFFF,
    0x0FFFFFFF, 0x07FFFFFFFFLL, 0x03FFFFFFFFFFLL,
    0x01FFFFFFFFFFFFLL, 0x00FFFFFFFFFFFFFFLL
  };

  if (!buf || !size || !p_info) {
    GST_MATROSKA_PARSER_DEBUG
        ("read ebml info, buf: %p, size: %" G_GINT64_FORMAT ", p_info: %p",
        buf, size, p_info);
    return 0;
  }

  rem_size = size;
  p8 = buf;

  /* extract id field */
  len = gst_matroska_parser_read_len (*p8);
  if ((rem_size <= len) || (len > 8)) {
    if (len > 8) {
      GST_ERROR ("id field error, size = %" G_GUINT64_FORMAT, len);
    }
    return 0;
  } else {
    p_info->id = gst_matroska_parser_read_data (p8, len);
    p8 += len;
    bytes += len;
    rem_size -= len;
  }

  /* extract size field */
  len = gst_matroska_parser_read_len (*p8);
  if ((rem_size <= len) || (len > 8)) {
    if (len > 8) {
      GST_ERROR ("data size field error, size = %" G_GUINT64_FORMAT ", id = 0x%x", len,
          p_info->id);
    }
    return 0;
  } else {
    p_info->size = gst_matroska_parser_read_data_len (p8, len);
  }
  /* check unknown length */
  if (unknown_length[len - 1] == p_info->size) {
    p_info->size = 0x7FFFFFFFFFFFFFFFULL;
  }
  p8 += len;
  bytes += len;
  rem_size -= len;

  /* extract data field */
  p_info->data_buf = p8;
  p_info->data_offset = (gint64) (p8 - buf);
  bytes += p_info->size;

  return bytes;
}

static GstMatroskaParserResult
gst_matroska_parser_check_id (GstMatroskaParser * parser, guint64 id,
    guint32 len, GstAdapter * adapter)
{
  GstMatroskaParserResult res = GST_MATROSKA_PARSER_OK;
  guint64 avail = gst_adapter_available (adapter);

  /* Check buffer length */
  if (avail < len) {
    GST_MATROSKA_PARSER_DEBUG ("insufficient data, len = %" G_GUINT64_FORMAT, avail);
    return GST_MATROSKA_PARSER_INSUFFICIENT_DATA;
  }

  /* Check id */
  if (gst_matroska_parser_read_data ((guint8 *) gst_adapter_map (adapter, len),
          len) != id) {
    res = GST_MATROSKA_PARSER_NOT_SUPPORTED;
  }

  gst_adapter_unmap (adapter);
  return res;
}

static GstMatroskaParserResult
gst_matroska_parser_extract_data (GstMatroskaParser * parser,
    GstAdapter * adapter)
{
  GstMatroskaPointData point_data;
  GstMatroskaEbmlInfo embl_info;
  guint64 consume = 0;
  guint64 offset = parser->offset;
  GstMatroskaPointData *entry = NULL;
  GstMatroskaParserResult res = GST_MATROSKA_PARSER_OK;
  guint64 len = 0;
  guint8 *p_buf = 0;

  INITIALIZE_DEBUG_CATEGORY;
  len = gst_adapter_available (adapter);
  p_buf = (guint8 *) gst_adapter_map (adapter, len);
  while (TRUE) {
    /* Read one ebml */
    consume = gst_matroska_read_one_ebml_info (p_buf + offset, len, &embl_info);

    /* Adjust some element type to parse sub element information */
    if (consume) {
      if ((embl_info.id == GST_MATROSKA_ID_SEGMENT) ||
          (embl_info.id == GST_MATROSKA_ID_CUES) ||
          (embl_info.id == GST_MATROSKA_ID_CLUSTER)) {
        if (consume > embl_info.size) {
          consume -= embl_info.size;
        }
      }
    }

    /* Check data integrity */
    if (!consume || (len < consume)) {
      if (offset) {
        parser->offset = offset;
      }
      gst_adapter_unmap (adapter);

      GST_MATROSKA_PARSER_DEBUG
          ("insufficient data, offset = %" G_GUINT64_FORMAT
              ", remain len = %" G_GUINT64_FORMAT
              ", consume = %" G_GUINT64_FORMAT,
          offset, len, consume);
      return GST_MATROSKA_PARSER_OK;
    }

    /* Get current cue to fill array list */
    if (parser->array && parser->cue_point_num) {
      entry = &g_array_index (parser->array,
          GstMatroskaPointData, (parser->cue_point_num - 1));
    }

    switch (embl_info.id) {
      case GST_EBML_ID_HEADER:
      {
        GST_MATROSKA_PARSER_DEBUG ("id: embl header, size = %" G_GUINT64_FORMAT,
            embl_info.size);
        break;
      }
      case GST_MATROSKA_ID_SEGMENT:
      {
        parser->segment_offset = parser->consume;
        /* update consume, need to parse childs id */
        consume = embl_info.data_offset;
        GST_MATROSKA_PARSER_DEBUG ("id: segment, size = %" G_GUINT64_FORMAT, embl_info.size);
        break;
      }
      case GST_MATROSKA_ID_SEEKHEAD:
      {
        parser->segment_head_offset = parser->consume;
        GST_MATROSKA_PARSER_DEBUG
            ("id: segment seek head, offset = %" G_GUINT64_FORMAT ", size = %" G_GUINT64_FORMAT,
            parser->segment_head_offset, embl_info.size);
        break;
      }
      case GST_MATROSKA_ID_SEGMENTINFO:
      {
        /* update consume, need to parse childs id */
        consume = embl_info.data_offset;
        GST_MATROSKA_PARSER_DEBUG ("id: segment information, size = %" G_GUINT64_FORMAT,
            embl_info.size);
        break;
      }
      case GST_MATROSKA_ID_TIMECODESCALE:
      {
        parser->time_scale =
            gst_matroska_parser_read_data (embl_info.data_buf, embl_info.size);
        break;
      }
      case GST_MATROSKA_ID_DURATION:
      {
        parser->duration =
            gst_matroska_parser_read_data (embl_info.data_buf, embl_info.size);
        break;
      }
      case GST_MATROSKA_ID_CUES:
      {
        /* update consume, need to parse childs id */
        consume = embl_info.data_offset;
        parser->len = parser->consume + consume + embl_info.size;
        break;
      }
      case GST_MATROSKA_ID_POINTENTRY:
      {
        if (!parser->array) {
          parser->array =
              g_array_new (FALSE, TRUE, sizeof (GstMatroskaPointData));
        }

        memset (&point_data, 0, sizeof (GstMatroskaPointData));
        g_array_append_val (parser->array, point_data);
        parser->cue_point_num++;
        /* need to parse childs id */
        consume = embl_info.data_offset;
        GST_MATROSKA_PARSER_DEBUG ("id: cue point, num = %" G_GUINT64_FORMAT ", size = %" G_GUINT64_FORMAT,
            parser->cue_point_num, embl_info.size);
        break;
      }
      case GST_MATROSKA_ID_CUETIME:
      {
        if (entry) {
          entry->cue_time =
              gst_matroska_parser_read_data (embl_info.data_buf,
              embl_info.size);
        }
        break;
      }
      case GST_MATROSKA_ID_CUETRACKPOSITION:
      {
        /* need to parse childs id */
        consume = embl_info.data_offset;
        GST_MATROSKA_PARSER_DEBUG ("id: track position, num = %" G_GUINT64_FORMAT ", size = %" G_GUINT64_FORMAT,
            parser->cue_point_num, embl_info.size);
        break;
      }
      case GST_MATROSKA_ID_CUECLUSTERPOSITION:
      {
        if (entry) {
          entry->track_pos.cluster_pos =
              gst_matroska_parser_read_data (embl_info.data_buf,
              embl_info.size);
        }
        break;
      }
      case GST_MATROSKA_ID_CUETRACK:
      {
        if (entry) {
          entry->track_pos.track =
              gst_matroska_parser_read_data (embl_info.data_buf,
              embl_info.size);
        }
        break;
      }
      case GST_MATROSKA_ID_CLUSTER:
      {
        GST_MATROSKA_PARSER_DEBUG ("id: cluster, offset = %" G_GUINT64_FORMAT,
            parser->consume);
        gst_adapter_unmap (adapter);
        return GST_MATROSKA_PARSER_DONE;
      }
      default:
      {
        GST_MATROSKA_PARSER_DEBUG
            ("unhandled id = 0x%x, size = %" G_GUINT64_FORMAT
                ", data_offset = %" G_GUINT64_FORMAT
                ", conume = %" G_GUINT64_FORMAT,
            embl_info.id, embl_info.size, embl_info.data_offset,
            parser->consume);
        break;
      }
    }
    len -= consume;
    offset += consume;
    parser->consume += consume;

    /* Get all cues information */
    if (parser->len) {
      if (parser->consume >= parser->len) {
        parser->offset = offset;
        GST_MATROSKA_PARSER_DEBUG
            ("get all cues data, offset in adapter = %" G_GUINT64_FORMAT
            ", conume = %" G_GUINT64_FORMAT ", len = %" G_GUINT64_FORMAT,
            parser->offset, parser->consume, parser->len);
        res = GST_MATROSKA_PARSER_DONE;
        break;
      }
    }
  }

  gst_adapter_unmap (adapter);
  return res;
}

GstMatroskaParserResult
gst_matroska_parser_entry (GstMatroskaParser * parser, GstAdapter * adapter)
{
  GstMatroskaParserResult res = GST_MATROSKA_PARSER_OK;

  INITIALIZE_DEBUG_CATEGORY;
  if (!parser || !adapter) {
    GST_MATROSKA_PARSER_DEBUG ("gst_matroska_parser_entry: error param");
    return GST_MATROSKA_PARSER_NOT_SUPPORTED;
  }

  switch (parser->status) {
    case GST_MATROSKA_PARSER_STATUS_INIT:
    {
      gst_matroska_parser_init (parser);
      parser->status = GST_MATROSKA_PARSER_STATUS_HEADER;
    }
    case GST_MATROSKA_PARSER_STATUS_HEADER:
    {
      res =
          gst_matroska_parser_check_id (parser, GST_EBML_ID_HEADER, 4, adapter);
      if (res != GST_MATROSKA_PARSER_OK) {
        return res;
      }
      parser->status = GST_MATROSKA_PARSER_STATUS_DATA;
    }
    case GST_MATROSKA_PARSER_STATUS_DATA:
    {
      res = gst_matroska_parser_extract_data (parser, adapter);
      if (res != GST_MATROSKA_PARSER_DONE) {
        return res;
      }
      parser->status = GST_MATROSKA_PARSER_STATUS_FINISHED;
    }
    case GST_MATROSKA_PARSER_STATUS_FINISHED:
      break;
    default:
      break;
  }
  return res;
}
