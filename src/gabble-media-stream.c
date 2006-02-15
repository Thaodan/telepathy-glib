/*
 * gabble-media-stream.c - Source for GabbleMediaStream
 * Copyright (C) 2005 Collabora Ltd.
 * Copyright (C) 2005 Nokia Corporation
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <dbus/dbus-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gabble-media-stream.h"
#include "gabble-media-stream-signals-marshal.h"

#include "gabble-media-stream-glue.h"

#include "gabble-media-session.h"

#include "telepathy-helpers.h"
#include "telepathy-constants.h"

#include "handles.h"

G_DEFINE_TYPE(GabbleMediaStream, gabble_media_stream, G_TYPE_OBJECT)

/* signal enum */
enum
{
    ADD_REMOTE_CANDIDATE,
    REMOVE_REMOTE_CANDIDATE,
    SET_ACTIVE_CANDIDATE_PAIR,
    SET_REMOTE_CANDIDATE_LIST,
    SET_REMOTE_CODECS,

    NEW_ACTIVE_CANDIDATE_PAIR,
    NEW_NATIVE_CANDIDATE,
    READY,
    SUPPORTED_CODECS,

    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_MEDIA_SESSION = 1,
  PROP_OBJECT_PATH,
  LAST_PROPERTY
};

/* private structure */
typedef struct _GabbleMediaStreamPrivate GabbleMediaStreamPrivate;

struct _GabbleMediaStreamPrivate
{
  GabbleMediaSession *session;
  gchar *object_path;

  GValue native_codecs;     /* intersected codec list */
  GValue native_candidates;

  GValue remote_codecs;
  GValue remote_candidates;

  gboolean dispose_has_run;
};

#define GABBLE_MEDIA_STREAM_GET_PRIVATE(o)     (G_TYPE_INSTANCE_GET_PRIVATE ((o), GABBLE_TYPE_MEDIA_STREAM, GabbleMediaStreamPrivate))

static void
gabble_media_stream_init (GabbleMediaStream *obj)
{
  //GabbleMediaStreamPrivate *priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (obj);

  /* allocate any data required by the object here */
}

static void session_state_changed_cb (GabbleMediaSession *session,
                                      GParamSpec *arg1,
                                      GabbleMediaStream *stream);

static GObject *
gabble_media_stream_constructor (GType type, guint n_props,
                                 GObjectConstructParam *props)
{
  GObject *obj;
  GabbleMediaStreamPrivate *priv;
  DBusGConnection *bus;

  /* call base class constructor */
  obj = G_OBJECT_CLASS (gabble_media_stream_parent_class)->
           constructor (type, n_props, props);
  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (GABBLE_MEDIA_STREAM (obj));

  g_signal_connect (priv->session, "notify::state",
      (GCallback) session_state_changed_cb, obj);

  g_value_init (&priv->native_codecs, TP_TYPE_CODEC_LIST);
  g_value_take_boxed (&priv->native_codecs,
      dbus_g_type_specialized_construct (TP_TYPE_CODEC_LIST));

  g_value_init (&priv->native_candidates, TP_TYPE_CANDIDATE_LIST);
  g_value_take_boxed (&priv->native_candidates,
      dbus_g_type_specialized_construct (TP_TYPE_CANDIDATE_LIST));

  g_value_init (&priv->remote_codecs, TP_TYPE_CODEC_LIST);
  g_value_take_boxed (&priv->remote_codecs,
      dbus_g_type_specialized_construct (TP_TYPE_CODEC_LIST));

  g_value_init (&priv->remote_candidates, TP_TYPE_CANDIDATE_LIST);
  g_value_take_boxed (&priv->remote_candidates,
      dbus_g_type_specialized_construct (TP_TYPE_CANDIDATE_LIST));

  /* go for the bus */
  bus = tp_get_bus ();
  dbus_g_connection_register_g_object (bus, priv->object_path, obj);

  return obj;
}

