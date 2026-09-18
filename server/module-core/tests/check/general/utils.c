/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include "kmsutils.h"
#include "sdp_utils.h"

#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <glib.h>

/* *INDENT-OFF* */
static const gchar *sdp_str = "v=0\r\n"
    "o=- 0 0 IN IP4 0.0.0.0\r\n"
    "s=TestSession\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "t=2873397496 2873404696\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 100 116 117 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:100 VP8/90000\r\n"
    "a=rtpmap:116 red/90000\r\n"
    "a=rtpmap:117 ulpfec/90000\r\n"
    "a=rtpmap:96 rtx/90000\r\n"
    "a=fmtp:96 apt=100\r\n"
    "a=rtcp:9 IN IP4 0.0.0.0\r\n"
    "a=rtcp-fb:100 ccm fir\r\n"
    "a=rtcp-fb:100 nack\r\n"
    "a=rtcp-fb:100 nack pli\r\n"
    "a=rtcp-fb:100 goog-remb\r\n"
    "a=extmap:2 urn:ietf:params:rtp-hdrext:toffset\r\n"
    "a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
    "a=extmap:4 urn:3gpp:video-orientation\r\n"
    "a=setup:actpass\r\n"
    "a=mid:video-1733429841\r\n"
    "a=msid:nnnwYrPTpGmyoJX5GFHMVv42y1ZthbnCx26c 9203939c-25cf-4d60-82c2-d25b19350926\r\n"
    "a=sendrecv\r\n"
    "a=ice-ufrag:xHOGnBsKDPCmHB5t\r\n"
    "a=ice-pwd:qpnbhhoyeTrypBkX5F1u338T\r\n"
    "a=fingerprint:sha-256 58:E0:FE:56:6A:8C:5A:AD:71:5B:A0:52:47:27:60:66:27:53:EC:B6:F3:03:A8:4B:9B:30:28:62:29:49:C6:73\r\n"
    "a=ssrc:1733429841 cname:5YcASuDc3X86mu+d\r\n"
    "a=ssrc:1733429841 mslabel:nnnwYrPTpGmyoJX5GFHMVv42y1ZthbnCx26c\r\n"
    "a=ssrc:1733429841 label:9203939c-25cf-4d60-82c2-d25b19350926\r\n"
    "a=ssrc:2560713622 cname:5YcASuDc3X86mu+d\r\n"
    "a=ssrc:2560713622 mslabel:nnnwYrPTpGmyoJX5GFHMVv42y1ZthbnCx26c\r\n"
    "a=ssrc:2560713622 label:9203939c-25cf-4d60-82c2-d25b19350926\r\n"
    "a=ssrc-group:FID 2560713622 1733429841\r\n"
    "a=rtcp-mux\r\n";
/* *INDENT-ON* */

GST_START_TEST (check_sdp_utils_media_get_fid_ssrc)
{
  GstSDPMessage *message;
  const GstSDPMedia *media;
  guint ssrc;

  fail_unless (gst_sdp_message_new (&message) == GST_SDP_OK);
  fail_unless (gst_sdp_message_parse_buffer ((const guint8 *)
          sdp_str, -1, message) == GST_SDP_OK);

  media = gst_sdp_message_get_media (message, 0);
  fail_if (media == NULL);

  ssrc = sdp_utils_media_get_fid_ssrc (media, 0);
  fail_if (ssrc != 2560713622);

  ssrc = sdp_utils_media_get_ssrc (media);
  fail_if (ssrc != 1733429841);

  gst_sdp_message_free (message);
}

GST_END_TEST;

GMainLoop *loop = NULL;
gint callbacks = 2;
gint destroy_count = 0;

static gboolean
quit_main_loop_idle (gpointer data)
{
  g_main_loop_quit (loop);
  return FALSE;
}

static gboolean
test_event_function (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gint *var = pad->eventdata;

  GST_DEBUG_OBJECT (pad, "Event received: %d", *var);

  /* Decrement var data */
  (*var)--;

  if (g_atomic_int_dec_and_test (&callbacks)) {
    /* Finish test */
    g_idle_add (quit_main_loop_idle, NULL);
  }

  gst_event_unref (event);

  return TRUE;
}

