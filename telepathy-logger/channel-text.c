/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2009 Collabora Ltd.
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
 *
 * Authors: Cosimo Alfarano <cosimo.alfarano@collabora.co.uk>
 */

#include "config.h"
#include "channel-text-internal.h"

#include <glib.h>
#include <telepathy-glib/contact.h>
#include <telepathy-glib/enums.h>
#include <telepathy-glib/proxy.h>

#include <telepathy-logger/channel-internal.h>
#include <telepathy-logger/observer-internal.h>
#include <telepathy-logger/entry-internal.h>
#include <telepathy-logger/entry-text.h>
#include <telepathy-logger/entry-text-internal.h>
#include <telepathy-logger/log-manager-internal.h>
#include <telepathy-logger/log-store-sqlite-internal.h>

#define DEBUG_FLAG TPL_DEBUG_CHANNEL
#include <telepathy-logger/action-chain-internal.h>
#include <telepathy-logger/entity-internal.h>
#include <telepathy-logger/datetime-internal.h>
#include <telepathy-logger/debug-internal.h>
#include <telepathy-logger/util-internal.h>

struct _TplChannelTextPriv
{
  gboolean chatroom;
  TpContact *my_contact;
  gchar *chatroom_id;          /* only set if chatroom==TRUE */

  /* Contacts participating in this channel.
   * This is used as a cache so we don't have to recreate TpContact objects
   * each time we receive something.
   *
   * TpHandle => reffed TpContact
   * */
  GHashTable *contacts;
};

static TpContactFeature features[3] = {
  TP_CONTACT_FEATURE_ALIAS,
  TP_CONTACT_FEATURE_PRESENCE,
  TP_CONTACT_FEATURE_AVATAR_TOKEN
};

static void call_when_ready_wrapper (TplChannel *tpl_chan,
    GAsyncReadyCallback cb, gpointer user_data);

static void got_tpl_chan_ready_cb (GObject *obj, GAsyncResult *result,
    gpointer user_data);
static void on_closed_cb (TpChannel *proxy, gpointer user_data,
    GObject *weak_object);
static void on_lost_message_cb (TpChannel *proxy, gpointer user_data,
    GObject *weak_object);
static void on_received_signal_cb (TpChannel *proxy, guint arg_ID,
    guint arg_Timestamp, guint arg_Sender, guint arg_Type, guint arg_Flags,
    const gchar *arg_Text, gpointer user_data, GObject *weak_object);
static void on_sent_signal_cb (TpChannel *proxy, guint arg_Timestamp,
    guint arg_Type, const gchar *arg_Text,  gpointer user_data,
    GObject *weak_object);
static void on_send_error_cb (TpChannel *proxy, guint arg_Error,
    guint arg_Timestamp, guint arg_Type, const gchar *arg_Text,
    gpointer user_data, GObject *weak_object);
static void on_pending_messages_removed_cb (TpChannel *proxy,
    const GArray *arg_Message_IDs, gpointer user_data, GObject *weak_object);

static void pendingproc_connect_message_signals (TplActionChain *ctx,
    gpointer user_data);
static void pendingproc_get_pending_messages (TplActionChain *ctx,
   gpointer user_data);
static void pendingproc_prepare_tpl_channel (TplActionChain *ctx,
   gpointer user_data);
static void pendingproc_cleanup_pending_messages_db (TplActionChain *ctx,
    gpointer user_data);

static void keepon_on_receiving_signal (TplEntryText *log,
    TpContact *remote);
static void got_message_pending_messages_cb (TpProxy *proxy,
    const GValue *out_Value, const GError *error, gpointer user_data,
    GObject *weak_object);
static void got_text_pending_messages_cb (TpChannel *proxy,
    const GPtrArray *result, const GError *error, gpointer user_data,
    GObject *weak_object);

G_DEFINE_TYPE (TplChannelText, _tpl_channel_text, TPL_TYPE_CHANNEL)

static void
_tpl_channel_text_set_my_contact (TplChannelText *self,
    TpContact *data)
{
  g_return_if_fail (TPL_IS_CHANNEL_TEXT (self));
  g_return_if_fail (TP_IS_CONTACT (data));
  g_return_if_fail (self->priv->my_contact == NULL);

  self->priv->my_contact = g_object_ref (data);
}

static void
get_self_contact_cb (TpConnection *connection,
    guint n_contacts,
    TpContact *const *contacts,
    guint n_failed,
    const TpHandle *failed,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  TplActionChain *ctx = user_data;
  TplChannelText *tpl_text = _tpl_action_chain_get_object (ctx);
  TplChannel *tpl_chan = TPL_CHANNEL (tpl_text);
  TpChannel *tp_chan = TP_CHANNEL (tpl_chan);

  g_return_if_fail (TPL_IS_CHANNEL_TEXT (tpl_text));

  g_assert_cmpuint (n_failed, ==, 0);
  g_assert_cmpuint (n_contacts, ==, 1);

  if (n_failed > 0)
    {
      TpConnection *tp_conn = tp_channel_borrow_connection (tp_chan);
      const gchar *conn_path;

      conn_path = tp_proxy_get_object_path (TP_PROXY (tp_conn));

      PATH_DEBUG (tpl_text, "Error resolving self handle for connection %s."
         " Aborting channel observation", conn_path);

      _tpl_action_chain_terminate (ctx);
      return;
    }

  _tpl_channel_text_set_my_contact (tpl_text, contacts[0]);

  _tpl_action_chain_continue (ctx);
}

static void
pendingproc_get_my_contact (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *tpl_text = _tpl_action_chain_get_object (ctx);
  TpConnection *tp_conn = tp_channel_borrow_connection (
      TP_CHANNEL (tpl_text));
  TpHandle my_handle = tp_connection_get_self_handle (tp_conn);

  tp_connection_get_contacts_by_handle (tp_conn, 1, &my_handle,
      G_N_ELEMENTS (features), features, get_self_contact_cb, ctx, NULL, NULL);
}

