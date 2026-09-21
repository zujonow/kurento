/*
 * (C) Copyright 2015 Kurento (http://kurento.org/)
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "kmsbasertpsession.h"
#include "constants.h"
#include "kmsutils.h"
#include "sdp_utils.h"

#include "kms-core-enumtypes.h"
#include "kms-core-marshal.h"

#define GST_DEFAULT_NAME "kmsbasertpsession"
#define GST_CAT_DEFAULT kms_base_rtp_session_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define kms_base_rtp_session_parent_class parent_class
G_DEFINE_TYPE (KmsBaseRtpSession, kms_base_rtp_session, KMS_TYPE_SDP_SESSION);

#define BUNDLE_CONN_ADDED "bundle-conn-added"
G_DEFINE_QUARK (BUNDLE_CONN_ADDED, bundle_conn_added);

#define RTCP_DEMUX_PEER "rtcp-demux-peer"
G_DEFINE_QUARK (RTCP_DEMUX_PEER, rtcp_demux_peer);

struct _KmsBaseRTPSessionStats
{
  gboolean enabled;
  gdouble vi;
  gdouble ai;
};

enum
{ CONNECTION_STATE_CHANGED, DTMF_EVENT_DETECTED, LAST_SIGNAL };

static guint obj_signals[LAST_SIGNAL] = { 0 };

#define DEFAULT_CONNECTION_STATE KMS_CONNECTION_STATE_DISCONNECTED

enum
{ PROP_0, PROP_CONNECTION_STATE };

KmsBaseRtpSession *
kms_base_rtp_session_new (KmsBaseSdpEndpoint *ep,
    guint id, KmsIRtpSessionManager *manager)
{
  GObject *obj;
  KmsBaseRtpSession *self;

  obj = g_object_new (KMS_TYPE_BASE_RTP_SESSION, NULL);
  self = KMS_BASE_RTP_SESSION (obj);
  KMS_BASE_RTP_SESSION_CLASS (G_OBJECT_GET_CLASS (self))
      ->post_constructor (self, ep, id, manager);

  return self;
}

/* Connection management begin */

KmsIRtpConnection *
kms_base_rtp_session_get_connection_by_name (KmsBaseRtpSession *self,
    const gchar *name)
{
  gpointer *conn;

  conn = g_hash_table_lookup (self->conns, name);
  if (conn == NULL) {
    return NULL;
  }

  return KMS_I_RTP_CONNECTION (conn);
}

static gchar *
kms_base_rtp_session_create_connection_name_from_handler (KmsBaseRtpSession
    *self, KmsSdpMediaHandler *handler)
{
  gchar *conn_name = NULL;
  gint gid, hid;

  g_object_get (handler, "id", &hid, NULL);

  gid = kms_sdp_agent_get_handler_group_id (KMS_SDP_SESSION (self)->agent, hid);

  if (gid >= 0) {
    conn_name =
        g_strdup_printf ("%s%" G_GINT32_FORMAT, BUNDLE_STREAM_NAME, gid);
  } else if (hid >= 0) {
    conn_name = g_strdup_printf ("%" G_GINT32_FORMAT, hid);
  } else {
    GST_ERROR_OBJECT (self, "Wrong handler");
    g_assert_not_reached ();
  }

  return conn_name;
}

KmsIRtpConnection *
kms_base_rtp_session_get_connection (KmsBaseRtpSession *self,
    KmsSdpMediaHandler *handler)
{
  gchar *name =
      kms_base_rtp_session_create_connection_name_from_handler (self, handler);
  KmsIRtpConnection *conn;

  conn = kms_base_rtp_session_get_connection_by_name (self, name);
  if (conn == NULL) {
    GST_WARNING_OBJECT (self, "Connection '%s' not found", name);
    g_free (name);
    return NULL;
  }
  g_free (name);

  return conn;
}

static KmsIRtpConnection *
kms_base_rtp_session_create_connection_default (KmsBaseRtpSession *self,
    const GstSDPMedia *media,
    const gchar *name, guint16 min_port, guint16 max_port)
{
  KmsBaseRtpSessionClass *klass =
      KMS_BASE_RTP_SESSION_CLASS (G_OBJECT_GET_CLASS (self));

  if (klass->create_connection
      == kms_base_rtp_session_create_connection_default) {
    GST_WARNING_OBJECT (self, "%s does not reimplement 'create_connection'",
        G_OBJECT_CLASS_NAME (klass));
  }

  return NULL;
}

static KmsIRtcpMuxConnection *
kms_base_rtp_session_create_rtcp_mux_connection_default (KmsBaseRtpSession
    *self, const gchar *name, guint16 min_port, guint16 max_port)
{
  KmsBaseRtpSessionClass *klass =
      KMS_BASE_RTP_SESSION_CLASS (G_OBJECT_GET_CLASS (self));

  if (klass->create_rtcp_mux_connection
      == kms_base_rtp_session_create_rtcp_mux_connection_default) {
    GST_WARNING_OBJECT (self,
        "%s does not reimplement 'create_rtcp_mux_connection'",
        G_OBJECT_CLASS_NAME (klass));
  }

  return NULL;
}

static KmsIBundleConnection *
kms_base_rtp_session_create_bundle_connection_default (KmsBaseRtpSession *self,
    const gchar *name, guint16 min_port, guint16 max_port)
{
  KmsBaseRtpSessionClass *klass =
      KMS_BASE_RTP_SESSION_CLASS (G_OBJECT_GET_CLASS (self));

  if (klass->create_bundle_connection
      == kms_base_rtp_session_create_bundle_connection_default) {
    GST_WARNING_OBJECT (self,
        "%s does not reimplement 'create_bundle_connection'",
        G_OBJECT_CLASS_NAME (klass));
  }

  return NULL;
}

static void
kms_base_rtp_session_e2e_latency_cb (GstPad *pad,
    KmsMediaType type, GstClockTimeDiff t, KmsList *mdata, gpointer user_data)
{
  KmsBaseRtpSession *self = KMS_BASE_RTP_SESSION (user_data);
  KmsListIter iter;
  gpointer key, value;
  gchar *name;

  name = gst_element_get_name (KMS_SDP_SESSION (self)->ep);

  kms_list_iter_init (&iter, mdata);
  while (kms_list_iter_next (&iter, &key, &value)) {
    gchar *id = (gchar *) key;
    StreamE2EAvgStat *stat;

    if (!g_str_has_prefix (id, name)) {
      /* This element did not add this mark to the metada */
      continue;
    }

    stat = (StreamE2EAvgStat *) value;
    stat->avg = KMS_STATS_CALCULATE_LATENCY_AVG (t, stat->avg);
  }

  g_free (name);
}

static void
kms_base_rtp_session_set_connection_stats (KmsBaseRtpSession *self,
    KmsIRtpConnection *conn)
{
  kms_i_rtp_connection_set_latency_callback (conn,
      kms_base_rtp_session_e2e_latency_cb, self);

  /* Active insertion of metadata if stats are enabled */
  kms_i_rtp_connection_collect_latency_stats (conn, self->stats_enabled);
}

KmsIRtpConnection *
kms_base_rtp_session_create_connection (KmsBaseRtpSession *self,
    KmsSdpMediaHandler *handler,
    GstSDPMedia *media, guint16 min_port, guint16 max_port)
{
  KmsBaseRtpSessionClass *base_rtp_class =
      KMS_BASE_RTP_SESSION_CLASS (G_OBJECT_GET_CLASS (self));
  gchar *name =
      kms_base_rtp_session_create_connection_name_from_handler (self, handler);
  KmsIRtpConnection *conn = NULL;

  if (name == NULL) {
    GST_WARNING_OBJECT (self, "Connection can not be created");
    goto end;
  }

  conn = kms_base_rtp_session_get_connection_by_name (self, name);
  if (conn != NULL) {
    GST_DEBUG_OBJECT (self, "Re-using connection '%s'", name);
    goto end;
  }

  if (g_str_has_prefix (name, BUNDLE_STREAM_NAME)) {    /* bundle */
    conn = KMS_I_RTP_CONNECTION (base_rtp_class->create_bundle_connection (self,
            name, min_port, max_port));
  } else if (gst_sdp_media_get_attribute_val (media, "rtcp-mux") != NULL) {
    conn =
        KMS_I_RTP_CONNECTION (base_rtp_class->create_rtcp_mux_connection (self,
            name, min_port, max_port));
  } else {
    conn = base_rtp_class->create_connection (self, media, name, min_port,
        max_port);
  }

  if (conn != NULL) {
    g_hash_table_insert (self->conns, g_strdup (name), conn);

    kms_base_rtp_session_set_connection_stats (self, conn);
  }

end:
  g_free (name);

  return conn;
}

