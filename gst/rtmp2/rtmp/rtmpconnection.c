/* GStreamer RTMP Library
 * Copyright (C) 2013 David Schleef <ds@schleef.org>
 * Copyright (C) 2017 Make.TV, Inc. <info@make.tv>
 *   Contact: Jan Alexander Steffens (heftig) <jsteffens@make.tv>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <string.h>
#include <math.h>
#include "rtmpconnection.h"
#include "rtmpchunkstream.h"
#include "rtmpmessage.h"
#include "rtmputils.h"
#include "amf.h"

GST_DEBUG_CATEGORY_STATIC (gst_rtmp_connection_debug_category);
#define GST_CAT_DEFAULT gst_rtmp_connection_debug_category

#define READ_SIZE 8192

typedef void (*GstRtmpConnectionCallback) (GstRtmpConnection * connection);

struct _GstRtmpConnection
{
  GObject parent_instance;

  /* should be properties */
  gboolean input_paused;
  gboolean error;

  /* private */
  GThread *thread;
  GSocketConnection *connection;
  GCancellable *cancellable;
  GSocketClient *socket_client;
  GAsyncQueue *output_queue;
  GMainContext *main_context;

  GSource *input_source;
  GByteArray *input_bytes;
  guint input_needed_bytes;
  GstRtmpChunkStreams *input_streams, *output_streams;
  GList *transactions;
  GList *expected_commands;
  guint transaction_count;

  GstRtmpConnectionMessageFunc input_handler;
  gpointer input_handler_user_data;
  GDestroyNotify input_handler_user_data_destroy;

  GstRtmpConnectionFunc output_handler;
  gpointer output_handler_user_data;
  GDestroyNotify output_handler_user_data_destroy;

  gboolean writing;

  /* RTMP configuration */
  guint32 in_chunk_size;
  guint32 out_chunk_size;
  guint32 in_window_ack_size;
  guint32 out_window_ack_size;

  guint64 in_bytes_total;
  guint64 in_bytes_acked;
};


typedef struct
{
  GObjectClass parent_class;
} GstRtmpConnectionClass;

/* prototypes */

static void gst_rtmp_connection_dispose (GObject * object);
static void gst_rtmp_connection_finalize (GObject * object);
static void gst_rtmp_connection_emit_error (GstRtmpConnection * self);
static gboolean gst_rtmp_connection_input_ready (GInputStream * is,
    gpointer user_data);
static void gst_rtmp_connection_start_write (GstRtmpConnection * self);
static void gst_rtmp_connection_write_buffer_done (GObject * obj,
    GAsyncResult * result, gpointer user_data);
static void gst_rtmp_connection_start_read (GstRtmpConnection * sc,
    guint needed_bytes);
static void gst_rtmp_connection_try_read (GstRtmpConnection * sc);
static void gst_rtmp_connection_do_read (GstRtmpConnection * sc);
static void gst_rtmp_connection_handle_protocol_control (GstRtmpConnection *
    connection, GstBuffer * buffer);
static void gst_rtmp_connection_handle_cm (GstRtmpConnection * connection,
    GstBuffer * buffer);
static void gst_rtmp_connection_handle_user_control (GstRtmpConnection * sc,
    GstBuffer * buffer);
static void gst_rtmp_connection_handle_message (GstRtmpConnection * sc,
    GstBuffer * buffer);
static void gst_rtmp_connection_handle_set_chunk_size (GstRtmpConnection * self,
    guint32 in_chunk_size);
static void gst_rtmp_connection_handle_window_ack_size (GstRtmpConnection *
    self, guint32 in_chunk_size);

static void gst_rtmp_connection_send_ack (GstRtmpConnection * connection);
static void
gst_rtmp_connection_send_ping_response (GstRtmpConnection * connection,
    guint32 event_data);

typedef struct
{
  gdouble transaction_id;
  GstRtmpCommandCallback func;
  gpointer user_data;
} Transaction;

static Transaction *
transaction_new (gdouble transaction_id, GstRtmpCommandCallback func,
    gpointer user_data)
{
  Transaction *data = g_slice_new (Transaction);
  data->transaction_id = transaction_id;
  data->func = func;
  data->user_data = user_data;
  return data;
}

static void
transaction_free (gpointer ptr)
{
  Transaction *data = ptr;
  g_slice_free (Transaction, data);
}

typedef struct
{
  guint32 stream_id;
  gchar *command_name;
  GstRtmpCommandCallback func;
  gpointer user_data;
} ExpectedCommand;

