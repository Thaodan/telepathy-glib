/*
 * TpTLSCertificate - a TpProxy for TLS certificates
 * Copyright © 2010 Collabora Ltd.
 *
 * Based on EmpathyTLSCertificate:
 * @author Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
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

#include <config.h>
#include "telepathy-glib/tls-certificate.h"

#include <glib/gstdio.h>

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/enums.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/proxy-internal.h>
#include <telepathy-glib/proxy-subclass.h>
#include <telepathy-glib/util.h>
#include <telepathy-glib/util-internal.h>

#define DEBUG_FLAG TP_DEBUG_TLS
#include "debug-internal.h"

/**
 * SECTION:tls-certificate
 * @title: TpTLSCertificate
 * @short_description: proxy object for a server or peer's TLS certificate
 *
 * #TpTLSCertificate is a #TpProxy subclass for TLSCertificate objects,
 * used in Channel.Type.ServerTLSConnection.
 *
 * Since: UNRELEASED
 */

/**
 * TpTLSCertificate:
 *
 * A #TpProxy subclass representing a server or peer's TLS certificate
 * being presented for acceptance/rejection.
 *
 * Since: UNRELEASED
 */

/**
 * TpTLSCertificateClass:
 *
 * The class of a #TpTLSCertificate.
 *
 * Since: UNRELEASED
 */

enum {
  /* proxy properties */
  PROP_CERT_TYPE = 1,
  PROP_CERT_DATA,
  PROP_STATE,
  PROP_PARENT,
  N_PROPS
};

struct _TpTLSCertificatePrivate {
  TpProxy *parent;

  /* TLSCertificate properties */
  gchar *cert_type;
  GPtrArray *cert_data;
  TpTLSCertificateState state;
  /* array of SignalledRejection received from the CM */
  GArray *rejections;
  /* GPtrArray of TP_STRUCT_TYPE_TLS_CERTIFICATE_REJECTION to send to CM */
  GPtrArray *pending_rejections;
};

G_DEFINE_TYPE (TpTLSCertificate, tp_tls_certificate,
    TP_TYPE_PROXY)

/**
 * TP_TLS_CERTIFICATE_FEATURE_CORE:
 *
 * Expands to a call to a function that returns a quark representing the
 * core functionality of a #TpTLSCertificate.
 *
 * When this feature is prepared, the basic properties of the
 * object have been retrieved and are available for use:
 *
 * <itemizedlist>
 * <listitem>#TpTLSCertificate:cert-type</listitem>
 * <listitem>#TpTLSCertificate:cert-data</listitem>
 * <listitem>#TpTLSCertificate:state</listitem>
 * </itemizedlist>
 *
 * In addition, #GObject::notify::state will be emitted if the state changes.
 *
 * One can ask for a feature to be prepared using the
 * tp_proxy_prepare_async() function, and waiting for it to callback.
 *
 * Since: UNRELEASED
 */

GQuark
tp_tls_certificate_get_feature_quark_core (void)
{
  return g_quark_from_static_string ("tp-tls-certificate-feature-core");
}

typedef struct {
    GError *error /* NULL-initialized later */ ;
    TpTLSCertificateRejectReason reason;
    gchar *dbus_error;
    GHashTable *details;
} SignalledRejection;

static void
tp_tls_certificate_clear_rejections (TpTLSCertificate *self)
{
  if (self->priv->rejections != NULL)
    {
      guint i;

      for (i = 0; i < self->priv->rejections->len; i++)
        {
          SignalledRejection *sr = &g_array_index (self->priv->rejections,
              SignalledRejection, i);

          g_clear_error (&sr->error);
          tp_clear_pointer (&sr->dbus_error, g_free);
          tp_clear_pointer (&sr->details, g_hash_table_unref);
        }
    }

  tp_clear_pointer (&self->priv->rejections, g_array_unref);
}

static void
tp_tls_certificate_accepted_cb (TpTLSCertificate *self,
    gpointer unused G_GNUC_UNUSED,
    GObject *unused_object G_GNUC_UNUSED)
{
  tp_tls_certificate_clear_rejections (self);
  self->priv->state = TP_TLS_CERTIFICATE_STATE_ACCEPTED;
  g_object_notify ((GObject *) self, "state");
}

