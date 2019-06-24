/* GStreamer
 *
 * Copyright (C) 2016 Pexip AS
 *   @author Stian Selnes <stian@pexip.com>
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

#include <gst/check/check.h>
#include <gst/check/gstharness.h>

#define RTP_VP8_CAPS_STR \
  "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96"

static guint8 intra_picid6336_seqnum0[] = {
  0x80, 0xe0, 0x00, 0x00, 0x9a, 0xbb, 0xe3, 0xb3, 0x8b, 0xe9, 0x1d, 0x61,
  0x90, 0x80, 0x98, 0xc0, 0xf0, 0x07, 0x00, 0x9d, 0x01, 0x2a, 0xb0, 0x00,
  0x90, 0x00, 0x06, 0x47, 0x08, 0x85, 0x85, 0x88, 0x99, 0x84, 0x88, 0x21,
};
static guint8 intra_picid24_seqnum0[] = {
  0x80, 0xe0, 0x00, 0x00, 0x9a, 0xbb, 0xe3, 0xb3, 0x8b, 0xe9, 0x1d, 0x61,
  0x90, 0x80, 0x18, 0xf0, 0x07, 0x00, 0x9d, 0x01, 0x2a, 0xb0, 0x00,
  0x90, 0x00, 0x06, 0x47, 0x08, 0x85, 0x85, 0x88, 0x99, 0x84, 0x88, 0x21,
};
static guint8 intra_nopicid_seqnum0[] = {
  0x80, 0xe0, 0x00, 0x00, 0x9a, 0xbb, 0xe3, 0xb3, 0x8b, 0xe9, 0x1d, 0x61,
  0x90, 0x00, 0xf0, 0x07, 0x00, 0x9d, 0x01, 0x2a, 0xb0, 0x00,
  0x90, 0x00, 0x06, 0x47, 0x08, 0x85, 0x85, 0x88, 0x99, 0x84, 0x88, 0x21,
};

static GstBuffer *
create_rtp_vp8_buffer (guint seqnum, guint picid, gint picid_bits,
    GstClockTime buf_pts)
{
  GstBuffer *ret;
  guint8 *packet;
  gsize size;

  g_assert (picid_bits == 0 || picid_bits == 7 || picid_bits == 15);

  if (picid_bits == 0) {
    size = sizeof (intra_nopicid_seqnum0);
    packet = g_memdup (intra_nopicid_seqnum0, size);
  } else if (picid_bits == 7) {
    size = sizeof (intra_picid24_seqnum0);
    packet = g_memdup (intra_picid24_seqnum0, size);
    packet[14] = picid & 0x7f;
  } else {
    size = sizeof (intra_picid6336_seqnum0);
    packet = g_memdup (intra_picid6336_seqnum0, size);
    packet[14] = ((picid >> 8) & 0xff) | 0x80;
    packet[15] = (picid >> 0) & 0xff;
  }

  packet[2] = (seqnum >> 8) & 0xff;
  packet[3] = (seqnum >> 0) & 0xff;

  ret = gst_buffer_new_wrapped (packet, size);
  GST_BUFFER_PTS (ret) = buf_pts;
  return ret;
}

typedef struct _DepayGapEventTestData
{
  gint seq_num;
  gint picid;
  gint picid_bits;
} DepayGapEventTestData;

static void
test_depay_gap_event_base (const DepayGapEventTestData *data,
    gboolean send_lost_event, gboolean expect_gap_event)
{
  GstEvent *event;
  GstClockTime pts = 0;
  GstHarness *h = gst_harness_new ("rtpvp8depay");
  if (send_lost_event == FALSE && expect_gap_event) {
    /* Expect picture ID gaps to be concealed, so tell the element to do so. */
    g_object_set (h->element, "hide-picture-id-gap", TRUE, NULL);
  }
  gst_harness_set_src_caps_str (h, RTP_VP8_CAPS_STR);

  gst_harness_push (h, create_rtp_vp8_buffer (data[0].seq_num, data[0].picid, data[0].picid_bits, pts));
  pts += 33 * GST_MSECOND;

  // Preparation before pushing gap event. Getting rid of all events which
  // came by this point - segment, caps, etc
  for (gint i = 0; i < 3; i++)
    gst_event_unref (gst_harness_pull_event (h));
  fail_unless_equals_int (gst_harness_events_in_queue (h), 0);

  if (send_lost_event) {
    gst_harness_push_event (h, gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM,
        gst_structure_new ("GstRTPPacketLost",
            "timestamp", G_TYPE_UINT64, pts,
            "duration", G_TYPE_UINT64, 33*GST_MSECOND, NULL)));
    pts += 33 * GST_MSECOND;
  }

  gst_harness_push (h, create_rtp_vp8_buffer (data[1].seq_num, data[1].picid, data[1].picid_bits, pts));
  fail_unless_equals_int (2, gst_harness_buffers_received (h));

  if (expect_gap_event) {
    gboolean noloss = FALSE;

    // Making shure the GAP event was pushed downstream
    event = gst_harness_pull_event (h);
    fail_unless_equals_string ("gap", gst_event_type_get_name (GST_EVENT_TYPE (event)));
    gst_structure_get_boolean (gst_event_get_structure (event), "no-packet-loss", &noloss);

    // If we didn't send GstRTPPacketLost event, the gap
    // event should indicate that with 'no-packet-loss' parameter
    fail_unless_equals_int (noloss, !send_lost_event);
    gst_event_unref (event);
  }
  fail_unless_equals_int (gst_harness_events_in_queue (h), 0);

  gst_harness_teardown (h);
}

