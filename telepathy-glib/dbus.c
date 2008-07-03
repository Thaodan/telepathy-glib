/*
 * dbus.c - Source for D-Bus utilities
 *
 * Copyright (C) 2005-2008 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright (C) 2005-2008 Nokia Corporation
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

/**
 * SECTION:dbus
 * @title: D-Bus utilities
 * @short_description: some D-Bus utility functions
 *
 * D-Bus utility functions used in telepathy-glib.
 */

/**
 * SECTION:asv
 * @title: Manipulating a{sv} mappings
 * @short_description: Functions to manipulate mappings from string to
 *  variant, as represented in dbus-glib by a #GHashTable from string
 *  to #GValue
 *
 * Mappings from string to variant (D-Bus signature a{sv}) are commonly used
 * to provide extensibility, but in dbus-glib they're somewhat awkward to deal
 * with.
 *
 * These functions (tp_asv_*) provide convenient access to the values in such
 * a mapping.
 *
 * They also work around the fact that none of the #GHashTable public API
 * takes a const pointer to a #GHashTable, even the read-only methods that
 * logically ought to.
 *
 * Parts of telepathy-glib return const pointers to #GHashTable, to encourage
 * the use of this API.
 *
 * Since: 0.7.9
 */

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/dbus-internal.h>

#include <stdlib.h>
#include <string.h>

#include <dbus/dbus-shared.h>

#include <telepathy-glib/errors.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/proxy-subclass.h>
#include <telepathy-glib/util.h>

#include "telepathy-glib/_gen/signals-marshal.h"

#include "telepathy-glib/_gen/tp-cli-dbus-daemon-body.h"

/**
 * tp_asv_size:
 * @asv: a GHashTable
 *
 * Return the size of @asv as if via g_hash_table_size().
 *
 * The only difference is that this version takes a const #GHashTable and
 * casts it.
 *
 * Since: 0.7.12
 */
/* (#define + static inline in dbus.h) */

/**
 * tp_dbus_g_method_return_not_implemented:
 * @context: The D-Bus method invocation context
 *
 * Return the Telepathy error NotImplemented from the method invocation
 * given by @context.
 */
void
tp_dbus_g_method_return_not_implemented (DBusGMethodInvocation *context)
{
  GError e = { TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED, "Not implemented" };

  dbus_g_method_return_error (context, &e);
}

/**
 * tp_get_bus:
 *
 * <!--Returns: says it all-->
 *
 * Returns: a connection to the starter or session D-Bus daemon.
 */
DBusGConnection *
tp_get_bus (void)
{
  static DBusGConnection *bus = NULL;

  if (bus == NULL)
    {
      GError *error = NULL;

      bus = dbus_g_bus_get (DBUS_BUS_STARTER, &error);

      if (bus == NULL)
        {
          g_warning ("Failed to connect to starter bus: %s", error->message);
          exit (1);
        }
    }

  return bus;
}

/**
 * tp_get_bus_proxy:
 *
 * <!--Returns: says it all-->
 *
 * Returns: a proxy for the bus daemon object on the starter or session bus.
 */
DBusGProxy *
tp_get_bus_proxy (void)
{
  static DBusGProxy *bus_proxy = NULL;

  if (bus_proxy == NULL)
    {
      DBusGConnection *bus = tp_get_bus ();

      bus_proxy = dbus_g_proxy_new_for_name (bus,
                                            "org.freedesktop.DBus",
                                            "/org/freedesktop/DBus",
                                            "org.freedesktop.DBus");

      if (bus_proxy == NULL)
        g_error ("Failed to get proxy object for bus.");
    }

  return bus_proxy;
}

/**
 * TpDBusNameType:
 * @TP_DBUS_NAME_TYPE_UNIQUE: accept unique names like :1.123
 *  (not including the name of the bus daemon itself)
 * @TP_DBUS_NAME_TYPE_WELL_KNOWN: accept well-known names like
 *  com.example.Service (not including the name of the bus daemon itself)
 * @TP_DBUS_NAME_TYPE_BUS_DAEMON: accept the name of the bus daemon
 *  itself, which has the syntax of a well-known name, but behaves like a
 *  unique name
 * @TP_DBUS_NAME_TYPE_NOT_BUS_DAEMON: accept either unique or well-known
 *  names, but not the bus daemon
 * @TP_DBUS_NAME_TYPE_ANY: accept any of the above
 *
 * A set of flags indicating which D-Bus bus names are acceptable.
 * They can be combined with the bitwise-or operator to accept multiple
 * types. %TP_DBUS_NAME_TYPE_NOT_BUS_DAEMON and %TP_DBUS_NAME_TYPE_ANY are
 * the bitwise-or of other appropriate types, for convenience.
 *
 * Since: 0.7.1
 */

/**
 * tp_dbus_check_valid_bus_name:
 * @name: a possible bus name
 * @allow_types: some combination of %TP_DBUS_NAME_TYPE_UNIQUE,
 *  %TP_DBUS_NAME_TYPE_WELL_KNOWN or %TP_DBUS_NAME_TYPE_BUS_DAEMON
 *  (often this will be %TP_DBUS_NAME_TYPE_NOT_BUS_DAEMON or
 *  %TP_DBUS_NAME_TYPE_ANY)
 * @error: used to raise %TP_DBUS_ERROR_INVALID_BUS_NAME if %FALSE is returned
 *
 * Check that the given string is a valid D-Bus bus name of an appropriate
 * type.
 *
 * Returns: %TRUE if @name is valid
 *
 * Since: 0.7.1
 */
