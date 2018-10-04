/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2018 Red Hat, Inc.

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

#define _GNU_SOURCE
#include <string.h>
#include <json-glib/json-glib.h>
#include "spice-client.h"

/**
 * SECTION:qmp-port
 * @short_description: QMP port helper
 * @title: QMP port channel helper
 * @section_id:
 * @see_also: #SpicePortChannel
 * @stability: Stable
 * @include: spice-client.h
 *
 * A helper to handle QMP messages over a %SpicePortChannel.
 *
 * Since: 0.36
 */

typedef struct _SpiceQmpPortPrivate SpiceQmpPortPrivate;

struct _SpiceQmpPortPrivate
{
    SpicePortChannel *channel;
    gboolean ready;

    gint id_counter;
    GString *qmp_data;
    JsonParser *qmp_parser;
    GHashTable *qmp_tasks;
};

struct _SpiceQmpPort
{
    GObject parent;

    SpiceQmpPortPrivate *priv;
};

struct _SpiceQmpPortClass
{
    GObjectClass parent_class;
};

enum {
    PROP_CHANNEL = 1,
    PROP_READY,

    PROP_LAST,
};

enum {
    SIGNAL_EVENT,

    SIGNAL_LAST,
};

static guint signals[SIGNAL_LAST];
static GParamSpec *props[PROP_LAST] = { NULL, };

G_DEFINE_TYPE_WITH_PRIVATE(SpiceQmpPort, spice_qmp_port, G_TYPE_OBJECT)

G_DEFINE_BOXED_TYPE(SpiceQmpStatus, spice_qmp_status, spice_qmp_status_ref, spice_qmp_status_unref)

typedef void (QMPCb)(GTask *task, JsonNode *node);

static void
qmp_error_return(GTask *task, const gchar *desc)
{
    g_task_return_new_error(task, SPICE_CLIENT_ERROR,
                            SPICE_CLIENT_ERROR_FAILED, "%s", desc);
    g_object_unref(task);
}

static gboolean
spice_qmp_dispatch_message(SpiceQmpPort *self)
{
    JsonObject *obj = json_node_get_object(json_parser_get_root(self->priv->qmp_parser));
    JsonNode *node;
    GTask *task;
    const gchar *event;

    if (json_object_get_member(obj, "QMP")) {
        g_warn_if_fail(!self->priv->ready);
        SPICE_DEBUG("QMP greeting received");
        return TRUE;
    }

    if ((node = json_object_get_member(obj, "error"))) {
        gint id = json_object_get_int_member(obj, "id");
        const gchar *desc = json_object_get_string_member(obj, "desc");

        SPICE_DEBUG("QMP return error: %s, id:%d", desc, id);
        task = g_hash_table_lookup(self->priv->qmp_tasks, GINT_TO_POINTER(id));
        g_return_val_if_fail(task != NULL, TRUE);
        g_hash_table_steal(self->priv->qmp_tasks, GINT_TO_POINTER(id));
        qmp_error_return(task, desc);
    } else if ((node = json_object_get_member(obj, "return"))) {
        gint id = json_object_get_int_member(obj, "id");
        QMPCb *cb;

        SPICE_DEBUG("QMP return id:%d", id);
        if (!self->priv->ready && id == 0) {
            self->priv->ready = TRUE;
            g_object_notify(G_OBJECT(self), "ready");
        }

        g_warn_if_fail(self->priv->ready);
        task = g_hash_table_lookup(self->priv->qmp_tasks, GINT_TO_POINTER(id));
        g_return_val_if_fail(task != NULL, TRUE);
        cb = g_task_get_task_data(task);
        g_hash_table_steal(self->priv->qmp_tasks, GINT_TO_POINTER(id));
        cb(task, node);
    } else if ((event = json_object_get_string_member(obj, "event"))) {
        SPICE_DEBUG("QMP event %s", event);
        g_signal_emit(G_OBJECT(self), signals[SIGNAL_EVENT], 0, event,
                      json_object_get_member(obj, "data"));
    } else {
        return FALSE;
    }

    return TRUE;
}

#define QMP_MAX_RESPONSE (10 * 1024 * 1024)