static ExpectedCommand *
expected_command_new (guint32 stream_id, const gchar * command_name,
    GstRtmpCommandCallback func, gpointer user_data)
{
  ExpectedCommand *data = g_slice_new (ExpectedCommand);
  data->stream_id = stream_id;
  data->command_name = g_strdup (command_name);
  data->func = func;
  data->user_data = user_data;
  return data;
}

static void
expected_command_free (gpointer ptr)
{
  ExpectedCommand *data = ptr;
  g_free (data->command_name);
  g_slice_free (ExpectedCommand, data);
}

enum
{
  SIGNAL_ERROR,
  SIGNAL_STREAM_CONTROL,

  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0, };

/* singletons */

static GstMemory *set_data_frame_value;

static void
init_set_data_frame_value (void)
{
  GstAmfNode *node = gst_amf_node_new_string ("@setDataFrame", -1);
  GBytes *bytes = gst_amf_node_serialize (node);
  gsize size;
  const gchar *data = g_bytes_get_data (bytes, &size);

  set_data_frame_value = gst_memory_new_wrapped (GST_MEMORY_FLAG_READONLY,
      (gpointer) data, size, 0, size, bytes, (GDestroyNotify) g_bytes_unref);
  gst_amf_node_free (node);
}

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstRtmpConnection, gst_rtmp_connection,
    G_TYPE_OBJECT,
    GST_DEBUG_CATEGORY_INIT (gst_rtmp_connection_debug_category,
        "rtmpconnection", 0, "debug category for GstRtmpConnection class");
    init_set_data_frame_value ());

static void
gst_rtmp_connection_class_init (GstRtmpConnectionClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = gst_rtmp_connection_dispose;
  gobject_class->finalize = gst_rtmp_connection_finalize;

  signals[SIGNAL_ERROR] = g_signal_new ("error", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_STREAM_CONTROL] = g_signal_new ("stream-control",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_UINT);

  GST_DEBUG_REGISTER_FUNCPTR (gst_rtmp_connection_do_read);
}

static void
gst_rtmp_connection_init (GstRtmpConnection * rtmpconnection)
{
  rtmpconnection->cancellable = g_cancellable_new ();
  rtmpconnection->output_queue =
      g_async_queue_new_full ((GDestroyNotify) gst_buffer_unref);
  rtmpconnection->input_streams = gst_rtmp_chunk_streams_new ();
  rtmpconnection->output_streams = gst_rtmp_chunk_streams_new ();

  rtmpconnection->in_chunk_size = GST_RTMP_DEFAULT_CHUNK_SIZE;
  rtmpconnection->out_chunk_size = GST_RTMP_DEFAULT_CHUNK_SIZE;

  rtmpconnection->input_bytes = g_byte_array_sized_new (2 * READ_SIZE);
  rtmpconnection->input_needed_bytes = 1;
}

void
gst_rtmp_connection_dispose (GObject * object)
{
  GstRtmpConnection *rtmpconnection = GST_RTMP_CONNECTION (object);
  GST_DEBUG_OBJECT (rtmpconnection, "dispose");

  /* clean up as possible.  may be called multiple times */

  gst_rtmp_connection_close (rtmpconnection);
  g_cancellable_cancel (rtmpconnection->cancellable);
  gst_rtmp_connection_set_input_handler (rtmpconnection, NULL, NULL, NULL);
  gst_rtmp_connection_set_output_handler (rtmpconnection, NULL, NULL, NULL);

  G_OBJECT_CLASS (gst_rtmp_connection_parent_class)->dispose (object);
}

void
gst_rtmp_connection_finalize (GObject * object)
{
  GstRtmpConnection *rtmpconnection = GST_RTMP_CONNECTION (object);
  GST_DEBUG_OBJECT (rtmpconnection, "finalize");

  /* clean up object here */

  g_clear_object (&rtmpconnection->cancellable);
  g_clear_object (&rtmpconnection->connection);
  g_clear_pointer (&rtmpconnection->output_queue, g_async_queue_unref);
  g_clear_pointer (&rtmpconnection->input_streams, gst_rtmp_chunk_streams_free);
  g_clear_pointer (&rtmpconnection->output_streams,
      gst_rtmp_chunk_streams_free);
  g_clear_pointer (&rtmpconnection->input_bytes, g_byte_array_unref);
  g_clear_pointer (&rtmpconnection->main_context, g_main_context_unref);
  g_clear_pointer (&rtmpconnection->thread, g_thread_unref);

  G_OBJECT_CLASS (gst_rtmp_connection_parent_class)->finalize (object);
}