static void
get_remote_contacts_cb (TpConnection *connection,
    guint n_contacts,
    TpContact *const *contacts,
    guint n_failed,
    const TpHandle *failed,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  TplActionChain *ctx = user_data;
  TplChannelText *self = TPL_CHANNEL_TEXT (weak_object);
  guint i;

  if (error != NULL)
    {
      DEBUG ("Failed to get remote contacts: %s", error->message);

      if (ctx != NULL)
        _tpl_action_chain_terminate (ctx);
      return;
    }

  for (i = 0; i < n_contacts; i++)
    {
      TpContact *contact = contacts[i];
      TpHandle handle = tp_contact_get_handle (contact);

      g_hash_table_insert (self->priv->contacts, GUINT_TO_POINTER (handle),
          g_object_ref (contact));
    }

  if (ctx != NULL)
    _tpl_action_chain_continue (ctx);
}

static void
chan_members_changed_cb (TpChannel *chan,
    gchar *message,
    GArray *added,
    GArray *removed,
    GArray *local_pending,
    GArray *remote_pending,
    TpHandle actor,
    guint reason,
    gpointer user_data)
{
  TplChannelText *self = user_data;
  guint i;

  if (added->len > 0)
    {
      tp_connection_get_contacts_by_handle (tp_channel_borrow_connection (chan),
          added->len, (TpHandle *) added->data,
          G_N_ELEMENTS (features), features, get_remote_contacts_cb, NULL, NULL,
          G_OBJECT (self));
    }

  for (i = 0; i < removed->len; i++)
    {
      TpHandle handle = g_array_index (removed, TpHandle, i);

      g_hash_table_remove (self->priv->contacts, GUINT_TO_POINTER (handle));
    }
}

static void
pendingproc_get_remote_contacts (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *self = _tpl_action_chain_get_object (ctx);
  TpChannel *chan = TP_CHANNEL (self);
  TpConnection *tp_conn = tp_channel_borrow_connection (chan);
  GArray *arr;

  if (tp_proxy_has_interface_by_id (chan,
        TP_IFACE_QUARK_CHANNEL_INTERFACE_GROUP))
    {
      /* Get the contacts of all the members */
      const TpIntSet *members;

      members = tp_channel_group_get_members (chan);
      arr = tp_intset_to_array (members);

      tp_g_signal_connect_object (chan, "group-members-changed",
          G_CALLBACK (chan_members_changed_cb), self, 0);
    }
  else
    {
      /* Get the contact of the TargetHandle */
      TpHandle handle;

      arr = g_array_sized_new (FALSE, FALSE, sizeof (TpHandle), 1);
      handle = tp_channel_get_handle (chan, NULL);

      g_array_append_val (arr, handle);
    }

  tp_connection_get_contacts_by_handle (tp_conn,
      arr->len, (TpHandle *) arr->data,
      G_N_ELEMENTS (features), features, get_remote_contacts_cb, ctx, NULL,
      G_OBJECT (self));

  g_array_free (arr, TRUE);
}

static void
_tpl_channel_text_set_chatroom_id (TplChannelText *self,
    const gchar *data)
{
  g_return_if_fail (TPL_IS_CHANNEL_TEXT (self));
  g_return_if_fail (!TPL_STR_EMPTY (data));
  g_return_if_fail (self->priv->chatroom_id == NULL);
  self->priv->chatroom_id = g_strdup (data);
}

static void
_tpl_channel_text_set_chatroom (TplChannelText *self,
    gboolean data)
{
  g_return_if_fail (TPL_IS_CHANNEL_TEXT (self));

  self->priv->chatroom = data;
}

static void
pendingproc_get_room_info (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *tpl_text = _tpl_action_chain_get_object (ctx);
  TpHandleType handle_type;
  TpChannel *chan = TP_CHANNEL (tpl_text);

  tp_channel_get_handle (chan, &handle_type);
  if (handle_type != TP_HANDLE_TYPE_ROOM)
    goto out;

  _tpl_channel_text_set_chatroom (tpl_text, TRUE);

  PATH_DEBUG (tpl_text, "Chatroom id: %s", tp_channel_get_identifier (chan));
  _tpl_channel_text_set_chatroom_id (tpl_text,
      tp_channel_get_identifier (chan));

out:
  _tpl_action_chain_continue (ctx);
}

static void
tpl_channel_text_dispose (GObject *obj)
{
  TplChannelTextPriv *priv = TPL_CHANNEL_TEXT (obj)->priv;

  if (priv->my_contact != NULL)
    {
      g_object_unref (priv->my_contact);
      priv->my_contact = NULL;
    }

  tp_clear_pointer (&priv->contacts, g_hash_table_unref);

  G_OBJECT_CLASS (_tpl_channel_text_parent_class)->dispose (obj);
}


static void
tpl_channel_text_finalize (GObject *obj)
{
  TplChannelTextPriv *priv = TPL_CHANNEL_TEXT (obj)->priv;

  PATH_DEBUG (obj, "finalizing channel %p", obj);

  g_free (priv->chatroom_id);
  priv->chatroom_id = NULL;

  G_OBJECT_CLASS (_tpl_channel_text_parent_class)->finalize (obj);
}


static void
_tpl_channel_text_class_init (TplChannelTextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  TplChannelClass *tpl_chan_class = TPL_CHANNEL_CLASS (klass);

  object_class->dispose = tpl_channel_text_dispose;
  object_class->finalize = tpl_channel_text_finalize;

  tpl_chan_class->call_when_ready = call_when_ready_wrapper;

  g_type_class_add_private (object_class, sizeof (TplChannelTextPriv));
}


