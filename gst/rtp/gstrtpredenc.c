/* GStreamer plugin for forward error correction
 * Copyright (C) 2017 Pexip
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Author: Mikhail Fludkov <misha@pexip.com>
 */

#include <gst/rtp/gstrtpbuffer.h>
#include <string.h>
#include <stdio.h>

#include "rtpredcommon.h"
#include "gstrtpredenc.h"

typedef struct
{
  guint8 pt;
  guint32 timestamp;
  GstBuffer *payload;
} RTPHistItem;

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp"));

#define DEFAULT_PT                  (0)
#define DEFAULT_DISTANCE            (0)
#define DEFAULT_ALLOW_NO_RED_BLOCKS (FALSE)

GST_DEBUG_CATEGORY_STATIC (gst_rtp_red_enc_debug);
#define GST_CAT_DEFAULT (gst_rtp_red_enc_debug)

G_DEFINE_TYPE (GstRtpRedEnc, gst_rtp_red_enc, GST_TYPE_ELEMENT);

enum
{
  PROP_0,
  PROP_PT,
  PROP_SENT,
  PROP_DISTANCE,
  PROP_ALLOW_NO_RED_BLOCKS
};

static void
rtp_hist_item_init (RTPHistItem * item, GstRTPBuffer * rtp,
    GstBuffer * rtp_payload)
{
  item->pt = gst_rtp_buffer_get_payload_type (rtp);
  item->timestamp = gst_rtp_buffer_get_timestamp (rtp);
  item->payload = rtp_payload;
}

static RTPHistItem *
rtp_hist_item_new (GstRTPBuffer * rtp, GstBuffer * rtp_payload)
{
  RTPHistItem *item = g_slice_new0 (RTPHistItem);
  rtp_hist_item_init (item, rtp, rtp_payload);
  return item;
}

static void
rtp_hist_item_replace (RTPHistItem * item, GstRTPBuffer * rtp,
    GstBuffer * rtp_payload)
{
  gst_buffer_unref (item->payload);
  rtp_hist_item_init (item, rtp, rtp_payload);
}

static void
rtp_hist_item_free (gpointer _item)
{
  RTPHistItem *item = _item;
  gst_buffer_unref (item->payload);
  g_slice_free (RTPHistItem, item);
}

static GstEvent *
_create_caps_event (const GstCaps * caps, guint8 pt)
{
  GstEvent *ret;
  GstCaps *new = gst_caps_copy (caps);
  GstStructure *s = gst_caps_get_structure (new, 0);
  gst_structure_set (s, "payload", G_TYPE_INT, pt, NULL);
  GST_INFO ("sinkcaps %" GST_PTR_FORMAT ", srccaps %" GST_PTR_FORMAT,
      caps, new);
  ret = gst_event_new_caps (new);
  gst_caps_unref (new);
  return ret;
}