gboolean
tp_dbus_check_valid_bus_name (const gchar *name,
                              TpDBusNameType allow_types,
                              GError **error)
{
  gboolean dot = FALSE;
  gboolean unique;
  gchar last;
  const gchar *ptr;

  if (name[0] == '\0')
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_BUS_NAME,
          "The empty string is not a valid bus name");
      return FALSE;
    }

  if (!tp_strdiff (name, DBUS_SERVICE_DBUS))
    {
      if (allow_types & TP_DBUS_NAME_TYPE_BUS_DAEMON)
        return TRUE;

      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_BUS_NAME,
          "The D-Bus daemon's bus name is not acceptable here");
      return FALSE;
    }

  unique = (name[0] == ':');
  if (unique && (allow_types & TP_DBUS_NAME_TYPE_UNIQUE) == 0)
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_BUS_NAME,
          "A well-known bus name not starting with ':'%s is required",
          allow_types & TP_DBUS_NAME_TYPE_BUS_DAEMON
            ? " (or the bus daemon itself)"
            : "");
      return FALSE;
    }

  if (!unique && (allow_types & TP_DBUS_NAME_TYPE_WELL_KNOWN) == 0)
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_BUS_NAME,
          "A unique bus name starting with ':'%s is required",
          allow_types & TP_DBUS_NAME_TYPE_BUS_DAEMON
            ? " (or the bus daemon itself)"
            : "");
      return FALSE;
    }

  if (strlen (name) > 255)
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_BUS_NAME,
          "Invalid bus name: too long (> 255 characters)");
      return FALSE;
    }

  last = '\0';

  for (ptr = name + (unique ? 1 : 0); *ptr != '\0'; ptr++)
    {
      if (*ptr == '.')
        {
          dot = TRUE;

          if (last == '.')
            {
              g_set_error (error, TP_DBUS_ERRORS,
                  TP_DBUS_ERROR_INVALID_BUS_NAME,
                  "Invalid bus name '%s': contains '..'", name);
              return FALSE;
            }
          else if (last == '\0')
            {
              g_set_error (error, TP_DBUS_ERRORS,
                  TP_DBUS_ERROR_INVALID_BUS_NAME,
                  "Invalid bus name '%s': must not start with '.'", name);
              return FALSE;
            }
        }
      else if (g_ascii_isdigit (*ptr))
        {
          if (!unique)
            {
              if (last == '.')
                {
                  g_set_error (error, TP_DBUS_ERRORS,
                      TP_DBUS_ERROR_INVALID_BUS_NAME,
                      "Invalid bus name '%s': a digit may not follow '.' "
                      "except in a unique name starting with ':'", name);
                  return FALSE;
                }
              else if (last == '\0')
                {
                  g_set_error (error, TP_DBUS_ERRORS,
                      TP_DBUS_ERROR_INVALID_BUS_NAME,
                      "Invalid bus name '%s': must not start with a digit",
                      name);
                  return FALSE;
                }
            }
        }
      else if (!g_ascii_isalpha (*ptr) && *ptr != '_' && *ptr != '-')
        {
          g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_BUS_NAME,
              "Invalid bus name '%s': contains invalid character '%c'",
              name, *ptr);
          return FALSE;
        }

      last = *ptr;
    }

  if (last == '.')
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_BUS_NAME,
          "Invalid bus name '%s': must not end with '.'", name);
      return FALSE;
    }

  if (!dot)
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_BUS_NAME,
          "Invalid bus name '%s': must contain '.'", name);
      return FALSE;
    }

  return TRUE;
}

/**
 * tp_dbus_check_valid_interface_name:
 * @name: a possible interface name
 * @error: used to raise %TP_DBUS_ERROR_INVALID_INTERFACE_NAME if %FALSE is
 *  returned
 *
 * Check that the given string is a valid D-Bus interface name. This is
 * also appropriate to use to check for valid error names.
 *
 * Returns: %TRUE if @name is valid
 *
 * Since: 0.7.1
 */