// Packet loss + no loss in picture ids
static const DepayGapEventTestData stop_gap_events_test_data[][2] = {
  // 7bit picture ids
  {{100, 24, 7}, {102, 25, 7}},

  // 15bit picture ids
  {{100, 250, 15}, {102, 251, 15}},

  // 7bit picture ids wrap
  {{100, 127, 7}, {102, 0, 7}},

  // 15bit picture ids wrap
  {{100, 32767, 15}, {102, 0, 15}},

  // 7bit to 15bit picture id
  {{100, 127, 7}, {102, 128, 15}},
};

GST_START_TEST (test_depay_stop_gap_events)
{
  test_depay_gap_event_base (&stop_gap_events_test_data[__i__][0], TRUE, FALSE);
}
GST_END_TEST;

// Packet loss + lost picture ids
static const DepayGapEventTestData resend_gap_event_test_data[][2] = {
  // 7bit picture ids
  {{100, 24, 7}, {102, 26, 7}},

  // 15bit picture ids
  {{100, 250, 15}, {102, 252, 15}},

  // 7bit picture ids wrap
  {{100, 127, 7}, {102, 1, 7}},

  // 15bit picture ids wrap
  {{100, 32767, 15}, {102, 1, 15}},

  // 7bit to 15bit picture id
  {{100, 126, 7}, {102, 129, 15}},
};

GST_START_TEST (test_depay_resend_gap_event)
{
  test_depay_gap_event_base (&resend_gap_event_test_data[__i__][0], TRUE, TRUE);
}
GST_END_TEST;

// Packet loss + one of picture ids does not exist
static const DepayGapEventTestData resend_gap_event_nopicid_test_data[][2] = {
  {{100, 24, 7}, {102, 0, 0}},
  {{100, 0, 0}, {102, 26, 7}},
  {{100, 0, 0}, {102, 0, 0}},
};

GST_START_TEST (test_depay_resend_gap_event_nopicid)
{
  test_depay_gap_event_base (&resend_gap_event_nopicid_test_data[__i__][0], TRUE, TRUE);
}
GST_END_TEST;

// No packet loss + lost picture ids
static const DepayGapEventTestData create_gap_event_on_picid_gaps_test_data[][2] = {
  // 7bit picture ids
  {{100, 24, 7}, {101, 26, 7}},

  // 15bit picture ids
  {{100, 250, 15}, {101, 252, 15}},

  // 7bit picture ids wrap
  {{100, 127, 7}, {101, 1, 7}},

  // 15bit picture ids wrap
  {{100, 32767, 15}, {101, 1, 15}},

  // 7bit to 15bit picture id
  {{100, 126, 7}, {101, 129, 15}},
};

GST_START_TEST (test_depay_create_gap_event_on_picid_gaps)
{
  test_depay_gap_event_base (&create_gap_event_on_picid_gaps_test_data[__i__][0], FALSE, TRUE);
}
GST_END_TEST;

// No packet loss + one of picture ids does not exist
static const DepayGapEventTestData nopicid_test_data[][2] = {
  {{100, 24, 7}, {101, 0, 0}},
  {{100, 0, 0}, {101, 26, 7}},
  {{100, 0, 0}, {101, 0, 0}},
};

GST_START_TEST (test_depay_nopicid)
{
  test_depay_gap_event_base (&nopicid_test_data[__i__][0], FALSE, FALSE);
}
GST_END_TEST;