static GstBuffer *
_alloc_red_packet_and_fill_headers (GstRtpRedEnc * self,
    RTPHistItem * redundant_block, GstRTPBuffer * inp_rtp)
{
  guint red_header_size = rtp_red_block_header_get_length (FALSE) +
      (redundant_block ? rtp_red_block_header_get_length (TRUE) : 0);

  guint32 timestmap = gst_rtp_buffer_get_timestamp (inp_rtp);
  guint csrc_count = gst_rtp_buffer_get_csrc_count (inp_rtp);
  GstBuffer *red = gst_rtp_buffer_new_allocate (red_header_size, 0, csrc_count);
  guint8 *red_block_header;
  GstRTPBuffer red_rtp = GST_RTP_BUFFER_INIT;

  if (!gst_rtp_buffer_map (red, GST_MAP_WRITE, &red_rtp))
    g_assert_not_reached ();

  /* Copying RTP header of incoming packet */
  if (gst_rtp_buffer_get_extension (inp_rtp))
    GST_WARNING_OBJECT (self, "FIXME: Ignoring RTP extension");

  gst_rtp_buffer_set_marker (&red_rtp, gst_rtp_buffer_get_marker (inp_rtp));
  gst_rtp_buffer_set_payload_type (&red_rtp, self->pt);
  gst_rtp_buffer_set_seq (&red_rtp, gst_rtp_buffer_get_seq (inp_rtp));
  gst_rtp_buffer_set_timestamp (&red_rtp, timestmap);
  gst_rtp_buffer_set_ssrc (&red_rtp, gst_rtp_buffer_get_ssrc (inp_rtp));
  for (guint i = 0; i != csrc_count; ++i)
    gst_rtp_buffer_set_csrc (&red_rtp, i,
        gst_rtp_buffer_get_csrc ((inp_rtp), i));

  /* Filling RED block headers */
  red_block_header = gst_rtp_buffer_get_payload (&red_rtp);
  if (redundant_block) {
    rtp_red_block_set_is_redundant (red_block_header, TRUE);
    rtp_red_block_set_payload_type (red_block_header, redundant_block->pt);
    rtp_red_block_set_timestamp_offset (red_block_header,
        timestmap - redundant_block->timestamp);
    rtp_red_block_set_payload_length (red_block_header,
        gst_buffer_get_size (redundant_block->payload));

    red_block_header += rtp_red_block_header_get_length (TRUE);
  }
  rtp_red_block_set_is_redundant (red_block_header, FALSE);
  rtp_red_block_set_payload_type (red_block_header,
      gst_rtp_buffer_get_payload_type (inp_rtp));

  gst_rtp_buffer_unmap (&red_rtp);

  gst_buffer_copy_into (red, inp_rtp->buffer, GST_BUFFER_COPY_METADATA, 0, -1);
  return red;
}

static GstBuffer *
_create_red_packet (GstRtpRedEnc * self,
    GstRTPBuffer * rtp, RTPHistItem * redundant_block, GstBuffer * main_block)
{
  GstBuffer *red =
      _alloc_red_packet_and_fill_headers (self, redundant_block, rtp);
  if (redundant_block)
    red = gst_buffer_append (red, gst_buffer_ref (redundant_block->payload));
  red = gst_buffer_append (red, gst_buffer_ref (main_block));
  return red;
}

static RTPHistItem *
_red_history_get_redundant_block (GstRtpRedEnc * self,
    guint32 current_timestamp, guint distance)
{
  RTPHistItem *item;
  gint32 timestamp_offset;

  if (0 == distance || 0 == self->rtp_history->length)
    return NULL;

  item = self->rtp_history->tail->data;
  timestamp_offset = current_timestamp - item->timestamp;
  if (G_UNLIKELY (timestamp_offset > RED_BLOCK_TIMESTAMP_OFFSET_MAX)) {
    GST_WARNING_OBJECT (self,
        "Can't create redundant block with distance %u, "
        "timestamp offset is too large %d (%u - %u) > %u",
        distance, timestamp_offset, current_timestamp, item->timestamp,
        RED_BLOCK_TIMESTAMP_OFFSET_MAX);
    return NULL;
  }

  if (G_UNLIKELY (timestamp_offset < 0)) {
    GST_WARNING_OBJECT (self,
        "Can't create redundant block with distance %u, "
        "timestamp offset is negative %d (%u - %u)",
        distance, timestamp_offset, current_timestamp, item->timestamp);
    return NULL;
  }

  if (G_UNLIKELY (gst_buffer_get_size (item->payload) > RED_BLOCK_LENGTH_MAX)) {
    GST_WARNING_OBJECT (self,
        "Can't create redundant block with distance %u, "
        "red block is too large %u > %u",
        distance, (guint) gst_buffer_get_size (item->payload),
        RED_BLOCK_LENGTH_MAX);
    return NULL;
  }

  /* _red_history_trim should take care it never happens */
  g_assert_cmpint (self->rtp_history->length, <=, distance);

  if (G_UNLIKELY (self->rtp_history->length < distance))
    GST_DEBUG_OBJECT (self,
        "Don't have enough buffers yet, "
        "adding redundant block with distance %u and timestamp %u",
        self->rtp_history->length, item->timestamp);
  return item;
}