static void
_tpl_channel_text_init (TplChannelText *self)
{
  TplChannelTextPriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TPL_TYPE_CHANNEL_TEXT, TplChannelTextPriv);

  self->priv = priv;

  self->priv->contacts = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_object_unref);
}


/**
 * _tpl_channel_text_new
 * @conn: TpConnection instance owning the channel
 * @object_path: the channel's DBus path
 * @tp_chan_props: channel's immutable properties, obtained for example by
 * %tp_channel_borrow_immutable_properties()
 * @account: TpAccount instance, related to the new #TplChannelText
 * @error: location of the GError, used in case a problem is raised while
 * creating the channel
 *
 * Convenience function to create a new TPL Channel Text proxy.
 * The returned #TplChannelText is not guaranteed to be ready at the point of
 * return.
 *
 * TplChannelText is actually a subclass of the abstract TplChannel which is a
 * subclass of TpChannel.
 * Use #TpChannel methods, casting the #TplChannelText instance to a
 * TpChannel, to access TpChannel data/methods from it.
 *
 * TplChannelText is usually created using #tpl_channel_factory_build, from
 * within a #TplObserver singleton, when its Observer_Channel method is called
 * by the Channel Dispatcher.
 *
 * Returns: the TplChannelText instance or %NULL in @object_path is not valid
 */
TplChannelText *
_tpl_channel_text_new (TpConnection *conn,
    const gchar *object_path,
    GHashTable *tp_chan_props,
    TpAccount *account,
    GError **error)
{
  TpProxy *conn_proxy = TP_PROXY (conn);

  /* Do what tpl_channel_new does + set TplChannelText specific properties */

  g_return_val_if_fail (TP_IS_CONNECTION (conn), NULL);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (!TPL_STR_EMPTY (object_path), NULL);
  g_return_val_if_fail (tp_chan_props != NULL, NULL);

  if (!tp_dbus_check_valid_object_path (object_path, error))
    return NULL;

  return g_object_new (TPL_TYPE_CHANNEL_TEXT,
      /* TplChannel properties */
      "account", account,
      /* TpChannel properties */
      "connection", conn,
      "dbus-daemon", conn_proxy->dbus_daemon,
      "bus-name", conn_proxy->bus_name,
      "object-path", object_path,
      "handle-type", (guint) TP_UNKNOWN_HANDLE_TYPE,
      "channel-properties", tp_chan_props,
      NULL);
}

static TpContact *
_tpl_channel_text_get_my_contact (TplChannelText *self)
{
  g_return_val_if_fail (TPL_IS_CHANNEL_TEXT (self), NULL);

  return self->priv->my_contact;
}

static gboolean
_tpl_channel_text_is_chatroom (TplChannelText *self)
{
  g_return_val_if_fail (TPL_IS_CHANNEL_TEXT (self), FALSE);

  return self->priv->chatroom;
}

static const gchar *
_tpl_channel_text_get_chatroom_id (TplChannelText *self)
{
  g_return_val_if_fail (TPL_IS_CHANNEL_TEXT (self), NULL);

  return self->priv->chatroom_id;
}

static void
_tpl_channel_text_call_when_ready (TplChannelText *self,
    GAsyncReadyCallback cb, gpointer user_data)
{
  TplActionChain *actions;

  /* first: connect signals, so none are lost
   * second: prepare all TplChannel
   * third: cache my contact and the remote one.
   * last: check for pending messages
   *
   * If for any reason, the order is changed, it's needed to check what objects
   * are unreferenced by g_object_unref but used by a next action AND what
   * object are actually not prepared but used anyway */
  actions = _tpl_action_chain_new_async (G_OBJECT (self), cb, user_data);
  _tpl_action_chain_append (actions, pendingproc_prepare_tpl_channel, NULL);
  _tpl_action_chain_append (actions, pendingproc_get_my_contact, NULL);
  _tpl_action_chain_append (actions, pendingproc_get_remote_contacts, NULL);
  _tpl_action_chain_append (actions, pendingproc_get_room_info, NULL);
  _tpl_action_chain_append (actions, pendingproc_connect_message_signals, NULL);
  _tpl_action_chain_append (actions, pendingproc_get_pending_messages, NULL);
  _tpl_action_chain_append (actions, pendingproc_cleanup_pending_messages_db,
      NULL);
  /* start the chain consuming */
  _tpl_action_chain_continue (actions);
}

static void
call_when_ready_wrapper (TplChannel *tpl_chan,
    GAsyncReadyCallback cb,
    gpointer user_data)
{
  _tpl_channel_text_call_when_ready (TPL_CHANNEL_TEXT (tpl_chan), cb,
      user_data);
}

static void
pendingproc_prepare_tpl_channel (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannel *tpl_chan = TPL_CHANNEL (_tpl_action_chain_get_object (ctx));

  TPL_CHANNEL_GET_CLASS (tpl_chan)->call_when_ready_protected (tpl_chan,
      got_tpl_chan_ready_cb, ctx);
}


static void
got_tpl_chan_ready_cb (GObject *obj,
    GAsyncResult *tpl_chan_result,
    gpointer user_data)
{
  TplActionChain *ctx = user_data;

  /* if TplChannel preparation is OK, keep on with the TplChannelText */
  if (_tpl_action_chain_new_finish (tpl_chan_result))
    _tpl_action_chain_continue (ctx);
  else
     _tpl_action_chain_terminate (ctx);
  return;
}


/* Clean up passed messages (GList of tokens), which are known to be stale,
 * setting them acknowledged in SQLite */