/* PictureID emum is not exported */
enum PictureID {
  VP8_PAY_NO_PICTURE_ID = 0,
  VP8_PAY_PICTURE_ID_7BITS = 1,
  VP8_PAY_PICTURE_ID_15BITS = 2,
};

static const struct no_meta_test_data {
  /* control inputs */
  enum PictureID pid; /* picture ID type of test */
  gboolean vp8_payload_header_m_flag;

  /* expected outputs */
  guint vp8_payload_header_size;
  guint vp8_payload_control_value;
} no_meta_test_data[] = {
  { VP8_PAY_NO_PICTURE_ID,     FALSE , 1, 0x10}, /* no picture ID single byte header, S set */
  { VP8_PAY_PICTURE_ID_7BITS,  FALSE,  3, 0x90 }, /* X bit to allow for I bit means header is three bytes, S and X set */
  { VP8_PAY_PICTURE_ID_15BITS, TRUE,   4, 0x90 }, /* X bit to allow for I bit with M bit means header is four bytes, S, X and M set */

  /* repeated with non reference frame */
  { VP8_PAY_NO_PICTURE_ID,     FALSE , 1, 0x30}, /* no picture ID single byte header, S set */
  { VP8_PAY_PICTURE_ID_7BITS,  FALSE,  3, 0xB0 }, /* X bit to allow for I bit means header is three bytes, S and X set */
  { VP8_PAY_PICTURE_ID_15BITS, TRUE,   4, 0xB0 }, /* X bit to allow for I bit with M bit means header is four bytes, S, X and M set */
};

GST_START_TEST (test_pay_no_meta)
{
  guint8 vp8_bitstream_payload[] = {
    0x30, 0x00, 0x00, 0x9d, 0x01, 0x2a, 0xb0, 0x00, 0x90, 0x00, 0x06, 0x47,
    0x08, 0x85, 0x85, 0x88, 0x99, 0x84, 0x88, 0x21, 0x00
  };
  const struct no_meta_test_data *test_data = &no_meta_test_data[__i__];
  GstBuffer *buffer;
  GstMapInfo map = GST_MAP_INFO_INIT;
  GstHarness *h = gst_harness_new ("rtpvp8pay");
  gst_harness_set_src_caps_str (h, "video/x-vp8");

  /* check unknown picture id enum value */
  fail_unless (test_data->pid <= VP8_PAY_PICTURE_ID_15BITS);

  g_object_set (h->element, "picture-id-mode", test_data->pid,
      "picture-id-offset", 0x5A5A, NULL);

  buffer = gst_buffer_new_wrapped (g_memdup (vp8_bitstream_payload,
          sizeof (vp8_bitstream_payload)), sizeof (vp8_bitstream_payload));

  /* set droppable if N flag set */
  if ((test_data->vp8_payload_control_value & 0x20) != 0) {
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DROPPABLE);
  }

  buffer = gst_harness_push_and_pull (h, buffer);

  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READ));
  fail_unless (map.data != NULL);

  /* check buffer size and content */
  fail_unless_equals_int (map.size,
      12 + test_data->vp8_payload_header_size + sizeof (vp8_bitstream_payload));

  fail_unless_equals_int (test_data->vp8_payload_control_value, map.data[12]);

  if (test_data->vp8_payload_header_size > 2) {
    /* vp8 header extension byte must have I set */
    fail_unless_equals_int (0x80, map.data[13]);
    /* check picture id */
    if (test_data->pid == VP8_PAY_PICTURE_ID_7BITS)  {
      fail_unless_equals_int (0x5a, map.data[14]);
    } else if (test_data->pid == VP8_PAY_PICTURE_ID_15BITS) {
      fail_unless_equals_int (0xDA, map.data[14]);
      fail_unless_equals_int (0x5A, map.data[15]);
    }
  }

  gst_buffer_unmap (buffer, &map);
  gst_buffer_unref (buffer);

  gst_harness_teardown (h);
}
GST_END_TEST;