GSocket *
gst_rtmp_connection_get_socket (GstRtmpConnection * sc)
{
  return g_socket_connection_get_socket (sc->connection);
}

static void
gst_rtmp_connection_set_socket_connection (GstRtmpConnection * sc,
    GSocketConnection * connection)
{
  GInputStream *is;

  sc->thread = g_thread_ref (g_thread_self ());
  sc->main_context = g_main_context_ref_thread_default ();
  sc->connection = g_object_ref (connection);

  /* refs the socket because it's creating an input stream, which holds a ref */
  is = g_io_stream_get_input_stream (G_IO_STREAM (sc->connection));
  /* refs the socket because it's creating a socket source */
  g_warn_if_fail (!sc->input_source);
  sc->input_source =
      g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (is),
      sc->cancellable);
  g_source_set_callback (sc->input_source,
      (GSourceFunc) gst_rtmp_connection_input_ready, g_object_ref (sc),
      g_object_unref);
  g_source_attach (sc->input_source, sc->main_context);
}

GstRtmpConnection *
gst_rtmp_connection_new (GSocketConnection * connection)
{
  GstRtmpConnection *sc;

  sc = g_object_new (GST_TYPE_RTMP_CONNECTION, NULL);

  gst_rtmp_connection_set_socket_connection (sc, connection);

  return sc;
}

static void
cancel_all_commands (GstRtmpConnection * self)
{
  GList *l;

  for (l = self->transactions; l; l = g_list_next (l)) {
    Transaction *cc = l->data;
    GST_LOG_OBJECT (self, "calling transaction callback %s",
        GST_DEBUG_FUNCPTR_NAME (cc->func));
    cc->func ("<cancelled>", NULL, cc->user_data);
  }
  g_list_free_full (self->transactions, transaction_free);
  self->transactions = NULL;

  for (l = self->expected_commands; l; l = g_list_next (l)) {
    ExpectedCommand *cc = l->data;
    GST_LOG_OBJECT (self, "calling expected command callback %s",
        GST_DEBUG_FUNCPTR_NAME (cc->func));
    cc->func ("<cancelled>", NULL, cc->user_data);
  }
  g_list_free_full (self->expected_commands, expected_command_free);
  self->expected_commands = NULL;
}

void
gst_rtmp_connection_close (GstRtmpConnection * self)
{
  if (self->thread != g_thread_self ()) {
    GST_ERROR_OBJECT (self, "Called from wrong thread");
  }

  g_cancellable_cancel (self->cancellable);
  cancel_all_commands (self);

  if (self->input_source) {
    g_source_destroy (self->input_source);
    g_clear_pointer (&self->input_source, g_source_unref);
  }

  if (self->connection) {
    g_io_stream_close_async (G_IO_STREAM (self->connection),
        G_PRIORITY_DEFAULT, NULL, NULL, NULL);
  }
}

void
gst_rtmp_connection_close_and_unref (gpointer ptr)
{
  GstRtmpConnection *connection;

  g_return_if_fail (ptr);

  connection = GST_RTMP_CONNECTION (ptr);
  gst_rtmp_connection_close (connection);
  g_object_unref (connection);
}

void
gst_rtmp_connection_set_input_handler (GstRtmpConnection * sc,
    GstRtmpConnectionMessageFunc callback, gpointer user_data,
    GDestroyNotify user_data_destroy)
{
  if (sc->input_handler_user_data_destroy) {
    sc->input_handler_user_data_destroy (sc->input_handler_user_data);
  }

  sc->input_handler = callback;
  sc->input_handler_user_data = user_data;
  sc->input_handler_user_data_destroy = user_data_destroy;
}

void
gst_rtmp_connection_set_output_handler (GstRtmpConnection * sc,
    GstRtmpConnectionFunc callback, gpointer user_data,
    GDestroyNotify user_data_destroy)
{
  if (sc->output_handler_user_data_destroy) {
    sc->output_handler_user_data_destroy (sc->output_handler_user_data);
  }

  sc->output_handler = callback;
  sc->output_handler_user_data = user_data;
  sc->output_handler_user_data_destroy = user_data_destroy;
}