static void
tp_tls_certificate_rejected_cb (TpTLSCertificate *self,
    const GPtrArray *rejections,
    gpointer unused G_GNUC_UNUSED,
    GObject *unused_object G_GNUC_UNUSED)
{
  self->priv->state = TP_TLS_CERTIFICATE_STATE_REJECTED;

  tp_tls_certificate_clear_rejections (self);

  if (rejections == NULL || rejections->len == 0)
    {
      SignalledRejection sr = {
            g_error_new_literal (TP_ERROR, TP_ERROR_CERT_INVALID,
                "Rejected, no reason given"),
            TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN,
            g_strdup (TP_ERROR_STR_CERT_INVALID),
            tp_asv_new (NULL, NULL) };

      self->priv->rejections = g_array_sized_new (FALSE, FALSE,
          sizeof (SignalledRejection), 1);
      g_array_append_val (self->priv->rejections, sr);
    }
  else
    {
      guint i;

      self->priv->rejections = g_array_sized_new (FALSE, FALSE,
          sizeof (SignalledRejection), rejections->len);

      for (i = 0; i < rejections->len; i++)
        {
          SignalledRejection sr = { NULL };
          GValueArray *va = g_ptr_array_index (rejections, i);
          const gchar *error_name;
          const GHashTable *details;

          tp_value_array_unpack (va, 3,
              &sr.reason,
              &error_name,
              &details);

          tp_proxy_dbus_error_to_gerror (self, error_name,
              tp_asv_get_string (details, "debug-message"), &sr.error);

          sr.details = g_hash_table_new_full (g_str_hash, g_str_equal,
              g_free, (GDestroyNotify) tp_g_value_slice_free);
          tp_g_hash_table_update (sr.details, (GHashTable *) details,
              (GBoxedCopyFunc) g_strdup,
              (GBoxedCopyFunc) tp_g_value_slice_dup);

          sr.dbus_error = g_strdup (error_name);

          g_array_append_val (self->priv->rejections, sr);
        }
    }

  g_object_notify ((GObject *) self, "state");
}

static void
tls_certificate_got_all_cb (TpProxy *proxy,
    GHashTable *properties,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  GPtrArray *cert_data;
  TpTLSCertificate *self = TP_TLS_CERTIFICATE (proxy);
  guint state;

  if (error != NULL)
    {
      tp_proxy_invalidate (proxy, error);
      return;
    }

  self->priv->cert_type = g_strdup (tp_asv_get_string (properties,
          "CertificateType"));

  state = tp_asv_get_uint32 (properties, "State", NULL);

  switch (state)
    {
      case TP_TLS_CERTIFICATE_STATE_PENDING:
        break;

      case TP_TLS_CERTIFICATE_STATE_ACCEPTED:
        tp_tls_certificate_accepted_cb (self, NULL, NULL);
        break;

      case TP_TLS_CERTIFICATE_STATE_REJECTED:
        tp_tls_certificate_rejected_cb (self,
            tp_asv_get_boxed (properties, "Rejections",
              TP_ARRAY_TYPE_TLS_CERTIFICATE_REJECTION_LIST),
            NULL, NULL);
        break;

      default:
        /* what does it mean? we just don't know */
        self->priv->state = state;
        g_object_notify ((GObject *) self, "state");
    }

  cert_data = tp_asv_get_boxed (properties, "CertificateChainData",
      TP_ARRAY_TYPE_UCHAR_ARRAY_LIST);
  g_assert (cert_data != NULL);
  self->priv->cert_data = g_boxed_copy (TP_ARRAY_TYPE_UCHAR_ARRAY_LIST,
      cert_data);

  DEBUG ("Got a certificate chain long %u, of type %s",
      self->priv->cert_data->len, self->priv->cert_type);

  _tp_proxy_set_feature_prepared (proxy, TP_TLS_CERTIFICATE_FEATURE_CORE,
      TRUE);
}