static void
_red_history_prepend (GstRtpRedEnc * self,
    GstRTPBuffer * rtp, GstBuffer * rtp_payload, guint max_history_length)
{
  GList *link;

  if (0 == max_history_length) {
    if (rtp_payload)
      gst_buffer_unref (rtp_payload);
    return;
  }

  g_assert (NULL != rtp_payload);

  if (self->rtp_history->length >= max_history_length) {
    link = g_queue_pop_tail_link (self->rtp_history);
    rtp_hist_item_replace (link->data, rtp, rtp_payload);
  } else {
    link = g_list_alloc ();
    link->data = rtp_hist_item_new (rtp, rtp_payload);
  }
  g_queue_push_head_link (self->rtp_history, link);
}

static void
_red_history_trim (GstRtpRedEnc * self, guint max_history_length)
{
  while (max_history_length < self->rtp_history->length)
    rtp_hist_item_free (g_queue_pop_tail (self->rtp_history));
}

static GstFlowReturn
_pad_push (GstRtpRedEnc * self, GstBuffer * buffer, gboolean is_red)
{
  if (self->send_caps || is_red != self->is_current_caps_red) {
    GstEvent *event;
    GstCaps *caps = gst_pad_get_current_caps (self->sinkpad);
    if (is_red)
      event = _create_caps_event (caps, self->pt);
    else
      event = gst_event_new_caps (caps);
    gst_caps_unref (caps);

    gst_pad_push_event (self->srcpad, event);
    self->send_caps = FALSE;
    self->is_current_caps_red = is_red;
  }
  return gst_pad_push (self->srcpad, buffer);
}

static GstFlowReturn
_push_nonred_packet (GstRtpRedEnc * self,
    GstRTPBuffer * rtp, GstBuffer * buffer, guint distance)
{
  GstBuffer *main_block = distance > 0 ?
      gst_rtp_buffer_get_payload_buffer (rtp) : NULL;
  _red_history_prepend (self, rtp, main_block, distance);

  gst_rtp_buffer_unmap (rtp);
  return _pad_push (self, buffer, FALSE);
}

static GstFlowReturn
_push_red_packet (GstRtpRedEnc * self,
    GstRTPBuffer * rtp, GstBuffer * buffer, RTPHistItem * redundant_block,
    guint distance)
{
  GstBuffer *main_block = gst_rtp_buffer_get_payload_buffer (rtp);
  GstBuffer *red_buffer =
      _create_red_packet (self, rtp, redundant_block, main_block);

  _red_history_prepend (self, rtp, main_block, distance);
  gst_rtp_buffer_unmap (rtp);
  gst_buffer_unref (buffer);

  self->num_sent++;
  return _pad_push (self, red_buffer, TRUE);
}

static GstFlowReturn
gst_rtp_red_enc_chain (GstPad G_GNUC_UNUSED * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstRtpRedEnc *self = GST_RTP_RED_ENC (parent);
  guint distance = self->distance;
  guint only_with_redundant_data = !self->allow_no_red_blocks;
  RTPHistItem *redundant_block;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;

  /* We need to "trim" the history if 'distance' property has changed */
  _red_history_trim (self, distance);

  if (0 == distance && only_with_redundant_data)
    return _pad_push (self, buffer, FALSE);

  if (!gst_rtp_buffer_map (buffer, GST_MAP_READ, &rtp))
    return _pad_push (self, buffer, self->is_current_caps_red);

  /* If can't get data for redundant block push the packet as is */
  redundant_block = _red_history_get_redundant_block (self,
      gst_rtp_buffer_get_timestamp (&rtp), distance);
  if (NULL == redundant_block && only_with_redundant_data)
    return _push_nonred_packet (self, &rtp, buffer, distance);

  /* About to create RED packet with or without redundant data */
  return _push_red_packet (self, &rtp, buffer, redundant_block, distance);
}