/* Connection management end */

/* Start Transport Send begin */

static gboolean
ssrcs_are_mapped (GstElement *ssrcdemux,
    guint32 local_ssrc, guint32 remote_ssrc)
{
  GstElement *rtcpdemux =
      g_object_get_qdata (G_OBJECT (ssrcdemux), rtcp_demux_peer_quark ());
  guint local_ssrc_pair;

  g_signal_emit_by_name (rtcpdemux, "get-local-rr-ssrc-pair", remote_ssrc,
      &local_ssrc_pair);

  return ((local_ssrc != SSRC_INVALID) && (local_ssrc_pair == local_ssrc));
}

static void
kms_base_rtp_session_link_pads (GstPad *src, GstPad *sink)
{
  // Link without the GstPad hierarchy checks.
  GstPadLinkReturn ret = gst_pad_link_full (src, sink,
      GST_PAD_LINK_CHECK_DEFAULT & ~GST_PAD_LINK_CHECK_HIERARCHY);

  if (ret != GST_PAD_LINK_OK) {
    GST_ERROR ("Error linking pads, src: %" GST_PTR_FORMAT
        ", sink: %" GST_PTR_FORMAT ", reason: '%s'",
        src, sink, gst_pad_link_get_name (ret));
  }
}

static void
on_rtcpdemux_new_ssrc_pad (GstElement *rtcpdemux,
    guint ssrc, GstPad *pad, KmsBaseRtpSession *self)
{
  GST_DEBUG_OBJECT (self, "Local SSRC: %u, pad: %" GST_PTR_FORMAT, ssrc, pad);

  KMS_SDP_SESSION_LOCK (self);

  // Link the new pad with the appropriate sink from GstBin.
  if (self->local_audio_ssrc == ssrc) {
    GstPad *sink = kms_i_rtp_session_manager_request_rtcp_sink (self->manager,
        self, self->audio_neg);

    kms_base_rtp_session_link_pads (pad, sink);
    gst_object_unref (sink);
  } else if (self->local_video_ssrc == ssrc) {
    GstPad *sink = kms_i_rtp_session_manager_request_rtcp_sink (self->manager,
        self, self->video_neg);

    kms_base_rtp_session_link_pads (pad, sink);
    gst_object_unref (sink);
  } else {
    GST_ERROR_OBJECT (self,
        "Local SSRC %u in RTCP doesn't match any SDP media; DATA WILL BE DROPPED",
        ssrc);

    // Cannot identify which local media corresponds to the SSRC.
    // The pad cannot be left without linking, so discard into a fakesink.
    GstElement *fakesink =
        kms_utils_element_factory_make ("fakesink", GST_DEFAULT_NAME);
    gst_bin_add (GST_BIN (self), fakesink);
    gst_element_sync_state_with_parent_target_state (fakesink);
    GstPad *sink = gst_element_get_static_pad (fakesink, "sink");

    kms_base_rtp_session_link_pads (pad, sink);
    gst_object_unref (sink);
  }

  KMS_SDP_SESSION_UNLOCK (self);
}

static void
rtp_ssrc_demux_new_ssrc_pad (GstElement *ssrcdemux,
    guint ssrc, GstPad *pad, KmsBaseRtpSession *self)
{
  const gchar *rtp_pad_name = GST_OBJECT_NAME (pad);
  gchar *rtcp_pad_name;
  const GstSDPMedia *media;
  GstPad *src, *sink;

  GST_DEBUG_OBJECT (self, "Remote SSRC: %u, pad: %" GST_PTR_FORMAT, ssrc, pad);

  KMS_SDP_SESSION_LOCK (self);

  if (self->remote_audio_ssrc == ssrc
      || ssrcs_are_mapped (ssrcdemux, self->local_audio_ssrc, ssrc)) {
    media = self->audio_neg;
  } else if (self->remote_video_ssrc == ssrc
      || ssrcs_are_mapped (ssrcdemux, self->local_video_ssrc, ssrc)) {
    media = self->video_neg;
  } else {
    if (kms_i_rtp_session_manager_custom_ssrc_management (self->manager, self,
            ssrcdemux, ssrc, pad)) {
      goto end;
    } else {
      GST_ERROR_OBJECT (self,
          "Remote SSRC %u doesn't match any SDP media; DATA WILL BE DROPPED",
          ssrc);

      media = NULL;
    }
  }

  /* RTP */
  sink =
      kms_i_rtp_session_manager_request_rtp_sink (self->manager, self, media);
  kms_base_rtp_session_link_pads (pad, sink);
  g_object_unref (sink);

  /* RTCP */
  rtcp_pad_name = g_strconcat ("rtcp_", rtp_pad_name, NULL);
  src = gst_element_get_static_pad (ssrcdemux, rtcp_pad_name);
  g_free (rtcp_pad_name);
  sink =
      kms_i_rtp_session_manager_request_rtcp_sink (self->manager, self, media);
  kms_base_rtp_session_link_pads (src, sink);
  g_object_unref (src);
  g_object_unref (sink);

end:
  KMS_SDP_SESSION_UNLOCK (self);
}

static void
kms_base_rtp_session_add_gst_bundle_elements (KmsBaseRtpSession *self,
    KmsIRtpConnection *conn, const GstSDPMedia *media, gboolean active)
{
  gboolean added;
  GstElement *rtcpdemux;
  GstElement *ssrcdemux;
  GstPad *src, *sink;

  if (GPOINTER_TO_UINT (g_object_get_qdata (G_OBJECT (conn),
              bundle_conn_added_quark ()))) {
    GST_DEBUG_OBJECT (self, "Connection configured");
    return;
  }

  g_object_set_qdata (G_OBJECT (conn), bundle_conn_added_quark (),
      GUINT_TO_POINTER (TRUE));

  g_object_get (conn, "added", &added, NULL);
  if (!added) {
    kms_i_rtp_connection_add (conn, GST_BIN (self), active);
  }

  rtcpdemux = kms_utils_element_factory_make ("rtcpdemux", GST_DEFAULT_NAME);

  g_signal_connect (rtcpdemux, "new-ssrc-pad",
      G_CALLBACK (on_rtcpdemux_new_ssrc_pad), self);

  ssrcdemux = kms_utils_element_factory_make ("rtpssrcdemux", GST_DEFAULT_NAME);

  g_object_set_qdata_full (G_OBJECT (ssrcdemux), rtcp_demux_peer_quark (),
      g_object_ref (rtcpdemux), g_object_unref);
  g_signal_connect (ssrcdemux, "new-ssrc-pad",
      G_CALLBACK (rtp_ssrc_demux_new_ssrc_pad), self);

  kms_i_rtp_connection_sink_sync_state_with_parent (conn);
  gst_bin_add_many (GST_BIN (self), ssrcdemux, rtcpdemux, NULL);

  /* RTP */

  src = kms_i_rtp_connection_request_rtp_src (conn);
  sink = gst_element_get_static_pad (ssrcdemux, "sink");
  kms_base_rtp_session_link_pads (src, sink);
  g_object_unref (src);
  g_object_unref (sink);

  /* RTCP */

  src = kms_i_rtp_connection_request_rtcp_src (conn);
  sink = gst_element_get_static_pad (rtcpdemux, "sink");
  kms_base_rtp_session_link_pads (src, sink);
  g_object_unref (src);
  g_object_unref (sink);

  gst_element_link_pads (rtcpdemux, "rtcp_src", ssrcdemux, "rtcp_sink");

  gst_element_sync_state_with_parent_target_state (rtcpdemux);
  gst_element_sync_state_with_parent_target_state (ssrcdemux);

  kms_i_rtp_connection_src_sync_state_with_parent (conn);
}