static void
tpl_channel_text_clean_up_stale_tokens (TplChannelText *self,
    GList *stale_tokens)
{
  TplLogStore *cache = _tpl_log_store_sqlite_dup ();
  GError *loc_error = NULL;

  for (; stale_tokens != NULL; stale_tokens = g_list_next (stale_tokens))
    {
      gchar *log_id = stale_tokens->data;

      _tpl_log_store_sqlite_set_acknowledgment (cache, log_id, &loc_error);

      if (loc_error != NULL)
        {
          PATH_CRITICAL (self, "Unable to set %s as acknoledged in "
              "TPL DB: %s", log_id, loc_error->message);
          g_clear_error (&loc_error);
        }
    }

  if (cache != NULL)
    g_object_unref (cache);
}

/* Cleans up stale log-ids in the index logstore.
 *
 * It 'brutally' considers as stale all log-ids which timestamp is older than
 * <time_limit> days AND are still not set as acknowledged.
 *
 * NOTE: While retrieving open channels, a partial clean-up for the channel's
 * stale pending messages is done. It's not enough, since it doesn't consider
 * all the channels being closed at retrieval time. This function tries to
 * catch stale ids in the rest of the DB, heuristically.
 *
 * It is wrong to consider all the log-ids not having an channel currently
 * open as stale, since a channel might be temporarely disconnected and
 * reconnected and some protocols might repropose not acknowledged messages on
 * reconnection. We need to consider only reasonably old log-ids.
 *
 * This function is meant only to reduce the size of the DB used for indexing.
 *
 * No _tpl_action_chain_terminate() is called if some fatal error occurs since
 * it's not considered a crucial point for TplChannel preparation.
 */
static void
pendingproc_cleanup_pending_messages_db (TplActionChain *ctx,
    gpointer user_data)
{
  /* five days ago in seconds */
  TplChannelText *self = _tpl_action_chain_get_object (ctx);
  const time_t time_limit = _tpl_time_get_current () -
    TPL_LOG_STORE_SQLITE_CLEANUP_DELTA_LIMIT;
  TplLogStore *cache = _tpl_log_store_sqlite_dup ();
  GList *l;
  GError *error = NULL;

  if (cache == NULL)
    {
      DEBUG ("Unable to obtain the TplLogStoreIndex singleton");
      goto out;
    }

  l = _tpl_log_store_sqlite_get_log_ids (cache, NULL, time_limit,
      &error);
  if (error != NULL)
    {
      DEBUG ("unable to obtain log-id in Index DB: %s", error->message);
      g_error_free (error);
      /* do not call _tpl_action_chain_terminate, if it's temporary next startup
       * TPL will re-do the clean-up. If it's fatal, the flow will stop later
       * anyway */
      goto out;
    }

  if (l != NULL)
    PATH_DEBUG (self, "Cleaning up stale messages");
  tpl_channel_text_clean_up_stale_tokens (self, l);
  while (l != NULL)
    {
      PATH_DEBUG (self, "%s is stale, removed from DB", (gchar *) l->data);
      g_free (l->data);
      l = g_list_delete_link (l, l);
    }

out:
  if (cache != NULL)
    g_object_unref (cache);

  _tpl_action_chain_continue (ctx);
}


static void
pendingproc_get_pending_messages (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *chan_text = _tpl_action_chain_get_object (ctx);

  if (tp_proxy_has_interface_by_id (chan_text,
        TP_IFACE_QUARK_CHANNEL_INTERFACE_MESSAGES))
    tp_cli_dbus_properties_call_get (chan_text, -1,
        TP_IFACE_CHANNEL_INTERFACE_MESSAGES, "PendingMessages",
        got_message_pending_messages_cb, ctx, NULL,
        G_OBJECT (chan_text));
  else
    tp_cli_channel_type_text_call_list_pending_messages (TP_CHANNEL (chan_text),
        -1, FALSE, got_text_pending_messages_cb, ctx, NULL, NULL);
}