static gboolean
gst_rtp_red_enc_event_sink (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstRtpRedEnc *self = GST_RTP_RED_ENC (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      gboolean replace_with_red_caps =
          self->is_current_caps_red || self->allow_no_red_blocks;

      if (replace_with_red_caps) {
        GstCaps *caps;
        gst_event_parse_caps (event, &caps);
        gst_event_take (&event, _create_caps_event (caps, self->pt));

        self->is_current_caps_red = TRUE;
      }
      break;
    }
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

static void
gst_rtp_red_enc_dispose (GObject * obj)
{
  GstRtpRedEnc *self = GST_RTP_RED_ENC (obj);

  g_queue_free_full (self->rtp_history, rtp_hist_item_free);

  G_OBJECT_CLASS (gst_rtp_red_enc_parent_class)->dispose (obj);
}

static void
gst_rtp_red_enc_init (GstRtpRedEnc * self)
{
  GstPadTemplate *pad_template;

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (self), "src");
  self->srcpad = gst_pad_new_from_template (pad_template, "src");
  gst_element_add_pad (GST_ELEMENT_CAST (self), self->srcpad);

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (self), "sink");
  self->sinkpad = gst_pad_new_from_template (pad_template, "sink");
  gst_pad_set_chain_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_red_enc_chain));
  gst_pad_set_event_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_red_enc_event_sink));
  GST_PAD_SET_PROXY_CAPS (self->sinkpad);
  GST_PAD_SET_PROXY_ALLOCATION (self->sinkpad);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

  self->pt = DEFAULT_PT;
  self->distance = DEFAULT_DISTANCE;
  self->allow_no_red_blocks = DEFAULT_ALLOW_NO_RED_BLOCKS;
  self->num_sent = 0;
  self->rtp_history = g_queue_new ();
}


static void
gst_rtp_red_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRtpRedEnc *self = GST_RTP_RED_ENC (object);
  switch (prop_id) {
    case PROP_PT:
    {
      gint prev_pt = self->pt;
      self->pt = g_value_get_int (value);
      self->send_caps = self->pt != prev_pt && self->is_current_caps_red;
    }
      break;
    case PROP_DISTANCE:
      self->distance = g_value_get_uint (value);
      break;
    case PROP_ALLOW_NO_RED_BLOCKS:
      self->allow_no_red_blocks = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_red_enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRtpRedEnc *self = GST_RTP_RED_ENC (object);
  switch (prop_id) {
    case PROP_PT:
      g_value_set_int (value, self->pt);
      break;
    case PROP_SENT:
      g_value_set_uint (value, self->num_sent);
      break;
    case PROP_DISTANCE:
      g_value_set_uint (value, self->distance);
      break;
    case PROP_ALLOW_NO_RED_BLOCKS:
      g_value_set_boolean (value, self->allow_no_red_blocks);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_red_enc_class_init (GstRtpRedEncClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));

  gst_element_class_set_metadata (element_class,
      "Redundant Audio Data (RED) Encoder",
      "Codec/Payloader/Network/RTP",
      "Encode Redundant Audio Data (RED)",
      "Hani Mustafa <hani@pexip.com>, Mikhail Fludkov <misha@pexip.com>");

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_rtp_red_enc_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_rtp_red_enc_get_property);
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_rtp_red_enc_dispose);

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_PT,
      g_param_spec_int ("pt", "payload type",
          "Payload type FEC packets (-1 disable)",
          0, 127, DEFAULT_PT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_SENT,
      g_param_spec_uint ("sent", "Sent",
          "Count of sent packets",
          0, G_MAXUINT32, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_DISTANCE,
      g_param_spec_uint ("distance", "RED distance",
          "Tells which media packet to use as a redundant block "
          "(0 - no redundant blocks, 1 to use previous packet, "
          "2 to use the packet before previous, etc.)",
          0, G_MAXUINT32, DEFAULT_DISTANCE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_ALLOW_NO_RED_BLOCKS, g_param_spec_boolean ("allow-no-red-blocks",
          "Allow no redundant blocks",
          "true - can produce RED packets even without redundant blocks (distance==0) "
          "false - RED packets will be produced only if distance>0",
          DEFAULT_ALLOW_NO_RED_BLOCKS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (gst_rtp_red_enc_debug, "rtpredenc", 0,
      "RTP RED Encoder");
}