static const struct with_meta_test_data {
  /* control inputs */
  enum PictureID pid; /* picture ID type of test */
  gboolean vp8_payload_header_m_flag;
  gboolean use_temporal_scaling;
  gboolean y_flag;

  /* expected outputs */
  guint vp8_payload_header_size;
  guint vp8_payload_control_value;
  guint vp8_payload_extended_value;
} with_meta_test_data[] = {
  { VP8_PAY_NO_PICTURE_ID,     FALSE, FALSE, FALSE, 1, 0x10, 0x80 }, /* no picture ID single byte header, S set */
  { VP8_PAY_PICTURE_ID_7BITS,  FALSE, FALSE, FALSE, 3, 0x90, 0x80 }, /* X bit to allow for I bit means header is three bytes, S and X set */
  { VP8_PAY_PICTURE_ID_15BITS, TRUE,  FALSE, FALSE, 4, 0x90, 0x80 }, /* X bit to allow for I bit with M bit means header is four bytes, S, X and M set */
  { VP8_PAY_NO_PICTURE_ID,     FALSE, TRUE,  FALSE, 4, 0x90, 0x60 }, /* no picture ID single byte header, S set */
  { VP8_PAY_PICTURE_ID_7BITS,  FALSE, TRUE,  FALSE, 5, 0x90, 0xE0 }, /* X bit to allow for I bit means header is three bytes, S and X set */
  { VP8_PAY_PICTURE_ID_15BITS, TRUE,  TRUE,  FALSE, 6, 0x90, 0xE0 }, /* X bit to allow for I bit with M bit means header is four bytes, S, X and M set */
  { VP8_PAY_NO_PICTURE_ID,     FALSE, TRUE,  TRUE,  4, 0x90, 0x60 }, /* no picture ID single byte header, S set */
  { VP8_PAY_PICTURE_ID_7BITS,  FALSE, TRUE,  TRUE,  5, 0x90, 0xE0 }, /* X bit to allow for I bit means header is three bytes, S and X set */
  { VP8_PAY_PICTURE_ID_15BITS, TRUE,  TRUE,  TRUE,  6, 0x90, 0xE0 }, /* X bit to allow for I bit with M bit means header is four bytes, S, X and M set */

  /* repeated with non reference frame */
  { VP8_PAY_NO_PICTURE_ID,     FALSE, FALSE, FALSE, 1, 0x30, 0x80 }, /* no picture ID single byte header, S set */
  { VP8_PAY_PICTURE_ID_7BITS,  FALSE, FALSE, FALSE, 3, 0xB0, 0x80 }, /* X bit to allow for I bit means header is three bytes, S and X set */
  { VP8_PAY_PICTURE_ID_15BITS, TRUE,  FALSE, FALSE, 4, 0xB0, 0x80 }, /* X bit to allow for I bit with M bit means header is four bytes, S, X and M set */
  { VP8_PAY_NO_PICTURE_ID,     FALSE, TRUE,  FALSE, 4, 0xB0, 0x60 }, /* no picture ID single byte header, S set */
  { VP8_PAY_PICTURE_ID_7BITS,  FALSE, TRUE,  FALSE, 5, 0xB0, 0xE0 }, /* X bit to allow for I bit means header is three bytes, S and X set */
  { VP8_PAY_PICTURE_ID_15BITS, TRUE,  TRUE,  FALSE, 6, 0xB0, 0xE0 }, /* X bit to allow for I bit with M bit means header is four bytes, S, X and M set */
  { VP8_PAY_NO_PICTURE_ID,     FALSE, TRUE,  TRUE,  4, 0xB0, 0x60 }, /* no picture ID single byte header, S set */
  { VP8_PAY_PICTURE_ID_7BITS,  FALSE, TRUE,  TRUE,  5, 0xB0, 0xE0 }, /* X bit to allow for I bit means header is three bytes, S and X set */
  { VP8_PAY_PICTURE_ID_15BITS, TRUE,  TRUE,  TRUE,  6, 0xB0, 0xE0 }, /* X bit to allow for I bit with M bit means header is four bytes, S, X and M set */

};