static gboolean
gst_rtmp_connection_input_ready (GInputStream * is, gpointer user_data)
{
  GstRtmpConnection *sc = user_data;
  gssize ret;
  guint oldsize;
  GError *error = NULL;
  guint64 bytes_since_ack;

  GST_TRACE_OBJECT (sc, "input ready");

  oldsize = sc->input_bytes->len;
  g_byte_array_set_size (sc->input_bytes, oldsize + READ_SIZE);
  ret =
      g_pollable_input_stream_read_nonblocking (G_POLLABLE_INPUT_STREAM (is),
      sc->input_bytes->data + oldsize, READ_SIZE, sc->cancellable, &error);
  g_byte_array_set_size (sc->input_bytes, oldsize + (ret > 0 ? ret : 0));

  if (ret < 0) {
    gint code = error->code;

    if (error->domain == G_IO_ERROR && (code == G_IO_ERROR_WOULD_BLOCK ||
            code == G_IO_ERROR_TIMED_OUT || code == G_IO_ERROR_AGAIN)) {
      /* should retry */
      GST_DEBUG_OBJECT (sc, "read IO error %d %s, continuing",
          code, error->message);
      g_error_free (error);
      return G_SOURCE_CONTINUE;
    }

    GST_ERROR_OBJECT (sc, "read error: %s %d %s",
        g_quark_to_string (error->domain), code, error->message);
    g_error_free (error);
  } else if (ret == 0) {
    GST_INFO_OBJECT (sc, "read EOF");
  }

  if (ret <= 0) {
    gst_rtmp_connection_emit_error (sc);
    return G_SOURCE_REMOVE;
  }

  GST_TRACE_OBJECT (sc, "read %" G_GSIZE_FORMAT " bytes", ret);

  sc->in_bytes_total += ret;
  bytes_since_ack = sc->in_bytes_total - sc->in_bytes_acked;
  if (sc->in_window_ack_size && bytes_since_ack >= sc->in_window_ack_size) {
    gst_rtmp_connection_send_ack (sc);
  }

  gst_rtmp_connection_try_read (sc);
  return G_SOURCE_CONTINUE;
}

static void
gst_rtmp_connection_start_write (GstRtmpConnection * self)
{
  GOutputStream *os;
  GstBuffer *chunks;

  if (self->writing) {
    return;
  }

  chunks = g_async_queue_try_pop (self->output_queue);
  if (!chunks) {
    return;
  }

  self->writing = TRUE;
  if (self->output_handler) {
    self->output_handler (self, self->output_handler_user_data);
  }

  os = g_io_stream_get_output_stream (G_IO_STREAM (self->connection));
  gst_rtmp_output_stream_write_all_buffer_async (os, chunks, G_PRIORITY_DEFAULT,
      self->cancellable, gst_rtmp_connection_write_buffer_done,
      g_object_ref (self));

  gst_buffer_unref (chunks);
}

static void
gst_rtmp_connection_emit_error (GstRtmpConnection * self)
{
  if (self->error) {
    return;
  }

  GST_INFO_OBJECT (self, "connection error");
  self->error = TRUE;

  cancel_all_commands (self);

  g_signal_emit (self, signals[SIGNAL_ERROR], 0);
}

static void
gst_rtmp_connection_write_buffer_done (GObject * obj,
    GAsyncResult * result, gpointer user_data)
{
  GOutputStream *os = G_OUTPUT_STREAM (obj);
  GstRtmpConnection *self = GST_RTMP_CONNECTION (user_data);
  GError *error = NULL;
  gboolean res;

  self->writing = FALSE;

  res = gst_rtmp_output_stream_write_all_buffer_finish (os, result, NULL,
      &error);

  if (!res) {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GST_INFO_OBJECT (self, "write cancelled");
    } else {
      GST_ERROR_OBJECT (self, "write error: %s", error->message);
    }
    gst_rtmp_connection_emit_error (self);
    g_error_free (error);
    g_object_unref (self);
    return;
  }

  GST_LOG_OBJECT (self, "write completed");
  gst_rtmp_connection_start_write (self);
  g_object_unref (self);
}

static void
gst_rtmp_connection_start_read (GstRtmpConnection * connection,
    guint needed_bytes)
{
  g_return_if_fail (needed_bytes > 0);
  connection->input_needed_bytes = needed_bytes;
  gst_rtmp_connection_try_read (connection);
}

static void
gst_rtmp_connection_try_read (GstRtmpConnection * connection)
{
  guint need = connection->input_needed_bytes,
      len = connection->input_bytes->len;

  if (len < need) {
    GST_TRACE_OBJECT (connection, "got %u < %u bytes, need more", len, need);
    return;
  }

  GST_TRACE_OBJECT (connection, "got %u >= %u bytes, proceeding", len, need);
  gst_rtmp_connection_do_read (connection);
}