static void
kms_base_rtp_session_link_gst_connection_sink (KmsBaseRtpSession *self,
    KmsIRtpConnection *conn, const GstSDPMedia *media)
{
  GstPad *src, *sink;

  /* RTP */
  src = kms_i_rtp_session_manager_request_rtp_src (self->manager, self, media);
  sink = kms_i_rtp_connection_request_rtp_sink (conn);
  kms_base_rtp_session_link_pads (src, sink);
  g_object_unref (src);
  g_object_unref (sink);

  /* RTCP */
  src = kms_i_rtp_session_manager_request_rtcp_src (self->manager, self, media);
  sink = kms_i_rtp_connection_request_rtcp_sink (conn);
  kms_base_rtp_session_link_pads (src, sink);
  g_object_unref (src);
  g_object_unref (sink);
}

/* ========================================
 * BUS HANDLER FUNCTIONS
 * ======================================== */

static GstBusSyncReply
kms_base_rtp_session_dtmf_bus_handler (GstBus *bus,
    GstMessage *msg, KmsBaseRtpSession *self)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ELEMENT:{
      const GstStructure *s = gst_message_get_structure (msg);

      if (gst_structure_has_name (s, "dtmf-event")) {
        gint number, volume, duration;
        gboolean end;

        gst_structure_get_int (s, "number", &number);
        gst_structure_get_boolean (s, "end", &end);
        gst_structure_get_int (s, "volume", &volume);
        gst_structure_get_int (s, "duration", &duration);

        g_print
            ("[PANKAJ DTMF DETECTED] Number: %d, End: %s, Volume: %d, Duration: %d\n",
            number, end ? "true" : "false", volume, duration);

        // Emit the DTMF_EVENT_DETECTED signal
        g_signal_emit (self, obj_signals[DTMF_EVENT_DETECTED], 0, number, end,
            volume, duration, "audio");

        GST_INFO_OBJECT (self,
            "DTMF Event: number=%d, end=%s, volume=%d, duration=%d", number,
            end ? "true" : "false", volume, duration);

      } else {
        // Let other element messages pass through
        return GST_BUS_PASS;
      }
      break;
    }
    default:
      return GST_BUS_PASS;
  }

  return GST_BUS_PASS;
}

/* Combined function to setup DTMF message handling */
static void
setup_dtmf_message_handler (KmsBaseRtpSession *self)
{
  GstBus *pipeline_bus = gst_element_get_bus (GST_ELEMENT (self));

  if (!pipeline_bus) {
    g_print ("Pankaj Failed to get pipeline bus for DTMF handler setup\n");
    return;
  }

  /* Set the sync handler to process DTMF events */
  gst_bus_set_sync_handler (pipeline_bus,
      (GstBusSyncHandler) kms_base_rtp_session_dtmf_bus_handler, self, NULL);

  gst_object_unref (pipeline_bus);
  g_print ("Pankaj Set up DTMF message handler\n");
}

/* ========================================
 * DTMF DETECTION FUNCTIONS
 * ======================================== */

// static GstPadProbeReturn
// dtmf_pad_probe_cb (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
// {
//   GstBuffer *buffer;
//   GstRTPBuffer rtp_buffer = GST_RTP_BUFFER_INIT;
//   guint8 payload_type;

//   if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
//     buffer = GST_PAD_PROBE_INFO_BUFFER (info);

//     if (gst_rtp_buffer_map (buffer, GST_MAP_READ, &rtp_buffer)) {
//       payload_type = gst_rtp_buffer_get_payload_type (&rtp_buffer);
//       gst_rtp_buffer_unmap (&rtp_buffer);

//       /* Only allow DTMF packets (payload type 101) to pass through */
//       if (payload_type != 101) {
//         g_print ("Pankaj Dropping non-DTMF packet with payload type %d\n",
//             payload_type);
//         return GST_PAD_PROBE_DROP;
//       }

//       g_print ("Pankaj Allowing DTMF packet with payload type %d\n",
//           payload_type);
//       return GST_PAD_PROBE_OK;
//     }
//   }

//   return GST_PAD_PROBE_OK;
// }
/* Combined function to setup RTP pipeline with optional DTMF detection */
// static gboolean
// setup_rtp_pipeline (KmsBaseRtpSession *self, GstPad *src, GstPad *sink)
// {

//   /* DTMF-enabled pipeline setup */
//   GstElement *rtp_tee, *rtp_queue;
//   GstElement *dtmf_capsfilter, *dtmf_depay, *dtmf_fakesink;

//   /* Create tee element */
//   rtp_tee = gst_element_factory_make ("tee", "rtp-tee");
//   if (!rtp_tee) {
//     g_print ("Pankaj Failed to create RTP tee element\n");
//     return FALSE;
//   }

//   /* Create and configure queue element for main branch */
//   rtp_queue = gst_element_factory_make ("queue", "rtp-queue");
//   if (!rtp_queue) {
//     g_print ("Pankaj Failed to create RTP queue element\n");
//     gst_object_unref (rtp_tee);
//     return FALSE;
//   }
//   /* Set queue properties for better performance */
//   g_object_set (rtp_queue, "max-size-time", (guint64)200000000, /* 200ms */
//       "max-size-buffers", 0, /* No buffer limit */
//       "max-size-bytes", 0, /* No byte limit */
//       NULL);

//   /* Create DTMF detection elements */
//   dtmf_capsfilter = gst_element_factory_make ("capsfilter", "dtmf-caps-filter");
//   dtmf_depay = gst_element_factory_make ("rtpdtmfdepay", "dtmf-depay");
//   dtmf_fakesink = gst_element_factory_make ("fakesink", "dtmf-fakesink");

//   if (!dtmf_capsfilter || !dtmf_depay || !dtmf_fakesink) {
//     g_print ("Pankaj Failed to create DTMF detection elements\n");
//     if (dtmf_capsfilter)
//       gst_object_unref (dtmf_capsfilter);
//     if (dtmf_depay)
//       gst_object_unref (dtmf_depay);
//     if (dtmf_fakesink)
//       gst_object_unref (dtmf_fakesink);
//     gst_object_unref (rtp_tee);
//     gst_object_unref (rtp_queue);
//     return FALSE;
//   }

//   /* Configure DTMF elements */
//   GstCaps *dtmf_caps = gst_caps_from_string (
//       "application/x-rtp, encoding-name=(string)TELEPHONE-EVENT, payload=(int)101, "
//       "media=(string)audio, clock-rate=(int)8000");
//   g_object_set (G_OBJECT (dtmf_capsfilter), "caps", dtmf_caps, NULL);
//   gst_caps_unref (dtmf_caps);
//   g_object_set (dtmf_fakesink, "sync", FALSE, "async", FALSE, NULL);

//   /* Add elements to the pipeline */
//   gst_bin_add_many (GST_BIN (self), rtp_tee, rtp_queue, dtmf_capsfilter,
//       dtmf_depay, dtmf_fakesink, NULL);
//   g_print ("Pankaj Added all elements to pipeline\n");

//   /* Link original source to tee sink */
//   GstPad *tee_sink_pad = gst_element_get_static_pad (rtp_tee, "sink");
//   if (gst_pad_link (src, tee_sink_pad) != GST_PAD_LINK_OK) {
//     g_print ("Pankaj Failed to link src to tee\n");
//     gst_object_unref (tee_sink_pad);
//     goto cleanup_elements;
//   }
//   gst_object_unref (tee_sink_pad);
//   g_print ("Pankaj Successfully linked src to tee\n");