/* PendingMessages CB for Message interface */
static void
got_message_pending_messages_cb (TpProxy *proxy,
    const GValue *out_Value,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  const gchar *channel_path = tp_proxy_get_object_path (proxy);
  TplLogStore *cache = _tpl_log_store_sqlite_dup ();
  TplActionChain *ctx = user_data;
  GPtrArray *result = NULL;
  GList *cached_pending_msgs = NULL;
  GError *loc_error = NULL;
  guint i;

  if (!TPL_IS_CHANNEL_TEXT (proxy))
    {
      CRITICAL ("Passed proxy not a is proper TplChannelText");
      goto out;
    }

  if (!TPL_IS_CHANNEL_TEXT (weak_object))
    {
      CRITICAL ("Passed weak_object is not a proper TplChannelText");
      goto out;
    }

  if (error != NULL)
    {
      PATH_CRITICAL (weak_object, "retrieving messages for Message iface: %s",
          error->message);
      goto out;
    }

  /* It's aaa{vs}, a list of message each containing a list of message's parts
   * each containing a dictioanry k:v */
  result = g_value_get_boxed (out_Value);

  /* getting messages ids known to be pending at last TPL exit */
  cached_pending_msgs = _tpl_log_store_sqlite_get_pending_messages (cache,
      TP_CHANNEL (proxy), &loc_error);
  if (loc_error != NULL)
    {
      CRITICAL ("Unable to obtain pending messages stored in TPL DB: %s",
          loc_error->message);
      goto out;
    }

  /* cycle the list of messages */
  if (result->len > 0)
    PATH_DEBUG (proxy, "Checking if there are any un-logged messages among "
        "%d pending messages", result->len);
  for (i = 0; i < result->len; ++i)
    {
      GPtrArray *message_parts;
      GHashTable *message_headers; /* string:gvalue */
      GHashTable *message_part; /* string:gvalue */
      GList *l;
      const gchar *message_token;
      gchar *tpl_message_token;
      guint64 message_timestamp;
      guint message_type = TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL;
      guint message_flags = 0;
      guint message_id;
      TpHandle message_sender_handle;
      const gchar *message_body;
      gboolean valid;

      /* list of message's parts */
      message_parts = g_ptr_array_index (result, i);
      /* message part 0 is the message's headers */
      message_headers = g_ptr_array_index (message_parts, 0);
      /* message part 1 is is the first part, the most 'faithful' among
       * alternatives.
       * TODO fully support alternatives and attachments/images
       * related to them */
      message_part = g_ptr_array_index (message_parts, 1);
      message_token = tp_asv_get_string (message_headers, "message-token");
      message_id = tp_asv_get_uint32 (message_headers, "pending-message-id",
          &valid);
      if (!valid)
        {
          DEBUG ("pending-message-id not in a valid range, setting to "
              "UNKNOWN");
            message_id = TPL_ENTRY_MSG_ID_UNKNOWN;
        }
      message_timestamp = tp_asv_get_uint64 (message_headers,
          "message-received", NULL);

      tpl_message_token = _tpl_create_message_token (channel_path,
          message_timestamp, message_id);

      message_sender_handle = tp_asv_get_uint32 (message_headers,
          "message-sender", NULL);

      message_type = tp_asv_get_uint32 (message_headers, "message-type",
          &valid);
      if (!valid)
        {
          PATH_DEBUG (proxy, "message-type not in a valid range, falling "
              "back to type=NORMAL");
          message_type = TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL;
        }

      if (tp_asv_get_boolean (message_headers, "rescued", &valid) && valid)
        message_flags |= TP_CHANNEL_TEXT_MESSAGE_FLAG_RESCUED;

      if (tp_asv_get_boolean (message_headers, "scrollback", NULL) && valid)
        message_flags |= TP_CHANNEL_TEXT_MESSAGE_FLAG_SCROLLBACK;

      message_body = tp_asv_get_string (message_part, "content");

      /* log only log-ids not in cached_pending_msgs -> not already logged */
      l = g_list_find_custom (cached_pending_msgs, tpl_message_token,
          (GCompareFunc) g_strcmp0);

      if (l == NULL)
        {
          /* call the received signal callback to trigger the message storing */
          on_received_signal_cb (TP_CHANNEL (proxy),
              message_id, message_timestamp, message_sender_handle,
              message_type, message_flags, message_body,
              NULL, NULL);
        }
      else
        {
          /* the message has been already logged, remove it from the list so
           * that, in the end of the loop, the items still in
           * cached_pending_msgs can be considered stale */
          g_free (l->data);
          cached_pending_msgs = g_list_delete_link (cached_pending_msgs, l);
        }

      g_free (tpl_message_token);
    }

  /* At this point all remaining elements of cached_pending_msgs are those
   * that the TplLogStoreSqlite knew as pending but currently not
   * listed as such in the current pending message list -> stale */
  tpl_channel_text_clean_up_stale_tokens (TPL_CHANNEL_TEXT (proxy),
      cached_pending_msgs);
  while (cached_pending_msgs != NULL)
    {
      PATH_DEBUG (proxy, "%s is stale, removed from DB",
          (gchar *) cached_pending_msgs->data);

      g_free (cached_pending_msgs->data);
      cached_pending_msgs = g_list_delete_link (cached_pending_msgs,
          cached_pending_msgs);
    }

out:
  if (cache != NULL)
    g_object_unref (cache);

  if (loc_error != NULL)
      g_error_free (loc_error);

/* If an error occured, do not terminate(), just have it logged.
 * terminate() would be fatal for TplChannel preparation,
 * but in this case it would just mean that it couldn't retrieve pending
 * messages, but it might still log the rest. If the next operation in chain
 * fails, it's fatal. Partial data loss is better than total data loss */
  _tpl_action_chain_continue (ctx);
}


/* PendingMessages CB for Text interface */
static void
got_text_pending_messages_cb (TpChannel *proxy,
    const GPtrArray *result,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  TplLogStore *cache = _tpl_log_store_sqlite_dup ();
  TplActionChain *ctx = user_data;
  GList *cached_pending_msgs, *l;
  const gchar *channel_path;
  GError *loc_error = NULL;
  guint i;

  if (error != NULL)
    {
      PATH_CRITICAL (proxy, "retrieving pending messages for Text iface: %s",
          error->message);
      _tpl_action_chain_terminate (ctx);
      return;
    }

  channel_path = tp_proxy_get_object_path (proxy);

  /* getting messages ids known to be pending at last TPL exit */
  cached_pending_msgs = _tpl_log_store_sqlite_get_pending_messages (cache,
      TP_CHANNEL (proxy), &loc_error);

  if (loc_error != NULL)
    {
      PATH_CRITICAL (proxy,
          "Unable to obtain pending messages stored in TPL DB: %s",
          loc_error->message);
      _tpl_action_chain_terminate (ctx);

      return;
    }

  PATH_DEBUG (proxy, "%d pending message(s) for Text iface", result->len);
  for (i = 0; i < result->len; ++i)
    {
      GValueArray *message_struct;
      const gchar *message_body;
      gchar *tpl_message_token;
      guint message_id;
      guint message_timestamp;
      guint from_handle;
      guint message_type;
      guint message_flags;

      message_struct = g_ptr_array_index (result, i);

      tp_value_array_unpack (message_struct, 6,
          &message_id,
          &message_timestamp,
          &from_handle,
          &message_type,
          &message_flags,
          &message_body);

      tpl_message_token = _tpl_create_message_token (channel_path,
          message_timestamp, message_id);

      /* log only log-ids not in cached_pending_msgs -> not already logged */
      l = g_list_find_custom (cached_pending_msgs, tpl_message_token,
          (GCompareFunc) g_strcmp0);

      if (l == NULL)
        {
          /* call the received signal callback to trigger the message storing */
          on_received_signal_cb (proxy, message_id, message_timestamp,
              from_handle, message_type, message_flags, message_body,
              NULL, NULL);
        }
      else
        {
          /* the message has been already logged, remove it from the list so
           * that, in the end of the loop, the items still in
           * cached_pending_msgs can be considered stale */
          g_free (l->data);
          cached_pending_msgs = g_list_delete_link (cached_pending_msgs, l);
        }

      g_free (tpl_message_token);
    }

  /* At this point all remaining elements of cached_pending_msgs are those
   * that the TplLogStoreSqlite knew as pending but currently not
   * listed as such in the current pending message list -> stale */
  tpl_channel_text_clean_up_stale_tokens (TPL_CHANNEL_TEXT (proxy),
      cached_pending_msgs);
  while (cached_pending_msgs != NULL)
    {
      PATH_DEBUG (proxy, "%s is stale, removed from DB",
          (gchar *) cached_pending_msgs->data);

      g_free (cached_pending_msgs->data);
      cached_pending_msgs = g_list_delete_link (cached_pending_msgs,
          cached_pending_msgs);
    }

  _tpl_action_chain_continue (ctx);
}