static gboolean
test_query_function (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gint *var = pad->querydata;

  GST_DEBUG_OBJECT (pad, "Query received: %d", *var);

  /* Decrement var data */
  (*var)--;

  if (g_atomic_int_dec_and_test (&callbacks)) {
    /* Finish test */
    g_idle_add (quit_main_loop_idle, NULL);
  }

  return FALSE;
}

static void
test_destroy_function (gpointer data)
{
  g_atomic_int_inc (&destroy_count);
}

GST_START_TEST (check_kms_utils_set_pad_event_function_full)
{
  GstElement *e = gst_element_factory_make ("fakesink", NULL);
  GstElement *pipeline = gst_pipeline_new (NULL);
  gint v1 = 1, v2 = 2;
  GstPad *pad;

  loop = g_main_loop_new (NULL, FALSE);

  pad = gst_element_get_static_pad (e, "sink");

  /* overwrite previous event function */
  kms_utils_set_pad_event_function_full (pad, test_event_function, &v1,
      test_destroy_function, FALSE);
  /* chain with previous event function */
  kms_utils_set_pad_event_function_full (pad, test_event_function, &v2,
      test_destroy_function, TRUE);

  gst_bin_add (GST_BIN (pipeline), e);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* Test chained callbacks */
  fail_if (!gst_pad_send_event (pad, gst_event_new_eos ()));
  g_object_unref (pad);

  GST_DEBUG ("Pipeline running");
  g_main_loop_run (loop);

  GST_DEBUG ("Test finished");

  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_object_unref (pipeline);
  g_main_loop_unref (loop);

  fail_if (destroy_count != 2);

  /* Check manipullation of data */
  fail_if (v1 != 0);
  fail_if (v2 != 1);
}

GST_END_TEST;

GST_START_TEST (check_kms_utils_set_pad_query_function_full)
{
  GstElement *e = gst_element_factory_make ("fakesink", NULL);
  GstElement *pipeline = gst_pipeline_new (NULL);
  gint v1 = 1, v2 = 2;
  GstPad *pad;
  GstCaps *query_caps;

  loop = g_main_loop_new (NULL, FALSE);

  pad = gst_element_get_static_pad (e, "sink");

  /* overwrite previous query function */
  kms_utils_set_pad_query_function_full (pad, test_query_function, &v1,
      test_destroy_function, TRUE);
  /* chain with previous query function */
  kms_utils_set_pad_query_function_full (pad, test_query_function, &v2,
      test_destroy_function, TRUE);

  g_object_unref (pad);

  gst_bin_add (GST_BIN (pipeline), e);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* Test chained callbacks */
  query_caps = gst_pad_query_caps (pad, NULL);
  gst_caps_unref (query_caps);

  GST_DEBUG ("Pipeline running");
  g_main_loop_run (loop);

  GST_DEBUG ("Test finished");

  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_object_unref (pipeline);
  g_main_loop_unref (loop);

  fail_if (destroy_count != 2);

  /* Check manipullation of data */
  fail_if (v1 != 0);
  fail_if (v2 != 1);
}

GST_END_TEST;

GST_START_TEST (check_kms_utils_drop_until_keyframe_buffer)
{
  GstElement *identity = gst_element_factory_make ("identity", NULL);

  GstHarness *h = gst_harness_new_with_element (identity, "sink", "src");
  gst_harness_set_src_caps_str (h, "mycaps");

  GstBuffer *buf, *out_buf;
  GstPad *srcpad;

  srcpad = gst_element_get_static_pad (identity, "src");
  kms_utils_drop_until_keyframe (srcpad, TRUE);
  g_object_unref (srcpad);

  buf = gst_harness_create_buffer (h, 50);
  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
  gst_harness_push (h, buf);
  out_buf = gst_harness_try_pull (h);
  fail_unless (out_buf == NULL);

  buf = gst_harness_create_buffer (h, 50);
  gst_harness_push (h, buf);
  out_buf = gst_harness_try_pull (h);
  fail_unless (out_buf == buf);
  gst_buffer_unref (buf);

  buf = gst_harness_create_buffer (h, 50);
  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
  gst_harness_push (h, buf);
  out_buf = gst_harness_try_pull (h);
  fail_unless (out_buf == buf);
  gst_buffer_unref (buf);

  gst_harness_teardown (h);
  g_object_unref (identity);
}

