/*
 * gabble-media-channel.h - Header for GabbleMediaChannel
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

#ifndef __GABBLE_MEDIA_CHANNEL_H__
#define __GABBLE_MEDIA_CHANNEL_H__

#include <glib-object.h>

#include "handles.h"

G_BEGIN_DECLS

typedef enum {
    JS_STATE_PENDING = 0,
    JS_STATE_ACTIVE = 1,
    JS_STATE_ENDED
} JingleSessionState;

typedef struct _JingleCandidate JingleCandidate;
typedef struct _JingleCodec JingleCodec;
typedef struct _JingleSession JingleSession;
typedef struct _GabbleMediaChannel GabbleMediaChannel;
typedef struct _GabbleMediaChannelClass GabbleMediaChannelClass;

struct _JingleCandidate {
    gchar *name;
    gchar *address;
    guint16 port;
    gchar *username;
    gchar *password;
    gfloat preference;
    gchar *protocol;
    gchar *type;
    guchar network;
    guchar generation;
};

struct _JingleCodec {
    guchar id;
    gchar *name;
};

struct _JingleSession {
    guint32 id;
    JingleSessionState state;
    
    GPtrArray *remote_candidates;
    GPtrArray *remote_codecs;
};

struct _GabbleMediaChannelClass {
    GObjectClass parent_class;
};

struct _GabbleMediaChannel {
    GObject parent;

    JingleSession session;
};

GType gabble_media_channel_get_type(void);

/* TYPE MACROS */
#define GABBLE_TYPE_MEDIA_CHANNEL \
  (gabble_media_channel_get_type())
#define GABBLE_MEDIA_CHANNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GABBLE_TYPE_MEDIA_CHANNEL, GabbleMediaChannel))
#define GABBLE_MEDIA_CHANNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GABBLE_TYPE_MEDIA_CHANNEL, GabbleMediaChannelClass))
#define GABBLE_IS_MEDIA_CHANNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GABBLE_TYPE_MEDIA_CHANNEL))
#define GABBLE_IS_MEDIA_CHANNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GABBLE_TYPE_MEDIA_CHANNEL))
#define GABBLE_MEDIA_CHANNEL_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), GABBLE_TYPE_MEDIA_CHANNEL, GabbleMediaChannelClass))


gboolean gabble_media_channel_close (GabbleMediaChannel *obj, GError **error);
gboolean gabble_media_channel_get_channel_type (GabbleMediaChannel *obj, gchar ** ret, GError **error);
gboolean gabble_media_channel_get_handle (GabbleMediaChannel *obj, guint* ret, guint* ret1, GError **error);
gboolean gabble_media_channel_get_interfaces (GabbleMediaChannel *obj, gchar *** ret, GError **error);
gboolean gabble_media_channel_get_session_handlers (GabbleMediaChannel *obj, GPtrArray ** ret, GError **error);

void gabble_media_channel_create_session_handler (GabbleMediaChannel *channel, GabbleHandle member);


JingleCandidate *jingle_candidate_new (const gchar *name,
                                       const gchar *address,
                                       guint16 port,
                                       const gchar *username,
                                       const gchar *password,
                                       gfloat preference,
                                       const gchar *protocol,
                                       const gchar *type,
                                       guchar network,
                                       guchar generation);
void jingle_candidate_free (JingleCandidate *candidate);


JingleCodec *jingle_codec_new (guchar id, const gchar *name);
void jingle_codec_free (JingleCodec *codec);

G_END_DECLS

#endif /* #ifndef __GABBLE_MEDIA_CHANNEL_H__*/