//   /* Create first branch - tee to queue (main RTP path) */
//   GstPad *tee_src_pad1 = gst_element_request_pad_simple (rtp_tee, "src_%u");
//   GstPad *queue_sink_pad = gst_element_get_static_pad (rtp_queue, "sink");
//   if (gst_pad_link (tee_src_pad1, queue_sink_pad) != GST_PAD_LINK_OK) {
//     g_print ("Pankaj Failed to link tee to queue\n");
//     gst_object_unref (tee_src_pad1);
//     gst_object_unref (queue_sink_pad);
//     goto cleanup_elements;
//   }
//   gst_object_unref (queue_sink_pad);
//   g_print ("Pankaj Successfully linked tee to queue\n");

//   /* Link queue to final sink */
//   GstPad *queue_src_pad = gst_element_get_static_pad (rtp_queue, "src");
//   kms_base_rtp_session_link_pads (queue_src_pad, sink);
//   gst_object_unref (queue_src_pad);
//   gst_object_unref (tee_src_pad1);
//   g_print ("Pankaj Successfully linked queue to main sink\n");

//   /* Create second branch - tee to DTMF detection pipeline */
//   GstPad *tee_src_pad2 = gst_element_request_pad_simple (rtp_tee, "src_%u");
//   GstPad *dtmf_caps_sink_pad =
//       gst_element_get_static_pad (dtmf_capsfilter, "sink");
//   if (gst_pad_link (tee_src_pad2, dtmf_caps_sink_pad) != GST_PAD_LINK_OK) {
//     g_print ("Pankaj Failed to link tee to DTMF caps filter\n");
//     gst_object_unref (tee_src_pad2);
//     gst_object_unref (dtmf_caps_sink_pad);
//     goto cleanup_elements;
//   }
//   gst_object_unref (dtmf_caps_sink_pad);
//   gst_object_unref (tee_src_pad2);
//   g_print ("Pankaj Successfully linked tee to DTMF caps filter\n");

//   /* Link DTMF detection chain: capsfilter -> dtmfdepay -> fakesink */
//   if (!gst_element_link_many (dtmf_capsfilter, dtmf_depay, dtmf_fakesink,
//           NULL)) {
//     g_print ("Pankaj Failed to link DTMF detection chain\n");
//     goto cleanup_elements;
//   }
//   g_print ("Pankaj Successfully linked DTMF detection chain\n");

//   /* Setup DTMF message handler */
//   setup_dtmf_message_handler (self);

//   /* Sync all element states with pipeline */
//   gst_element_sync_state_with_parent (rtp_tee);
//   gst_element_sync_state_with_parent (rtp_queue);
//   gst_element_sync_state_with_parent (dtmf_capsfilter);
//   gst_element_sync_state_with_parent (dtmf_depay);
//   gst_element_sync_state_with_parent (dtmf_fakesink);

//   g_print ("Pankaj RTP tee with DTMF detection setup completed successfully\n");
//   g_print (
//       "Pankaj Pipeline structure: src -> tee -> [queue -> main_sink] + [caps -> dtmfdepay -> fakesink]\n");
//   return TRUE;

// cleanup_elements:
//   /* Cleanup on failure */
//   gst_element_set_state (rtp_tee, GST_STATE_NULL);
//   gst_element_set_state (rtp_queue, GST_STATE_NULL);
//   gst_element_set_state (dtmf_capsfilter, GST_STATE_NULL);
//   gst_element_set_state (dtmf_depay, GST_STATE_NULL);
//   gst_element_set_state (dtmf_fakesink, GST_STATE_NULL);
//   gst_bin_remove_many (GST_BIN (self), rtp_tee, rtp_queue, dtmf_capsfilter,
//       dtmf_depay, dtmf_fakesink, NULL);
//   return FALSE;
// }

// static gboolean
// setup_rtp_pipeline (KmsBaseRtpSession *self, GstPad *src, GstPad *sink)
// {
//   /* DTMF-enabled pipeline setup */
//   GstElement *rtp_tee, *rtp_queue;
//   GstElement *dtmf_capsfilter, *dtmf_depay, *dtmf_fakesink;
//   GstElement *dtmf_queue; // Add queue for DTMF branch

//   /* Create tee element */
//   rtp_tee = gst_element_factory_make ("tee", "rtp-tee");
//   if (!rtp_tee) {
//     g_print ("Pankaj Failed to create RTP tee element\n");
//     return FALSE;
//   }

//   /* Create and configure queue element for main branch */
//   rtp_queue = gst_element_factory_make ("queue", "rtp-queue");
//   if (!rtp_queue) {
//     g_print ("Pankaj Failed to create RTP queue element\n");
//     gst_object_unref (rtp_tee);
//     return FALSE;
//   }
//   /* Set queue properties for better performance */
//   g_object_set (rtp_queue, "max-size-time", (guint64)200000000, /* 200ms */
//       "max-size-buffers", 0, /* No buffer limit */
//       "max-size-bytes", 0, /* No byte limit */
//       NULL);

//   /* Create DTMF queue for better isolation */
//   dtmf_queue = gst_element_factory_make ("queue", "dtmf-queue");
//   if (!dtmf_queue) {
//     g_print ("Pankaj Failed to create DTMF queue element\n");
//     gst_object_unref (rtp_tee);
//     gst_object_unref (rtp_queue);
//     return FALSE;
//   }
//   /* Configure DTMF queue with smaller buffer */
//   g_object_set (dtmf_queue, "max-size-time", (guint64)100000000, /* 100ms */
//       "max-size-buffers", 50,
//       "max-size-bytes", 0,
//       NULL);

//   /* Create DTMF detection elements */
//   dtmf_capsfilter = gst_element_factory_make ("capsfilter", "dtmf-caps-filter");
//   dtmf_depay = gst_element_factory_make ("rtpdtmfdepay", "dtmf-depay");
//   dtmf_fakesink = gst_element_factory_make ("fakesink", "dtmf-fakesink");

//   if (!dtmf_capsfilter || !dtmf_depay || !dtmf_fakesink) {
//     g_print ("Pankaj Failed to create DTMF detection elements\n");
//     if (dtmf_capsfilter)
//       gst_object_unref (dtmf_capsfilter);
//     if (dtmf_depay)
//       gst_object_unref (dtmf_depay);
//     if (dtmf_fakesink)
//       gst_object_unref (dtmf_fakesink);
//     gst_object_unref (rtp_tee);
//     gst_object_unref (rtp_queue);
//     gst_object_unref (dtmf_queue);
//     return FALSE;
//   }

//   /* Configure DTMF elements with more specific caps */
//   GstCaps *dtmf_caps = gst_caps_from_string (
//       "application/x-rtp, "
//       "encoding-name=(string)TELEPHONE-EVENT, "
//       "payload=(int)101, "
//       "media=(string)audio, "
//       "clock-rate=(int)8000");
//   g_object_set (G_OBJECT (dtmf_capsfilter), "caps", dtmf_caps, NULL);
//   gst_caps_unref (dtmf_caps);

//   /* Configure fakesink to be silent and not sync */
//   g_object_set (dtmf_fakesink, 
//       "sync", FALSE, 
//       "async", FALSE,
//       "silent", TRUE,
//       "dump", FALSE,
//       NULL);

//   /* Configure rtpdtmfdepay for better validation */
//   g_object_set (dtmf_depay,
//       "drop-redundant", TRUE,  // Drop redundant DTMF packets
//       NULL);

//   /* Add elements to the pipeline including the DTMF queue */
//   gst_bin_add_many (GST_BIN (self), rtp_tee, rtp_queue, dtmf_queue, 
//       dtmf_capsfilter, dtmf_depay, dtmf_fakesink, NULL);
//   g_print ("Pankaj Added all elements to pipeline\n");

//   /* Link original source to tee sink */
//   GstPad *tee_sink_pad = gst_element_get_static_pad (rtp_tee, "sink");
//   if (gst_pad_link (src, tee_sink_pad) != GST_PAD_LINK_OK) {
//     g_print ("Pankaj Failed to link src to tee\n");
//     gst_object_unref (tee_sink_pad);
//     goto cleanup_elements;
//   }
//   gst_object_unref (tee_sink_pad);
//   g_print ("Pankaj Successfully linked src to tee\n");