static void
gst_rtmp_connection_take_input_bytes (GstRtmpConnection * sc, gsize size,
    GBytes ** outbytes)
{
  g_return_if_fail (size <= sc->input_bytes->len);

  if (outbytes) {
    *outbytes = g_bytes_new (sc->input_bytes->data, size);
  }

  g_byte_array_remove_range (sc->input_bytes, 0, size);
}

static void
gst_rtmp_connection_do_read (GstRtmpConnection * sc)
{
  GByteArray *input_bytes = sc->input_bytes;
  gsize needed_bytes = 1;

  while (1) {
    GstRtmpChunkStream *cstream;
    guint32 chunk_stream_id, header_size, next_size;
    guint8 *data;

    chunk_stream_id = gst_rtmp_chunk_stream_parse_id (input_bytes->data,
        input_bytes->len);

    if (!chunk_stream_id) {
      needed_bytes = input_bytes->len + 1;
      break;
    }

    cstream = gst_rtmp_chunk_streams_get (sc->input_streams, chunk_stream_id);
    header_size = gst_rtmp_chunk_stream_parse_header (cstream,
        input_bytes->data, input_bytes->len);

    if (input_bytes->len < header_size) {
      needed_bytes = header_size;
      break;
    }

    next_size = gst_rtmp_chunk_stream_parse_payload (cstream,
        sc->in_chunk_size, &data);

    if (input_bytes->len < header_size + next_size) {
      needed_bytes = header_size + next_size;
      break;
    }

    memcpy (data, input_bytes->data + header_size, next_size);
    gst_rtmp_connection_take_input_bytes (sc, header_size + next_size, NULL);

    next_size = gst_rtmp_chunk_stream_wrote_payload (cstream,
        sc->in_chunk_size);

    if (next_size == 0) {
      GstBuffer *buffer = gst_rtmp_chunk_stream_parse_finish (cstream);
      gst_rtmp_connection_handle_message (sc, buffer);
      gst_buffer_unref (buffer);
    }
  }

  gst_rtmp_connection_start_read (sc, needed_bytes);
}

static void
gst_rtmp_connection_handle_message (GstRtmpConnection * sc, GstBuffer * buffer)
{
  if (gst_rtmp_message_is_protocol_control (buffer)) {
    gst_rtmp_connection_handle_protocol_control (sc, buffer);
    return;
  }

  if (gst_rtmp_message_is_user_control (buffer)) {
    gst_rtmp_connection_handle_user_control (sc, buffer);
    return;
  }

  switch (gst_rtmp_message_get_type (buffer)) {
    case GST_RTMP_MESSAGE_TYPE_COMMAND_AMF0:
      gst_rtmp_connection_handle_cm (sc, buffer);
      return;

    default:
      if (sc->input_handler) {
        sc->input_handler (sc, buffer, sc->input_handler_user_data);
      }
      return;
  }
}

static void
gst_rtmp_connection_handle_protocol_control (GstRtmpConnection * connection,
    GstBuffer * buffer)
{
  GstRtmpProtocolControl pc;

  if (!gst_rtmp_message_parse_protocol_control (buffer, &pc)) {
    GST_ERROR_OBJECT (connection, "can't parse protocol control message");
    return;
  }

  GST_LOG_OBJECT (connection, "got protocol control message %d:%s", pc.type,
      gst_rtmp_message_type_get_nick (pc.type));

  switch (pc.type) {
    case GST_RTMP_MESSAGE_TYPE_SET_CHUNK_SIZE:
      GST_INFO_OBJECT (connection, "incoming chunk size %" G_GUINT32_FORMAT,
          pc.param);
      gst_rtmp_connection_handle_set_chunk_size (connection, pc.param);
      break;

    case GST_RTMP_MESSAGE_TYPE_ABORT_MESSAGE:
      GST_ERROR_OBJECT (connection, "unimplemented: chunk abort, stream_id = %"
          G_GUINT32_FORMAT, pc.param);
      break;

    case GST_RTMP_MESSAGE_TYPE_ACKNOWLEDGEMENT:
      /* We don't really send ack requests that we care about, so ignore */
      GST_DEBUG_OBJECT (connection, "acknowledgement %" G_GUINT32_FORMAT,
          pc.param);
      break;

    case GST_RTMP_MESSAGE_TYPE_WINDOW_ACK_SIZE:
      GST_INFO_OBJECT (connection,
          "incoming window ack size: %" G_GUINT32_FORMAT, pc.param);
      gst_rtmp_connection_handle_window_ack_size (connection, pc.param);
      break;

    case GST_RTMP_MESSAGE_TYPE_SET_PEER_BANDWIDTH:
      GST_FIXME_OBJECT (connection, "set peer bandwidth: %" G_GUINT32_FORMAT
          ", %" G_GUINT32_FORMAT, pc.param, pc.param2);
      /* FIXME this is not correct, but close enough */
      gst_rtmp_connection_request_window_size (connection, pc.param);
      break;

    default:
      GST_ERROR_OBJECT (connection, "unimplemented protocol control type %d:%s",
          pc.type, gst_rtmp_message_type_get_nick (pc.type));
      break;
  }
}