static void
spice_qmp_handle_port_data(SpiceQmpPort *self, gpointer data,
                           int size G_GNUC_UNUSED,
                           SpicePortChannel *port G_GNUC_UNUSED)
{
    GString *qmp = self->priv->qmp_data;
    gchar *str, *crlf;

    g_string_append_len(qmp, data, size);
    if (qmp->len > QMP_MAX_RESPONSE) {
        g_warning("QMP response is too large, over %d bytes, truncating",
                  QMP_MAX_RESPONSE);
        g_string_set_size(qmp, 0);
        return;
    }

    str = qmp->str;
    while ((crlf = strstr(str, "\r\n")) != NULL) {
        GError *err = NULL;

        *crlf = '\0';
        json_parser_load_from_data(self->priv->qmp_parser, str, crlf - str, &err);
        if (err) {
            g_warning("JSON parsing error: %s", err->message);
            g_error_free(err);
        } else {
            if (!spice_qmp_dispatch_message(self))
                g_warning("Failed to dispatch: %s", str);
        }
        str = crlf + 2;
    }

    g_string_erase(qmp, 0, str - qmp->str);
}

static void qmp_task_disposed_cb(gpointer data)
{
    qmp_error_return(G_TASK(data), "Task got disposed");
}

static void spice_qmp_port_init(SpiceQmpPort *self)
{
     self->priv = spice_qmp_port_get_instance_private(self);
     self->priv->qmp_data = g_string_sized_new(256);
     self->priv->qmp_parser = json_parser_new();
     self->priv->qmp_tasks = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                   NULL, qmp_task_disposed_cb);
}

static void spice_qmp_port_dispose(GObject *gobject)
{
    SpiceQmpPort *self = SPICE_QMP_PORT(gobject);

    g_string_free(self->priv->qmp_data, TRUE);
    g_object_unref(self->priv->qmp_parser);
    g_hash_table_unref(self->priv->qmp_tasks);

    g_object_set_data(G_OBJECT(self->priv->channel),
                      "spice-qmp-port", NULL);

    g_clear_object(&self->priv->channel);

    if (G_OBJECT_CLASS(spice_qmp_port_parent_class)->dispose)
        G_OBJECT_CLASS(spice_qmp_port_parent_class)->dispose(gobject);
}

static void spice_qmp_handle_port_event(SpiceQmpPort *self, gint event)
{
    SPICE_DEBUG("QMP port event:%d", event);

    if (event == SPICE_PORT_EVENT_CLOSED) {
        g_hash_table_remove_all(self->priv->qmp_tasks);
    }
}

static void spice_qmp_port_constructed(GObject *gobject)
{
    SpiceQmpPort *self = SPICE_QMP_PORT(gobject);

    g_object_set_data(G_OBJECT(self->priv->channel),
                      "spice-qmp-port", self);

    spice_g_signal_connect_object(self->priv->channel,
                                  "port-data", G_CALLBACK(spice_qmp_handle_port_data),
                                  self, G_CONNECT_SWAPPED);

    spice_g_signal_connect_object(self->priv->channel,
                                  "port-event", G_CALLBACK(spice_qmp_handle_port_event),
                                  self, G_CONNECT_SWAPPED);

    if (G_OBJECT_CLASS(spice_qmp_port_parent_class)->constructed)
        G_OBJECT_CLASS(spice_qmp_port_parent_class)->constructed(gobject);
}