gboolean
tp_dbus_check_valid_interface_name (const gchar *name,
                                    GError **error)
{
  gboolean dot = FALSE;
  gchar last;
  const gchar *ptr;

  if (name[0] == '\0')
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_INTERFACE_NAME,
          "The empty string is not a valid interface name");
      return FALSE;
    }

  if (strlen (name) > 255)
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_INTERFACE_NAME,
          "Invalid interface name: too long (> 255 characters)");
      return FALSE;
    }

  last = '\0';

  for (ptr = name; *ptr != '\0'; ptr++)
    {
      if (*ptr == '.')
        {
          dot = TRUE;

          if (last == '.')
            {
              g_set_error (error, TP_DBUS_ERRORS,
                  TP_DBUS_ERROR_INVALID_INTERFACE_NAME,
                  "Invalid interface name '%s': contains '..'", name);
              return FALSE;
            }
          else if (last == '\0')
            {
              g_set_error (error, TP_DBUS_ERRORS,
                  TP_DBUS_ERROR_INVALID_INTERFACE_NAME,
                  "Invalid interface name '%s': must not start with '.'",
                  name);
              return FALSE;
            }
        }
      else if (g_ascii_isdigit (*ptr))
        {
          if (last == '\0')
            {
              g_set_error (error, TP_DBUS_ERRORS,
                  TP_DBUS_ERROR_INVALID_INTERFACE_NAME,
                  "Invalid interface name '%s': must not start with a digit",
                  name);
              return FALSE;
            }
          else if (last == '.')
            {
              g_set_error (error, TP_DBUS_ERRORS,
                  TP_DBUS_ERROR_INVALID_INTERFACE_NAME,
                  "Invalid interface name '%s': a digit must not follow '.'",
                  name);
              return FALSE;
            }
        }
      else if (!g_ascii_isalpha (*ptr) && *ptr != '_')
        {
          g_set_error (error, TP_DBUS_ERRORS,
              TP_DBUS_ERROR_INVALID_INTERFACE_NAME,
              "Invalid interface name '%s': contains invalid character '%c'",
              name, *ptr);
          return FALSE;
        }

      last = *ptr;
    }

  if (last == '.')
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_INTERFACE_NAME,
          "Invalid interface name '%s': must not end with '.'", name);
      return FALSE;
    }

  if (!dot)
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_INTERFACE_NAME,
          "Invalid interface name '%s': must contain '.'", name);
      return FALSE;
    }

  return TRUE;
}

/**
 * tp_dbus_check_valid_member_name:
 * @name: a possible member name
 * @error: used to raise %TP_DBUS_ERROR_INVALID_MEMBER_NAME if %FALSE is
 *  returned
 *
 * Check that the given string is a valid D-Bus member (method or signal) name.
 *
 * Returns: %TRUE if @name is valid
 *
 * Since: 0.7.1
 */
gboolean
tp_dbus_check_valid_member_name (const gchar *name,
                                 GError **error)
{
  const gchar *ptr;

  if (name[0] == '\0')
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_MEMBER_NAME,
          "The empty string is not a valid method or signal name");
      return FALSE;
    }

  if (strlen (name) > 255)
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_MEMBER_NAME,
          "Invalid method or signal name: too long (> 255 characters)");
      return FALSE;
    }

  for (ptr = name; *ptr != '\0'; ptr++)
    {
      if (g_ascii_isdigit (*ptr))
        {
          if (ptr == name)
            {
              g_set_error (error, TP_DBUS_ERRORS,
                  TP_DBUS_ERROR_INVALID_MEMBER_NAME,
                  "Invalid method or signal name '%s': must not start with "
                  "a digit", name);
              return FALSE;
            }
        }
      else if (!g_ascii_isalpha (*ptr) && *ptr != '_')
        {
          g_set_error (error, TP_DBUS_ERRORS,
              TP_DBUS_ERROR_INVALID_MEMBER_NAME,
              "Invalid method or signal name '%s': contains invalid "
              "character '%c'",
              name, *ptr);
          return FALSE;
        }
    }

  return TRUE;
}

/**
 * tp_dbus_check_valid_object_path:
 * @path: a possible object path
 * @error: used to raise %TP_DBUS_ERROR_INVALID_OBJECT_PATH if %FALSE is
 *  returned
 *
 * Check that the given string is a valid D-Bus object path.
 *
 * Returns: %TRUE if @path is valid
 *
 * Since: 0.7.1
 */
gboolean
tp_dbus_check_valid_object_path (const gchar *path, GError **error)
{
  const gchar *ptr;

  if (path[0] != '/')
    {
      g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_OBJECT_PATH,
          "Invalid object path '%s': must start with '/'",
          path);
      return FALSE;
    }

  if (path[1] == '\0')
    return TRUE;

  for (ptr = path + 1; *ptr != '\0'; ptr++)
    {
      if (*ptr == '/')
        {
          if (ptr[-1] == '/')
            {
              g_set_error (error, TP_DBUS_ERRORS,
                  TP_DBUS_ERROR_INVALID_OBJECT_PATH,
                  "Invalid object path '%s': contains '//'", path);
              return FALSE;
            }
        }
      else if (!g_ascii_isalnum (*ptr) && *ptr != '_')
        {
          g_set_error (error, TP_DBUS_ERRORS,
              TP_DBUS_ERROR_INVALID_OBJECT_PATH,
              "Invalid object path '%s': contains invalid character '%c'",
              path, *ptr);
          return FALSE;
        }
    }

  if (ptr[-1] == '/')
    {
        g_set_error (error, TP_DBUS_ERRORS, TP_DBUS_ERROR_INVALID_OBJECT_PATH,
            "Invalid object path '%s': is not '/' but does end with '/'",
            path);
        return FALSE;
    }

  return TRUE;
}

/**
 * TpDBusDaemonClass:
 *
 * The class of #TpDBusDaemon.
 *
 * Since: 0.7.1
 */
struct _TpDBusDaemonClass
{
  /*<private>*/
  TpProxyClass parent_class;
  gpointer priv;
};

/**
 * TpDBusDaemon:
 *
 * A subclass of #TpProxy that represents the D-Bus daemon. It mainly provides
 * functionality to manage well-known names on the bus.
 *
 * Since: 0.7.1
 */
struct _TpDBusDaemon
{
  /*<private>*/
  TpProxy parent;

  TpDBusDaemonPrivate *priv;
};

struct _TpDBusDaemonPrivate
{
  /* dup'd name => _NameOwnerWatch */
  GHashTable *name_owner_watches;
};