static void
parent_invalidated_cb (TpProxy *parent,
    guint domain,
    gint code,
    gchar *message,
    TpTLSCertificate *self)
{
  GError e = { domain, code, message };

  tp_clear_object (&self->priv->parent);

  tp_proxy_invalidate ((TpProxy *) self, &e);
  g_object_notify ((GObject *) self, "parent");
}

static void
tp_tls_certificate_constructed (GObject *object)
{
  TpTLSCertificate *self = TP_TLS_CERTIFICATE (object);
  void (*constructed) (GObject *) =
    G_OBJECT_CLASS (tp_tls_certificate_parent_class)->constructed;

  if (constructed != NULL)
    constructed (object);

  g_return_if_fail (TP_IS_CHANNEL (self->priv->parent) ||
      TP_IS_CONNECTION (self->priv->parent));

  if (self->priv->parent->invalidated != NULL)
    {
      GError *invalidated = self->priv->parent->invalidated;

      parent_invalidated_cb (self->priv->parent, invalidated->domain,
          invalidated->code, invalidated->message, self);
    }
  else
    {
      tp_g_signal_connect_object (self->priv->parent,
          "invalidated", G_CALLBACK (parent_invalidated_cb), self, 0);
    }

  tp_cli_authentication_tls_certificate_connect_to_accepted (self,
      tp_tls_certificate_accepted_cb, NULL, NULL, NULL, NULL);
  tp_cli_authentication_tls_certificate_connect_to_rejected (self,
      tp_tls_certificate_rejected_cb, NULL, NULL, NULL, NULL);

  tp_cli_dbus_properties_call_get_all (self,
      -1, TP_IFACE_AUTHENTICATION_TLS_CERTIFICATE,
      tls_certificate_got_all_cb, NULL, NULL, NULL);
}

static void
tp_tls_certificate_finalize (GObject *object)
{
  TpTLSCertificate *self = TP_TLS_CERTIFICATE (object);
  TpTLSCertificatePrivate *priv = self->priv;

  DEBUG ("%p", object);

  tp_tls_certificate_clear_rejections (self);
  g_free (priv->cert_type);
  tp_clear_boxed (TP_ARRAY_TYPE_UCHAR_ARRAY_LIST, &priv->cert_data);
  tp_clear_boxed (TP_ARRAY_TYPE_TLS_CERTIFICATE_REJECTION_LIST,
      &self->priv->pending_rejections);

  G_OBJECT_CLASS (tp_tls_certificate_parent_class)->finalize (object);
}