GST_END_TEST;

GstFlowReturn
check_chain_list_func (GstPad * pad, GstObject * parent, GstBufferList * list)
{
  *(GstBufferList **) (pad->chainlistdata) = list;

  return GST_FLOW_OK;
}

GST_START_TEST (check_kms_utils_drop_until_keyframe_bufferlist)
{
  GstBuffer *buf, *buf1, *buf2, *buf3;
  GstBufferList *bufflist;
  GstPad *sinkpad, *srcpad;
  GstPadLinkReturn plr;
  GstBufferList *received_bufflist;
  GstEvent *start_event;
  GstEvent *segment_event;
  GstSegment *segment;

  srcpad = gst_pad_new ("src", GST_PAD_SRC);
  fail_if (srcpad == NULL);
  gst_pad_set_active (srcpad, TRUE);
  kms_utils_drop_until_keyframe (srcpad, TRUE);

  sinkpad = gst_pad_new ("sink", GST_PAD_SINK);
  fail_if (sinkpad == NULL);
  gst_pad_set_chain_function (sinkpad, gst_check_chain_func);
  gst_pad_set_chain_list_function_full (sinkpad, check_chain_list_func,
      &received_bufflist, NULL);
  gst_pad_set_active (sinkpad, TRUE);

  plr = gst_pad_link (srcpad, sinkpad);
  fail_unless (GST_PAD_LINK_SUCCESSFUL (plr));

  start_event = gst_event_new_stream_start ("default");
  segment = gst_segment_new ();
  gst_segment_init (segment, GST_FORMAT_DEFAULT);
  segment_event = gst_event_new_segment  (segment);
  gst_pad_push_event (srcpad, start_event);
  gst_pad_push_event (srcpad, segment_event);

  GST_DEBUG ("Drop entire list");
  bufflist = gst_buffer_list_new ();
  buf = gst_buffer_new ();
  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
  gst_buffer_list_add (bufflist, buf);
  received_bufflist = NULL;
  gst_pad_push_list (srcpad, bufflist);
  fail_if (received_bufflist != NULL);

  GST_DEBUG ("Drop entire list");
  bufflist = gst_buffer_list_new ();
  buf = gst_buffer_new ();
  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
  gst_buffer_list_add (bufflist, buf);
  buf = gst_buffer_new ();
  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
  gst_buffer_list_add (bufflist, buf);
  received_bufflist = NULL;
  gst_pad_push_list (srcpad, bufflist);
  fail_if (received_bufflist != NULL);

  GST_DEBUG ("Keep bufferlist, drop first buffers until keyframe");
  bufflist = gst_buffer_list_new ();
  buf1 = gst_buffer_new ();
  GST_BUFFER_FLAG_SET (buf1, GST_BUFFER_FLAG_DELTA_UNIT);
  gst_buffer_list_add (bufflist, buf1);
  buf2 = gst_buffer_new ();
  gst_buffer_list_add (bufflist, buf2);
  buf3 = gst_buffer_new ();
  GST_BUFFER_FLAG_SET (buf3, GST_BUFFER_FLAG_DELTA_UNIT);
  gst_buffer_list_add (bufflist, buf3);
  received_bufflist = NULL;
  gst_pad_push_list (srcpad, bufflist);
  fail_unless (received_bufflist != NULL);
  fail_unless (gst_buffer_list_length (received_bufflist) == 2);
  fail_unless (gst_buffer_list_get (received_bufflist, 0) == buf2);
  fail_unless (gst_buffer_list_get (received_bufflist, 1) == buf3);
  gst_buffer_list_unref (received_bufflist);

  kms_utils_drop_until_keyframe (srcpad, TRUE);

  GST_DEBUG ("Keep bufferlist, the first buffer is keyfram");
  bufflist = gst_buffer_list_new ();
  buf1 = gst_buffer_new ();
  gst_buffer_list_add (bufflist, buf1);
  buf2 = gst_buffer_new ();
  GST_BUFFER_FLAG_SET (buf2, GST_BUFFER_FLAG_DELTA_UNIT);
  gst_buffer_list_add (bufflist, buf2);
  buf3 = gst_buffer_new ();
  GST_BUFFER_FLAG_SET (buf3, GST_BUFFER_FLAG_DELTA_UNIT);
  gst_buffer_list_add (bufflist, buf3);
  received_bufflist = NULL;
  gst_pad_push_list (srcpad, bufflist);
  fail_unless (received_bufflist != NULL);
  fail_unless (gst_buffer_list_length (received_bufflist) == 3);
  fail_unless (gst_buffer_list_get (received_bufflist, 0) == buf1);
  fail_unless (gst_buffer_list_get (received_bufflist, 1) == buf2);
  fail_unless (gst_buffer_list_get (received_bufflist, 2) == buf3);
  gst_buffer_list_unref (received_bufflist);


  gst_segment_free (segment);
  gst_pad_unlink (srcpad, sinkpad);
  gst_object_unref (srcpad);
  gst_object_unref (sinkpad);
}