//   /* Create first branch - tee to queue (main RTP path) */
//   GstPad *tee_src_pad1 = gst_element_request_pad_simple (rtp_tee, "src_%u");
//   GstPad *queue_sink_pad = gst_element_get_static_pad (rtp_queue, "sink");
//   if (gst_pad_link (tee_src_pad1, queue_sink_pad) != GST_PAD_LINK_OK) {
//     g_print ("Pankaj Failed to link tee to queue\n");
//     gst_object_unref (tee_src_pad1);
//     gst_object_unref (queue_sink_pad);
//     goto cleanup_elements;
//   }
//   gst_object_unref (queue_sink_pad);
//   g_print ("Pankaj Successfully linked tee to queue\n");

//   /* Link queue to final sink */
//   GstPad *queue_src_pad = gst_element_get_static_pad (rtp_queue, "src");
//   kms_base_rtp_session_link_pads (queue_src_pad, sink);
//   gst_object_unref (queue_src_pad);
//   gst_object_unref (tee_src_pad1);
//   g_print ("Pankaj Successfully linked queue to main sink\n");

//   /* Create second branch - tee to DTMF detection pipeline with queue */
//   GstPad *tee_src_pad2 = gst_element_request_pad_simple (rtp_tee, "src_%u");
//   GstPad *dtmf_queue_sink_pad = gst_element_get_static_pad (dtmf_queue, "sink");
//   if (gst_pad_link (tee_src_pad2, dtmf_queue_sink_pad) != GST_PAD_LINK_OK) {
//     g_print ("Pankaj Failed to link tee to DTMF queue\n");
//     gst_object_unref (tee_src_pad2);
//     gst_object_unref (dtmf_queue_sink_pad);
//     goto cleanup_elements;
//   }
//   gst_object_unref (dtmf_queue_sink_pad);
//   gst_object_unref (tee_src_pad2);
//   g_print ("Pankaj Successfully linked tee to DTMF queue\n");

//   /* Link DTMF detection chain: queue -> capsfilter -> dtmfdepay -> fakesink */
//   if (!gst_element_link_many (dtmf_queue, dtmf_capsfilter, dtmf_depay, 
//           dtmf_fakesink, NULL)) {
//     g_print ("Pankaj Failed to link DTMF detection chain\n");
//     goto cleanup_elements;
//   }
//   g_print ("Pankaj Successfully linked DTMF detection chain\n");

//   /* Setup DTMF message handler */
//   setup_dtmf_message_handler (self);

//   /* Sync all element states with pipeline */
//   gst_element_sync_state_with_parent (rtp_tee);
//   gst_element_sync_state_with_parent (rtp_queue);
//   gst_element_sync_state_with_parent (dtmf_queue);
//   gst_element_sync_state_with_parent (dtmf_capsfilter);
//   gst_element_sync_state_with_parent (dtmf_depay);
//   gst_element_sync_state_with_parent (dtmf_fakesink);

//   g_print ("Pankaj RTP tee with DTMF detection setup completed successfully\n");
//   g_print (
//       "Pankaj Pipeline structure: src -> tee -> [queue -> main_sink] + [dtmf_queue -> caps -> dtmfdepay -> fakesink]\n");
//   return TRUE;

// cleanup_elements:
//   /* Cleanup on failure */
//   gst_element_set_state (rtp_tee, GST_STATE_NULL);
//   gst_element_set_state (rtp_queue, GST_STATE_NULL);
//   gst_element_set_state (dtmf_queue, GST_STATE_NULL);
//   gst_element_set_state (dtmf_capsfilter, GST_STATE_NULL);
//   gst_element_set_state (dtmf_depay, GST_STATE_NULL);
//   gst_element_set_state (dtmf_fakesink, GST_STATE_NULL);
//   gst_bin_remove_many (GST_BIN (self), rtp_tee, rtp_queue, dtmf_queue,
//       dtmf_capsfilter, dtmf_depay, dtmf_fakesink, NULL);
//   return FALSE;
// }

/* Add this include at the top of your file if not already present */
// #include <gst/rtp/gstrtpbuffer.h>

/* Pad probe to filter only DTMF packets based on payload type */
static GstPadProbeReturn
dtmf_packet_filter_probe (GstPad *pad, GstPadProbeInfo *info,
    gpointer user_data)
{
  GstBuffer *buffer;
  GstMapInfo map;
  guint8 payload_type;

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    buffer = GST_PAD_PROBE_INFO_BUFFER (info);

    /* Map the buffer to read RTP header directly */
    if (gst_buffer_map (buffer, &map, GST_MAP_READ)) {
      /* Check if buffer has at least RTP header (12 bytes minimum) */
      if (map.size >= 12) {
        /* Extract payload type from RTP header (bits 1-7 of second byte) */
        payload_type = map.data[1] & 0x7F;

        /* Only allow DTMF payload type (typically 101) - adjust if different */
        if (payload_type != 101) {
          gst_buffer_unmap (buffer, &map);
          return GST_PAD_PROBE_DROP;    /* Drop non-DTMF packets */
        }
      }
      gst_buffer_unmap (buffer, &map);
    }
  }

  return GST_PAD_PROBE_OK;      /* Allow DTMF packets */
}