static void
spice_qmp_port_set_property(GObject *object,
                            guint property_id,
                            const GValue *value,
                            GParamSpec *pspec)
{
    SpiceQmpPort *self = SPICE_QMP_PORT(object);

    switch (property_id) {
    case PROP_CHANNEL:
        g_clear_object(&self->priv->channel);
        self->priv->channel = g_value_dup_object(value);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
spice_qmp_port_get_property(GObject *object,
                            guint property_id,
                            GValue *value,
                            GParamSpec *pspec)
{
    SpiceQmpPort *self = SPICE_QMP_PORT(object);

    switch (property_id) {
    case PROP_CHANNEL:
        g_value_set_object(value, self->priv->channel);
        break;

    case PROP_READY:
        g_value_set_boolean(value, self->priv->ready);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void spice_qmp_port_class_init(SpiceQmpPortClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    gobject_class->dispose = spice_qmp_port_dispose;
    gobject_class->get_property = spice_qmp_port_get_property;
    gobject_class->set_property = spice_qmp_port_set_property;
    gobject_class->constructed = spice_qmp_port_constructed;

    /**
     * SpiceQmpPort::event:
     * @self: the #SpiceQmpPort that emitted the signal
     * @name: the QMP event name
     * @node: the event data json-node, or NULL
     *
     * Event emitted whenever a QMP event is received.
     *
     * Since: 0.36
     */
    signals[SIGNAL_EVENT] =
        g_signal_new("event",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     NULL, NULL, NULL,
                     G_TYPE_NONE,
                     2, G_TYPE_STRING, G_TYPE_POINTER);

    props[PROP_CHANNEL] =
        g_param_spec_object("channel",
                            "Channel",
                            "Associated port channel",
                            SPICE_TYPE_PORT_CHANNEL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    props[PROP_READY] =
        g_param_spec_boolean("ready",
                             "Ready",
                             "Whether the QMP port is ready",
                             FALSE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(gobject_class, PROP_LAST, props);
 }

static void
qmp_empty_return_cb(GTask *task, G_GNUC_UNUSED JsonNode *node)
{
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
}

static void
spice_qmp_port_write_finished(GObject *source_object,
                              GAsyncResult *res,
                              gpointer t)
{
    SpicePortChannel *port = SPICE_PORT_CHANNEL(source_object);
    GTask *task = G_TASK(t);
    SpiceQmpPort *self = g_task_get_source_object(task);
    gint id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(task), "qmp-id"));
    GError *err = NULL;

    spice_port_channel_write_finish(port, res, &err);
    if (err) {
        g_hash_table_steal(self->priv->qmp_tasks, GINT_TO_POINTER(id));
        qmp_error_return(task, err->message);
        g_error_free(err);
    }
}

static void
qmp(SpiceQmpPort *self, GTask *task,
    const char *cmd, const gchar *args)
{
    GString *str = g_string_sized_new(256);
    gsize len;
    gchar *data;
    gint id = self->priv->id_counter;

    g_string_append_printf(str, "{ 'execute': '%s'", cmd);
    if (args)
        g_string_append_printf(str, ", 'arguments': { %s }", args);
    g_string_append_printf(str, ", 'id': %d", id);
    g_string_append(str, " }");

    g_hash_table_insert(self->priv->qmp_tasks, GINT_TO_POINTER(id), task);

    len = str->len;
    data = g_string_free(str, FALSE);
    spice_port_channel_write_async(self->priv->channel, data, len,
                                   g_task_get_cancellable(task),
                                   spice_qmp_port_write_finished, task);
    g_object_set_data_full(G_OBJECT(task), "qmp-data", data, g_free);
    g_object_set_data(G_OBJECT(task), "qmp-id", GINT_TO_POINTER(id));

    self->priv->id_counter++;
}

/**
 * spice_qmp_port_vm_action_finish:
 * @self: a qmp port helper
 * @result: The async #GAsyncResult result
 * @error: a #GError pointer, or %NULL
 *
 * Finishes asynchronous VM action and returns the result.
 *
 * Since: 0.36
 **/
gboolean spice_qmp_port_vm_action_finish(SpiceQmpPort *self,
                                         GAsyncResult *result,
                                         GError **error)
{
    g_return_val_if_fail(SPICE_IS_QMP_PORT(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

/**
 * spice_qmp_port_vm_action_async:
 * @self: a qmp port helper
 * @action: a VM action
 * @cancellable: a #GCancellable, or %NULL
 * @callback: callback to call when the action is complete
 * @user_data: the data to pass to the callback function
 *
 * Request the VM to perform an action.
 *
 * Since: 0.36
 **/
void spice_qmp_port_vm_action_async(SpiceQmpPort *self,
                                    SpiceQmpPortVmAction action,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
    GTask *task;
    const gchar *cmd;

    g_return_if_fail(SPICE_IS_QMP_PORT(self));
    g_return_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable));
    g_return_if_fail(self->priv->ready);
    g_return_if_fail(action >= 0 && action < SPICE_QMP_PORT_VM_ACTION_LAST);

    task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_task_data(task, qmp_empty_return_cb, NULL);

    switch (action) {
    case SPICE_QMP_PORT_VM_ACTION_QUIT:
        cmd = "quit";
        break;
    case SPICE_QMP_PORT_VM_ACTION_RESET:
        cmd = "system_reset";
        break;
    case SPICE_QMP_PORT_VM_ACTION_POWER_DOWN:
        cmd = "system_powerdown";
        break;
    case SPICE_QMP_PORT_VM_ACTION_PAUSE:
        cmd = "stop";
        break;
    case SPICE_QMP_PORT_VM_ACTION_CONTINUE:
        cmd = "cont";
        break;
    default:
        g_return_if_reached();
    }

    qmp(self, task, cmd, NULL);
}

static void
qmp_capabilities_cb(GTask *task, JsonNode *node)
{
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
}

/**
 * spice_qmp_port_get:
 * @channel: the QMP port channel
 *
 * Associate a QMP port helper to the given port channel.  If there is
 * already a helper associated with the channel, it is simply returned.
 *
 * Returns: (transfer none): a weak reference to the associated SpiceQmpPort
 *
 * Since: 0.36
 **/
SpiceQmpPort *spice_qmp_port_get(SpicePortChannel *channel)
{
    GObject *self;

    g_return_val_if_fail(SPICE_IS_PORT_CHANNEL(channel), NULL);

    self = g_object_get_data(G_OBJECT(channel), "spice-qmp-port");

    if (self == NULL) {
        GTask *task;

        self = g_object_new(SPICE_TYPE_QMP_PORT, "channel", channel, NULL);
        task = g_task_new(self, NULL, NULL, NULL);
        g_task_set_task_data(task, qmp_capabilities_cb, NULL);
        qmp(SPICE_QMP_PORT(self), task, "qmp_capabilities", NULL);
    }

    return SPICE_QMP_PORT(self);
}

/**
 * spice_qmp_status_ref:
 * @status: a #SpiceQmpStatus
 *
 * References a @status.
 *
 * Returns: The same @status
 *
 * Since: 0.36
 **/
SpiceQmpStatus *
spice_qmp_status_ref(SpiceQmpStatus *status)
{
    g_return_val_if_fail(status != NULL, NULL);

    status->ref++;

    return status;
}

/**
 * spice_qmp_status_unref:
 * @status: a #SpiceQmpStatus
 *
 * Removes a reference from the given @status.
 *
 * Since: 0.36
 **/
void spice_qmp_status_unref(SpiceQmpStatus *status)
{
    if (status && --status->ref == 0) {
        g_free(status->status);
        g_free(status);
    }
}

static void
qmp_query_status_return_cb(GTask *task, JsonNode *node)
{
    SpiceQmpStatus *status = g_new0(SpiceQmpStatus, 1);
    JsonObject *obj = json_node_get_object(node);

    status->version = 1;
    status->ref = 1;
    status->running = json_object_get_boolean_member(obj, "running");
    status->singlestep = json_object_get_boolean_member(obj, "singlestep");
    status->status = g_strdup(json_object_get_string_member(obj, "status"));

    g_task_return_pointer(task, status, (GDestroyNotify)spice_qmp_status_unref);
    g_object_unref(task);
}

/**
 * spice_qmp_port_query_status_async:
 * @self: A #SpiceQmpPort
 * @cancellable: A #GCancellable
 * @callback: The async callback.
 * @user_data: The async callback user data.
 *
 * Query the run status of all VCPUs.
 *
 * Since: 0.36
 **/
void spice_qmp_port_query_status_async(SpiceQmpPort *self,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    GTask *task;

    g_return_if_fail(SPICE_IS_QMP_PORT(self));
    g_return_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable));
    g_return_if_fail(self->priv->ready);

    task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_task_data(task, qmp_query_status_return_cb, NULL);

    qmp(self, task, "query-status", NULL);
}

/**
 * spice_qmp_port_query_status_finish:
 * @self: A #SpiceQmpPort
 * @result: The async #GAsyncResult result
 * @error: a #GError pointer, or %NULL
 *
 * Finish the asynchronous status query.
 *
 * Returns: The #SpiceQmpStatus result or %NULL, in which case @error
 * will be set.
 *
 * Since: 0.36
 **/
SpiceQmpStatus *
spice_qmp_port_query_status_finish(SpiceQmpPort *self,
                                   GAsyncResult *result,
                                   GError **error)
{
    g_return_val_if_fail(SPICE_IS_QMP_PORT(self), NULL);
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}