GST_END_TEST;

/* ---- Depayloader PTS out ------------------------------------------------ */

/* A depayloader is destroyed and rebuilt on every SSRC change; only the
 * KmsPtsTracker crosses the switch. These tests reproduce that literally: one
 * stream is pushed through a harness holding the tracker, that harness is torn
 * down, and the next stream is pushed through a fresh one sharing the same
 * tracker. Nothing about the internals is simulated. */

#define PTS_TEST_PACKET (20 * GST_MSECOND)

typedef struct _PtsRun
{
  guint buffers;                /* buffers pulled */
  guint until_converged;        /* buffer at which out caught up with in, 1-based */
  guint smallest_step;          /* smallest gap between consecutive emitted PTS */
  gboolean increasing;          /* emitted PTS strictly increased throughout */
  GstClockTime last_out;
} PtsRun;

/* Push `count` buffers `PTS_TEST_PACKET` apart starting at `start`, through a
 * depayloader sharing `tracker`. A gap of `gap_len` is inserted before buffer
 * `gap_at` (G_MAXUINT for none). With `no_duration`, buffers carry no duration,
 * as rtpopusdepay's do. */
static PtsRun
pts_push_stream (KmsPtsTracker * tracker, GstClockTime start, guint count,
    guint gap_at, GstClockTime gap_len, gboolean no_duration)
{
  GstElement *identity = gst_element_factory_make ("identity", NULL);
  GstHarness *h = gst_harness_new_with_element (identity, "sink", "src");
  PtsRun run = { 0, 0, G_MAXUINT, TRUE, GST_CLOCK_TIME_NONE };
  GstClockTime in = start;
  GstClockTime prev_out = GST_CLOCK_TIME_NONE;
  guint i;

  gst_harness_set_src_caps_str (h, "mycaps");
  kms_utils_depayloader_monitor_pts_out_tracked (identity, tracker);

  for (i = 0; i < count; i++) {
    GstBuffer *buf = gst_harness_create_buffer (h, 50);
    GstBuffer *out;
    GstClockTime pts_in;

    if (i == gap_at) {
      in += gap_len;
    }
    pts_in = in;

    GST_BUFFER_PTS (buf) = pts_in;
    GST_BUFFER_DURATION (buf) =
        no_duration ? GST_CLOCK_TIME_NONE : PTS_TEST_PACKET;

    gst_harness_push (h, buf);
    out = gst_harness_pull (h);
    fail_unless (out != NULL, "Harness swallowed a buffer");

    if (GST_CLOCK_TIME_IS_VALID (prev_out)) {
      if (GST_BUFFER_PTS (out) <= prev_out) {
        run.increasing = FALSE;
      } else if (GST_BUFFER_PTS (out) - prev_out < run.smallest_step) {
        run.smallest_step = GST_BUFFER_PTS (out) - prev_out;
      }
    }

    if (run.until_converged == 0 && GST_BUFFER_PTS (out) == pts_in && i > 0) {
      run.until_converged = i + 1;
    }

    prev_out = GST_BUFFER_PTS (out);
    run.last_out = prev_out;
    run.buffers++;
    in += PTS_TEST_PACKET;
    gst_buffer_unref (out);
  }

  gst_harness_teardown (h);
  g_object_unref (identity);

  return run;
}

/* Leaves `tracker` holding exactly `end`, the way the outgoing stream does when
 * it has burst ahead of the clock: the harness has no clock, so a burst is
 * simply a tight loop. Ten buffers is enough, and anchoring the last one on
 * `end` keeps the tracker off arbitrary multiples of the packet duration. */