static void
pendingproc_connect_message_signals (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *tpl_text = _tpl_action_chain_get_object (ctx);
  GError *error = NULL;
  gboolean is_error = FALSE;
  TpChannel *channel = TP_CHANNEL (tpl_text);

  tp_cli_channel_type_text_connect_to_received (channel,
      on_received_signal_cb, NULL, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'received' signal connect: %s", error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  tp_cli_channel_type_text_connect_to_sent (channel,
      on_sent_signal_cb, tpl_text, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'sent' signal connect: %s", error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  tp_cli_channel_type_text_connect_to_send_error (channel,
      on_send_error_cb, tpl_text, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'send error' signal connect: %s", error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  tp_cli_channel_type_text_connect_to_lost_message (channel,
      on_lost_message_cb, tpl_text, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'lost message' signal connect: %s",
          error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  tp_cli_channel_connect_to_closed (channel, on_closed_cb,
      tpl_text, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'closed' signal connect: %s", error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  if (tp_proxy_has_interface_by_id (tpl_text,
        TP_IFACE_QUARK_CHANNEL_INTERFACE_MESSAGES))
    {
      tp_cli_channel_interface_messages_connect_to_pending_messages_removed (
          channel, on_pending_messages_removed_cb, NULL, NULL,
          G_OBJECT (tpl_text), &error);
      if (error != NULL)
        {
          PATH_DEBUG (tpl_text, "'PendingMessagesRemoved' signal connect: %s",
              error->message);
          g_clear_error (&error);
          is_error = TRUE;
        }
    }

  if (is_error)
    _tpl_action_chain_terminate (ctx);
  else
    _tpl_action_chain_continue (ctx);
}



/* Signal's Callbacks */
static void
on_pending_messages_removed_cb (TpChannel *proxy,
    const GArray *message_ids,
    gpointer user_data,
    GObject *weak_object)
{
  TplLogStore *cache = _tpl_log_store_sqlite_dup ();
  guint i;
  GError *error = NULL;

  for (i = 0; i < message_ids->len; ++i)
    {
      guint msg_id = g_array_index (message_ids, guint, i);
      _tpl_log_store_sqlite_set_acknowledgment_by_msg_id (cache, proxy, msg_id,
          &error);
      if (error != NULL)
        {
          PATH_DEBUG (proxy, "cannot set the ACK flag for msg_id %u: %s",
              msg_id, error->message);
          g_clear_error (&error);
        }
      else
        {
          PATH_DEBUG (proxy, "msg_id %d acknowledged", msg_id);
        }
    }

  if (cache != NULL)
    g_object_unref (cache);
}


static void
on_closed_cb (TpChannel *proxy,
    gpointer user_data,
    GObject *weak_object)
{
  TplChannelText *tpl_text = TPL_CHANNEL_TEXT (user_data);
  TplChannel *tpl_chan = TPL_CHANNEL (tpl_text);
  TplObserver *observer = _tpl_observer_new ();

  if (!_tpl_observer_unregister_channel (observer, tpl_chan))
    PATH_DEBUG (tpl_chan, "Channel couldn't be unregistered correctly (BUG?)");

  g_object_unref (observer);
}


static void
on_lost_message_cb (TpChannel *proxy,
           gpointer user_data,
           GObject *weak_object)
{
  PATH_DEBUG (proxy, "lost message signal catched. nothing logged");
  /* TODO log that the system lost a message */
}


static void
on_send_error_cb (TpChannel *proxy,
         guint arg_Error,
         guint arg_Timestamp,
         guint arg_Type,
         const gchar *arg_Text,
         gpointer user_data,
         GObject *weak_object)
{
  PATH_DEBUG (proxy, "unlogged event: TP was unable to send the message: %s",
      arg_Text);
  /* TODO log that the system was unable to send the message */
}