static gboolean
setup_rtp_pipeline (KmsBaseRtpSession *self, GstPad *src, GstPad *sink)
{
  /* DTMF-enabled pipeline setup */
  GstElement *rtp_tee, *rtp_queue;
  GstElement *dtmf_capsfilter, *dtmf_depay, *dtmf_fakesink;
  GstElement *dtmf_queue;       // Add queue for DTMF branch

  /* Create tee element */
  rtp_tee = gst_element_factory_make ("tee", "rtp-tee");
  if (!rtp_tee) {
    g_print ("Pankaj Failed to create RTP tee element\n");
    return FALSE;
  }

  /* Create and configure queue element for main branch */
  rtp_queue = gst_element_factory_make ("queue", "rtp-queue");
  if (!rtp_queue) {
    g_print ("Pankaj Failed to create RTP queue element\n");
    gst_object_unref (rtp_tee);
    return FALSE;
  }
  /* Set queue properties for better performance */
  g_object_set (rtp_queue, "max-size-time", (guint64) 200000000,        /* 200ms */
      "max-size-buffers", 0,    /* No buffer limit */
      "max-size-bytes", 0,      /* No byte limit */
      NULL);

  /* Create DTMF capsfilter for proper caps negotiation after filtering */
  dtmf_capsfilter = gst_element_factory_make ("capsfilter", "dtmf-caps-filter");
  if (!dtmf_capsfilter) {
    g_print ("Pankaj Failed to create DTMF capsfilter element\n");
    gst_object_unref (rtp_tee);
    gst_object_unref (rtp_queue);
    return FALSE;
  }

  /* Create DTMF queue for better isolation */
  dtmf_queue = gst_element_factory_make ("queue", "dtmf-queue");
  if (!dtmf_queue) {
    g_print ("Pankaj Failed to create DTMF queue element\n");
    gst_object_unref (rtp_tee);
    gst_object_unref (rtp_queue);
    gst_object_unref (dtmf_capsfilter);
    return FALSE;
  }
  /* Configure DTMF queue with smaller buffer */
  g_object_set (dtmf_queue, "max-size-time", (guint64) 100000000,       /* 100ms */
      "max-size-buffers", 50, "max-size-bytes", 0, NULL);

  /* Create DTMF detection elements */
  dtmf_depay = gst_element_factory_make ("rtpdtmfdepay", "dtmf-depay");
  dtmf_fakesink = gst_element_factory_make ("fakesink", "dtmf-fakesink");

  if (!dtmf_depay || !dtmf_fakesink) {
    g_print ("Pankaj Failed to create DTMF detection elements\n");
    if (dtmf_depay)
      gst_object_unref (dtmf_depay);
    if (dtmf_fakesink)
      gst_object_unref (dtmf_fakesink);
    gst_object_unref (rtp_tee);
    gst_object_unref (rtp_queue);
    gst_object_unref (dtmf_capsfilter);
    gst_object_unref (dtmf_queue);
    return FALSE;
  }

  /* Configure DTMF capsfilter with proper caps for negotiation after pad probe filtering */
  GstCaps *dtmf_caps = gst_caps_from_string ("application/x-rtp, "
      "encoding-name=(string)TELEPHONE-EVENT, "
      "payload=(int)101, " "media=(string)audio, " "clock-rate=(int)8000");
  g_object_set (G_OBJECT (dtmf_capsfilter), "caps", dtmf_caps, NULL);
  gst_caps_unref (dtmf_caps);

  /* Configure fakesink to be silent and not sync */
  g_object_set (dtmf_fakesink,
      "sync", FALSE, "async", FALSE, "silent", TRUE, "dump", FALSE, NULL);

  /* Add elements to the pipeline - capsfilter for caps negotiation after pad probe */
  gst_bin_add_many (GST_BIN (self), rtp_tee, rtp_queue,
      dtmf_queue, dtmf_capsfilter, dtmf_depay, dtmf_fakesink, NULL);
  g_print ("Pankaj Added all elements to pipeline\n");

  /* Link original source to tee sink */
  GstPad *tee_sink_pad = gst_element_get_static_pad (rtp_tee, "sink");

  if (gst_pad_link (src, tee_sink_pad) != GST_PAD_LINK_OK) {
    g_print ("Pankaj Failed to link src to tee\n");
    gst_object_unref (tee_sink_pad);
    goto cleanup_elements;
  }
  gst_object_unref (tee_sink_pad);
  g_print ("Pankaj Successfully linked src to tee\n");

  /* Create first branch - tee to queue (main RTP path) */
  GstPad *tee_src_pad1 = gst_element_request_pad_simple (rtp_tee, "src_%u");
  GstPad *queue_sink_pad = gst_element_get_static_pad (rtp_queue, "sink");

  if (gst_pad_link (tee_src_pad1, queue_sink_pad) != GST_PAD_LINK_OK) {
    g_print ("Pankaj Failed to link tee to queue\n");
    gst_object_unref (tee_src_pad1);
    gst_object_unref (queue_sink_pad);
    goto cleanup_elements;
  }
  gst_object_unref (queue_sink_pad);
  g_print ("Pankaj Successfully linked tee to queue\n");

  /* Link queue to final sink */
  GstPad *queue_src_pad = gst_element_get_static_pad (rtp_queue, "src");

  kms_base_rtp_session_link_pads (queue_src_pad, sink);
  gst_object_unref (queue_src_pad);
  gst_object_unref (tee_src_pad1);
  g_print ("Pankaj Successfully linked queue to main sink\n");

  /* Create second branch - tee to DTMF detection pipeline with pad probe filtering */
  GstPad *tee_src_pad2 = gst_element_request_pad_simple (rtp_tee, "src_%u");

  /* Add pad probe to filter only DTMF packets based on payload type */
  gst_pad_add_probe (tee_src_pad2,
      GST_PAD_PROBE_TYPE_BUFFER, dtmf_packet_filter_probe, NULL, NULL);

  GstPad *dtmf_queue_sink_pad = gst_element_get_static_pad (dtmf_queue, "sink");

  if (gst_pad_link (tee_src_pad2, dtmf_queue_sink_pad) != GST_PAD_LINK_OK) {
    g_print ("Pankaj Failed to link tee to DTMF queue\n");
    gst_object_unref (tee_src_pad2);
    gst_object_unref (dtmf_queue_sink_pad);
    goto cleanup_elements;
  }
  gst_object_unref (dtmf_queue_sink_pad);
  gst_object_unref (tee_src_pad2);
  g_print
      ("Pankaj Successfully linked tee to DTMF queue with pad probe filtering\n");

  /* Link DTMF detection chain: queue -> capsfilter -> dtmfdepay -> fakesink */
  if (!gst_element_link_many (dtmf_queue, dtmf_capsfilter, dtmf_depay,
          dtmf_fakesink, NULL)) {
    g_print ("Pankaj Failed to link DTMF detection chain\n");
    goto cleanup_elements;
  }
  g_print ("Pankaj Successfully linked DTMF detection chain\n");

  /* Setup DTMF message handler */
  setup_dtmf_message_handler (self);

  /* Sync all element states with pipeline */
  gst_element_sync_state_with_parent (rtp_tee);
  gst_element_sync_state_with_parent (rtp_queue);
  gst_element_sync_state_with_parent (dtmf_queue);
  gst_element_sync_state_with_parent (dtmf_capsfilter);
  gst_element_sync_state_with_parent (dtmf_depay);
  gst_element_sync_state_with_parent (dtmf_fakesink);

  g_print ("Pankaj RTP tee with DTMF detection setup completed successfully\n");
  g_print
      ("Pankaj Pipeline structure: src -> tee -> [queue -> main_sink] + [pad_probe -> dtmf_queue -> capsfilter -> dtmfdepay -> fakesink]\n");
  return TRUE;

cleanup_elements:
  /* Cleanup on failure */
  gst_element_set_state (rtp_tee, GST_STATE_NULL);
  gst_element_set_state (rtp_queue, GST_STATE_NULL);
  gst_element_set_state (dtmf_queue, GST_STATE_NULL);
  gst_element_set_state (dtmf_capsfilter, GST_STATE_NULL);
  gst_element_set_state (dtmf_depay, GST_STATE_NULL);
  gst_element_set_state (dtmf_fakesink, GST_STATE_NULL);
  gst_bin_remove_many (GST_BIN (self), rtp_tee, rtp_queue, dtmf_queue,
      dtmf_capsfilter, dtmf_depay, dtmf_fakesink, NULL);
  return FALSE;
}

static void
kms_base_rtp_session_link_gst_connection_src (KmsBaseRtpSession *self,
    KmsIRtpConnection *conn, const GstSDPMedia *media)
{
  gboolean is_dtmf_enabled = kms_sdp_session_get_listen_dtmf (&self->parent);

  g_print ("kms_sdp_session_get_listen_dtmf (&self->parent): %d\n",
      is_dtmf_enabled);
  g_print
      ("Pankaj kms_base_rtp_session_link_gst_connection_src Linking connection src for media: %s (DTMF: %s)\n",
      gst_sdp_media_get_media (media),
      is_dtmf_enabled ? "enabled" : "disabled");

  GstPad *src, *sink;

  /* RTP Setup */
  src = kms_i_rtp_connection_request_rtp_src (conn);
  sink =
      kms_i_rtp_session_manager_request_rtp_sink (self->manager, self, media);

  g_print ("Pankaj linking src RTP src pad: %s (%s)\n", GST_OBJECT_NAME (src),
      G_OBJECT_TYPE_NAME (src));
  g_print ("Pankaj linking src RTP sink pad: %s (%s)\n", GST_OBJECT_NAME (sink),
      G_OBJECT_TYPE_NAME (sink));

  /* Setup RTP pipeline (with or without DTMF) */
  if (is_dtmf_enabled) {
    setup_rtp_pipeline (self, src, sink);
  } else {
    kms_base_rtp_session_link_pads (src, sink);
    g_print ("Pankaj Simple RTP pipeline setup completed (direct link)\n");
  }

  /* Cleanup RTP pads */
  g_object_unref (src);
  g_object_unref (sink);

  /* RTCP - Keep original implementation */
  src = kms_i_rtp_connection_request_rtcp_src (conn);
  sink =
      kms_i_rtp_session_manager_request_rtcp_sink (self->manager, self, media);
  kms_base_rtp_session_link_pads (src, sink);
  g_object_unref (src);
  g_object_unref (sink);

  g_print ("Pankaj RTP session linking completed successfully\n");
}