G_DEFINE_TYPE (TpDBusDaemon, tp_dbus_daemon, TP_TYPE_PROXY);

/**
 * tp_dbus_daemon_new:
 * @connection: a connection to D-Bus
 *
 * <!-- -->
 *
 * Returns: a new proxy for signals and method calls on the bus daemon
 *  to which @connection is connected
 *
 * Since: 0.7.1
 */
TpDBusDaemon *
tp_dbus_daemon_new (DBusGConnection *connection)
{
  g_return_val_if_fail (connection != NULL, NULL);

  return TP_DBUS_DAEMON (g_object_new (TP_TYPE_DBUS_DAEMON,
        "dbus-connection", connection,
        "bus-name", DBUS_SERVICE_DBUS,
        "object-path", DBUS_PATH_DBUS,
        NULL));
}

typedef struct
{
  TpDBusDaemonNameOwnerChangedCb callback;
  gpointer user_data;
  GDestroyNotify destroy;
  gchar *last_owner;
} _NameOwnerWatch;

typedef struct
{
  TpDBusDaemonNameOwnerChangedCb callback;
  gpointer user_data;
  GDestroyNotify destroy;
} _NameOwnerSubWatch;

static void
_tp_dbus_daemon_name_owner_changed_multiple (TpDBusDaemon *self,
                                             const gchar *name,
                                             const gchar *new_owner,
                                             gpointer user_data)
{
  GArray *array = user_data;
  guint i;

  for (i = 0; i < array->len; i++)
    {
      _NameOwnerSubWatch *watch = &g_array_index (array, _NameOwnerSubWatch,
          i);

      watch->callback (self, name, new_owner, watch->user_data);
    }
}

static void
_tp_dbus_daemon_name_owner_changed_multiple_free (gpointer data)
{
  GArray *array = data;
  guint i;

  for (i = 0; i < array->len; i++)
    {
      _NameOwnerSubWatch *watch = &g_array_index (array, _NameOwnerSubWatch,
          i);

      if (watch->destroy)
        watch->destroy (watch->user_data);
    }

  g_array_free (array, TRUE);
}

static void
_tp_dbus_daemon_name_owner_changed (TpDBusDaemon *self,
                                    const gchar *name,
                                    const gchar *new_owner)
{
  _NameOwnerWatch *watch = g_hash_table_lookup (self->priv->name_owner_watches,
      name);

  if (watch == NULL)
    return;

  /* This is partly to handle the case where an owner change happens
   * while GetNameOwner is in flight, partly to be able to optimize by only
   * calling GetNameOwner if we didn't already know, and partly because of a
   * dbus-glib bug that means we get every signal twice
   * (it thinks org.freedesktop.DBus is both a well-known name and a unique
   * name). */
  if (!tp_strdiff (watch->last_owner, new_owner))
    return;

  g_free (watch->last_owner);
  watch->last_owner = g_strdup (new_owner);

  watch->callback (self, name, new_owner, watch->user_data);
}

static void
_tp_dbus_daemon_name_owner_changed_cb (TpDBusDaemon *self,
                                       const gchar *name,
                                       const gchar *old_owner,
                                       const gchar *new_owner,
                                       gpointer user_data,
                                       GObject *object)
{
  _tp_dbus_daemon_name_owner_changed (self, name, new_owner);
}

static void
_tp_dbus_daemon_got_name_owner (TpDBusDaemon *self,
                                const gchar *owner,
                                const GError *error,
                                gpointer user_data,
                                GObject *user_object)
{
  gchar *name = user_data;

  if (error != NULL)
    owner = "";

  _tp_dbus_daemon_name_owner_changed (self, name, owner);
}

/**
 * TpDBusDaemonNameOwnerChangedCb:
 * @daemon: The D-Bus daemon
 * @name: The name whose ownership has changed or been discovered
 * @new_owner: The unique name that now owns @name
 * @user_data: Arbitrary user-supplied data as passed to
 *  tp_dbus_daemon_watch_name_owner()
 *
 * The signature of the callback called by tp_dbus_daemon_watch_name_owner().
 *
 * Since: 0.7.1
 */

/**
 * tp_dbus_daemon_watch_name_owner:
 * @self: The D-Bus daemon
 * @name: The name whose ownership is to be watched
 * @callback: Callback to call when the ownership is discovered or changes
 * @user_data: Arbitrary data to pass to @callback
 * @destroy: Called to destroy @user_data when the name owner watch is
 *  cancelled due to tp_dbus_daemon_cancel_name_owner_watch()
 *
 * Arrange for @callback to be called with the owner of @name as soon as
 * possible (which might even be before this function returns!), then
 * again every time the ownership of @name changes.
 *
 * If multiple watches are registered for the same @name, they will be called
 * in the order they were registered.
 *
 * Since: 0.7.1
 */