static void
pts_prime_tracker (KmsPtsTracker * tracker, GstClockTime end)
{
  pts_push_stream (tracker, end - 9 * PTS_TEST_PACKET, 10, G_MAXUINT, 0, FALSE);

  fail_unless (kms_pts_tracker_peek_last (tracker) == end,
      "Priming did not leave the tracker where the outgoing stream ended");
}

GST_START_TEST (check_kms_utils_depayloader_pts_passthrough)
{
  KmsPtsTracker *tracker = kms_pts_tracker_new ();
  PtsRun run;

  fail_unless (!GST_CLOCK_TIME_IS_VALID (kms_pts_tracker_peek_last (tracker)),
      "A new tracker must not claim to have emitted anything");

  run = pts_push_stream (tracker, 5 * GST_SECOND, 100, G_MAXUINT, 0, FALSE);

  fail_unless (run.increasing, "Emitted PTS did not strictly increase");
  fail_unless (run.last_out == 5 * GST_SECOND + 99 * PTS_TEST_PACKET,
      "A healthy stream on a fresh tracker must pass through untouched");
  fail_unless (run.smallest_step == PTS_TEST_PACKET,
      "A healthy stream must keep its own pacing");

  kms_pts_tracker_unref (tracker);
}

GST_END_TEST;

GST_START_TEST (check_kms_utils_depayloader_pts_rebase_small)
{
  /* Measured on a live SSRC change: the incoming stream started 7.694711ms
   * behind what the outgoing one had already emitted. */
  KmsPtsTracker *tracker = kms_pts_tracker_new ();
  PtsRun run;

  pts_prime_tracker (tracker, 10557864864ULL);

  run = pts_push_stream (tracker, 10550170153ULL, 100, G_MAXUINT, 0, FALSE);

  fail_unless (run.increasing, "Emitted PTS went backwards across the switch");
  fail_unless (run.until_converged > 0 && run.until_converged <= 12,
      "A sub-10ms offset should unwind within a handful of buffers");

  kms_pts_tracker_unref (tracker);
}

GST_END_TEST;

GST_START_TEST (check_kms_utils_depayloader_pts_rebase_burst)
{
  /* The case this mechanism exists for, measured on a live switch: the
   * outgoing stream burst 4.389254295s beyond the incoming one's timeline. */
  KmsPtsTracker *tracker = kms_pts_tracker_new ();
  PtsRun run;

  pts_prime_tracker (tracker, 23318577272ULL);

  run = pts_push_stream (tracker, 18929322977ULL, 6000, G_MAXUINT, 0, FALSE);

  fail_unless (run.increasing, "Emitted PTS went backwards across the switch");

  /* The assertion that pins the fix. The old code emitted every buffer 1ms
   * after the last, compressing 4.4s of audio into a few hundred ms; the
   * rebase keeps the stream's own pacing less the drift. */
  fail_unless (run.smallest_step >= 19 * GST_MSECOND,
      "Audio was compressed: emitted PTS advanced by less than one packet "
      "minus the drift");

  fail_unless (run.until_converged > 4000 && run.until_converged < 4800,
      "A 4.39s offset at 1ms per buffer should unwind in about 4390 buffers");

  kms_pts_tracker_unref (tracker);
}

GST_END_TEST;

GST_START_TEST (check_kms_utils_depayloader_pts_absorbs_gap)
{
  /* A gap in the incoming stream is time nothing has to be emitted in, so the
   * offset can be given back there for free. */
  KmsPtsTracker *tracker = kms_pts_tracker_new ();
  PtsRun run;

  pts_prime_tracker (tracker, 23318577272ULL);

  run = pts_push_stream (tracker, 18929322977ULL, 6000, 100,
      2 * GST_SECOND, FALSE);

  fail_unless (run.increasing,
      "Absorbing a gap pushed emitted PTS backwards");
  fail_unless (run.smallest_step >= 19 * GST_MSECOND,
      "Absorbing a gap compressed the audio around it");
  fail_unless (run.until_converged > 0 && run.until_converged < 3000,
      "A 2s gap should have absorbed most of a 4.39s offset, converging far "
      "sooner than the ~4390 buffers drift alone needs");

  kms_pts_tracker_unref (tracker);
}