static void
on_sent_signal_cb (TpChannel *proxy,
    guint arg_Timestamp,
    guint arg_Type,
    const gchar *arg_Text,
    gpointer user_data,
    GObject *weak_object)
{
  GError *error = NULL;
  TplChannelText *tpl_text = TPL_CHANNEL_TEXT (user_data);
  TpContact *me;
  TplEntity *tpl_entity_sender;
  TplEntity *tpl_entity_receiver = NULL;
  TplEntryText *text_log;
  TplEntry *log;
  TplLogManager *logmanager;
  const gchar *chat_id;
  TpAccount *account;
  const gchar *channel_path;
  gchar *log_id;

  g_return_if_fail (TPL_IS_CHANNEL_TEXT (tpl_text));

  channel_path = tp_proxy_get_object_path (TP_PROXY (tpl_text));
  log_id = _tpl_create_message_token (channel_path, arg_Timestamp,
      TPL_ENTRY_MSG_ID_ACKNOWLEDGED);

  /* Initialize data for TplEntity */
  me = _tpl_channel_text_get_my_contact (tpl_text);
  tpl_entity_sender = _tpl_entity_from_tp_contact (me);
  _tpl_entity_set_entity_type (tpl_entity_sender, TPL_ENTITY_SELF);

  if (!_tpl_channel_text_is_chatroom (tpl_text))
    {
      TpContact *remote;
      TpHandle handle = tp_channel_get_handle (TP_CHANNEL (tpl_text), NULL);

      remote = g_hash_table_lookup (tpl_text->priv->contacts,
          GUINT_TO_POINTER (handle));
      g_assert (remote != NULL);

      tpl_entity_receiver = _tpl_entity_from_tp_contact (remote);
      _tpl_entity_set_entity_type (tpl_entity_receiver, TPL_ENTITY_CONTACT);

      DEBUG ("sent:\n\tlog_id=\"%s\"\n\tto=\"%s "
          "(%s)\"\n\tfrom=\"%s (%s)\"\n\tmsg=\"%s\"",
          log_id,
          tpl_entity_get_identifier (tpl_entity_receiver),
          tpl_entity_get_alias (tpl_entity_receiver),
          tpl_entity_get_identifier (tpl_entity_sender),
          tpl_entity_get_alias (tpl_entity_sender),
          arg_Text);

    }
  else
    {
      DEBUG ("sent:\n\tlog_id=\"%s\"\n\tto "
          "chatroom=\"%s\"\n\tfrom=\"%s (%s)\"\n\tmsg=\"%s\"",
          log_id,
          _tpl_channel_text_get_chatroom_id (tpl_text),
          tpl_entity_get_identifier (tpl_entity_sender),
          tpl_entity_get_alias (tpl_entity_sender),
          arg_Text);
    }

  /* Initialise TplEntryText */
  if (!_tpl_channel_text_is_chatroom (tpl_text))
    chat_id = tpl_entity_get_identifier (tpl_entity_receiver);
  else
    chat_id = _tpl_channel_text_get_chatroom_id (tpl_text);

  account = _tpl_channel_get_account (TPL_CHANNEL (tpl_text));

  text_log = _tpl_entry_text_new (log_id, account,
      TPL_ENTRY_DIRECTION_OUT);
  log = TPL_ENTRY (text_log);

  _tpl_entry_text_set_pending_msg_id (text_log,
      TPL_ENTRY_MSG_ID_ACKNOWLEDGED);
  _tpl_entry_set_channel_path (TPL_ENTRY (log), channel_path);
  _tpl_entry_set_chat_id (log, chat_id);
  _tpl_entry_set_timestamp (log, (time_t) arg_Timestamp);
  _tpl_entry_set_signal_type (log, TPL_ENTRY_TEXT_SIGNAL_SENT);
  _tpl_entry_set_sender (log, tpl_entity_sender);
  /* NULL when it's a chatroom */
  if (tpl_entity_receiver != NULL)
    _tpl_entry_set_receiver (log, tpl_entity_receiver);
  _tpl_entry_text_set_message (text_log, arg_Text);
  _tpl_entry_text_set_message_type (text_log, arg_Type);
  _tpl_entry_text_set_tpl_channel_text (text_log, tpl_text);

  /* Initialized LogStore and send the log entry */
  _tpl_entry_text_set_chatroom (text_log,
      _tpl_channel_text_is_chatroom (tpl_text));

  logmanager = tpl_log_manager_dup_singleton ();
  _tpl_log_manager_add_message (logmanager, TPL_ENTRY (log), &error);

  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "LogStore: %s", error->message);
      g_error_free (error);
    }

  if (tpl_entity_receiver != NULL)
    g_object_unref (tpl_entity_receiver);
  g_object_unref (tpl_entity_sender);
  g_object_unref (logmanager);
  g_object_unref (log);

  g_free (log_id);
}


static void
on_received_signal_with_contact_cb (TpConnection *connection,
    guint n_contacts,
    TpContact *const *contacts,
    guint n_failed,
    const TpHandle *failed,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  TplEntryText *log = user_data;
  TplChannelText *tpl_text;
  TpContact *remote;
  TpHandle handle;

  g_return_if_fail (TPL_IS_ENTRY_TEXT (log));

  tpl_text = _tpl_entry_text_get_tpl_channel_text (log);

  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "An Unrecoverable error retrieving remote contact "
         "information occured: %s", error->message);
      PATH_DEBUG (tpl_text, "Unable to log the received message: %s",
         tpl_entry_text_get_message (log));
      g_object_unref (log);
      return;
    }

  if (n_failed > 0)
    {
      PATH_DEBUG (tpl_text, "%d invalid handle(s) passed to "
         "tp_connection_get_contacts_by_handle()", n_failed);
      PATH_DEBUG (tpl_text, "Not able to log the received message: %s",
         tpl_entry_text_get_message (log));
      g_object_unref (log);
      return;
    }

  remote = contacts[0];
  handle = tp_contact_get_handle (remote);

  g_hash_table_insert (tpl_text->priv->contacts, GUINT_TO_POINTER (handle),
      remote);

  keepon_on_receiving_signal (log, remote);
}