static void
gabble_media_stream_get_property (GObject    *object,
                                  guint       property_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  GabbleMediaStream *stream = GABBLE_MEDIA_STREAM (object);
  GabbleMediaStreamPrivate *priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (stream);

  switch (property_id) {
    case PROP_MEDIA_SESSION:
      g_value_set_object (value, priv->session);
      break;
    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gabble_media_stream_set_property (GObject      *object,
                                  guint         property_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  GabbleMediaStream *stream = GABBLE_MEDIA_STREAM (object);
  GabbleMediaStreamPrivate *priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (stream);

  switch (property_id) {
    case PROP_MEDIA_SESSION:
      priv->session = g_value_get_object (value);
      break;
    case PROP_OBJECT_PATH:
      if (priv->object_path)
        g_free (priv->object_path);

      priv->object_path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void gabble_media_stream_dispose (GObject *object);
static void gabble_media_stream_finalize (GObject *object);

static void
gabble_media_stream_class_init (GabbleMediaStreamClass *gabble_media_stream_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_media_stream_class);
  GParamSpec *param_spec;

  g_type_class_add_private (gabble_media_stream_class, sizeof (GabbleMediaStreamPrivate));

  object_class->constructor = gabble_media_stream_constructor;

  object_class->get_property = gabble_media_stream_get_property;
  object_class->set_property = gabble_media_stream_set_property;

  object_class->dispose = gabble_media_stream_dispose;
  object_class->finalize = gabble_media_stream_finalize;

  param_spec = g_param_spec_object ("media-session", "GabbleMediaSession object",
                                    "Gabble media session object that owns this "
                                    "media stream object.",
                                    GABBLE_TYPE_MEDIA_SESSION,
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_READWRITE |
                                    G_PARAM_STATIC_NICK |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_MEDIA_SESSION, param_spec);

  param_spec = g_param_spec_string ("object-path", "D-Bus object path",
                                    "The D-Bus object path used for this "
                                    "object on the bus.",
                                    NULL,
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_READWRITE |
                                    G_PARAM_STATIC_NAME |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, param_spec);

  /* signals exported by DBus interface */
  signals[ADD_REMOTE_CANDIDATE] =
    g_signal_new ("add-remote-candidate",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_media_stream_marshal_VOID__STRING_BOXED,
                  G_TYPE_NONE, 2, G_TYPE_STRING, TP_TYPE_TRANSPORT_LIST);

  signals[REMOVE_REMOTE_CANDIDATE] =
    g_signal_new ("remove-remote-candidate",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_media_stream_marshal_VOID__STRING,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SET_ACTIVE_CANDIDATE_PAIR] =
    g_signal_new ("set-active-candidate-pair",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_media_stream_marshal_VOID__STRING_STRING,
                  G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[SET_REMOTE_CANDIDATE_LIST] =
    g_signal_new ("set-remote-candidate-list",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_media_stream_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1, TP_TYPE_CANDIDATE_LIST);

  signals[SET_REMOTE_CODECS] =
    g_signal_new ("set-remote-codecs",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_media_stream_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1, TP_TYPE_CODEC_LIST);

  /* signals not exported by DBus interface */
  signals[NEW_ACTIVE_CANDIDATE_PAIR] =
    g_signal_new ("new-active-candidate-pair",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_media_stream_marshal_VOID__STRING_STRING,
                  G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[NEW_NATIVE_CANDIDATE] =
    g_signal_new ("new-native-candidate",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_media_stream_marshal_VOID__STRING_BOXED,
                  G_TYPE_NONE, 2, G_TYPE_STRING, TP_TYPE_TRANSPORT_LIST);

  signals[READY] =
    g_signal_new ("ready",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_media_stream_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1, TP_TYPE_CODEC_LIST);

  signals[SUPPORTED_CODECS] =
    g_signal_new ("supported-codecs",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_media_stream_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1, TP_TYPE_CODEC_LIST);

  dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (gabble_media_stream_class), &dbus_glib_gabble_media_stream_object_info);
}

void
gabble_media_stream_dispose (GObject *object)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (object);
  GabbleMediaStreamPrivate *priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  /* release any references held by the object here */

  if (G_OBJECT_CLASS (gabble_media_stream_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_media_stream_parent_class)->dispose (object);
}

void
gabble_media_stream_finalize (GObject *object)
{
  //GabbleMediaStream *self = GABBLE_MEDIA_STREAM (object);
  //GabbleMediaStreamPrivate *priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (self);

  /* free any data held directly by the object here */

  G_OBJECT_CLASS (gabble_media_stream_parent_class)->finalize (object);
}

static void push_native_candidates (GabbleMediaStream *stream);
static void push_remote_codecs (GabbleMediaStream *stream);
static void push_remote_candidates (GabbleMediaStream *stream);

static void
session_state_changed_cb (GabbleMediaSession *session,
                          GParamSpec *arg1,
                          GabbleMediaStream *stream)
{
  JingleSessionState state;

  g_debug ("%s called", G_STRFUNC);

  g_object_get (session, "state", &state, NULL);

  if (state == JS_STATE_PENDING_INITIATED)
    {
      push_native_candidates (stream);
      push_remote_codecs (stream);
      push_remote_candidates (stream);
    }
}


/**
 * gabble_media_stream_codec_choice
 *
 * Implements DBus method CodecChoice
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean gabble_media_stream_codec_choice (GabbleMediaStream *obj, guint codec_id, GError **error)
{
  g_debug ("%s called", G_STRFUNC);

  return TRUE;
}


/**
 * gabble_media_stream_error
 *
 * Implements DBus method Error
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean gabble_media_stream_error (GabbleMediaStream *obj, guint errno, const gchar * message, GError **error)
{
  g_debug ("%s called", G_STRFUNC);

  return TRUE;
}


/**
 * gabble_media_stream_native_candidates_prepared
 *
 * Implements DBus method NativeCandidatesPrepared
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean gabble_media_stream_native_candidates_prepared (GabbleMediaStream *obj, GError **error)
{
  g_debug ("%s called", G_STRFUNC);

  return TRUE;
}


/**
 * gabble_media_stream_new_active_candidate_pair
 *
 * Implements DBus method NewActiveCandidatePair
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean gabble_media_stream_new_active_candidate_pair (GabbleMediaStream *obj, const gchar * native_candidate_id, const gchar * remote_candidate_id, GError **error)
{
  GabbleMediaStreamPrivate *priv;

  g_debug ("%s called", G_STRFUNC);

  g_assert (GABBLE_IS_MEDIA_STREAM (obj));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (obj);

  g_signal_emit (obj, signals[NEW_ACTIVE_CANDIDATE_PAIR], 0,
                 native_candidate_id, remote_candidate_id);

  return TRUE;
}


/**
 * gabble_media_stream_new_native_candidate
 *
 * Implements DBus method NewNativeCandidate
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean gabble_media_stream_new_native_candidate (GabbleMediaStream *obj, const gchar * candidate_id, const GPtrArray * transports, GError **error)
{
  GabbleMediaStreamPrivate *priv;
  JingleSessionState state;
  GPtrArray *candidates;
  GValue candidate = { 0, };
  GValueArray *transport;
  const gchar *addr;

  g_debug ("%s called", G_STRFUNC);

  g_assert (GABBLE_IS_MEDIA_STREAM (obj));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (obj);

  g_object_get (priv->session, "state", &state, NULL);

  g_assert (state < JS_STATE_ACTIVE);

  candidates = g_value_get_boxed (&priv->native_candidates);

  g_value_init (&candidate, TP_TYPE_CANDIDATE_STRUCT);
  g_value_set_static_boxed (&candidate,
      dbus_g_type_specialized_construct (TP_TYPE_CANDIDATE_STRUCT));

  dbus_g_type_struct_set (&candidate,
      0, candidate_id,
      1, transports,
      G_MAXUINT);

  transport = g_ptr_array_index (transports, 0);
  addr = g_value_get_string (g_value_array_get_nth (transport, 1));
  if (!strcmp (addr, "127.0.0.1"))
    {
      g_debug ("%s: ignoring localhost candidate", G_STRFUNC);
      return TRUE;
    }

  g_ptr_array_add (candidates, g_value_get_boxed (&candidate));

  push_native_candidates (obj);

  g_signal_emit (obj, signals[NEW_NATIVE_CANDIDATE], 0,
                 candidate_id, transports);

  return TRUE;
}


/**
 * gabble_media_stream_ready
 *
 * Implements DBus method Ready
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean gabble_media_stream_ready (GabbleMediaStream *obj, const GPtrArray * codecs, GError **error)
{
  GabbleMediaStreamPrivate *priv;
  GValue val = { 0, };

  g_debug ("%s called", G_STRFUNC);

  g_assert (GABBLE_IS_MEDIA_STREAM (obj));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (obj);

  g_value_init (&val, TP_TYPE_CODEC_LIST);
  g_value_set_static_boxed (&val, codecs);
  g_value_copy (&val, &priv->native_codecs);

  g_signal_emit (obj, signals[READY], 0, codecs);

  return TRUE;
}


/**
 * gabble_media_stream_supported_codecs
 *
 * Implements DBus method SupportedCodecs
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean gabble_media_stream_supported_codecs (GabbleMediaStream *obj, const GPtrArray * codecs, GError **error)
{
  GabbleMediaStreamPrivate *priv;

  g_debug ("%s called", G_STRFUNC);

  g_assert (GABBLE_IS_MEDIA_STREAM (obj));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (obj);

  /* store the intersection for later on */
  g_value_set_boxed (&priv->native_codecs, codecs);

  g_signal_emit (obj, signals[SUPPORTED_CODECS], 0, codecs);

  return TRUE;
}

static void
push_native_candidates (GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv;
  JingleSessionState state;
  GPtrArray *candidates;
  guint i;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (stream);

  g_object_get (priv->session, "state", &state, NULL);
  if (state < JS_STATE_PENDING_INITIATED)
    return;

  g_assert (state == JS_STATE_PENDING_INITIATED);

  candidates = g_value_get_boxed (&priv->native_candidates);

  for (i = 0; i < candidates->len; i++)
    {
      GValueArray *candidate;
      const gchar *candidate_id;
      const GPtrArray *transports;
      GValue transport = { 0, };
      LmMessage *msg;
      LmMessageNode *session_node, *cand_node;
      gchar *addr;
      guint port;
      gchar *port_str;
      TpMediaStreamProto proto;
      gdouble pref;
      gchar *pref_str;
      TpMediaStreamTransportType type;
      const gchar *type_str;
      gchar *user, *pass;

      candidate = g_ptr_array_index (candidates, i);

      candidate_id = g_value_get_string (g_value_array_get_nth (candidate, 0));
      transports = g_value_get_boxed (g_value_array_get_nth (candidate, 1));

      /* jingle audio only supports the concept of one transport per candidate */
      g_assert (transports->len == 1);

      /* grab the interesting fields from the struct */
      g_value_init (&transport, TP_TYPE_TRANSPORT_STRUCT);
      g_value_set_static_boxed (&transport, g_ptr_array_index (transports, 0));

      dbus_g_type_struct_get (&transport,
          1, &addr,
          2, &port,
          3, &proto,
          6, &pref,
          7, &type,
          8, &user,
          9, &pass,
          G_MAXUINT);

      /* convert to strings */
      port_str = g_strdup_printf ("%d", port);

      pref_str = g_strdup_printf ("%f", pref);

      switch (type) {
        case TP_MEDIA_STREAM_TRANSPORT_TYPE_LOCAL:
          type_str = "local";
          break;
        case TP_MEDIA_STREAM_TRANSPORT_TYPE_DERIVED:
          type_str = "stun";
          break;
        case TP_MEDIA_STREAM_TRANSPORT_TYPE_RELAY:
          type_str = "relay";
          break;
        default:
          g_critical ("%s: TpMediaStreamTransportType has an invalid value", G_STRFUNC);
      }

      /* construct a session message */
      msg = gabble_media_session_message_new (priv->session, "candidates", &session_node);

      /* create a sub-node called "candidate" and fill it with candidate info */
      cand_node = lm_message_node_add_child (session_node, "candidate", NULL);

      lm_message_node_set_attributes (cand_node,
          "name", "rtp",
          "address", addr,
          "port", port_str,
          "username", user,
          "password", pass,
          "preference", pref_str,
          "protocol", (proto == TP_MEDIA_STREAM_PROTO_UDP) ? "udp" : "tcp",
          "type", type_str,
          "network", "0",
          "generation", "0",
          NULL);

      /* send it */
      gabble_media_session_message_send (priv->session, msg);

      /* clean up */
      lm_message_unref (msg);

      g_free (addr);
      g_free (user);
      g_free (pass);

      g_free (port_str);
      g_free (pref_str);
    }

  g_value_take_boxed (&priv->native_candidates,
      dbus_g_type_specialized_construct (TP_TYPE_CANDIDATE_LIST));
}

gboolean
gabble_media_stream_post_remote_codecs (GabbleMediaStream *stream,
                                        LmMessageNode *desc_node)
{
  GabbleMediaStreamPrivate *priv;
  LmMessageNode *node;
  const gchar *str;
  GPtrArray *codecs;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (stream);

  codecs = g_value_get_boxed (&priv->remote_codecs);

  g_assert (codecs->len == 0);

  for (node = desc_node->children; node; node = node->next)
    {
      guchar id;
      const gchar *name;
      GValue codec = { 0, };

      /* id of codec */
      str = lm_message_node_get_attribute (node, "id");
      if (!str)
        return FALSE;

      id = atoi(str);

      /* codec name */
      name = lm_message_node_get_attribute (node, "name");
      if (!name)
        return FALSE;

      g_value_init (&codec, TP_TYPE_CODEC_STRUCT);
      g_value_set_static_boxed (&codec,
          dbus_g_type_specialized_construct (TP_TYPE_CODEC_STRUCT));

      dbus_g_type_struct_set (&codec,
          0, id,
          1, name,
          2, TP_CODEC_MEDIA_TYPE_AUDIO,
          3, 0,                          /* clock rate */
          4, 1,                          /* number of supported channels */
          5, g_hash_table_new (g_str_hash, g_str_equal),
          G_MAXUINT);

      g_ptr_array_add (codecs, g_value_get_boxed (&codec));
    }

  g_debug ("%s: parsed %d remote codecs", G_STRFUNC, codecs->len);

  push_remote_codecs (stream);

  return TRUE;
}

static void
push_remote_codecs (GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv;
  JingleSessionState state;
  GPtrArray *codecs;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (stream);

  g_object_get (priv->session, "state", &state, NULL);
  if (state < JS_STATE_PENDING_INITIATED)
    return;

  g_assert (state == JS_STATE_PENDING_INITIATED);

  codecs = g_value_get_boxed (&priv->remote_codecs);
  if (codecs->len == 0)
    return;

  g_debug ("%s: emitting MediaStreamHandler::SetRemoteCodecs signal",
      G_STRFUNC);

  g_signal_emit (stream, signals[SET_REMOTE_CODECS], 0,
                 codecs);

  g_value_take_boxed (&priv->remote_codecs,
      dbus_g_type_specialized_construct (TP_TYPE_CODEC_LIST));
}

static void push_remote_candidates (GabbleMediaStream *stream);

gboolean
gabble_media_stream_post_remote_candidates (GabbleMediaStream *stream,
                                            LmMessageNode *session_node)
{
  GabbleMediaStreamPrivate *priv;
  LmMessageNode *node;
  const gchar *str;
  GPtrArray *candidates;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (stream);

  candidates = g_value_get_boxed (&priv->remote_candidates);

  for (node = session_node->children; node; node = node->next)
    {
      const gchar /**name, */*addr;
      guint16 port;
      TpMediaStreamProto proto;
      gdouble pref;
      TpMediaStreamTransportType type;
      const gchar *user, *pass;
      guchar net, gen;

      GValue candidate = { 0, };
      GPtrArray *transports;
      GValue transport = { 0, };


      /*
       * Candidate
       */

      /* id/name: assuming "username" here for now */


      /*
       * Transport
       */

      /* ip address */
      addr = lm_message_node_get_attribute (node, "address");
      if (!addr)
        return FALSE;

      /* port */
      str = lm_message_node_get_attribute (node, "port");
      if (!str)
        return FALSE;
      port = atoi (str);

      /* protocol */
      str = lm_message_node_get_attribute (node, "protocol");
      if (!str)
        return FALSE;

      if (!strcmp (str, "udp"))
        proto = TP_MEDIA_STREAM_PROTO_UDP;
      else if (!strcmp (str, "tcp"))
        proto = TP_MEDIA_STREAM_PROTO_TCP;
      else if (!strcmp (str, "ssltcp"))
        {
          g_warning ("%s: ssltcp candidates not yet supported", G_STRFUNC);
          return FALSE;
        }
      else
        return FALSE;

      /* protocol subtype: only "rtp" is supported here for now */
      str = lm_message_node_get_attribute (node, "name");
      if (!str)
        return FALSE;
      if (strcmp (str, "rtp"))
        return FALSE;

      /* protocol profile: hardcoded to "AVP" for now */

      /* preference */
      str = lm_message_node_get_attribute (node, "preference");
      if (!str)
        return FALSE;
      pref = g_ascii_strtod (str, NULL);

      /* type */
      str = lm_message_node_get_attribute (node, "type");
      if (!str)
        return FALSE;

      if (!strcmp (str, "local"))
        type = TP_MEDIA_STREAM_TRANSPORT_TYPE_LOCAL;
      else if (!strcmp (str, "stun"))
        type = TP_MEDIA_STREAM_TRANSPORT_TYPE_DERIVED;
      else if (!strcmp (str, "relay"))
        type = TP_MEDIA_STREAM_TRANSPORT_TYPE_RELAY;
      else
        return FALSE;

      /* username */
      user = lm_message_node_get_attribute (node, "username");
      if (!user)
        return FALSE;

      /* password */
      pass = lm_message_node_get_attribute (node, "password");
      if (!pass)
        return FALSE;

      /* unknown */
      str = lm_message_node_get_attribute (node, "network");
      if (!str)
        return FALSE;
      net = atoi (str);

      /* unknown */
      str = lm_message_node_get_attribute (node, "generation");
      if (!str)
        return FALSE;
      gen = atoi (str);


      g_value_init (&transport, TP_TYPE_TRANSPORT_STRUCT);
      g_value_set_static_boxed (&transport,
          dbus_g_type_specialized_construct (TP_TYPE_TRANSPORT_STRUCT));

      dbus_g_type_struct_set (&transport,
          0, 0,         /* component number */
          1, addr,
          2, port,
          3, proto,
          4, "RTP",
          5, "AVP",
          6, pref,
          7, type,
          8, user,
          9, pass,
          G_MAXUINT);

      transports = g_ptr_array_sized_new (1);
      g_ptr_array_add (transports, g_value_get_boxed (&transport));


      g_value_init (&candidate, TP_TYPE_CANDIDATE_STRUCT);
      g_value_set_static_boxed (&candidate,
          dbus_g_type_specialized_construct (TP_TYPE_CANDIDATE_STRUCT));

      dbus_g_type_struct_set (&candidate,
          0, user,
          1, transports,
          G_MAXUINT);

      g_ptr_array_add (candidates, g_value_get_boxed (&candidate));

      g_debug ("%s: added new candidate %s, "
               "%d candidate(s) in total now",
               G_STRFUNC,
               user,
               candidates->len);
    }

  push_remote_candidates (stream);

  return TRUE;
}

static void
push_remote_candidates (GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv;
  JingleSessionState state;
  GPtrArray *candidates;
  guint i;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (stream);

  candidates = g_value_get_boxed (&priv->remote_candidates);

  if (candidates->len == 0)
    return;

  g_object_get (priv->session, "state", &state, NULL);
  if (state < JS_STATE_PENDING_INITIATED)
    return;

  g_assert (state == JS_STATE_PENDING_INITIATED);

  for (i = 0; i < candidates->len; i++)
    {
      GValueArray *candidate = g_ptr_array_index (candidates, i);
      const gchar *candidate_id;
      const GPtrArray *transports;

      candidate_id = g_value_get_string (g_value_array_get_nth (candidate, 0));
      transports = g_value_get_boxed (g_value_array_get_nth (candidate, 1));

      g_debug ("%s: emitting Media.StreamHandler::AddRemoteCandidate signal",
          G_STRFUNC);

      g_signal_emit (stream, signals[ADD_REMOTE_CANDIDATE], 0,
                     candidate_id, transports);
    }

  g_value_take_boxed (&priv->remote_candidates,
      dbus_g_type_specialized_construct (TP_TYPE_CANDIDATE_LIST));
}

void
gabble_media_stream_session_node_add_description (GabbleMediaStream *stream,
                                                  LmMessageNode *session_node)
{
  GabbleMediaStreamPrivate *priv;
  const GPtrArray *codecs;
  LmMessageNode *desc_node;
  guint i;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = GABBLE_MEDIA_STREAM_GET_PRIVATE (stream);

  codecs = g_value_get_boxed (&priv->native_codecs);

  desc_node = lm_message_node_add_child (session_node, "description", NULL);
  lm_message_node_set_attribute (desc_node, "xmlns",
      "http://www.google.com/session/phone");

  for (i = 0; i < codecs->len; i++)
    {
      GValue codec = { 0, };
      guint id;
      gchar *id_str, *name;
      LmMessageNode *pt_node;

      g_value_init (&codec, TP_TYPE_CODEC_STRUCT);
      g_value_set_static_boxed (&codec, g_ptr_array_index (codecs, i));

      dbus_g_type_struct_get (&codec,
          0, &id,
          1, &name,
          G_MAXUINT);

      id_str = g_strdup_printf ("%d", id);

      /* FIXME: parse the rest of the struct */

      /* create a sub-node called "payload-type" and fill it */
      pt_node = lm_message_node_add_child (desc_node, "payload-type", NULL);

      lm_message_node_set_attributes (pt_node,
          "xmlns", "http://www.google.com/session/phone",
          "id", id_str,
          "name", name,
          NULL);

      /* clean up */
      g_free (id_str);
      g_free (name);
    }
}