GST_END_TEST;

GST_START_TEST (check_kms_utils_depayloader_pts_no_duration)
{
  /* Without a duration there is no way to tell a gap from an ordinary packet,
   * so absorption must stay off rather than guess: every buffer would look
   * like a gap its own size and the audio would be overlapped. rtpopusdepay
   * emits buffers like this. */
  KmsPtsTracker *tracker = kms_pts_tracker_new ();
  PtsRun run;

  pts_prime_tracker (tracker, 23318577272ULL);

  run = pts_push_stream (tracker, 18929322977ULL, 6000, 100,
      2 * GST_SECOND, TRUE);

  fail_unless (run.increasing, "Emitted PTS went backwards across the switch");
  fail_unless (run.smallest_step >= 19 * GST_MSECOND,
      "Audio was compressed when buffer duration was unknown");

  /* Drift only: the 2s gap must NOT have been absorbed, so this converges no
   * sooner than the burst case without a gap at all. */
  fail_unless (run.until_converged > 4000,
      "A gap was absorbed although the buffer duration was unknown");

  kms_pts_tracker_unref (tracker);
}

GST_END_TEST;

GST_START_TEST (check_kms_utils_depayloader_pts_invalid)
{
  /* GST_CLOCK_TIME_NONE is G_MAXUINT64, so a buffer with no PTS would sail
   * through the "strictly increasing" test and be stored, after which every
   * validity check fails and the mechanism silently stops working. */
  GstElement *identity = gst_element_factory_make ("identity", NULL);
  GstHarness *h = gst_harness_new_with_element (identity, "sink", "src");
  KmsPtsTracker *tracker = kms_pts_tracker_new ();
  GstBuffer *buf, *out;

  gst_harness_set_src_caps_str (h, "mycaps");
  kms_utils_depayloader_monitor_pts_out_tracked (identity, tracker);

  buf = gst_harness_create_buffer (h, 50);
  GST_BUFFER_PTS (buf) = 5 * GST_SECOND;
  GST_BUFFER_DURATION (buf) = PTS_TEST_PACKET;
  gst_harness_push (h, buf);
  out = gst_harness_pull (h);
  gst_buffer_unref (out);

  fail_unless (kms_pts_tracker_peek_last (tracker) == 5 * GST_SECOND,
      "The tracker did not record an ordinary buffer");

  buf = gst_harness_create_buffer (h, 50);
  GST_BUFFER_PTS (buf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION (buf) = PTS_TEST_PACKET;
  gst_harness_push (h, buf);
  out = gst_harness_pull (h);
  gst_buffer_unref (out);

  fail_unless (kms_pts_tracker_peek_last (tracker) == 5 * GST_SECOND,
      "A buffer with no PTS poisoned the tracker");

  gst_harness_teardown (h);
  g_object_unref (identity);
  kms_pts_tracker_unref (tracker);
}

GST_END_TEST;

/* Suite initialization */
static Suite *
utils_suite (void)
{
  Suite *s = suite_create ("utils");
  TCase *tc_chain = tcase_create ("element");

  suite_add_tcase (s, tc_chain);

  tcase_add_test (tc_chain, check_sdp_utils_media_get_fid_ssrc);
  tcase_add_test (tc_chain, check_kms_utils_set_pad_event_function_full);

  tcase_add_test (tc_chain, check_kms_utils_set_pad_query_function_full);
  
  tcase_add_test (tc_chain, check_kms_utils_drop_until_keyframe_buffer);
  tcase_add_test (tc_chain, check_kms_utils_drop_until_keyframe_bufferlist);

  tcase_add_test (tc_chain, check_kms_utils_depayloader_pts_passthrough);
  tcase_add_test (tc_chain, check_kms_utils_depayloader_pts_rebase_small);
  tcase_add_test (tc_chain, check_kms_utils_depayloader_pts_rebase_burst);
  tcase_add_test (tc_chain, check_kms_utils_depayloader_pts_absorbs_gap);
  tcase_add_test (tc_chain, check_kms_utils_depayloader_pts_no_duration);
  tcase_add_test (tc_chain, check_kms_utils_depayloader_pts_invalid);

  return s;
}

GST_CHECK_MAIN (utils);