static void
gst_rtmp_connection_handle_user_control (GstRtmpConnection * connection,
    GstBuffer * buffer)
{
  GstRtmpUserControl uc;

  if (!gst_rtmp_message_parse_user_control (buffer, &uc)) {
    GST_ERROR_OBJECT (connection, "can't parse user control message");
    return;
  }

  GST_LOG_OBJECT (connection, "got user control message %d:%s", uc.type,
      gst_rtmp_user_control_type_get_nick (uc.type));

  switch (uc.type) {
    case GST_RTMP_USER_CONTROL_TYPE_STREAM_BEGIN:
    case GST_RTMP_USER_CONTROL_TYPE_STREAM_EOF:
    case GST_RTMP_USER_CONTROL_TYPE_STREAM_DRY:
    case GST_RTMP_USER_CONTROL_TYPE_STREAM_IS_RECORDED:
      GST_INFO_OBJECT (connection, "stream %u got %s", uc.param,
          gst_rtmp_user_control_type_get_nick (uc.type));
      g_signal_emit (connection, signals[SIGNAL_STREAM_CONTROL], 0,
          uc.type, uc.param);
      break;

    case GST_RTMP_USER_CONTROL_TYPE_SET_BUFFER_LENGTH:
      GST_FIXME_OBJECT (connection, "ignoring set buffer length: %"
          G_GUINT32_FORMAT ", %" G_GUINT32_FORMAT " ms", uc.param, uc.param2);
      break;

    case GST_RTMP_USER_CONTROL_TYPE_PING_REQUEST:
      GST_DEBUG_OBJECT (connection, "ping request: %" G_GUINT32_FORMAT,
          uc.param);
      gst_rtmp_connection_send_ping_response (connection, uc.param);
      break;

    case GST_RTMP_USER_CONTROL_TYPE_PING_RESPONSE:
      GST_DEBUG_OBJECT (connection,
          "ignoring ping response: %" G_GUINT32_FORMAT, uc.param);
      break;

    case GST_RTMP_USER_CONTROL_TYPE_BUFFER_EMPTY:
      GST_LOG_OBJECT (connection, "ignoring buffer empty: %" G_GUINT32_FORMAT,
          uc.param);
      break;

    case GST_RTMP_USER_CONTROL_TYPE_BUFFER_READY:
      GST_LOG_OBJECT (connection, "ignoring buffer ready: %" G_GUINT32_FORMAT,
          uc.param);
      break;

    default:
      GST_ERROR_OBJECT (connection, "unimplemented user control type %d:%s",
          uc.type, gst_rtmp_user_control_type_get_nick (uc.type));
      break;
  }
}

static void
gst_rtmp_connection_handle_set_chunk_size (GstRtmpConnection * self,
    guint32 chunk_size)
{
  if (chunk_size < GST_RTMP_MINIMUM_CHUNK_SIZE) {
    GST_ERROR_OBJECT (self,
        "peer requested chunk size %" G_GUINT32_FORMAT "; too small",
        chunk_size);
    return;
  }

  if (chunk_size > GST_RTMP_MAXIMUM_CHUNK_SIZE) {
    GST_ERROR_OBJECT (self,
        "peer requested chunk size %" G_GUINT32_FORMAT "; too large",
        chunk_size);
    return;
  }

  if (chunk_size < GST_RTMP_DEFAULT_CHUNK_SIZE) {
    GST_WARNING_OBJECT (self,
        "peer requested small chunk size %" G_GUINT32_FORMAT, chunk_size);
  }

  self->in_chunk_size = chunk_size;
}

static void
gst_rtmp_connection_handle_window_ack_size (GstRtmpConnection * self,
    guint32 window_ack_size)
{
  if (window_ack_size < GST_RTMP_DEFAULT_WINDOW_ACK_SIZE) {
    GST_WARNING_OBJECT (self,
        "peer requested small window ack size %" G_GUINT32_FORMAT,
        window_ack_size);
  }

  self->in_window_ack_size = window_ack_size;
}