GST_START_TEST (test_pay_with_meta)
{
  guint8 vp8_bitstream_payload[] = {
    0x30, 0x00, 0x00, 0x9d, 0x01, 0x2a, 0xb0, 0x00, 0x90, 0x00, 0x06, 0x47,
    0x08, 0x85, 0x85, 0x88, 0x99, 0x84, 0x88, 0x21, 0x00
  };
  const struct with_meta_test_data *test_data = &with_meta_test_data[__i__];
  GstBuffer *buffer;
  GstVideoVP8Meta *meta;
  GstMapInfo map = GST_MAP_INFO_INIT;
  GstHarness *h = gst_harness_new ("rtpvp8pay");
  gst_harness_set_src_caps_str (h, "video/x-vp8");

  /* check for unknown picture id enum value */
  fail_unless (test_data->pid <= VP8_PAY_PICTURE_ID_15BITS);

  g_object_set (h->element, "picture-id-mode", test_data->pid,
      "picture-id-offset", 0x5A5A, NULL);

  /* Push a buffer in */
  buffer = gst_buffer_new_wrapped (g_memdup (vp8_bitstream_payload,
          sizeof (vp8_bitstream_payload)), sizeof (vp8_bitstream_payload));
  gst_buffer_add_video_vp8_meta_full (buffer,
      test_data->use_temporal_scaling,
      test_data->y_flag, /* Y bit */
      2, /* set temporal layer ID */
      255); /* tl0picidx */

  /* set droppable if N flag set */
  if ((test_data->vp8_payload_control_value & 0x20) != 0) {
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DROPPABLE);
  }

  buffer = gst_harness_push_and_pull (h, buffer);

  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READ));
  fail_unless (map.data != NULL);

  meta = gst_buffer_get_video_vp8_meta (buffer);
  fail_unless (meta == NULL);

  /* check buffer size and content */
  fail_unless_equals_int (map.size,
      12 + test_data->vp8_payload_header_size + sizeof (vp8_bitstream_payload));
  fail_unless_equals_int (test_data->vp8_payload_control_value, map.data[12]);

  if (test_data->vp8_payload_header_size > 1) {
    int hdridx = 13;
    fail_unless_equals_int (test_data->vp8_payload_extended_value, map.data[hdridx++]);

    /* check picture ID */
    if (test_data->pid == VP8_PAY_PICTURE_ID_7BITS) {
      fail_unless_equals_int (0x5A, map.data[hdridx++]);
    } else if (test_data->pid == VP8_PAY_PICTURE_ID_15BITS) {
      fail_unless_equals_int (0xDA, map.data[hdridx++]);
      fail_unless_equals_int (0x5A, map.data[hdridx++]);
    }

    if (test_data->use_temporal_scaling) {
      /* check temporal layer 0 picture ID value */
      fail_unless_equals_int (255, map.data[hdridx++]);
      /* check temporal layer ID value */
      fail_unless_equals_int (2, (map.data[hdridx] >> 6) & 0x3);

      if (test_data->y_flag) {
        fail_unless_equals_int (1, (map.data[hdridx] >> 5) & 1);
      } else {
        fail_unless_equals_int (0, (map.data[hdridx] >> 5) & 1);
      }
    }
  }

  gst_buffer_unmap (buffer, &map);
  gst_buffer_unref (buffer);

  gst_harness_teardown (h);
}
GST_END_TEST;