void
tp_dbus_daemon_watch_name_owner (TpDBusDaemon *self,
                                 const gchar *name,
                                 TpDBusDaemonNameOwnerChangedCb callback,
                                 gpointer user_data,
                                 GDestroyNotify destroy)
{
  _NameOwnerWatch *watch = g_hash_table_lookup (self->priv->name_owner_watches,
      name);

  if (watch == NULL)
    {
      /* Allocate a single watch (common case) */
      watch = g_slice_new (_NameOwnerWatch);
      watch->callback = callback;
      watch->user_data = user_data;
      watch->destroy = destroy;
      watch->last_owner = NULL;

      g_hash_table_insert (self->priv->name_owner_watches, g_strdup (name),
          watch);

      tp_cli_dbus_daemon_call_get_name_owner (self, -1, name,
          _tp_dbus_daemon_got_name_owner,
          g_strdup (name), g_free, NULL);
    }
  else
    {
      _NameOwnerSubWatch tmp = { callback, user_data, destroy };

      if (watch->callback == _tp_dbus_daemon_name_owner_changed_multiple)
        {
          /* The watch is already a "multiplexer", just append to it */
          GArray *array = watch->user_data;

          g_array_append_val (array, tmp);
        }
      else
        {
          /* Replace the old contents of the watch with one that dispatches
           * the signal to (potentially) more than one watcher */
          GArray *array = g_array_sized_new (FALSE, FALSE,
              sizeof (_NameOwnerSubWatch), 2);

          /* The new watcher */
          g_array_append_val (array, tmp);
          /* The old watcher */
          tmp.callback = watch->callback;
          tmp.user_data = watch->user_data;
          tmp.destroy = watch->destroy;
          g_array_prepend_val (array, tmp);

          watch->callback = _tp_dbus_daemon_name_owner_changed_multiple;
          watch->user_data = array;
          watch->destroy = _tp_dbus_daemon_name_owner_changed_multiple_free;
        }

      if (watch->last_owner != NULL)
        {
          /* FIXME: should avoid reentrancy? */
          callback (self, name, watch->last_owner, user_data);
        }
    }
}

/**
 * tp_dbus_daemon_cancel_name_owner_watch:
 * @self: the D-Bus daemon
 * @name: the name that was being watched
 * @callback: the callback that was called
 * @user_data: the user data that was provided
 *
 * If there was a previous call to tp_dbus_daemon_watch_name_owner()
 * with exactly the given @name, @callback and @user_data, remove it.
 *
 * If more than one watch matching the details provided was active, remove
 * only the most recently added one.
 *
 * Returns: %TRUE if there was such a watch, %FALSE otherwise
 *
 * Since: 0.7.1
 */
gboolean
tp_dbus_daemon_cancel_name_owner_watch (TpDBusDaemon *self,
                                        const gchar *name,
                                        TpDBusDaemonNameOwnerChangedCb callback,
                                        gconstpointer user_data)
{
  _NameOwnerWatch *watch = g_hash_table_lookup (self->priv->name_owner_watches,
      name);

  if (watch == NULL)
    {
      /* No watch at all */
      return FALSE;
    }
  else if (watch->callback == callback && watch->user_data == user_data)
    {
      /* Simple case: there is one name-owner watch and it's what we wanted */
      if (watch->destroy)
        watch->destroy (watch->user_data);

      g_free (watch->last_owner);
      g_slice_free (_NameOwnerWatch, watch);
      g_hash_table_remove (self->priv->name_owner_watches, name);
      return TRUE;
    }
  else if (watch->callback == _tp_dbus_daemon_name_owner_changed_multiple)
    {
      /* Complicated case: this watch is a "multiplexer", we need to check
       * its contents */
      GArray *array = watch->user_data;
      guint i;

      for (i = 1; i <= array->len; i++)
        {
          _NameOwnerSubWatch *entry = &g_array_index (array,
              _NameOwnerSubWatch, array->len - i);

          if (entry->callback == callback && entry->user_data == user_data)
            {
              if (entry->destroy != NULL)
                entry->destroy (entry->user_data);

              g_array_remove_index (array, array->len - i);

              if (array->len == 0)
                {
                  watch->destroy (watch->user_data);
                  g_free (watch->last_owner);
                  g_slice_free (_NameOwnerWatch, watch);
                  g_hash_table_remove (self->priv->name_owner_watches, name);
                }

              return TRUE;
            }
        }
    }

  /* We haven't found it */
  return FALSE;
}

/* for internal use (TpChannel, TpConnection _new convenience functions) */
gboolean
_tp_dbus_daemon_get_name_owner (TpDBusDaemon *self,
                                gint timeout_ms,
                                const gchar *well_known_name,
                                gchar **unique_name,
                                GError **error)
{
  DBusGProxy *iface = tp_proxy_borrow_interface_by_id ((TpProxy *) self,
      TP_IFACE_QUARK_DBUS_DAEMON, error);

  if (iface == NULL)
    return FALSE;

  return dbus_g_proxy_call_with_timeout (iface, "GetNameOwner", timeout_ms,
      error,
      G_TYPE_STRING, well_known_name,
      G_TYPE_INVALID,
      G_TYPE_STRING, unique_name,
      G_TYPE_INVALID);
}

static GObject *
tp_dbus_daemon_constructor (GType type,
                            guint n_params,
                            GObjectConstructParam *params)
{
  GObjectClass *object_class =
      (GObjectClass *) tp_dbus_daemon_parent_class;
  TpDBusDaemon *self = TP_DBUS_DAEMON (object_class->constructor (type,
        n_params, params));
  TpProxy *as_proxy = (TpProxy *) self;

  /* Connect to my own NameOwnerChanged signal.
   * The proxy hasn't had a chance to become invalid yet, so we can
   * assume that this signal connection will work */
  tp_cli_dbus_daemon_connect_to_name_owner_changed (self,
      _tp_dbus_daemon_name_owner_changed_cb, NULL, NULL, NULL, NULL);

  g_assert (!tp_strdiff (as_proxy->bus_name, DBUS_SERVICE_DBUS));
  g_assert (!tp_strdiff (as_proxy->object_path, DBUS_PATH_DBUS));

  return (GObject *) self;
}

