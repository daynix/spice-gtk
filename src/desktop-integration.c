/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2012 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <glib-object.h>

#include "spice-session-priv.h"
#include "desktop-integration.h"

#include <glib/gi18n-lib.h>

#define GNOME_SESSION_INHIBIT_AUTOMOUNT 16

#if defined(G_OS_UNIX) && !__APPLE__
# define WITH_GNOME
#endif

struct _SpiceDesktopIntegrationPrivate {
#ifdef WITH_GNOME
    GDBusProxy *gnome_session_proxy;
    guint gnome_automount_inhibit_cookie;
#else
    /* private structures cannot be empty in GLib */
    int dummy;
#endif
};

G_DEFINE_TYPE_WITH_PRIVATE(SpiceDesktopIntegration, spice_desktop_integration, G_TYPE_OBJECT)

/* ------------------------------------------------------------------ */
/* Gnome specific code                                                */
#ifdef WITH_GNOME
static void handle_dbus_call_error(const char *call, GError **_error)
{
    GError *error = *_error;
    const char *message = error->message;

    g_warning("Error calling '%s': %s", call, message);
    g_clear_error(_error);
}

static gboolean gnome_integration_init(SpiceDesktopIntegration *self)
{
    SpiceDesktopIntegrationPrivate *priv = self->priv;
    GError *error = NULL;
    gboolean success = TRUE;
    gchar *name_owner = NULL;

    priv->gnome_session_proxy =
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                      G_DBUS_PROXY_FLAGS_NONE,
                                      NULL,
                                      "org.gnome.SessionManager",
                                      "/org/gnome/SessionManager",
                                      "org.gnome.SessionManager",
                                      NULL,
                                      &error);
    if (!error &&
        (name_owner = g_dbus_proxy_get_name_owner(priv->gnome_session_proxy)) == NULL) {
        g_clear_object(&priv->gnome_session_proxy);
        success = FALSE;
    }
    g_free(name_owner);

    if (error) {
        g_warning("Could not create org.gnome.SessionManager dbus proxy: %s",
                  error->message);
        g_clear_error(&error);
        return FALSE;
    }

    return success;
}

static void gnome_integration_inhibit_automount(SpiceDesktopIntegration *self)
{
    SpiceDesktopIntegrationPrivate *priv = self->priv;
    GError *error = NULL;
    const gchar *reason =
        _("Automounting has been inhibited for USB auto-redirecting");

    if (!priv->gnome_session_proxy)
        return;

    g_return_if_fail(priv->gnome_automount_inhibit_cookie == 0);

    GVariant *v = g_dbus_proxy_call_sync(priv->gnome_session_proxy,
                "Inhibit",
                g_variant_new("(susu)",
                              g_get_prgname(),
                              0,
                              reason,
                              GNOME_SESSION_INHIBIT_AUTOMOUNT),
                G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (v)
        g_variant_get(v, "(u)", &priv->gnome_automount_inhibit_cookie);

    g_clear_pointer(&v, g_variant_unref);

    if (error)
        handle_dbus_call_error("org.gnome.SessionManager.Inhibit", &error);
}

static void gnome_integration_uninhibit_automount(SpiceDesktopIntegration *self)
{
    SpiceDesktopIntegrationPrivate *priv = self->priv;
    GError *error = NULL;

    if (!priv->gnome_session_proxy)
        return;

    /* Cookie is 0 when we failed to inhibit (and when called from dispose) */
    if (priv->gnome_automount_inhibit_cookie == 0)
        return;

    GVariant *v = g_dbus_proxy_call_sync(priv->gnome_session_proxy,
                "Uninhibit",
                g_variant_new("(u)",
                              priv->gnome_automount_inhibit_cookie),
                G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_clear_pointer(&v, g_variant_unref);
    if (error)
        handle_dbus_call_error("org.gnome.SessionManager.Uninhibit", &error);

    priv->gnome_automount_inhibit_cookie = 0;
}

static void gnome_integration_dispose(SpiceDesktopIntegration *self)
{
    SpiceDesktopIntegrationPrivate *priv = self->priv;

    g_clear_object(&priv->gnome_session_proxy);
}
#endif /* WITH_GNOME */

static void spice_desktop_integration_init(SpiceDesktopIntegration *self)
{
    SpiceDesktopIntegrationPrivate *priv;

    priv = spice_desktop_integration_get_instance_private(self);
    self->priv = priv;

#ifdef WITH_GNOME
    if (gnome_integration_init(self)) {
        return;
    }
#endif

    g_warning("Warning no automount-inhibiting implementation available");
}

static void spice_desktop_integration_dispose(GObject *gobject)
{
#ifdef WITH_GNOME
    SpiceDesktopIntegration *self = SPICE_DESKTOP_INTEGRATION(gobject);

    gnome_integration_dispose(self);
#endif

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_desktop_integration_parent_class)->dispose)
        G_OBJECT_CLASS(spice_desktop_integration_parent_class)->dispose(gobject);
}

static void spice_desktop_integration_class_init(SpiceDesktopIntegrationClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    gobject_class->dispose      = spice_desktop_integration_dispose;
}

SpiceDesktopIntegration *spice_desktop_integration_get(SpiceSession *session)
{
    SpiceDesktopIntegration *self;
    static GMutex mutex;

    g_return_val_if_fail(session != NULL, NULL);

    g_mutex_lock(&mutex);
    self = g_object_get_data(G_OBJECT(session), "spice-desktop");
    if (self == NULL) {
        self = g_object_new(SPICE_TYPE_DESKTOP_INTEGRATION, NULL);
        g_object_set_data_full(G_OBJECT(session), "spice-desktop", self, g_object_unref);
    }
    g_mutex_unlock(&mutex);

    return self;
}

void spice_desktop_integration_inhibit_automount(SpiceDesktopIntegration *self)
{
#ifdef WITH_GNOME
    gnome_integration_inhibit_automount(self);
#endif
}

void spice_desktop_integration_uninhibit_automount(SpiceDesktopIntegration *self)
{
#ifdef WITH_GNOME
    gnome_integration_uninhibit_automount(self);
#endif
}