static gboolean
is_command_response (const gchar * command_name)
{
  return g_strcmp0 (command_name, "_result") == 0 ||
      g_strcmp0 (command_name, "_error") == 0;
}

static void
gst_rtmp_connection_handle_cm (GstRtmpConnection * sc, GstBuffer * buffer)
{
  GstRtmpMeta *meta;
  gchar *command_name;
  gdouble transaction_id;
  GPtrArray *args;

  meta = gst_buffer_get_rtmp_meta (buffer);
  g_return_if_fail (meta);

  {
    GstMapInfo map;
    gst_buffer_map (buffer, &map, GST_MAP_READ);
    args = gst_amf_parse_command (map.data, map.size, &transaction_id,
        &command_name);
    gst_buffer_unmap (buffer, &map);
  }

  if (!args) {
    return;
  }

  if (!isfinite (transaction_id) || transaction_id < 0 ||
      transaction_id > G_MAXUINT) {
    GST_WARNING_OBJECT (sc,
        "Server sent command \"%s\" with extreme transaction ID %.0f",
        GST_STR_NULL (command_name), transaction_id);
  } else if (transaction_id > sc->transaction_count) {
    GST_WARNING_OBJECT (sc,
        "Server sent command \"%s\" with unused transaction ID (%.0f > %u)",
        GST_STR_NULL (command_name), transaction_id, sc->transaction_count);
    sc->transaction_count = transaction_id;
  }

  GST_DEBUG_OBJECT (sc,
      "got control message \"%s\" transaction %.0f size %"
      G_GUINT32_FORMAT, GST_STR_NULL (command_name), transaction_id,
      meta->size);

  if (is_command_response (command_name)) {
    if (transaction_id != 0) {
      GList *l;

      for (l = sc->transactions; l; l = g_list_next (l)) {
        Transaction *t = l->data;

        if (t->transaction_id != transaction_id) {
          continue;
        }

        GST_LOG_OBJECT (sc, "calling transaction callback %s",
            GST_DEBUG_FUNCPTR_NAME (t->func));
        sc->transactions = g_list_remove_link (sc->transactions, l);
        t->func (command_name, args, t->user_data);
        g_list_free_full (l, transaction_free);
        break;
      }
    } else {
      GST_WARNING_OBJECT (sc, "Server sent response \"%s\" without transaction",
          GST_STR_NULL (command_name));
    }
  } else {
    GList *l;

    if (transaction_id != 0) {
      GST_FIXME_OBJECT (sc, "Server sent command \"%s\" expecting reply",
          GST_STR_NULL (command_name));
    }

    for (l = sc->expected_commands; l; l = g_list_next (l)) {
      ExpectedCommand *ec = l->data;

      if (ec->stream_id != meta->mstream) {
        continue;
      }

      if (g_strcmp0 (ec->command_name, command_name)) {
        continue;
      }

      GST_LOG_OBJECT (sc, "calling expected command callback %s",
          GST_DEBUG_FUNCPTR_NAME (ec->func));
      sc->expected_commands = g_list_remove_link (sc->expected_commands, l);
      ec->func (command_name, args, ec->user_data);
      g_list_free_full (l, expected_command_free);
      break;
    }
  }

  g_free (command_name);
  g_ptr_array_unref (args);
}

static gboolean
start_write (gpointer user_data)
{
  GstRtmpConnection *sc = user_data;
  gst_rtmp_connection_start_write (sc);
  return G_SOURCE_REMOVE;
}

void
gst_rtmp_connection_queue_message (GstRtmpConnection * self, GstBuffer * buffer)
{
  GstRtmpMeta *meta;
  GstRtmpChunkStream *cstream;
  GstBuffer *chunks;

  g_return_if_fail (GST_IS_RTMP_CONNECTION (self));
  g_return_if_fail (GST_IS_BUFFER (buffer));

  meta = gst_buffer_get_rtmp_meta (buffer);
  g_return_if_fail (meta);

  cstream = gst_rtmp_chunk_streams_get (self->output_streams, meta->cstream);
  g_return_if_fail (cstream);

  chunks = gst_rtmp_chunk_stream_serialize_all (cstream, buffer,
      self->out_chunk_size);
  g_return_if_fail (chunks);

  g_async_queue_push (self->output_queue, chunks);
  g_main_context_invoke_full (self->main_context, G_PRIORITY_DEFAULT,
      start_write, g_object_ref (self), g_object_unref);
}