static void
tp_dbus_daemon_init (TpDBusDaemon *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, TP_TYPE_DBUS_DAEMON,
      TpDBusDaemonPrivate);

  self->priv->name_owner_watches = g_hash_table_new_full (g_str_hash,
      g_str_equal, g_free, NULL);
}

static void
tp_dbus_daemon_dispose (GObject *object)
{
  TpDBusDaemon *self = TP_DBUS_DAEMON (object);

  if (self->priv->name_owner_watches != NULL)
    {
      GHashTable *tmp = self->priv->name_owner_watches;

      self->priv->name_owner_watches = NULL;
      g_hash_table_destroy (tmp);
    }

  G_OBJECT_CLASS (tp_dbus_daemon_parent_class)->dispose (object);
}

static void
tp_dbus_daemon_class_init (TpDBusDaemonClass *klass)
{
  TpProxyClass *proxy_class = (TpProxyClass *) klass;
  GObjectClass *object_class = (GObjectClass *) klass;

  g_type_class_add_private (klass, sizeof (TpDBusDaemonPrivate));

  object_class->constructor = tp_dbus_daemon_constructor;
  object_class->dispose = tp_dbus_daemon_dispose;

  proxy_class->interface = TP_IFACE_QUARK_DBUS_DAEMON;
  tp_proxy_or_subclass_hook_on_interface_add (TP_TYPE_DBUS_DAEMON,
      tp_cli_dbus_daemon_add_signals);
}

/* Auto-generated implementation of _tp_register_dbus_glib_marshallers */
#include "_gen/register-dbus-glib-marshallers-body.h"


/**
 * tp_asv_get_boolean:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 * @valid: Either %NULL, or a location to store %TRUE if the key actually
 *  exists and has a boolean value
 *
 * If a value for @key in @asv is present and boolean, return it,
 * and set *@valid to %TRUE if @valid is not %NULL.
 *
 * Otherwise return %FALSE, and set *@valid to %FALSE if @valid is not %NULL.
 *
 * (FIXME: should we also allow 'i' and 'u' with nonzero <=> True?)
 *
 * Returns: a boolean value for @key
 * @since 0.7.9
 */
gboolean
tp_asv_get_boolean (const GHashTable *asv,
                    const gchar *key,
                    gboolean *valid)
{
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL || !G_VALUE_HOLDS_BOOLEAN (value))
    {
      if (valid != NULL)
        *valid = FALSE;

      return FALSE;
    }

  if (valid != NULL)
    *valid = TRUE;

  return g_value_get_boolean (value);
}


/**
 * tp_asv_get_bytes:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 *
 * If a value for @key in @asv is present and is an array of bytes
 * (its GType is %DBUS_TYPE_G_UCHAR_ARRAY), return it.
 *
 * Otherwise return %NULL.
 *
 * The returned value is not copied, and is only valid as long as the value
 * for @key in @asv is not removed or altered. Copy it with
 * g_boxed_copy (DBUS_TYPE_G_UCHAR_ARRAY, ...) if you need to keep
 * it for longer.
 *
 * Returns: the string value of @key, or %NULL
 * @since 0.7.9
 */
const GArray *
tp_asv_get_bytes (const GHashTable *asv,
                   const gchar *key)
{
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL || !G_VALUE_HOLDS (value, DBUS_TYPE_G_UCHAR_ARRAY))
    return NULL;

  return g_value_get_boxed (value);
}


/**
 * tp_asv_get_string:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 *
 * If a value for @key in @asv is present and is a string, return it.
 *
 * Otherwise return %NULL.
 *
 * The returned value is not copied, and is only valid as long as the value
 * for @key in @asv is not removed or altered. Copy it with g_strdup() if you
 * need to keep it for longer.
 *
 * Returns: the string value of @key, or %NULL
 * @since 0.7.9
 */
const gchar *
tp_asv_get_string (const GHashTable *asv,
                   const gchar *key)
{
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL || !G_VALUE_HOLDS_STRING (value))
    return NULL;

  return g_value_get_string (value);
}


/**
 * tp_asv_get_int32:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 * @valid: Either %NULL, or a location in which to store %TRUE on success or
 *    %FALSE on failure
 *
 * If a value for @key in @asv is present, has an integer type used by
 * dbus-glib (guchar, gint, guint, gint64 or guint64) and fits in the
 * range of a gint32, return it, and if @valid is not %NULL, set *@valid to
 * %TRUE.
 *
 * Otherwise, return 0, and if @valid is not %NULL, set *@valid to %FALSE.
 *
 * Returns: the 32-bit signed integer value of @key, or 0
 * @since 0.7.9
 */