// static void
// kms_base_rtp_session_link_gst_connection_src (KmsBaseRtpSession * self,
//     KmsIRtpConnection * conn, const GstSDPMedia * media)
// {
//   GstPad *src, *sink;

//   /* RTP */
//   src = kms_i_rtp_connection_request_rtp_src (conn);
//   sink = kms_i_rtp_session_manager_request_rtp_sink (self->manager, self, media);
//   kms_base_rtp_session_link_pads (src, sink);
//   g_object_unref (src);
//   g_object_unref (sink);

//   /* RTCP */
//   src = kms_i_rtp_connection_request_rtcp_src (conn);
//   sink = kms_i_rtp_session_manager_request_rtcp_sink (self->manager, self, media);
//   kms_base_rtp_session_link_pads (src, sink);
//   g_object_unref (src);
//   g_object_unref (sink);
// }

static void
kms_base_rtp_session_add_gst_rtcp_mux_elements (KmsBaseRtpSession *self,
    KmsIRtpConnection *conn, const GstSDPMedia *media, gboolean active)
{
  GstPad *src, *sink;

  kms_i_rtp_connection_add (conn, GST_BIN (self), active);
  kms_i_rtp_connection_sink_sync_state_with_parent (conn);

  /* RTP */
  src = kms_i_rtp_connection_request_rtp_src (conn);
  sink =
      kms_i_rtp_session_manager_request_rtp_sink (self->manager, self, media);
  kms_base_rtp_session_link_pads (src, sink);
  g_object_unref (src);
  g_object_unref (sink);

  /* RTCP */
  src = kms_i_rtp_connection_request_rtcp_src (conn);
  sink =
      kms_i_rtp_session_manager_request_rtcp_sink (self->manager, self, media);
  kms_base_rtp_session_link_pads (src, sink);
  g_object_unref (src);
  g_object_unref (sink);

  kms_base_rtp_session_link_gst_connection_sink (self, conn, media);

  kms_i_rtp_connection_src_sync_state_with_parent (conn);
}

static void
kms_base_rtp_session_add_gst_basic_elements (KmsBaseRtpSession *self,
    KmsIRtpConnection *conn, const GstSDPMedia *media, gboolean active)
{
  kms_i_rtp_connection_add (conn, GST_BIN (self), active);
  kms_i_rtp_connection_sink_sync_state_with_parent (conn);

  kms_base_rtp_session_link_gst_connection_sink (self, conn, media);
  kms_base_rtp_session_link_gst_connection_src (self, conn, media);

  kms_i_rtp_connection_src_sync_state_with_parent (conn);
}

static gboolean
kms_base_rtp_session_add_gst_connection_elements (KmsBaseRtpSession *self,
    KmsSdpMediaHandler *handler, const GstSDPMedia *media, gboolean active)
{
  KmsIRtpConnection *conn;
  gint hid, gid;

  conn = kms_base_rtp_session_get_connection (self, handler);
  if (conn == NULL) {
    return FALSE;
  }

  g_object_get (handler, "id", &hid, NULL);
  gid = kms_sdp_agent_get_handler_group_id (KMS_SDP_SESSION (self)->agent, hid);

  if (gid >= 0) {
    // BUNDLE connection
    kms_base_rtp_session_add_gst_bundle_elements (self, conn, media, active);
    kms_base_rtp_session_link_gst_connection_sink (self, conn, media);
  } else if (gst_sdp_media_get_attribute_val (media, "rtcp-mux") != NULL) {
    // Multiplexed RTP & RTCP connection (both go through the same port)
    kms_base_rtp_session_add_gst_rtcp_mux_elements (self, conn, media, active);
  } else {
    // Basic connection (typical separated ports for RTP and RTCP)
    kms_base_rtp_session_add_gst_basic_elements (self, conn, media, active);
  }

  return TRUE;
}

static const gchar *
kms_base_rtp_session_process_remote_ssrc (KmsBaseRtpSession *self,
    const GstSDPMedia *remote_media, const GstSDPMedia *neg_media)
{
  const gchar *media_str = gst_sdp_media_get_media (remote_media);
  guint ssrc;

  ssrc = sdp_utils_media_get_fid_ssrc (remote_media, 0);
  if (ssrc == SSRC_INVALID) {
    ssrc = sdp_utils_media_get_ssrc (remote_media);
  }

  if (g_strcmp0 (AUDIO_STREAM_NAME, media_str) == 0) {
    if (ssrc != SSRC_INVALID) {
      GST_DEBUG_OBJECT (self, "Add remote audio ssrc: %u", ssrc);
      self->remote_audio_ssrc = ssrc;
    } else {
      GST_DEBUG_OBJECT (self, "Remote SDP doesn't include audio SSRC");
    }

    if (self->audio_neg != NULL) {
      gst_sdp_media_free (self->audio_neg);
    }
    gst_sdp_media_copy (neg_media, &self->audio_neg);

    return AUDIO_RTP_SESSION_STR;
  } else if (g_strcmp0 (VIDEO_STREAM_NAME, media_str) == 0) {
    if (ssrc != SSRC_INVALID) {
      GST_DEBUG_OBJECT (self, "Add remote video ssrc: %u", ssrc);
      self->remote_video_ssrc = ssrc;
    } else {
      GST_DEBUG_OBJECT (self, "Remote SDP doesn't include video SSRC");
    }

    if (self->video_neg != NULL) {
      gst_sdp_media_free (self->video_neg);
    }
    gst_sdp_media_copy (neg_media, &self->video_neg);

    return VIDEO_RTP_SESSION_STR;
  }

  GST_WARNING_OBJECT (self, "Media '%s' not supported", media_str);

  return NULL;
}

static gboolean
kms_base_rtp_session_configure_media_connection (KmsBaseRtpSession *self,
    KmsSdpMediaHandler *handler,
    const GstSDPMedia *neg_media,
    const GstSDPMedia *remote_media, gboolean offerer)
{
  const gchar *neg_proto_str = gst_sdp_media_get_proto (neg_media);
  const gchar *neg_media_str = gst_sdp_media_get_media (neg_media);
  const gchar *remote_proto_str = gst_sdp_media_get_proto (remote_media);
  const gchar *remote_media_str = gst_sdp_media_get_media (remote_media);
  gboolean active;

  if (g_strcmp0 (neg_proto_str, remote_proto_str) != 0) {
    GST_WARNING_OBJECT (self,
        "Negotiated proto ('%s') not matching with remote proto ('%s')",
        neg_proto_str, remote_proto_str);
    return FALSE;
  }

  if (!kms_utils_contains_proto (neg_proto_str, "RTP")) {
    GST_DEBUG_OBJECT (self, "'%s' protocol does not need RTP connection",
        neg_proto_str);
    /* It cannot be managed here but could be managed by the child class */
    return FALSE;
  }

  if (g_strcmp0 (neg_media_str, remote_media_str) != 0) {
    GST_WARNING_OBJECT (self,
        "Negotiated media ('%s') not matching with remote media ('%s')",
        neg_media_str, remote_media_str);
    return FALSE;
  }

  if (kms_base_rtp_session_process_remote_ssrc (self, remote_media, neg_media)
      == NULL) {
    return TRUE;                /* It cannot be managed here but could be managed by the child class */
  }

  active = sdp_utils_media_is_active (neg_media, offerer);

  return kms_base_rtp_session_add_gst_connection_elements (self, handler,
      neg_media, active);
}

static void
kms_base_rtp_session_update_conn_state (KmsBaseRtpSession *self)
{
  GHashTableIter iter;
  gpointer key, v;
  gboolean emit = FALSE;
  KmsConnectionState new_state = KMS_CONNECTION_STATE_CONNECTED;

  KMS_SDP_SESSION_LOCK (self);

  g_hash_table_iter_init (&iter, self->conns);
  while (g_hash_table_iter_next (&iter, &key, &v)) {
    KmsIRtpConnection *conn = KMS_I_RTP_CONNECTION (v);
    gboolean connected;

    g_object_get (conn, "connected", &connected, NULL);
    if (!connected) {
      new_state = KMS_CONNECTION_STATE_DISCONNECTED;
      break;
    }
  }

  if (self->conn_state != new_state) {
    GST_DEBUG_OBJECT (self, "Connection state changed to '%d'", new_state);
    self->conn_state = new_state;
    emit = TRUE;
  }

  KMS_SDP_SESSION_UNLOCK (self);

  if (emit) {
    g_signal_emit (G_OBJECT (self), obj_signals[CONNECTION_STATE_CHANGED], 0,
        new_state);
  }
}