static void
keepon_on_receiving_signal (TplEntryText *text_log,
    TpContact *remote)
{
  TplEntry *log = TPL_ENTRY (text_log);
  TplChannelText *tpl_text;
  GError *e = NULL;
  TplLogManager *logmanager;
  TplEntity *tpl_entity_sender;
  TplEntity *tpl_entity_receiver;
  TpContact *local;

  g_return_if_fail (TPL_IS_ENTRY_TEXT (text_log));

  tpl_text = _tpl_entry_text_get_tpl_channel_text (text_log);
  local = _tpl_channel_text_get_my_contact (tpl_text);

  tpl_entity_sender = _tpl_entity_from_tp_contact (remote);
  _tpl_entity_set_entity_type (tpl_entity_sender, TPL_ENTITY_CONTACT);
  _tpl_entry_set_sender (log, tpl_entity_sender);

  tpl_entity_receiver = _tpl_entity_from_tp_contact (local);

  DEBUG ("recvd:\n\tlog_id=\"%s\"\n\tto=\"%s "
      "(%s)\"\n\tfrom=\"%s (%s)\"\n\tmsg=\"%s\"",
      _tpl_entry_get_log_id (log),
      tpl_entity_get_identifier (tpl_entity_receiver),
      tpl_entity_get_alias (tpl_entity_receiver),
      tpl_entity_get_identifier (tpl_entity_sender),
      tpl_entity_get_alias (tpl_entity_sender),
      tpl_entry_text_get_message (text_log));


  if (!_tpl_channel_text_is_chatroom (tpl_text))
    _tpl_entry_set_chat_id (log, tpl_entity_get_identifier (
          tpl_entity_sender));
  else
    _tpl_entry_set_chat_id (log, _tpl_channel_text_get_chatroom_id (
          tpl_text));

  _tpl_entry_text_set_chatroom (text_log,
      _tpl_channel_text_is_chatroom (tpl_text));

  logmanager = tpl_log_manager_dup_singleton ();
  _tpl_log_manager_add_message (logmanager, TPL_ENTRY (log), &e);
  if (e != NULL)
    {
      DEBUG ("%s", e->message);
      g_error_free (e);
    }

  g_object_unref (tpl_entity_sender);
  g_object_unref (log);
  g_object_unref (logmanager);
}


static void
on_received_signal_cb (TpChannel *proxy,
    guint arg_ID,
    guint arg_Timestamp,
    TpHandle sender,
    guint arg_Type,
    guint arg_Flags,
    const gchar *arg_Text,
    gpointer user_data,
    GObject *weak_object)
{
  TplChannelText *tpl_text = TPL_CHANNEL_TEXT (proxy);
  TplChannel *tpl_chan = TPL_CHANNEL (tpl_text);
  TpConnection *tp_conn;
  TpContact *me, *remote;
  TplEntity *tpl_entity_receiver = NULL;
  TplEntryText *text_log = NULL;
  TplEntry *log;
  TpAccount *account = _tpl_channel_get_account (TPL_CHANNEL (tpl_text));
  TplLogStore *index = _tpl_log_store_sqlite_dup ();
  const gchar *channel_path = tp_proxy_get_object_path (TP_PROXY (tpl_text));
  gchar *log_id = _tpl_create_message_token (channel_path, arg_Timestamp,
      arg_ID);

  /* First, check if log_id has already been logged
   *
   * FIXME: There is a race condition for which, right after a 'NewChannel'
   * signal is raised and a message is received, the 'received' signal handler
   * may be cateched before or being slower and arriving after the TplChannel
   * preparation (in which pending message list is examined)
   *
   * Workaround:
   * In the first case the analisys of P.M.L will detect that actually the
   * handler has already received and logged the message.
   * In the latter (here), the handler will detect that the P.M.L analisys
   * has found and logged it, returning immediatly */
  if (_tpl_log_store_sqlite_log_id_is_present (index, log_id))
    {
      PATH_DEBUG (tpl_text, "%s found, not logging", log_id);
      goto out;
    }

  /* TODO use the Message iface to check the delivery
     notification and handle it correctly */
  if (arg_Flags & TP_CHANNEL_TEXT_MESSAGE_FLAG_NON_TEXT_CONTENT)
    {
      PATH_DEBUG (tpl_text, "Non text content flag set. "
          "Probably a delivery notification for a sent message. "
          "Ignoring");
      return;
    }

  if (arg_Flags & TP_CHANNEL_TEXT_MESSAGE_FLAG_RESCUED)
    {
      PATH_DEBUG (tpl_text, "Ignore 'rescued' message");
      return;
    }

  /* Initialize TplEntryText (part 1) - chat_id still unknown */
  text_log = _tpl_entry_text_new (log_id, account,
      TPL_ENTRY_DIRECTION_IN);
  log = TPL_ENTRY (text_log);

  _tpl_entry_set_channel_path (log, channel_path);
  _tpl_entry_text_set_pending_msg_id (text_log, arg_ID);
  _tpl_entry_text_set_tpl_channel_text (text_log, tpl_text);
  _tpl_entry_text_set_message (text_log, arg_Text);
  _tpl_entry_text_set_message_type (text_log, arg_Type);
  _tpl_entry_set_signal_type (log,
      TPL_ENTRY_TEXT_SIGNAL_RECEIVED);

  me = _tpl_channel_text_get_my_contact (tpl_text);
  tpl_entity_receiver = _tpl_entity_from_tp_contact (me);
  _tpl_entity_set_entity_type (tpl_entity_receiver, TPL_ENTITY_SELF);
  _tpl_entry_set_receiver (log, tpl_entity_receiver);

  _tpl_entry_set_timestamp (log, (time_t) arg_Timestamp);

  tp_conn = tp_channel_borrow_connection (TP_CHANNEL (tpl_chan));
  remote = g_hash_table_lookup (tpl_text->priv->contacts,
      GUINT_TO_POINTER (sender));

  if (remote == NULL)
    {
      /* Contact is not in the cache */
      tp_connection_get_contacts_by_handle (tp_conn, 1, &sender,
          G_N_ELEMENTS (features), features, on_received_signal_with_contact_cb,
          log, NULL, G_OBJECT (tpl_text));
    }
  else
    {
      keepon_on_receiving_signal (text_log, remote);
    }

out:
  if (tpl_entity_receiver != NULL)
    g_object_unref (tpl_entity_receiver);

  g_object_unref (index);
  /* log is unrefed in keepon_on_receiving_signal() */

  g_free (log_id);
}