guint
gst_rtmp_connection_get_num_queued (GstRtmpConnection * connection)
{
  return g_async_queue_length (connection->output_queue);
}

guint
gst_rtmp_connection_send_command (GstRtmpConnection * connection,
    GstRtmpCommandCallback response_command, gpointer user_data,
    guint32 stream_id, const gchar * command_name, const GstAmfNode * argument,
    ...)
{
  GstBuffer *buffer;
  gdouble transaction_id = 0;
  va_list ap;
  GBytes *payload;
  guint8 *data;
  gsize size;

  g_return_val_if_fail (GST_IS_RTMP_CONNECTION (connection), 0);

  if (connection->thread != g_thread_self ()) {
    GST_ERROR_OBJECT (connection, "Called from wrong thread");
  }

  GST_DEBUG_OBJECT (connection,
      "Sending command '%s' on stream id %" G_GUINT32_FORMAT,
      command_name, stream_id);

  if (response_command) {
    Transaction *t;

    transaction_id = ++connection->transaction_count;

    GST_LOG_OBJECT (connection, "Registering %s for transid %.0f",
        GST_DEBUG_FUNCPTR_NAME (response_command), transaction_id);

    t = transaction_new (transaction_id, response_command, user_data);

    connection->transactions = g_list_append (connection->transactions, t);
  }

  va_start (ap, argument);
  payload = gst_amf_serialize_command_valist (transaction_id,
      command_name, argument, ap);
  va_end (ap);

  data = g_bytes_unref_to_data (payload, &size);
  buffer = gst_rtmp_message_new_wrapped (GST_RTMP_MESSAGE_TYPE_COMMAND_AMF0,
      3, stream_id, data, size);

  gst_rtmp_connection_queue_message (connection, buffer);
  return transaction_id;
}

void
gst_rtmp_connection_expect_command (GstRtmpConnection * connection,
    GstRtmpCommandCallback response_command, gpointer user_data,
    guint32 stream_id, const gchar * command_name)
{
  ExpectedCommand *ec;

  g_return_if_fail (response_command);
  g_return_if_fail (command_name);
  g_return_if_fail (!is_command_response (command_name));

  GST_LOG_OBJECT (connection,
      "Registering %s for stream id %" G_GUINT32_FORMAT " name \"%s\"",
      GST_DEBUG_FUNCPTR_NAME (response_command), stream_id, command_name);

  ec = expected_command_new (stream_id, command_name, response_command,
      user_data);

  connection->expected_commands =
      g_list_append (connection->expected_commands, ec);
}

static void
gst_rtmp_connection_send_ack (GstRtmpConnection * connection)
{
  guint64 in_bytes_total = connection->in_bytes_total;
  GstRtmpProtocolControl pc = {
    .type = GST_RTMP_MESSAGE_TYPE_ACKNOWLEDGEMENT,
    .param = (guint32) in_bytes_total,
  };

  gst_rtmp_connection_queue_message (connection,
      gst_rtmp_message_new_protocol_control (&pc));

  connection->in_bytes_acked = in_bytes_total;
}

static void
gst_rtmp_connection_send_ping_response (GstRtmpConnection * connection,
    guint32 event_data)
{
  GstRtmpUserControl uc = {
    .type = GST_RTMP_USER_CONTROL_TYPE_PING_RESPONSE,
    .param = event_data,
  };

  gst_rtmp_connection_queue_message (connection,
      gst_rtmp_message_new_user_control (&uc));
}

void
gst_rtmp_connection_request_window_size (GstRtmpConnection * connection,
    guint32 window_ack_size)
{
  GstRtmpProtocolControl pc = {
    .type = GST_RTMP_MESSAGE_TYPE_WINDOW_ACK_SIZE,
    .param = window_ack_size,
  };

  g_return_if_fail (GST_IS_RTMP_CONNECTION (connection));

  if (connection->out_window_ack_size == window_ack_size)
    return;

  connection->out_window_ack_size = window_ack_size;

  gst_rtmp_connection_queue_message (connection,
      gst_rtmp_message_new_protocol_control (&pc));
}

void
gst_rtmp_connection_set_data_frame (GstRtmpConnection * connection,
    GstBuffer * buffer)
{
  g_return_if_fail (GST_IS_RTMP_CONNECTION (connection));
  g_return_if_fail (GST_IS_BUFFER (buffer));

  gst_buffer_prepend_memory (buffer, gst_memory_ref (set_data_frame_value));
  gst_rtmp_connection_queue_message (connection, buffer);
}