GST_START_TEST (test_pay_contiuous_picture_id_and_tl0picidx)
{
  guint8 vp8_bitstream_payload[] = {
      0x30, 0x00, 0x00, 0x9d, 0x01, 0x2a, 0xb0, 0x00, 0x90, 0x00, 0x06, 0x47,
      0x08, 0x85, 0x85, 0x88, 0x99, 0x84, 0x88, 0x21, 0x00
  };
  GstHarness *h = gst_harness_new ("rtpvp8pay");
  const gint header_len_without_tl0picidx = 3;
  const gint header_len_with_tl0picidx = 5;
  const gint packet_len_without_tl0picidx = 12 + header_len_without_tl0picidx +
    sizeof (vp8_bitstream_payload);
  const gint packet_len_with_tl0picidx = 12 + header_len_with_tl0picidx +
    sizeof (vp8_bitstream_payload);
  const gint picid_offset = 14;
  const gint tl0picidx_offset = 15;
  GstBuffer *buffer;
  GstMapInfo map;

  g_object_set (h->element, "picture-id-mode", VP8_PAY_PICTURE_ID_7BITS,
      "picture-id-offset", 0, NULL);
  gst_harness_set_src_caps_str (h, "video/x-vp8");

  /* First, push a frame without temporal scalability meta */
  buffer = gst_buffer_new_from_array (vp8_bitstream_payload);
  buffer = gst_harness_push_and_pull (h, buffer);
  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READ));
  fail_unless_equals_int (map.size, packet_len_without_tl0picidx);
  fail_unless_equals_int (map.data[picid_offset], 0x00);
  gst_buffer_unmap (buffer, &map);
  gst_buffer_unref (buffer);

  /* Push a frame for temporal layer 0 with meta */
  buffer = gst_buffer_new_from_array (vp8_bitstream_payload);
  gst_buffer_add_video_vp8_meta_full (buffer, TRUE, TRUE, 0, 0);
  buffer = gst_harness_push_and_pull (h, buffer);
  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READ));
  fail_unless_equals_int (map.size, packet_len_with_tl0picidx);
  fail_unless_equals_int (map.data[picid_offset], 0x01);
  fail_unless_equals_int (map.data[tl0picidx_offset], 0x00);
  gst_buffer_unmap (buffer, &map);
  gst_buffer_unref (buffer);

  /* Push a frame for temporal layer 1 with meta */
  buffer = gst_buffer_new_from_array (vp8_bitstream_payload);
  gst_buffer_add_video_vp8_meta_full (buffer, TRUE, TRUE, 1, 0);
  buffer = gst_harness_push_and_pull (h, buffer);
  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READ));
  fail_unless_equals_int (map.size, packet_len_with_tl0picidx);
  fail_unless_equals_int (map.data[picid_offset], 0x02);
  fail_unless_equals_int (map.data[tl0picidx_offset], 0x00);
  gst_buffer_unmap (buffer, &map);
  gst_buffer_unref (buffer);

  /* Push next frame for temporal layer 0 with meta */
  buffer = gst_buffer_new_from_array (vp8_bitstream_payload);
  gst_buffer_add_video_vp8_meta_full (buffer, TRUE, TRUE, 0, 1);
  buffer = gst_harness_push_and_pull (h, buffer);
  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READ));
  fail_unless_equals_int (map.size, packet_len_with_tl0picidx);
  fail_unless_equals_int (map.data[picid_offset], 0x03);
  fail_unless_equals_int (map.data[tl0picidx_offset], 0x01);
  gst_buffer_unmap (buffer, &map);
  gst_buffer_unref (buffer);

  /* Another frame for temporal layer 0, but now the meta->tl0picidx has been
   * reset to 0 (simulating an encoder reset). Payload must ensure tl0picidx
   * is increasing. */
  buffer = gst_buffer_new_from_array (vp8_bitstream_payload);
  gst_buffer_add_video_vp8_meta_full (buffer, TRUE, TRUE, 0, 0);
  buffer = gst_harness_push_and_pull (h, buffer);
  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READ));
  fail_unless_equals_int (map.size, packet_len_with_tl0picidx);
  fail_unless_equals_int (map.data[picid_offset], 0x04);
  fail_unless_equals_int (map.data[tl0picidx_offset], 0x02);
  gst_buffer_unmap (buffer, &map);
  gst_buffer_unref (buffer);

  /* If we receive a frame without meta, we should continue to increase and
   * add tl0picidx (assuming TID=0) in order to maximize interop. */
  buffer = gst_buffer_new_from_array (vp8_bitstream_payload);
  buffer = gst_harness_push_and_pull (h, buffer);
  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READ));
  fail_unless_equals_int (map.size, packet_len_with_tl0picidx);
  fail_unless_equals_int (map.data[picid_offset], 0x05);
  fail_unless_equals_int (map.data[tl0picidx_offset], 0x03);
  gst_buffer_unmap (buffer, &map);
  gst_buffer_unref (buffer);

  gst_harness_teardown (h);
}
GST_END_TEST;

static Suite *
rtpvp8_suite (void)
{
  Suite *s = suite_create ("rtpvp8");
  TCase *tc_chain;

  suite_add_tcase (s, (tc_chain = tcase_create ("vp8depay")));
  tcase_add_loop_test (tc_chain, test_depay_stop_gap_events, 0, G_N_ELEMENTS (stop_gap_events_test_data));
  tcase_add_loop_test (tc_chain, test_depay_resend_gap_event, 0, G_N_ELEMENTS (resend_gap_event_test_data));
  tcase_add_loop_test (tc_chain, test_depay_resend_gap_event_nopicid, 0, G_N_ELEMENTS (resend_gap_event_nopicid_test_data));
  tcase_add_loop_test (tc_chain, test_depay_create_gap_event_on_picid_gaps, 0, G_N_ELEMENTS (create_gap_event_on_picid_gaps_test_data));
  tcase_add_loop_test (tc_chain, test_depay_nopicid, 0, G_N_ELEMENTS (nopicid_test_data));

  suite_add_tcase (s, (tc_chain = tcase_create ("vp8pay")));
  tcase_add_loop_test (tc_chain, test_pay_no_meta, 0 , G_N_ELEMENTS(no_meta_test_data));
  tcase_add_loop_test (tc_chain, test_pay_with_meta, 0 , G_N_ELEMENTS(with_meta_test_data));
  tcase_add_test (tc_chain, test_pay_contiuous_picture_id_and_tl0picidx);

  return s;
}

GST_CHECK_MAIN (rtpvp8);