gint32
tp_asv_get_int32 (const GHashTable *asv,
                  const gchar *key,
                  gboolean *valid)
{
  gint64 i;
  guint64 u;
  gint32 ret;
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL)
    goto return_invalid;

  switch (G_VALUE_TYPE (value))
    {
    case G_TYPE_UCHAR:
      ret = g_value_get_uchar (value);
      break;

    case G_TYPE_UINT:
      u = g_value_get_uint (value);

      if (G_UNLIKELY (u > G_MAXINT32))
        goto return_invalid;

      ret = u;
      break;

    case G_TYPE_INT:
      ret = g_value_get_int (value);
      break;

    case G_TYPE_INT64:
      i = g_value_get_int64 (value);

      if (G_UNLIKELY (i < G_MININT32 || i > G_MAXINT32))
        goto return_invalid;

      ret = i;
      break;

    case G_TYPE_UINT64:
      u = g_value_get_uint64 (value);

      if (G_UNLIKELY (u > G_MAXINT32))
        goto return_invalid;

      ret = u;
      break;

    default:
      goto return_invalid;
    }

  if (valid != NULL)
    *valid = TRUE;

  return ret;

return_invalid:
  if (valid != NULL)
    *valid = FALSE;

  return 0;
}


/**
 * tp_asv_get_uint32:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 * @valid: Either %NULL, or a location in which to store %TRUE on success or
 *    %FALSE on failure
 *
 * If a value for @key in @asv is present, has an integer type used by
 * dbus-glib (guchar, gint, guint, gint64 or guint64) and fits in the
 * range of a guint32, return it, and if @valid is not %NULL, set *@valid to
 * %TRUE.
 *
 * Otherwise, return 0, and if @valid is not %NULL, set *@valid to %FALSE.
 *
 * Returns: the 32-bit unsigned integer value of @key, or 0
 * @since 0.7.9
 */
guint32
tp_asv_get_uint32 (const GHashTable *asv,
                   const gchar *key,
                   gboolean *valid)
{
  gint64 i;
  guint64 u;
  guint32 ret;
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL)
    goto return_invalid;

  switch (G_VALUE_TYPE (value))
    {
    case G_TYPE_UCHAR:
      ret = g_value_get_uchar (value);
      break;

    case G_TYPE_UINT:
      ret = g_value_get_uint (value);
      break;

    case G_TYPE_INT:
      i = g_value_get_int (value);

      if (G_UNLIKELY (i < 0))
        goto return_invalid;

      ret = i;
      break;

    case G_TYPE_INT64:
      i = g_value_get_int64 (value);

      if (G_UNLIKELY (i < 0 || i > G_MAXUINT32))
        goto return_invalid;

      ret = i;
      break;

    case G_TYPE_UINT64:
      u = g_value_get_uint64 (value);

      if (G_UNLIKELY (u > G_MAXUINT32))
        goto return_invalid;

      ret = u;
      break;

    default:
      goto return_invalid;
    }

  if (valid != NULL)
    *valid = TRUE;

  return ret;

return_invalid:
  if (valid != NULL)
    *valid = FALSE;

  return 0;
}


/**
 * tp_asv_get_int64:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 * @valid: Either %NULL, or a location in which to store %TRUE on success or
 *    %FALSE on failure
 *
 * If a value for @key in @asv is present, has an integer type used by
 * dbus-glib (guchar, gint, guint, gint64 or guint64) and fits in the
 * range of a gint64, return it, and if @valid is not %NULL, set *@valid to
 * %TRUE.
 *
 * Otherwise, return 0, and if @valid is not %NULL, set *@valid to %FALSE.
 *
 * Returns: the 64-bit signed integer value of @key, or 0
 * @since 0.7.9
 */
gint64
tp_asv_get_int64 (const GHashTable *asv,
                  const gchar *key,
                  gboolean *valid)
{
  gint64 ret;
  guint64 u;
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL)
    goto return_invalid;

  switch (G_VALUE_TYPE (value))
    {
    case G_TYPE_UCHAR:
      ret = g_value_get_uchar (value);
      break;

    case G_TYPE_UINT:
      ret = g_value_get_uint (value);
      break;

    case G_TYPE_INT:
      ret = g_value_get_int (value);
      break;

    case G_TYPE_INT64:
      ret = g_value_get_int64 (value);
      break;

    case G_TYPE_UINT64:
      u = g_value_get_uint64 (value);

      if (G_UNLIKELY (u > G_MAXINT64))
        goto return_invalid;

      ret = u;
      break;

    default:
      goto return_invalid;
    }

  if (valid != NULL)
    *valid = TRUE;

  return ret;

return_invalid:
  if (valid != NULL)
    *valid = FALSE;

  return 0;
}


/**
 * tp_asv_get_uint64:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 * @valid: Either %NULL, or a location in which to store %TRUE on success or
 *    %FALSE on failure
 *
 * If a value for @key in @asv is present, has an integer type used by
 * dbus-glib (guchar, gint, guint, gint64 or guint64) and is non-negative,
 * return it, and if @valid is not %NULL, set *@valid to %TRUE.
 *
 * Otherwise, return 0, and if @valid is not %NULL, set *@valid to %FALSE.
 *
 * Returns: the 64-bit unsigned integer value of @key, or 0
 * @since 0.7.9
 */