static void
tp_tls_certificate_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  TpTLSCertificate *self = TP_TLS_CERTIFICATE (object);
  TpTLSCertificatePrivate *priv = self->priv;

  switch (property_id)
    {
    case PROP_CERT_TYPE:
      g_value_set_string (value, priv->cert_type);
      break;

    case PROP_CERT_DATA:
      g_value_set_boxed (value, priv->cert_data);
      break;

    case PROP_STATE:
      g_value_set_uint (value, priv->state);
      break;

    case PROP_PARENT:
      g_value_set_object (value, priv->parent);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
tp_tls_certificate_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  TpTLSCertificate *self = TP_TLS_CERTIFICATE (object);

  switch (property_id)
    {
    case PROP_PARENT:
      self->priv->parent = TP_PROXY (g_value_dup_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
tp_tls_certificate_init (TpTLSCertificate *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TP_TYPE_TLS_CERTIFICATE, TpTLSCertificatePrivate);
}

enum {
    FEAT_CORE,
    N_FEAT
};

static const TpProxyFeature *
tp_tls_certificate_list_features (TpProxyClass *cls G_GNUC_UNUSED)
{
  static TpProxyFeature features[N_FEAT + 1] = { { 0 } };

  if (G_LIKELY (features[0].name != 0))
    return features;

  features[FEAT_CORE].name = TP_TLS_CERTIFICATE_FEATURE_CORE;
  features[FEAT_CORE].core = TRUE;

  g_assert (features[N_FEAT].name == 0);
  return features;
}

static void
tp_tls_certificate_class_init (TpTLSCertificateClass *klass)
{
  GParamSpec *pspec;
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  TpProxyClass *pclass = TP_PROXY_CLASS (klass);

  tp_tls_certificate_init_known_interfaces ();

  oclass->get_property = tp_tls_certificate_get_property;
  oclass->set_property = tp_tls_certificate_set_property;
  oclass->constructed = tp_tls_certificate_constructed;
  oclass->finalize = tp_tls_certificate_finalize;

  pclass->interface = TP_IFACE_QUARK_AUTHENTICATION_TLS_CERTIFICATE;
  pclass->must_have_unique_name = TRUE;
  pclass->list_features = tp_tls_certificate_list_features;

  g_type_class_add_private (klass, sizeof (TpTLSCertificatePrivate));

  /**
   * TpTLSCertificate:cert-type:
   *
   * The type of the certificate, typically either "x509" or "pgp".
   *
   * Since: UNRELEASED
   */
  pspec = g_param_spec_string ("cert-type", "Certificate type",
      "The type of this certificate.",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_CERT_TYPE, pspec);

  /**
   * TpTLSCertificate:cert-data:
   *
   * The raw data of the certificate or certificate chain, represented
   * as a #GPtrArray of #GArray of #guchar. It should be interpreted
   * according to #TpTLSCertificate:cert-type.
   *
   * The first certificate in this array is the server's certificate,
   * followed by its issuer, followed by the issuer's issuer and so on.
   *
   * For "x509" certificates, each certificate is an X.509 certificate in
   * binary (DER) format.
   *
   * For "pgp" certificates, each certificate is a binary OpenPGP key.
   *
   * Since: UNRELEASED
   */
  pspec = g_param_spec_boxed ("cert-data", "Certificate chain data",
      "The raw DER-encoded certificate chain data.",
      TP_ARRAY_TYPE_UCHAR_ARRAY_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_CERT_DATA, pspec);

  /**
   * TpTLSCertificate:state:
   *
   * The state of this TLS certificate as a #TpTLSCertificateState,
   * initially %TP_TLS_CERTIFICATE_STATE_PENDING.
   *
   * #GObject::notify::state will be emitted when this changes.
   *
   * Since: UNRELEASED
   */
  pspec = g_param_spec_uint ("state", "State",
      "The state of this certificate.",
      0, G_MAXUINT32, TP_TLS_CERTIFICATE_STATE_PENDING,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_STATE, pspec);

  /**
   * TpTLSCertificate:parent:
   *
   * A #TpConnection or #TpChannel which owns this TLS certificate. If the
   * parent object is invalidated, the certificate is also invalidated, and
   * this property is set to %NULL.
   *
   * Since: UNRELEASED
   */
  pspec = g_param_spec_object ("parent", "Parent",
      "The TpConnection or TpChannel to which this belongs", TP_TYPE_PROXY,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_PARENT, pspec);
}

static void
cert_proxy_accept_cb (TpTLSCertificate *self,
    const GError *error,
    gpointer user_data,
    GObject *unused_object G_GNUC_UNUSED)
{
  GSimpleAsyncResult *accept_result = user_data;

  DEBUG ("Callback for accept(), error %p", error);

  if (error != NULL)
    {
      DEBUG ("Error was %s", error->message);
      g_simple_async_result_set_from_error (accept_result, error);
    }

  g_simple_async_result_complete (accept_result);
}

static void
cert_proxy_reject_cb (TpTLSCertificate *self,
    const GError *error,
    gpointer user_data,
    GObject *unused_object G_GNUC_UNUSED)
{
  GSimpleAsyncResult *reject_result = user_data;

  DEBUG ("Callback for reject(), error %p", error);

  if (error != NULL)
    {
      DEBUG ("Error was %s", error->message);
      g_simple_async_result_set_from_error (reject_result, error);
    }

  g_simple_async_result_complete (reject_result);
}

static const gchar *
reject_reason_get_dbus_error (TpTLSCertificateRejectReason reason)
{
  const gchar *retval = NULL;

  switch (reason)
    {
#define EASY_CASE(x) \
    case TP_TLS_CERTIFICATE_REJECT_REASON_ ## x: \
      retval = tp_error_get_dbus_name (TP_ERROR_CERT_ ## x); \
      break
    EASY_CASE (UNTRUSTED);
    EASY_CASE (EXPIRED);
    EASY_CASE (NOT_ACTIVATED);
    EASY_CASE (FINGERPRINT_MISMATCH);
    EASY_CASE (HOSTNAME_MISMATCH);
    EASY_CASE (SELF_SIGNED);
    EASY_CASE (REVOKED);
    EASY_CASE (INSECURE);
    EASY_CASE (LIMIT_EXCEEDED);
#undef EASY_CASE

    case TP_TLS_CERTIFICATE_REJECT_REASON_UNKNOWN:
    default:
      retval = tp_error_get_dbus_name (TP_ERROR_CERT_INVALID);
      break;
    }

  return retval;
}

/**
 * tp_tls_certificate_new:
 * @conn_or_chan: a #TpConnection or #TpChannel parent for this object, whose
 *  invalidation will also result in invalidation of the returned object
 * @object_path: the object path of this TLS certificate
 * @error: a #GError used to return an error if %NULL is returned, or %NULL
 *
 * <!-- -->
 *
 * Returns: (transfer full): a new TLS certificate proxy. Prepare the
 *  feature %TP_TLS_CERTIFICATE_FEATURE_CORE to make it useful.
 * Since: UNRELEASED
 */
TpTLSCertificate *
tp_tls_certificate_new (TpProxy *conn_or_chan,
    const gchar *object_path,
    GError **error)
{
  TpTLSCertificate *retval = NULL;

  g_return_val_if_fail (TP_IS_CONNECTION (conn_or_chan) ||
      TP_IS_CHANNEL (conn_or_chan), NULL);

  if (!tp_dbus_check_valid_object_path (object_path, error))
    goto finally;

  retval = g_object_new (TP_TYPE_TLS_CERTIFICATE,
      "parent", conn_or_chan,
      "dbus-daemon", conn_or_chan->dbus_daemon,
      "bus-name", conn_or_chan->bus_name,
      "object-path", object_path,
      NULL);

finally:
  return retval;
}

/**
 * tp_tls_certificate_accept_async:
 * @self: a TLS certificate
 * @callback: called on success or failure
 * @user_data: user data for the callback
 *
 * Accept this certificate, asynchronously. In or after @callback,
 * you may call tp_tls_certificate_accept_finish() to check the result.
 *
 * #GObject::notify::state will also be emitted when the connection manager
 * signals that the certificate has been accepted.
 * Since: UNRELEASED
 */
void
tp_tls_certificate_accept_async (TpTLSCertificate *self,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GSimpleAsyncResult *accept_result;

  g_assert (TP_IS_TLS_CERTIFICATE (self));

  DEBUG ("Accepting TLS certificate");

  accept_result = g_simple_async_result_new (G_OBJECT (self),
      callback, user_data, tp_tls_certificate_accept_async);

  tp_cli_authentication_tls_certificate_call_accept (self,
      -1, cert_proxy_accept_cb,
      accept_result, g_object_unref, NULL);
}

/**
 * tp_tls_certificate_accept_finish:
 * @self: a TLS certificate
 * @result: the result passed to the callback by
 *  tp_tls_certificate_accept_async()
 * @error: used to raise an error if %FALSE is returned
 *
 * Check the result of tp_tls_certificate_accept_async().
 *
 * Returns: %TRUE if acceptance was successful
 * Since: UNRELEASED
 */
gboolean
tp_tls_certificate_accept_finish (TpTLSCertificate *self,
    GAsyncResult *result,
    GError **error)
{
  _tp_implement_finish_void (self, tp_tls_certificate_accept_async)
}

/**
 * tp_tls_certificate_add_rejection:
 * @self: a TLS certificate
 * @reason: the reason for rejection
 * @dbus_error: a D-Bus error name such as %TP_ERROR_STR_CERT_REVOKED, or
 *  %NULL to derive one from @reason
 * @details: (transfer none) (element-type utf8 GObject.Value): details of the
 *  rejection
 *
 * Add a pending reason for rejection. The first call to this method is
 * considered "most important". After calling this method as many times
 * as are required, call tp_tls_certificate_reject_async() to reject the
 * certif
 * Since: UNRELEASED
ate.
 */
void
tp_tls_certificate_add_rejection (TpTLSCertificate *self,
    TpTLSCertificateRejectReason reason,
    const gchar *dbus_error,
    GHashTable *details)
{
  GValueArray *rejection;

  g_return_if_fail (dbus_error == NULL ||
      tp_dbus_check_valid_interface_name (dbus_error, NULL));

  if (self->priv->pending_rejections == NULL)
    self->priv->pending_rejections = g_ptr_array_new ();

  if (dbus_error == NULL)
    dbus_error = reject_reason_get_dbus_error (reason);

  rejection = tp_value_array_build (3,
      G_TYPE_UINT, reason,
      G_TYPE_STRING, dbus_error,
      TP_HASH_TYPE_STRING_VARIANT_MAP, details,
      NULL);

  g_ptr_array_add (self->priv->pending_rejections, rejection);
}

/**
 * tp_tls_certificate_reject_async:
 * @self: a TLS certificate
 * @callback: called on success or failure
 * @user_data: user data for the callback
 *
 * Reject this certificate, asynchronously.
 *
 * Before calling this method, you must call
 * tp_tls_certificate_add_rejection() at least once, to set the reason(s)
 * for rejection (for instance, a certificate might be both self-signed and
 * expired).
 *
 * In or after @callback,
 * you may call tp_tls_certificate_reject_finish() to check the result.
 *
 * #GObject::notify::state will also be emitted when the connection manager
 * signals that the certificate has been rejected.
 * Since: UNRELEASED
 */
void
tp_tls_certificate_reject_async (TpTLSCertificate *self,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  GSimpleAsyncResult *reject_result;

  g_return_if_fail (TP_IS_TLS_CERTIFICATE (self));
  g_return_if_fail (self->priv->pending_rejections != NULL);
  g_return_if_fail (self->priv->pending_rejections->len >= 1);

  reject_result = g_simple_async_result_new (G_OBJECT (self),
      callback, user_data, tp_tls_certificate_reject_async);

  tp_cli_authentication_tls_certificate_call_reject (self,
      -1, self->priv->pending_rejections, cert_proxy_reject_cb,
      reject_result, g_object_unref, NULL);

  tp_clear_boxed (TP_ARRAY_TYPE_TLS_CERTIFICATE_REJECTION_LIST,
      &self->priv->pending_rejections);
}

/**
 * tp_tls_certificate_reject_finish:
 * @self: a TLS certificate
 * @result: the result passed to the callback by
 *  tp_tls_certificate_reject_async()
 * @error: used to raise an error if %FALSE is returned
 *
 * Check the result of tp_tls_certificate_reject_async().
 *
 * Returns: %TRUE if rejection was successful
 * Since: UNRELEASED
 */
gboolean
tp_tls_certificate_reject_finish (TpTLSCertificate *self,
    GAsyncResult *result,
    GError **error)
{
  _tp_implement_finish_void (self, tp_tls_certificate_reject_async)
}

#include <telepathy-glib/_gen/tp-cli-tls-cert-body.h>

/**
 * tp_tls_certificate_init_known_interfaces:
 *
 * Ensure that the known interfaces for TpTLSCertificate have been set up.
 * This is done automatically when necessary, but for correct
 * overriding of library interfaces by local extensions, you should
 * call this function before calling
 * tp_proxy_or_subclass_hook_on_interface_add() with first argument
 * %TP_TYPE_TLS_CERTIFICATE.
 *
 * Since: UNRELEASED
 */
void
tp_tls_certificate_init_known_interfaces (void)
{
  static gsize once = 0;

  if (g_once_init_enter (&once))
    {
      GType tp_type = TP_TYPE_TLS_CERTIFICATE;

      tp_proxy_init_known_interfaces ();
      tp_proxy_or_subclass_hook_on_interface_add (tp_type,
          tp_cli_tls_cert_add_signals);
      tp_proxy_subclass_add_error_mapping (tp_type,
          TP_ERROR_PREFIX, TP_ERRORS, TP_TYPE_ERROR);

      g_once_init_leave (&once, 1);
    }
}

/**
 * tp_tls_certificate_get_rejection:
 * @self: a TLS certificate
 * @reason: (out) (allow-none): optionally used to return the reason code
 * @dbus_error: (out) (type utf8) (allow-none) (transfer none): optionally
 *  used to return the D-Bus error name
 * @details: (out) (allow-none) (transfer none) (element-type utf8 GObject.Value):
 *  optionally used to return a map from string to #GValue, which must not be
 *  modified or destroyed by the caller
 *
 * If this certificate has been rejected, return a #GError (likely to be in
 * the %TP_ERROR domain) indicating the first rejection reason (by convention,
 * the most important).
 *
 * If you want to list all the things that are wrong with the certificate
 * (for instance, it might be self-signed and also have expired)
 * you can call tp_tls_certificate_get_nth_rejection(), increasing @n until
 * it returns %NULL.
 *
 * Returns: (transfer none) (allow-none): a #GError, or %NULL
 * Since: UNRELEASED
 */
const GError *
tp_tls_certificate_get_rejection (TpTLSCertificate *self,
    TpTLSCertificateRejectReason *reason,
    const gchar **dbus_error,
    const GHashTable **details)
{
  return tp_tls_certificate_get_nth_rejection (self, 0, reason, dbus_error,
      details);
}

/**
 * tp_tls_certificate_get_nth_rejection:
 * @self: a TLS certificate
 * @n: the rejection reason to return; if 0, return the same thing as
 *  tp_tls_certificate_get_detailed_rejection()
 * @reason: (out) (allow-none): optionally used to return the reason code
 * @dbus_error: (out) (type utf8) (allow-none) (transfer none): optionally
 *  used to return the D-Bus error name
 * @details: (out) (allow-none) (transfer none) (element-type utf8 GObject.Value):
 *  optionally used to return a map from string to #GValue, which must not be
 *  modified or destroyed by the caller
 *
 * If this certificate has been rejected and @n is less than the number of
 * rejection reasons, return a #GError representing the @n<!---->th rejection
 * reason (starting from 0), with additional information returned via the
 * 'out' parameters.
 *
 * With @n == 0 this is equivalent to tp_tls_certificate_get_rejection().
 *
 * Returns: (transfer none) (allow-none): a #GError, or %NULL
 * Since: UNRELEASED
 */
const GError *
tp_tls_certificate_get_nth_rejection (TpTLSCertificate *self,
    guint n,
    TpTLSCertificateRejectReason *reason,
    const gchar **dbus_error,
    const GHashTable **details)
{
  const SignalledRejection *rej;

  if (self->priv->rejections == NULL || n >= self->priv->rejections->len)
    return NULL;

  rej = &g_array_index (self->priv->rejections, SignalledRejection, n);

  if (reason != NULL)
    *reason = rej->reason;

  if (dbus_error != NULL)
    *dbus_error = rej->dbus_error;

  if (details != NULL)
    *details = rej->details;

  return rej->error;
}

/**
 * tp_tls_certificate_get_cert_type:
 * @self: a #TpTLSCertificate
 *
 * Return the #TpTLSCertificate:cert-type property
 *
 * Returns: the value of #TpTLSCertificate:cert-type property
 *
 * Since: UNRELEASED
 */
const gchar *
tp_tls_certificate_get_cert_type (TpTLSCertificate *self)
{
  return self->priv->cert_type;
}

/**
 * tp_tls_certificate_get_cert_data:
 * @self: a #TpTLSCertificate
 *
 * Return the #TpTLSCertificate:cert-data property
 *
 * Returns: (transfer none) (type GLib.PtrArray) (element-type GLib.Array): the value of #TpTLSCertificate:cert-data property
 *
 * Since: UNRELEASED
 */
GPtrArray *
tp_tls_certificate_get_cert_data (TpTLSCertificate *self)
{
  return self->priv->cert_data;
}

/**
 * tp_tls_certificate_get_state:
 * @self: a #TpTLSCertificate
 *
 * Return the #TpTLSCertificate:state property
 *
 * Returns: the value of #TpTLSCertificate:state property
 *
 * Since: UNRELEASED
 */
TpTLSCertificateState
tp_tls_certificate_get_state (TpTLSCertificate *self)
{
  return self->priv->state;
}