static void
kms_base_rtp_session_connected_cb (KmsIRtpConnection *conn, gpointer user_data)
{
  KmsBaseRtpSession *self = KMS_BASE_RTP_SESSION (user_data);

  kms_base_rtp_session_update_conn_state (self);
}

static void
kms_base_rtp_session_check_conn_status (KmsBaseRtpSession *self)
{
  GHashTableIter iter;
  gpointer key, v;

  KMS_SDP_SESSION_LOCK (self);

  g_hash_table_iter_init (&iter, self->conns);
  while (g_hash_table_iter_next (&iter, &key, &v)) {
    KmsIRtpConnection *conn = KMS_I_RTP_CONNECTION (v);

    g_signal_connect_data (conn, "connected",
        G_CALLBACK (kms_base_rtp_session_connected_cb), self, NULL, 0);
  }

  KMS_SDP_SESSION_UNLOCK (self);

  kms_base_rtp_session_update_conn_state (self);
}

void
kms_base_rtp_session_start_transport_send (KmsBaseRtpSession *self,
    gboolean offerer)
{
  KmsSdpSession *sdp_sess = KMS_SDP_SESSION (self);
  guint i, len;

  kms_base_rtp_session_check_conn_status (self);

  len = gst_sdp_message_medias_len (sdp_sess->neg_sdp);

  if (len != gst_sdp_message_medias_len (sdp_sess->remote_sdp)) {
    GST_ERROR_OBJECT (self, "Remote SDP has different number of medias");
    g_assert_not_reached ();
  }

  for (i = 0; i < len; i++) {
    const GstSDPMedia *neg_media =
        gst_sdp_message_get_media (sdp_sess->neg_sdp, i);
    const GstSDPMedia *rem_media =
        gst_sdp_message_get_media (sdp_sess->remote_sdp, i);
    KmsSdpMediaHandler *handler;

    if (sdp_utils_media_is_inactive (neg_media)) {
      GST_DEBUG_OBJECT (self, "Media is inactive (id=%u)", i);
      continue;
    }

    handler =
        kms_sdp_agent_get_handler_by_index (KMS_SDP_SESSION (self)->agent, i);

    if (handler == NULL) {
      GST_ERROR_OBJECT (self, "Cannot get handler for media (id=%u)", i);
      continue;
    }

    if (!kms_base_rtp_session_configure_media_connection (self, handler,
            neg_media, rem_media, offerer)) {
      GST_WARNING_OBJECT (self, "Cannot configure connection for media (id=%u)",
          i);
    }

    g_object_unref (handler);
  }
}

/* Start Transport Send end */

static void
kms_base_rtp_session_enable_connection_stats (gpointer key,
    gpointer value, gpointer user_data)
{
  kms_i_rtp_connection_collect_latency_stats (KMS_I_RTP_CONNECTION (value),
      TRUE);
}

static void
kms_base_rtp_session_disable_connection_stats (gpointer key,
    gpointer value, gpointer user_data)
{
  kms_i_rtp_connection_collect_latency_stats (KMS_I_RTP_CONNECTION (value),
      FALSE);
}

void
kms_base_rtp_session_enable_connections_stats (KmsBaseRtpSession *self)
{
  self->stats_enabled = TRUE;

  g_hash_table_foreach (self->conns,
      kms_base_rtp_session_enable_connection_stats, NULL);
}

void
kms_base_rtp_session_disable_connections_stats (KmsBaseRtpSession *self)
{
  self->stats_enabled = FALSE;

  g_hash_table_foreach (self->conns,
      kms_base_rtp_session_disable_connection_stats, NULL);
}

static void
kms_base_rtp_session_get_property (GObject *object,
    guint property_id, GValue *value, GParamSpec *pspec)
{
  KmsBaseRtpSession *self = KMS_BASE_RTP_SESSION (object);

  KMS_SDP_SESSION_LOCK (self);

  switch (property_id) {
    case PROP_CONNECTION_STATE:
      g_value_set_enum (value, self->conn_state);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }

  KMS_SDP_SESSION_UNLOCK (self);
}

static void
kms_base_rtp_session_finalize (GObject *object)
{
  KmsBaseRtpSession *self = KMS_BASE_RTP_SESSION (object);

  GST_DEBUG_OBJECT (self, "finalize");

  if (self->audio_neg != NULL) {
    gst_sdp_media_free (self->audio_neg);
  }

  if (self->video_neg != NULL) {
    gst_sdp_media_free (self->video_neg);
  }

  g_hash_table_destroy (self->conns);

  /* chain up */
  G_OBJECT_CLASS (kms_base_rtp_session_parent_class)->finalize (object);
}

static void
kms_base_rtp_session_post_constructor (KmsBaseRtpSession *self,
    KmsBaseSdpEndpoint *ep, guint id, KmsIRtpSessionManager *manager)
{
  KmsSdpSession *sdp_sess = KMS_SDP_SESSION (self);

  self->manager = manager;
  KMS_SDP_SESSION_CLASS (kms_base_rtp_session_parent_class)
      ->post_constructor (sdp_sess, ep, id);
}

static void
kms_base_rtp_session_init (KmsBaseRtpSession *self)
{
  self->conns =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  self->local_audio_ssrc = SSRC_INVALID;
  self->remote_audio_ssrc = SSRC_INVALID;

  self->local_video_ssrc = SSRC_INVALID;
  self->remote_video_ssrc = SSRC_INVALID;

  self->stats_enabled = FALSE;
}

static void
kms_base_rtp_session_class_init (KmsBaseRtpSessionClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
      GST_DEFAULT_NAME);

  gobject_class->finalize = kms_base_rtp_session_finalize;
  gobject_class->get_property = kms_base_rtp_session_get_property;

  klass->post_constructor = kms_base_rtp_session_post_constructor;

  /* Connection management */
  klass->create_connection = kms_base_rtp_session_create_connection_default;
  klass->create_rtcp_mux_connection =
      kms_base_rtp_session_create_rtcp_mux_connection_default;
  klass->create_bundle_connection =
      kms_base_rtp_session_create_bundle_connection_default;

  gst_element_class_set_details_simple (gstelement_class, "BaseRtpSession",
      "Generic", "Base bin to manage elements related with a RTP session.",
      "Miguel París Díaz <mparisdiaz@gmail.com>");

  g_object_class_install_property (gobject_class, PROP_CONNECTION_STATE,
      g_param_spec_enum ("connection-state", "Connection state",
          "Connection state", KMS_TYPE_CONNECTION_STATE,
          DEFAULT_CONNECTION_STATE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  obj_signals[CONNECTION_STATE_CHANGED] =
      g_signal_new ("connection-state-changed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (KmsBaseRtpSessionClass,
          connection_state_changed), NULL, NULL, g_cclosure_marshal_VOID__ENUM,
      G_TYPE_NONE, 1, KMS_TYPE_CONNECTION_STATE);

  obj_signals[DTMF_EVENT_DETECTED] = g_signal_new ("dtmf-event-detected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (KmsBaseRtpSessionClass, dtmf_event_detected),  // No class offset
      NULL, NULL, __kms_core_marshal_VOID__INT_BOOLEAN_INT_INT_STRING,  // Custom marshaller
      G_TYPE_NONE, 5, G_TYPE_INT,       // number
      G_TYPE_BOOLEAN,           // end
      G_TYPE_INT,               // volume
      G_TYPE_INT,               // duration
      G_TYPE_STRING);           // media_type
}