guint64
tp_asv_get_uint64 (const GHashTable *asv,
                   const gchar *key,
                   gboolean *valid)
{
  gint64 tmp;
  guint64 ret;
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL)
    goto return_invalid;

  switch (G_VALUE_TYPE (value))
    {
    case G_TYPE_UCHAR:
      ret = g_value_get_uchar (value);
      break;

    case G_TYPE_UINT:
      ret = g_value_get_uint (value);
      break;

    case G_TYPE_INT:
      tmp = g_value_get_int (value);

      if (G_UNLIKELY (tmp < 0))
        goto return_invalid;

      ret = tmp;
      break;

    case G_TYPE_INT64:
      tmp = g_value_get_int64 (value);

      if (G_UNLIKELY (tmp < 0))
        goto return_invalid;

      ret = tmp;
      break;

    case G_TYPE_UINT64:
      ret = g_value_get_uint64 (value);
      break;

    default:
      goto return_invalid;
    }

  if (valid != NULL)
    *valid = TRUE;

  return ret;

return_invalid:
  if (valid != NULL)
    *valid = FALSE;

  return 0;
}


/* FIXME: reviewers: should this succeed on all numeric types, or just on
 * doubles? */
/**
 * tp_asv_get_double:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 * @valid: Either %NULL, or a location in which to store %TRUE on success or
 *    %FALSE on failure
 *
 * If a value for @key in @asv is present and has any numeric type used by
 * dbus-glib (guchar, gint, guint, gint64, guint64 or gdouble),
 * return it as a double, and if @valid is not %NULL, set *@valid to %TRUE.
 *
 * Otherwise, return 0.0, and if @valid is not %NULL, set *@valid to %FALSE.
 *
 * Returns: the double precision floating-point value of @key, or 0.0
 * @since 0.7.9
 */
gdouble
tp_asv_get_double (const GHashTable *asv,
                   const gchar *key,
                   gboolean *valid)
{
  gdouble ret;
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL)
    goto return_invalid;

  switch (G_VALUE_TYPE (value))
    {
    case G_TYPE_DOUBLE:
      ret = g_value_get_double (value);
      break;

    case G_TYPE_UCHAR:
      ret = g_value_get_uchar (value);
      break;

    case G_TYPE_UINT:
      ret = g_value_get_uint (value);
      break;

    case G_TYPE_INT:
      ret = g_value_get_int (value);
      break;

    case G_TYPE_INT64:
      ret = g_value_get_int64 (value);
      break;

    case G_TYPE_UINT64:
      ret = g_value_get_uint64 (value);
      break;

    default:
      goto return_invalid;
    }

  if (valid != NULL)
    *valid = TRUE;

  return ret;

return_invalid:
  if (valid != NULL)
    *valid = FALSE;

  return 0;
}


/**
 * tp_asv_get_object_path:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 *
 * If a value for @key in @asv is present and is an object path, return it.
 *
 * Otherwise return %NULL.
 *
 * The returned value is not copied, and is only valid as long as the value
 * for @key in @asv is not removed or altered. Copy it with g_strdup() if you
 * need to keep it for longer.
 *
 * Returns: the object-path value of @key, or %NULL
 * @since 0.7.9
 */
const gchar *
tp_asv_get_object_path (const GHashTable *asv,
                        const gchar *key)
{
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL || !G_VALUE_HOLDS (value, DBUS_TYPE_G_OBJECT_PATH))
    return NULL;

  return g_value_get_boxed (value);
}


/**
 * tp_asv_get_boxed:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 * @type: The type that the key's value should have, which must be derived
 *  from %G_TYPE_BOXED
 *
 * If a value for @key in @asv is present and is of the desired type,
 * return it.
 *
 * Otherwise return %NULL.
 *
 * The returned value is not copied, and is only valid as long as the value
 * for @key in @asv is not removed or altered. Copy it, for instance with
 * g_boxed_copy(), if you need to keep it for longer.
 *
 * Returns: the value of @key, or %NULL
 * @since 0.7.9
 */
gpointer
tp_asv_get_boxed (const GHashTable *asv,
                  const gchar *key,
                  GType type)
{
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  g_return_val_if_fail (G_TYPE_FUNDAMENTAL (type) == G_TYPE_BOXED, NULL);

  if (value == NULL || !G_VALUE_HOLDS (value, type))
    return NULL;

  return g_value_get_boxed (value);
}


/**
 * tp_asv_get_strv:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 *
 * If a value for @key in @asv is present and is an array of strings (strv),
 * return it.
 *
 * Otherwise return %NULL.
 *
 * The returned value is not copied, and is only valid as long as the value
 * for @key in @asv is not removed or altered. Copy it with g_strdupv() if you
 * need to keep it for longer.
 *
 * Returns: the %NULL-terminated string-array value of @key, or %NULL
 * @since 0.7.9
 */
const gchar * const *
tp_asv_get_strv (const GHashTable *asv,
                 const gchar *key)
{
  GValue *value = g_hash_table_lookup ((GHashTable *) asv, key);

  if (value == NULL || !G_VALUE_HOLDS (value, G_TYPE_STRV))
    return NULL;

  return g_value_get_boxed (value);
}


/**
 * tp_asv_lookup:
 * @asv: A GHashTable where the keys are strings and the values are GValues
 * @key: The key to look up
 *
 * If a value for @key in @asv is present, return it. Otherwise return %NULL.
 *
 * The returned value is not copied, and is only valid as long as the value
 * for @key in @asv is not removed or altered. Copy it with (for instance)
 * g_value_copy() if you need to keep it for longer.
 *
 * Returns: the value of @key, or %NULL
 * @since 0.7.9
 */
const GValue *
tp_asv_lookup (const GHashTable *asv,
               const gchar *key)
{
  return g_hash_table_lookup ((GHashTable *) asv, key);
}
