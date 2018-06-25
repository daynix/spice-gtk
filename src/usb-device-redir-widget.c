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
#ifndef USB_WIDGET_TEST
    #include <glib/gi18n-lib.h>
    #include "spice-client.h"
    #include "spice-marshal.h"
#else
    #include "spice-client.h"
#endif
#include "usb-device-widget.h"

/**
 * SECTION:usb-device-widget
 * @short_description: USB device selection widget
 * @title: Spice USB device selection widget
 * @section_id:
 * @see_also:
 * @stability: Stable
 * @include: spice-client-gtk.h
 *
 * #SpiceUsbDeviceWidget is a gtk widget which apps can use to easily
 * add an UI to select USB devices to redirect (or unredirect).
 */

struct _SpiceUsbDeviceWidget
{
    GtkBox parent;

    SpiceUsbDeviceWidgetPrivate *priv;
};

struct _SpiceUsbDeviceWidgetClass
{
    GtkBoxClass parent_class;

    /* signals */
    void (*connect_failed) (SpiceUsbDeviceWidget *widget,
                            SpiceUsbDevice *device, GError *error);
};

/* ------------------------------------------------------------------ */
/* Prototypes for callbacks  */
static void device_added_cb(SpiceUsbDeviceManager *manager,
    SpiceUsbDevice *device, gpointer user_data);
static void device_removed_cb(SpiceUsbDeviceManager *manager,
    SpiceUsbDevice *device, gpointer user_data);
static void device_changed_cb(SpiceUsbDeviceManager *manager,
    SpiceUsbDevice *device, gpointer user_data);
static void device_error_cb(SpiceUsbDeviceManager *manager,
    SpiceUsbDevice *device, GError *err, gpointer user_data);
static gboolean spice_usb_device_widget_update_status(gpointer user_data);

/* ------------------------------------------------------------------ */
/* gobject glue                                                       */

#define SPICE_USB_DEVICE_WIDGET_GET_PRIVATE(obj) \
    (G_TYPE_INSTANCE_GET_PRIVATE((obj), SPICE_TYPE_USB_DEVICE_WIDGET, \
                                 SpiceUsbDeviceWidgetPrivate))

enum {
    PROP_0,
    PROP_SESSION,
    PROP_DEVICE_FORMAT_STRING,
};

enum {
    CONNECT_FAILED,
    LAST_SIGNAL,
};

struct _SpiceUsbDeviceWidgetPrivate {
    SpiceSession *session;
    gchar *device_format_string;
    SpiceUsbDeviceManager *manager;
    GtkWidget *info_bar;
    GtkWidget *label;
    GtkTreeView *tree_view;
    GtkTreeStore *tree_store;
    GdkPixbuf *icon_cd;
    GdkPixbuf *icon_connected;
    GdkPixbuf *icon_disconn;
    GdkPixbuf *icon_warning;
    GdkPixbuf *icon_info;
    gchar *err_msg;
    gsize device_count;
};

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE(SpiceUsbDeviceWidget, spice_usb_device_widget, GTK_TYPE_BOX);

/* TREE */

enum column_id
{
    COL_REDIRECT = 0,
    COL_ADDRESS,
    COL_CONNECT_ICON,
    COL_CD_ICON,
    COL_VENDOR,
    COL_PRODUCT,
    COL_REVISION,
    COL_ALIAS,
    COL_STARTED,
    COL_LOADED,
    COL_LOCKED,
    COL_FILE,
    /* internal columns */
    COL_CD_DEV,
    COL_LUN_ITEM,
    COL_DEV_ITEM,
    COL_ITEM_DATA,
    COL_CONNECTED,
    COL_CAN_REDIRECT,
    COL_ROW_COLOR,
    COL_ROW_COLOR_SET,
    NUM_COLS,

    INVALID_COL
};

static const char *col_name[NUM_COLS] =
{
    "Redirect",
    "Address",
    "Conn",
    "CD",
    "Vendor",
    "Product", 
    "Revision",
    "Alias",
    "Started",
    "Loaded",
    "Locked",
    "File/Device Path",
    /* internal columns - should not be displayed */
    "?CD_DEV",
    "?LUN_ITEM",
    "?DEV_ITEM",
    "?ITEM_DATA",
    "?CONNECTED",
    "?CAN_REDIRECT",
    "?ROW_COLOR",
    "?ROW_COLOR_SET"
};

typedef struct _usb_widget_lun_item {
    SpiceUsbDeviceManager *manager;
    SpiceUsbDevice *device;
    guint lun;
    spice_usb_device_lun_info info;
} usb_widget_lun_item;

typedef struct _tree_find_usb_dev {
    SpiceUsbDevice *usb_dev;
    GtkTreeIter *dev_iter;
} tree_find_usb_dev;

typedef void (*tree_item_toggled_cb)(GtkCellRendererToggle *, gchar *, gpointer);

static GtkTreePath *usb_widget_add_device(SpiceUsbDeviceWidget *self,
                                          SpiceUsbDevice *usb_device,
                                          GtkTreeIter *old_dev_iter);

static GtkTreeStore* usb_widget_create_tree_store(void)
{
    GtkTreeStore *tree_store;

    tree_store = gtk_tree_store_new(NUM_COLS,
                        G_TYPE_BOOLEAN, /* COL_REDIRECT */
                        G_TYPE_STRING, /* COL_ADDRESS */
                        GDK_TYPE_PIXBUF, /* COL_CONNECT_ICON */
                        GDK_TYPE_PIXBUF, /* COL_CD_ICON */
                        G_TYPE_STRING, /* COL_VENDOR */
                        G_TYPE_STRING, /* COL_PRODUCT */
                        G_TYPE_STRING, /* COL_ADDR_REV */
                        G_TYPE_STRING, /* COL_ALIAS */
                        G_TYPE_BOOLEAN, /* COL_STARTED */
                        G_TYPE_BOOLEAN, /* COL_LOADED */
                        G_TYPE_BOOLEAN, /* COL_LOCKED */
                        G_TYPE_STRING, /* COL_FILE */
                        G_TYPE_BOOLEAN, /* COL_CD_DEV */
                        G_TYPE_BOOLEAN, /* COL_LUN_ITEM */
                        G_TYPE_BOOLEAN, /* COL_DEV_ITEM */
                        G_TYPE_POINTER, /* COL_ITEM_DATA */
                        G_TYPE_BOOLEAN, /* COL_CONNECTED */
                        G_TYPE_BOOLEAN, /* COL_CAN_REDIRECT */
                        G_TYPE_STRING, /* COL_ROW_COLOR */
                        G_TYPE_BOOLEAN /* COL_ROW_COLOR_SET */ );
    g_print("tree store created\n");

    return tree_store;
}

static GdkPixbuf *get_named_icon(const gchar *name, gint size)
{
    GtkIconInfo *info = gtk_icon_theme_lookup_icon(gtk_icon_theme_get_default(), name, size, 0);
    GdkPixbuf *pixbuf = gtk_icon_info_load_icon(info, NULL);
    g_object_unref (info);
    return pixbuf;
}

static GtkTreePath *usb_widget_add_device(SpiceUsbDeviceWidget *self,
                                          SpiceUsbDevice *usb_device,
                                          GtkTreeIter *old_dev_iter)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkTreeStore *tree_store = priv->tree_store;
    SpiceUsbDeviceManager *usb_dev_mgr = priv->manager;
    GtkTreeIter new_dev_iter;
    spice_usb_device_info dev_info;
    gboolean is_dev_redirected, is_dev_connected, is_dev_cd;
    gchar *addr_str;
    GArray *lun_array;
    guint lun_index;

    if (old_dev_iter == NULL) {
        gtk_tree_store_append(tree_store, &new_dev_iter, NULL);
    } else {
        gtk_tree_store_insert_after(tree_store, &new_dev_iter, NULL, old_dev_iter);
        gtk_tree_store_remove(tree_store, old_dev_iter);
    }

    spice_usb_device_get_info(usb_device, &dev_info);
    addr_str = g_strdup_printf("%d:%d", (gint)dev_info.bus, (gint)dev_info.address);
    is_dev_connected = spice_usb_device_manager_is_device_connected(usb_dev_mgr, usb_device);
    is_dev_redirected = is_dev_connected;
    is_dev_cd = spice_usb_device_manager_is_device_cd(usb_dev_mgr, usb_device);
    g_print("usb device a:[%s] p:[%s] m:[%s] conn:%d cd:%d\n",
        addr_str, dev_info.vendor, dev_info.product, is_dev_connected, is_dev_cd);

    gtk_tree_store_set(tree_store, &new_dev_iter,
        COL_REDIRECT, is_dev_redirected,
        COL_ADDRESS, addr_str,
        COL_CONNECT_ICON, is_dev_connected ? priv->icon_connected : priv->icon_disconn,
        COL_CD_ICON, priv->icon_cd,
        COL_VENDOR, dev_info.vendor,
        COL_PRODUCT, dev_info.product,
        COL_CD_DEV, is_dev_cd,
        COL_LUN_ITEM, FALSE, /* USB device item */
        COL_DEV_ITEM, TRUE, /* USB device item */
        COL_ITEM_DATA, (gpointer)usb_device,
        COL_CONNECTED, is_dev_connected,
        COL_CAN_REDIRECT, TRUE,
        COL_ROW_COLOR, "beige",
        COL_ROW_COLOR_SET, TRUE,
        -1);

    priv->device_count++;

    /* get all the luns */
    lun_array = spice_usb_device_manager_get_device_luns(usb_dev_mgr, usb_device);
    for (lun_index = 0; lun_index < lun_array->len; lun_index++) {
        usb_widget_lun_item *lun_item;
        GtkTreeIter lun_iter;
        gchar lun_str[8];

        lun_item = g_malloc(sizeof(*lun_item));
        lun_item->manager = usb_dev_mgr;
        lun_item->device = usb_device;
        lun_item->lun = g_array_index(lun_array, guint, lun_index);
        spice_usb_device_manager_device_lun_get_info(usb_dev_mgr, usb_device, lun_item->lun, &lun_item->info);
        g_print("lun %u v:[%s] p:[%s] r:[%s] file:[%s] lun_item:%p\n",
                lun_index, lun_item->info.vendor, lun_item->info.product,
                lun_item->info.revision, lun_item->info.file_path, lun_item);
        g_snprintf(lun_str, 8, "↳%u", lun_item->lun);

        /* Append LUN as a child of USB device */
        gtk_tree_store_append(tree_store, &lun_iter, &new_dev_iter);
        gtk_tree_store_set(tree_store, &lun_iter,
                COL_ADDRESS, lun_str,
                COL_VENDOR, lun_item->info.vendor,
                COL_PRODUCT, lun_item->info.product,
                COL_REVISION, lun_item->info.revision,
                COL_ALIAS, lun_item->info.alias,
                COL_STARTED, lun_item->info.started,
                COL_LOADED, lun_item->info.loaded,
                COL_LOCKED, lun_item->info.locked,
                COL_FILE, lun_item->info.file_path,
                COL_CD_DEV, FALSE,
                COL_LUN_ITEM, TRUE, /* LUN item */
                COL_DEV_ITEM, FALSE, /* LUN item */
                COL_ITEM_DATA, (gpointer)lun_item,
                COL_ROW_COLOR, "azure",
                COL_ROW_COLOR_SET, TRUE,
                -1);
    }

    return gtk_tree_model_get_path(GTK_TREE_MODEL(tree_store), &new_dev_iter);
}

static gboolean usb_widget_tree_store_find_usb_dev_foreach_cb(GtkTreeModel *tree_model,
                                                              GtkTreePath *path, GtkTreeIter *iter,
                                                              gpointer user_data)
{
    tree_find_usb_dev *find_info = (tree_find_usb_dev *)user_data;
    SpiceUsbDevice *find_usb_device = find_info->usb_dev;
    SpiceUsbDevice *usb_device;
    gboolean is_lun_item;

    gtk_tree_model_get(tree_model, iter,
                       COL_LUN_ITEM, &is_lun_item,
                       COL_ITEM_DATA, (gpointer *)&usb_device,
                       -1);
    if (!is_lun_item && usb_device == find_usb_device) {
        find_info->dev_iter = iter;
        g_print("Usb dev found %p iter %p\n", usb_device, iter);
        return TRUE; /* stop iterating */
    } else {
        return FALSE; /* continue iterating */
    }
}

static GtkTreeIter *usb_widget_tree_store_find_usb_device(GtkTreeStore *tree_store,
                                                          SpiceUsbDevice *usb_device)
{
    tree_find_usb_dev find_info = { .usb_dev = usb_device, .dev_iter = NULL };
    GtkTreeIter *iter;

    gtk_tree_model_foreach(GTK_TREE_MODEL(tree_store),
                           usb_widget_tree_store_find_usb_dev_foreach_cb, (gpointer)&find_info);

    if (find_info.dev_iter) {
        iter = g_malloc(sizeof(*iter));
        *iter = *find_info.dev_iter;
        return iter;
    } else {
        return NULL;
    }
}

static gboolean usb_widget_remove_device(SpiceUsbDeviceWidget *self,
                                         SpiceUsbDevice *usb_device)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkTreeIter *old_dev_iter;

    old_dev_iter = usb_widget_tree_store_find_usb_device(priv->tree_store, usb_device);
    if (old_dev_iter != NULL) {
        g_print("Device removed\n");
        gtk_tree_store_remove(priv->tree_store, old_dev_iter);
        priv->device_count--;
        g_free(old_dev_iter);
        return TRUE;
    } else {
        g_print("Device not found!\n");
        return FALSE;
    }
}

static GtkTreeViewColumn* view_add_toggle_column(SpiceUsbDeviceWidget *self,
                                                 enum column_id toggle_col_id,
                                                 enum column_id visible_col_id,
                                                 enum column_id sensitive_col_id,
                                                 tree_item_toggled_cb toggled_cb)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkCellRenderer     *renderer;
    GtkTreeViewColumn   *view_col;

    renderer = gtk_cell_renderer_toggle_new();

    if (sensitive_col_id != INVALID_COL) {
        view_col = gtk_tree_view_column_new_with_attributes(
                        col_name[toggle_col_id],
                        renderer,
                        "active", toggle_col_id,
                        "visible", visible_col_id,
                        "activatable", sensitive_col_id,
                        NULL);
    } else {
        view_col = gtk_tree_view_column_new_with_attributes(
                        col_name[toggle_col_id],
                        renderer,
                        "active", toggle_col_id,
                        "visible", visible_col_id,
                        NULL);
    }

    gtk_tree_view_append_column(priv->tree_view, view_col);

    g_object_set_data(G_OBJECT(renderer), "column", (gint *)toggle_col_id);
    g_signal_connect(renderer, "toggled", G_CALLBACK(toggled_cb), self);

    g_print("view added toggle column [%u : %s] visible when [%u : %s]\n",
            toggle_col_id, col_name[toggle_col_id],
            visible_col_id, col_name[visible_col_id]);
    return view_col;
}

static GtkTreeViewColumn* view_add_text_column(SpiceUsbDeviceWidget *self,
                                               enum column_id col_id)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkCellRenderer     *renderer;
    GtkTreeViewColumn   *view_col;

    renderer = gtk_cell_renderer_text_new();

    view_col = gtk_tree_view_column_new_with_attributes(
                    col_name[col_id],
                    renderer,
                    "text", col_id,
                    //"cell-background", COL_ROW_COLOR,
                    //"cell-background-set", COL_ROW_COLOR_SET,
                    NULL);

    gtk_tree_view_append_column(priv->tree_view, view_col);

    g_print("view added text column [%u : %s]\n", col_id, col_name[col_id]);
    return view_col;
}

static GtkTreeViewColumn* view_add_pixbuf_column(SpiceUsbDeviceWidget *self,
                                                 enum column_id col_id,
                                                 enum column_id visible_col_id)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkCellRenderer     *renderer;
    GtkTreeViewColumn   *view_col;

    renderer = gtk_cell_renderer_pixbuf_new();

    if (visible_col_id == INVALID_COL) {
        view_col = gtk_tree_view_column_new_with_attributes(
                        col_name[col_id],
                        renderer,
                        "pixbuf", col_id,
                        NULL);
        g_print("view added pixbuf col[%u : %s] visible always\n", col_id, col_name[col_id]);
    } else {
        view_col = gtk_tree_view_column_new_with_attributes(
                        col_name[col_id],
                        renderer,
                        "pixbuf", col_id,
                        "visible", visible_col_id,
                        NULL);
        g_print("view added pixbuf col[%u : %s] visible when[%u : %s]\n",
                col_id, col_name[col_id], visible_col_id, col_name[visible_col_id]);
    }
    gtk_tree_view_append_column(priv->tree_view, view_col);
    return view_col;
}

/* Toggle handlers */

static gboolean tree_item_toggle_get_val(GtkTreeStore *tree_store, gchar *path_str, GtkTreeIter *iter, enum column_id col_id)
{
    gboolean toggle_val;

    gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(tree_store), iter, path_str);
    gtk_tree_model_get(GTK_TREE_MODEL(tree_store), iter, col_id, &toggle_val, -1);

    return toggle_val;
}

static usb_widget_lun_item* tree_item_toggle_lun_item(GtkTreeStore *tree_store, GtkTreeIter *iter)
{
    usb_widget_lun_item *lun_item;
    gboolean is_lun;
    gtk_tree_model_get(GTK_TREE_MODEL(tree_store), iter, COL_LUN_ITEM, &is_lun, COL_ITEM_DATA, (gpointer *)&lun_item, -1);
    return is_lun ? lun_item : NULL;
}

static void tree_item_toggle_set(GtkTreeStore *tree_store, GtkTreeIter *iter, enum column_id col_id, gboolean new_val)
{
    gtk_tree_store_set(tree_store, iter, col_id, new_val, -1);
}

typedef struct _connect_cb_data {
    SpiceUsbDeviceWidget *self;
    SpiceUsbDevice *usb_dev;
} connect_cb_data;

static void connect_cb_data_free(connect_cb_data *user_data)
{
    spice_usb_device_widget_update_status(user_data->self);
    g_object_unref(user_data->self);
    //g_object_unref(user_data->usb_dev);
    g_free(user_data);
}

static void usb_widget_connect_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    connect_cb_data *cb_data = user_data;
    SpiceUsbDeviceWidget *self = cb_data->self;
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    SpiceUsbDevice *usb_dev = cb_data->usb_dev;
    GError *err = NULL;
    GtkTreeIter *dev_iter;
    gchar *desc;
    gboolean finished;

    dev_iter = usb_widget_tree_store_find_usb_device(priv->tree_store, usb_dev);
    if (!dev_iter) {
        return;
    }

    desc = spice_usb_device_get_description(usb_dev, priv->device_format_string);
    g_print("Connect cb: %p %s\n", usb_dev, desc);

    finished = spice_usb_device_manager_connect_device_finish(priv->manager, res, &err);
    if (finished) {
        gtk_tree_store_set(priv->tree_store, dev_iter,
                           COL_CONNECT_ICON, priv->icon_connected,
                           -1);
    } else {
        gtk_tree_store_set(priv->tree_store, dev_iter,
                           COL_REDIRECT, FALSE,
                           -1);
        g_prefix_error(&err, "Device connect failed %s: ", desc);
        if (err) {
            SPICE_DEBUG("%s", err->message);
            g_signal_emit(self, signals[CONNECT_FAILED], 0, usb_dev, err);
            g_error_free(err);
        } else {
            g_signal_emit(self, signals[CONNECT_FAILED], 0, usb_dev, NULL);
        }

        /* don't trigger a disconnect if connect failed */
        /*
        g_signal_handlers_block_by_func(GTK_TOGGLE_BUTTON(user_data->check),
                                        checkbox_clicked_cb, self);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(user_data->check), FALSE);
        g_signal_handlers_unblock_by_func(GTK_TOGGLE_BUTTON(user_data->check),
                                        checkbox_clicked_cb, self);
        */
    }
    g_free(desc);
    g_free(dev_iter);
    connect_cb_data_free(user_data);
}

static void usb_widget_disconnect_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    connect_cb_data *cb_data = user_data;
    SpiceUsbDeviceWidget *self = cb_data->self;
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    SpiceUsbDevice *usb_dev = cb_data->usb_dev;
    GError *err = NULL;
    GtkTreeIter *dev_iter;
    gchar *desc;
    gboolean finished;

    dev_iter = usb_widget_tree_store_find_usb_device(priv->tree_store, usb_dev);
    if (!dev_iter) {
        return;
    }

    desc = spice_usb_device_get_description(usb_dev, priv->device_format_string);
    g_print("Disconnect cb: %p %s\n", usb_dev, desc);

    finished = spice_usb_device_manager_disconnect_device_finish(priv->manager, res, &err);
    if (finished) {
        gtk_tree_store_set(priv->tree_store, dev_iter,
                           COL_CONNECT_ICON, priv->icon_disconn,
                           -1);
    } else {
        gtk_tree_store_set(priv->tree_store, dev_iter,
                           COL_REDIRECT, TRUE,
                           -1);
        g_prefix_error(&err, "Device disconnect failed %s: ", desc);
        if (err) {
            SPICE_DEBUG("%s", err->message);
            g_error_free(err);
        }
    }
    g_free(desc);
    g_free(dev_iter);
    connect_cb_data_free(user_data);
}

static void tree_item_toggled_cb_redirect(GtkCellRendererToggle *cell, gchar *path_str, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkTreeStore *tree_store = priv->tree_store;
    connect_cb_data *cb_data = g_new(connect_cb_data, 1);
    SpiceUsbDevice *usb_dev;
    GtkTreeIter iter;
    gboolean new_redirect_val;

    new_redirect_val = !tree_item_toggle_get_val(tree_store, path_str, &iter, COL_REDIRECT);
    g_print("Redirect: %s\n", new_redirect_val ? "ON" : "OFF");
    tree_item_toggle_set(tree_store, &iter, COL_REDIRECT, new_redirect_val);

    gtk_tree_model_get(GTK_TREE_MODEL(tree_store), &iter, COL_ITEM_DATA, (gpointer *)&usb_dev, -1);
    cb_data->self = g_object_ref(self);
    cb_data->usb_dev = usb_dev; // g_object_ref(usb_dev);

    if (new_redirect_val) {
        spice_usb_device_manager_connect_device_async(priv->manager, usb_dev,
                                                      NULL, /* cancellable */
                                                      usb_widget_connect_cb, cb_data);
    } else {
        spice_usb_device_manager_disconnect_device_async(priv->manager, usb_dev,
                                                         NULL, /* cancellable */
                                                         usb_widget_disconnect_cb, cb_data);

    }
    spice_usb_device_widget_update_status(self);
}

static void tree_item_toggled_cb_started(GtkCellRendererToggle *cell, gchar *path_str, gpointer user_data)
{
    GtkTreeStore *tree_store = (GtkTreeStore *)user_data;
    GtkTreeIter iter;
    gboolean started;
    usb_widget_lun_item *lun_item;

    started = tree_item_toggle_get_val(tree_store, path_str, &iter, COL_STARTED);
    lun_item = tree_item_toggle_lun_item(tree_store, &iter);
    if (!lun_item) {
        g_print("not a LUN toggled?\n");
        return;
    }
    g_print("toggled lun: %u [%s,%s,%s] alias:%s started: %d --> %d\n",
            lun_item->lun, lun_item->info.vendor, lun_item->info.product, lun_item->info.revision, lun_item->info.alias,
            started, !started);

    tree_item_toggle_set(tree_store, &iter, COL_STARTED, !started);
}

static void tree_item_toggled_cb_locked(GtkCellRendererToggle *cell, gchar *path_str, gpointer user_data)
{
    GtkTreeStore *tree_store = (GtkTreeStore *)user_data;
    GtkTreeIter iter;
    gboolean locked;
    usb_widget_lun_item *lun_item;

    locked = tree_item_toggle_get_val(tree_store, path_str, &iter, COL_LOCKED);
    lun_item = tree_item_toggle_lun_item(tree_store, &iter);
    if (!lun_item) {
        g_print("not a LUN toggled?\n");
        return;
    }

    g_print("toggled lun:%u [%s,%s,%s] alias:%s locked: %d --> %d\n",
            lun_item->lun, lun_item->info.vendor, lun_item->info.product, lun_item->info.revision, lun_item->info.alias,
            locked, !locked);

    tree_item_toggle_set(tree_store, &iter, COL_LOCKED, !locked);
}

static void tree_item_toggled_cb_loaded(GtkCellRendererToggle *cell, gchar *path_str, gpointer user_data)
{
    GtkTreeStore *tree_store = (GtkTreeStore *)user_data;
    GtkTreeIter iter;
    gboolean loaded;
    usb_widget_lun_item *lun_item;

    loaded = tree_item_toggle_get_val(tree_store, path_str, &iter, COL_LOADED);
    lun_item = tree_item_toggle_lun_item(tree_store, &iter);
    if (!lun_item) {
        g_print("not a LUN toggled?\n");
        return;
    }

    g_print("toggled lun:%u [%s,%s,%s] alias:%s loaded: %d --> %d\n",
            lun_item->lun, lun_item->info.vendor, lun_item->info.product, lun_item->info.revision, lun_item->info.alias,
            loaded, !loaded);

    tree_item_toggle_set(tree_store, &iter, COL_LOADED, !loaded);
}

/* Signal handlers */

static void device_added_cb(SpiceUsbDeviceManager *usb_dev_mgr,
    SpiceUsbDevice *usb_device, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkTreePath *new_dev_path;

    g_print("Signal: Device Added\n");

    new_dev_path = usb_widget_add_device(self, usb_device, NULL);

    gtk_tree_view_expand_row(priv->tree_view, new_dev_path, FALSE);
    gtk_tree_path_free(new_dev_path);

    spice_usb_device_widget_update_status(self);
}

static void device_removed_cb(SpiceUsbDeviceManager *usb_dev_mgr,
    SpiceUsbDevice *usb_device, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    gboolean dev_removed;

    g_print("Signal: Device Removed\n");

    dev_removed = usb_widget_remove_device(self, usb_device);
    if (dev_removed) {
        spice_usb_device_widget_update_status(self);
    }
}

static void device_changed_cb(SpiceUsbDeviceManager *usb_dev_mgr,
    SpiceUsbDevice *usb_device, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkTreeIter *old_dev_iter;

    g_print("Signal: Device Changed\n");

    old_dev_iter = usb_widget_tree_store_find_usb_device(priv->tree_store, usb_device);
    if (old_dev_iter != NULL) {
        GtkTreePath *new_dev_path;

        new_dev_path = usb_widget_add_device(self, usb_device, old_dev_iter);

        gtk_tree_view_expand_row(priv->tree_view, new_dev_path, FALSE);
        gtk_tree_path_free(new_dev_path);

        spice_usb_device_widget_update_status(self);
        g_free(old_dev_iter);
    } else {
        g_print("Device not found!\n");
    }
}

static void device_error_cb(SpiceUsbDeviceManager *manager,
    SpiceUsbDevice *usb_device, GError *err, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkTreeIter *dev_iter;

    g_print("Signal: Device Error\n");

    dev_iter = usb_widget_tree_store_find_usb_device(priv->tree_store, usb_device);
    if (dev_iter != NULL) {
        tree_item_toggle_set(priv->tree_store, dev_iter, COL_REDIRECT, FALSE);
        spice_usb_device_widget_update_status(self);
        g_free(dev_iter);
        //gtk_widget_show_all(GTK_WIDGET(priv->tree_view));
    } else {
        g_print("Device not found!\n");
    }
}

/* Selection handler */

static void tree_selection_changed_cb(GtkTreeSelection *select, gpointer user_data)
{
    GtkTreeModel *tree_model;
    GtkTreeIter iter;
    GtkTreePath *path;
    gboolean is_lun;
    usb_widget_lun_item *lun_item;
    gchar *txt[4];

    if (gtk_tree_selection_get_selected(select, &tree_model, &iter)) {
        gtk_tree_model_get(tree_model, &iter,
                COL_VENDOR, &txt[0],
                COL_PRODUCT, &txt[1],
                COL_REVISION, &txt[2],
                COL_ALIAS, &txt[3],
                COL_LUN_ITEM, &is_lun,
                COL_ITEM_DATA, (gpointer *)&lun_item,
                -1);
        path = gtk_tree_model_get_path(tree_model, &iter);

        g_print("selected: %s,%s,%s,%s [%s %s] [%s]\n",
                txt[0], txt[1],
                is_lun ? txt[2] : "--",
                is_lun ? txt[3] : "--",
                is_lun ? "LUN" : "USB-DEV",
                is_lun ? lun_item->info.file_path : "--",
                gtk_tree_path_to_string(path));

        if (txt[0])
        g_free(txt[0]);
        if (txt[1])
        g_free(txt[1]);
        if (txt[2])
        g_free(txt[2]);
        if (txt[3])
        g_free(txt[3]);
        gtk_tree_path_free(path);
    }
}

static GtkTreeSelection* set_selection_handler(GtkTreeView *tree_view)
{
    GtkTreeSelection *select;

    select = gtk_tree_view_get_selection(tree_view);
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);

    g_signal_connect(G_OBJECT(select), "changed",
                     G_CALLBACK(tree_selection_changed_cb),
                     NULL);

    g_print("selection handler set\n");
    return select;
}

/* LUN properties dialog */

typedef struct _lun_properties_dialog {
    GtkWidget *dialog;

    GtkWidget *file_entry;
    GtkWidget *vendor_entry;
    GtkWidget *product_entry;
    GtkWidget *revision_entry;
    GtkWidget *alias_entry;

    GtkWidget *started_toggle;
    GtkWidget *loaded_toggle;
    GtkWidget *locked_toggle;
} lun_properties_dialog;

static void usb_cd_choose_file(GtkWidget *button, gpointer user_data)
{
    GtkWidget *file_entry = (GtkWidget *)user_data;
    GtkWidget *dialog;
    gint res;

    dialog = gtk_file_chooser_dialog_new ("Choose File for USB CD",
                                          GTK_WINDOW(gtk_widget_get_toplevel(file_entry)),
                                          GTK_FILE_CHOOSER_ACTION_OPEN,
                                          "_Cancel",
                                          GTK_RESPONSE_CANCEL,
                                          "_Ok",
                                          GTK_RESPONSE_ACCEPT,
                                          NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_entry_set_alignment(GTK_ENTRY(file_entry), 1);
        gtk_entry_set_text(GTK_ENTRY(file_entry), filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void create_lun_properties_dialog(SpiceUsbDeviceWidget *self,
                                         GtkWidget *parent_window,
                                         spice_usb_device_lun_info *lun_info,
                                         lun_properties_dialog *lun_dialog)
{
    // SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkWidget *dialog, *content_area, *grid;
    GtkWidget *file_entry, *choose_button;
    GtkWidget *vendor_entry, *product_entry, *revision_entry, *alias_entry;
    GtkWidget *started_toggle, *loaded_toggle, *locked_toggle;
    gint nrow = 0;

    dialog = gtk_dialog_new_with_buttons (!lun_info ? "Add CD LUN" : "CD LUN Settings",
                    GTK_WINDOW(parent_window),
                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, /* flags */
                    !lun_info ? "Add" : "OK", GTK_RESPONSE_ACCEPT,
                    "Cancel", GTK_RESPONSE_REJECT,
                    NULL);

    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), 12);
    gtk_box_set_spacing(GTK_BOX(gtk_bin_get_child(GTK_BIN(dialog))), 12);

    content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(content_area), grid);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);

    /* File path label */
    gtk_grid_attach(GTK_GRID(grid),
            gtk_label_new("Select file or device"),
            0, nrow++, // left top
            7, 1); // width height

    /* file/device path entry */
    file_entry = gtk_entry_new();
    if (!lun_info) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(file_entry), "file-path");
    } else {
        gtk_entry_set_text(GTK_ENTRY(file_entry), lun_info->file_path);
        if (lun_info->loaded) {
            gtk_editable_set_editable(GTK_EDITABLE(file_entry), FALSE);
            gtk_widget_set_can_focus(file_entry, FALSE);
        }
    }
    gtk_grid_attach(GTK_GRID(grid),
            file_entry,
            0, nrow, // left top
            6, 1); // width height

    /* choose button */
    choose_button = gtk_button_new_with_mnemonic("_Choose File");
    g_signal_connect(GTK_BUTTON(choose_button),
                     "clicked", G_CALLBACK(usb_cd_choose_file), file_entry);
    gtk_grid_attach(GTK_GRID(grid),
            choose_button,
            6, nrow++, // left top
            1, 1); // width height

    /* product id labels */
    gtk_grid_attach(GTK_GRID(grid),
            gtk_label_new("Vendor"),
            0, nrow, // left top
            2, 1); // width height

    gtk_grid_attach(GTK_GRID(grid),
            gtk_label_new("Product"),
            2, nrow, // left top
            4, 1); // width height

    gtk_grid_attach(GTK_GRID(grid),
            gtk_label_new("Revision"),
            6, nrow++, // left top
            1, 1); // width height

    /* vendor entry */
    vendor_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(vendor_entry), 8);
    gtk_entry_set_text(GTK_ENTRY(vendor_entry), !lun_info ? "RedHat" : lun_info->vendor);
    gtk_grid_attach(GTK_GRID(grid),
            vendor_entry,
            0, nrow, // left top
            2, 1); // width height

    /* tree_store entry */
    product_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(product_entry), 16);
    gtk_entry_set_text(GTK_ENTRY(product_entry), !lun_info ? "USB-CD" : lun_info->product);
    gtk_grid_attach(GTK_GRID(grid),
            product_entry,
            2, nrow, // left top
            4, 1); // width height

    /* revision entry */
    revision_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(revision_entry), 4);
    gtk_entry_set_text(GTK_ENTRY(revision_entry), !lun_info ? "0.1" : lun_info->revision);
    gtk_grid_attach(GTK_GRID(grid),
            revision_entry,
            6, nrow++, // left top
            1, 1); // width height

    /* alias label */
    gtk_grid_attach(GTK_GRID(grid),
            gtk_label_new("Revision"),
            0, nrow++, // left top
            7, 1); // width height

    /* alias entry */
    alias_entry = gtk_entry_new();
    if (!lun_info) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(alias_entry), "device alias");
    } else {
        gtk_entry_set_text(GTK_ENTRY(alias_entry), lun_info->alias);
    }
    gtk_grid_attach(GTK_GRID(grid),
            alias_entry,
            0, nrow++, // left top
            7, 1); // width height

    /* Started checkbox */
    started_toggle = gtk_check_button_new_with_label("Started");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(started_toggle), !lun_info ? TRUE : lun_info->started);
    gtk_grid_attach(GTK_GRID(grid),
            started_toggle,
            1, nrow, // left top
            1, 1); // width height

    /* Loaded checkbox */
    loaded_toggle = gtk_check_button_new_with_label("Loaded");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(loaded_toggle), !lun_info ? TRUE : lun_info->loaded);
    gtk_grid_attach(GTK_GRID(grid),
            loaded_toggle,
            3, nrow, // left top
            1, 1); // width height

    /* Locked checkbox */
    locked_toggle = gtk_check_button_new_with_label("Locked");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(locked_toggle), !lun_info ? FALSE : lun_info->locked);
    gtk_grid_attach(GTK_GRID(grid),
            locked_toggle,
            6, nrow++, // left top
            1, 1); // width height

    gtk_widget_show_all(dialog);

    lun_dialog->dialog = dialog;
    lun_dialog->file_entry = file_entry;
    lun_dialog->vendor_entry = vendor_entry;
    lun_dialog->product_entry = product_entry;
    lun_dialog->revision_entry = revision_entry;
    lun_dialog->alias_entry = alias_entry;
    lun_dialog->started_toggle = started_toggle;
    lun_dialog->loaded_toggle = loaded_toggle;
    lun_dialog->locked_toggle = locked_toggle;
}

static void lun_properties_dialog_get_info(lun_properties_dialog *lun_dialog,
                                           spice_usb_device_lun_info *lun_info)
{
    lun_info->file_path = gtk_entry_get_text(GTK_ENTRY(lun_dialog->file_entry));
    lun_info->vendor = gtk_entry_get_text(GTK_ENTRY(lun_dialog->vendor_entry));
    lun_info->product = gtk_entry_get_text(GTK_ENTRY(lun_dialog->product_entry));
    lun_info->revision = gtk_entry_get_text(GTK_ENTRY(lun_dialog->revision_entry));
    lun_info->alias = gtk_entry_get_text(GTK_ENTRY(lun_dialog->alias_entry));
    lun_info->started = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lun_dialog->started_toggle));
    lun_info->loaded = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lun_dialog->loaded_toggle));
    lun_info->locked = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lun_dialog->locked_toggle));
}

/* Popup menu */
static void view_popup_menu_on_eject(GtkWidget *menuitem, gpointer user_data)
{
    //SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    //SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    //GtkTreeView *tree_view = priv->tree_view;
    g_print ("Do Eject!\n");
}

static void view_popup_menu_on_remove(GtkWidget *menuitem, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkTreeSelection *select = gtk_tree_view_get_selection(priv->tree_view);
    GtkTreeModel *tree_model;
    GtkTreeIter iter;
    gboolean is_lun;

    if (gtk_tree_selection_get_selected(select, &tree_model, &iter)) {
        gtk_tree_model_get(tree_model, &iter,
                           COL_LUN_ITEM, &is_lun,
                           -1);
        if (!is_lun) {
            SpiceUsbDevice *usb_device;
            gtk_tree_model_get(tree_model, &iter, COL_ITEM_DATA, (gpointer *)&usb_device, -1);
            g_print("Remove USB device\n");
        } else {
            usb_widget_lun_item *lun_item;
            gtk_tree_model_get(tree_model, &iter, COL_ITEM_DATA, (gpointer *)&lun_item, -1);
            gtk_tree_selection_unselect_all(select);
            spice_usb_device_manager_device_lun_remove(lun_item->manager, lun_item->device, lun_item->lun);
        }
    } else {
        g_print("Remove - failed to get selection\n");
    }
}

static void view_popup_menu_on_settings(GtkWidget *menuitem, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkTreeSelection *select = gtk_tree_view_get_selection(priv->tree_view);
    GtkTreeModel *tree_model;
    GtkTreeIter iter;
    gboolean is_lun;

    if (gtk_tree_selection_get_selected(select, &tree_model, &iter)) {
        gtk_tree_model_get(tree_model, &iter,
                           COL_LUN_ITEM, &is_lun,
                           -1);
        if (!is_lun) {
            g_print("No settings for USB device yet\n");
        } else {
            lun_properties_dialog lun_dialog;
            usb_widget_lun_item *lun_item;
            gint resp;

            gtk_tree_model_get(tree_model, &iter, COL_ITEM_DATA, (gpointer *)&lun_item, -1);
            gtk_tree_selection_unselect_all(select);
            create_lun_properties_dialog(self, NULL, &lun_item->info, &lun_dialog);

            resp = gtk_dialog_run(GTK_DIALOG(lun_dialog.dialog));
            if (resp == GTK_RESPONSE_ACCEPT) {
                spice_usb_device_lun_info lun_info;
                g_print("response is ACCEPT\n");
                lun_properties_dialog_get_info(&lun_dialog, &lun_info);
                spice_usb_device_manager_add_cd_lun(priv->manager, &lun_info);
            } else {
                g_print("response is REJECT\n");
            }
            gtk_widget_destroy(lun_dialog.dialog);
        }
    } else {
        g_print("Remove - failed to get selection\n");
    }
}

static GtkWidget *view_popup_add_menu_item(GtkWidget *menu,
    const gchar *label_str,
    const gchar *icon_name,
    GCallback cb_func, gpointer user_data)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *menu_item = gtk_menu_item_new();
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    GtkWidget *label = gtk_accel_label_new(label_str);
    GtkAccelGroup *accel_group = gtk_accel_group_new();
    guint accel_key;

    g_signal_connect(menu_item, "activate", cb_func, user_data);

    /* add icon */
    gtk_container_add(GTK_CONTAINER(box), icon);

    /* add label */
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_use_underline(GTK_LABEL(label), TRUE);
    g_object_get(G_OBJECT(label), "mnemonic-keyval", &accel_key, NULL);
    gtk_widget_add_accelerator(menu_item, "activate", accel_group, accel_key,
                               GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_accel_label_set_accel_widget(GTK_ACCEL_LABEL(label), menu_item);
    gtk_box_pack_end(GTK_BOX(box), label, TRUE, TRUE, 0);

    /* add menu item */
    gtk_container_add(GTK_CONTAINER(menu_item), box);
    gtk_widget_show_all(menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

    return menu_item;
}

static void view_popup_menu(GtkTreeView *tree_view, GdkEventButton *event, gpointer user_data)
{
    GtkWidget *menu; // *menu_item;

    menu = gtk_menu_new();

    //menu_item = 
    view_popup_add_menu_item(menu, "_Eject", "media-eject", G_CALLBACK(view_popup_menu_on_eject), user_data);
    view_popup_add_menu_item(menu, "_Settings", "preferences-system", G_CALLBACK(view_popup_menu_on_settings), user_data);
    view_popup_add_menu_item(menu, "_Remove", "edit-delete", G_CALLBACK(view_popup_menu_on_remove), user_data);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
}

static gboolean treeview_on_right_button_pressed_cb(GtkWidget *view, GdkEventButton *event, gpointer user_data)
{
    GtkTreeView *tree_view = GTK_TREE_VIEW(view);
    /* single click with the right mouse button */
    if (event->type == GDK_BUTTON_PRESS  &&  event->button == 3)
    {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
        if (gtk_tree_selection_count_selected_rows(selection) <= 1) {
            GtkTreePath *path;
            /* Get tree path for row that was clicked */
            if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree_view),
                                                (gint) event->x,
                                                (gint) event->y,
                                                &path, NULL, NULL, NULL))
            {
                gtk_tree_selection_unselect_all(selection);
                gtk_tree_selection_select_path(selection, path);
                gtk_tree_path_free(path);
            }
        }
        view_popup_menu(tree_view, event, user_data);
        return TRUE; /* we handled this */
    } else {
        return FALSE; /* we did not handle this */
    }
}

static gboolean treeview_on_popup_key_pressed_cb(GtkWidget *view, gpointer user_data)
{
    view_popup_menu(GTK_TREE_VIEW(view), NULL, user_data);
    return TRUE; /* we handled this */
}

/* Add LUN dialog */

static void add_cd_lun_button_clicked_cb(GtkWidget *add_cd_button, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkWidget *parent_window = gtk_widget_get_toplevel(add_cd_button);
    lun_properties_dialog lun_dialog;
    gint resp;

    create_lun_properties_dialog(self, parent_window, NULL, &lun_dialog);

    resp = gtk_dialog_run(GTK_DIALOG(lun_dialog.dialog));
    if (resp == GTK_RESPONSE_ACCEPT) {
        spice_usb_device_lun_info lun_info;
        g_print("response is ACCEPT\n");
        lun_properties_dialog_get_info(&lun_dialog, &lun_info);
        spice_usb_device_manager_add_cd_lun(priv->manager, &lun_info);
    } else {
        g_print("response is REJECT\n");
    }
    gtk_widget_destroy(lun_dialog.dialog);
}

static void spice_usb_device_widget_get_property(GObject     *gobject,
                                                 guint        prop_id,
                                                 GValue      *value,
                                                 GParamSpec  *pspec)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(gobject);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        g_value_set_object(value, priv->session);
        break;
    case PROP_DEVICE_FORMAT_STRING:
        g_value_set_string(value, priv->device_format_string);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void spice_usb_device_widget_set_property(GObject       *gobject,
                                                 guint          prop_id,
                                                 const GValue  *value,
                                                 GParamSpec    *pspec)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(gobject);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        priv->session = g_value_dup_object(value);
        break;
    case PROP_DEVICE_FORMAT_STRING:
        priv->device_format_string = g_value_dup_string(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void spice_usb_device_widget_hide_info_bar(SpiceUsbDeviceWidget *self)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    if (priv->info_bar) {
        gtk_widget_destroy(priv->info_bar);
        priv->info_bar = NULL;
    }
}

static void
spice_usb_device_widget_show_info_bar(SpiceUsbDeviceWidget *self,
                                      const gchar          *message,
                                      GtkMessageType        message_type,
                                      GdkPixbuf            *icon_pixbuf)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkWidget *info_bar, *content_area, *hbox, *icon, *label;

    spice_usb_device_widget_hide_info_bar(self);

    info_bar = gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(info_bar), message_type);

    content_area = gtk_info_bar_get_content_area(GTK_INFO_BAR(info_bar));
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(content_area), hbox);

    icon = gtk_image_new_from_pixbuf(icon_pixbuf);
    gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 10);

    label = gtk_label_new(message);
    gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);

    priv->info_bar = gtk_alignment_new(0.0, 0.0, 1.0, 0.0);
    gtk_alignment_set_padding(GTK_ALIGNMENT(priv->info_bar), 0, 0, 0, 0);
    gtk_container_add(GTK_CONTAINER(priv->info_bar), info_bar);

    gtk_box_pack_start(GTK_BOX(self), priv->info_bar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(self), priv->info_bar, 1); /* put after the lable */
    gtk_widget_show_all(priv->info_bar);
}

static void spice_usb_device_widget_create_tree_view(SpiceUsbDeviceWidget *self)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkTreeStore *tree_store = usb_widget_create_tree_store();
    GtkTreeView *tree_view = GTK_TREE_VIEW(gtk_tree_view_new());

    priv->tree_view = tree_view;
    priv->tree_store = tree_store;

    gtk_tree_view_set_model(tree_view, GTK_TREE_MODEL(tree_store));
    g_object_unref(tree_store); /* destroy tree_store automatically with tree_view */

    view_add_toggle_column(self, COL_REDIRECT, COL_DEV_ITEM, COL_CAN_REDIRECT, tree_item_toggled_cb_redirect);

    view_add_text_column(self, COL_ADDRESS);

    view_add_pixbuf_column(self, COL_CONNECT_ICON, COL_REDIRECT);
    view_add_pixbuf_column(self, COL_CD_ICON, COL_CD_DEV);

    view_add_text_column(self, COL_VENDOR);
    view_add_text_column(self, COL_PRODUCT);
    view_add_text_column(self, COL_REVISION);
    view_add_text_column(self, COL_ALIAS);

    view_add_toggle_column(self, COL_STARTED, COL_LUN_ITEM, INVALID_COL, tree_item_toggled_cb_started);
    view_add_toggle_column(self, COL_LOADED, COL_LUN_ITEM, INVALID_COL, tree_item_toggled_cb_loaded);
    view_add_toggle_column(self, COL_LOCKED, COL_LUN_ITEM, INVALID_COL, tree_item_toggled_cb_locked);

    view_add_text_column(self, COL_FILE);

    /* ???? */
    gtk_tree_selection_set_mode(
            gtk_tree_view_get_selection(tree_view),
            GTK_SELECTION_NONE);

    set_selection_handler(tree_view);
}

static void spice_usb_device_widget_signals_connect(SpiceUsbDeviceWidget *self)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    g_signal_connect(priv->manager, "device-added",
                     G_CALLBACK(device_added_cb), self);
    g_signal_connect(priv->manager, "device-removed",
                     G_CALLBACK(device_removed_cb), self);
    g_signal_connect(priv->manager, "device-changed",
                     G_CALLBACK(device_changed_cb), self);
    // TODO: connect failed
    g_signal_connect(priv->manager, "device-error",
                     G_CALLBACK(device_error_cb), self);

    g_signal_connect(priv->tree_view, "button-press-event",
                     G_CALLBACK(treeview_on_right_button_pressed_cb), self);
    g_signal_connect(priv->tree_view, "popup-menu",
                     G_CALLBACK(treeview_on_popup_key_pressed_cb), self);
}

static void spice_usb_device_widget_constructed(GObject *gobject)
{
    SpiceUsbDeviceWidget *self;
    GtkRequisition min_size, natural_size;
    SpiceUsbDeviceWidgetPrivate *priv;
    GtkWidget *hbox, *dev_label;
    GtkWidget *add_cd_button, *add_cd_icon;
    GtkWidget *sw;
    GPtrArray *devices = NULL;
    GError *err = NULL;
    gchar *str;
    guint i;

    self = SPICE_USB_DEVICE_WIDGET(gobject);
    priv = self->priv;
    if (!priv->session)
        g_error("SpiceUsbDeviceWidget constructed without a session");

    min_size.width = 600;
    min_size.height = 300;
    natural_size.width = 1200;
    natural_size.height = 600;
    gtk_widget_get_preferred_size(GTK_WIDGET(self), &min_size, &natural_size);

    priv->label = gtk_label_new(NULL);
    str = g_strdup_printf("<b>%s</b>", _("Select USB devices to redirect"));
    gtk_label_set_markup(GTK_LABEL(priv->label), str);
    g_free(str);
    gtk_box_pack_start(GTK_BOX(self), priv->label, FALSE, FALSE, 0);

    priv->icon_cd = get_named_icon("media-optical", GTK_ICON_SIZE_LARGE_TOOLBAR);
    priv->icon_connected = get_named_icon("network-transmit-receive", GTK_ICON_SIZE_LARGE_TOOLBAR);
    priv->icon_disconn = get_named_icon("network-offline", GTK_ICON_SIZE_LARGE_TOOLBAR);
    priv->icon_warning = get_named_icon("dialog-warning", GTK_ICON_SIZE_LARGE_TOOLBAR);
    priv->icon_info = get_named_icon("dialog-information", GTK_ICON_SIZE_LARGE_TOOLBAR);

    priv->manager = spice_usb_device_manager_get(priv->session, &err);
    if (err) {
        spice_usb_device_widget_show_info_bar(self, err->message,
                                              GTK_MESSAGE_WARNING, priv->icon_warning);
        g_clear_error(&err);
        return;
    }

    spice_usb_device_widget_create_tree_view(self);
    spice_usb_device_widget_signals_connect(self);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(self), hbox, FALSE, FALSE, 0);

    /* "Available devices" label - in hbox */
    dev_label = gtk_label_new("Available USB devices");
    gtk_box_pack_start(GTK_BOX(hbox), dev_label, TRUE, FALSE, 0);

    /* "Add CD" button - in hbox */
    add_cd_button = gtk_button_new_with_label("Add CD");
    gtk_button_set_always_show_image(GTK_BUTTON(add_cd_button), TRUE);
    add_cd_icon = gtk_image_new_from_icon_name("list-add", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(add_cd_button), add_cd_icon);

    gtk_widget_set_halign(add_cd_button, GTK_ALIGN_END);
    g_signal_connect(add_cd_button, "clicked", G_CALLBACK(add_cd_lun_button_clicked_cb), self);
    gtk_box_pack_start(GTK_BOX(hbox), add_cd_button, FALSE, FALSE, 0);

    /* scrolled window */
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
            GTK_SHADOW_ETCHED_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_widget_set_halign(sw, GTK_ALIGN_FILL);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_widget_set_valign(sw, GTK_ALIGN_FILL);
    gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(priv->tree_view));
    gtk_box_pack_start(GTK_BOX(self), sw, TRUE, TRUE, 0);

    devices = spice_usb_device_manager_get_devices(priv->manager);
    if (!devices)
        goto end;

    for (i = 0; i < devices->len; i++) {
        SpiceUsbDevice *usb_device = g_ptr_array_index(devices, i);
        usb_widget_add_device(self, usb_device, NULL);
    }
    g_ptr_array_unref(devices);

end:
    spice_usb_device_widget_update_status(self);
}

static void spice_usb_device_widget_finalize(GObject *object)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(object);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    if (priv->manager) {
        g_signal_handlers_disconnect_by_func(priv->manager,
                                             device_added_cb, self);
        g_signal_handlers_disconnect_by_func(priv->manager,
                                             device_removed_cb, self);
        g_signal_handlers_disconnect_by_func(priv->manager,
                                             device_changed_cb, self);
        g_signal_handlers_disconnect_by_func(priv->manager,
                                             device_error_cb, self);
    }
    g_object_unref(priv->session);
    g_free(priv->device_format_string);

    if (G_OBJECT_CLASS(spice_usb_device_widget_parent_class)->finalize)
        G_OBJECT_CLASS(spice_usb_device_widget_parent_class)->finalize(object);
}

static void spice_usb_device_widget_class_init(
    SpiceUsbDeviceWidgetClass *klass)
{
    GObjectClass *gobject_class = (GObjectClass *)klass;
    GParamSpec *pspec;

    g_type_class_add_private (klass, sizeof (SpiceUsbDeviceWidgetPrivate));

    gobject_class->constructed  = spice_usb_device_widget_constructed;
    gobject_class->finalize     = spice_usb_device_widget_finalize;
    gobject_class->get_property = spice_usb_device_widget_get_property;
    gobject_class->set_property = spice_usb_device_widget_set_property;

    /**
     * SpiceUsbDeviceWidget:session:
     *
     * #SpiceSession this #SpiceUsbDeviceWidget is associated with
     *
     **/
    pspec = g_param_spec_object("session",
                                "Session",
                                "SpiceSession",
                                SPICE_TYPE_SESSION,
                                G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
                                G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(gobject_class, PROP_SESSION, pspec);

    /**
     * SpiceUsbDeviceWidget:device-format-string:
     *
     * Format string to pass to spice_usb_device_get_description() for getting
     * the device USB descriptions.
     */
    pspec = g_param_spec_string("device-format-string",
                                "Device format string",
                                "Format string for device description",
                                NULL,
                                G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
                                G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(gobject_class, PROP_DEVICE_FORMAT_STRING,
                                    pspec);

    /**
     * SpiceUsbDeviceWidget::connect-failed:
     * @widget: The #SpiceUsbDeviceWidget that emitted the signal
     * @device: #SpiceUsbDevice boxed object corresponding to the added device
     * @error:  #GError describing the reason why the connect failed
     *
     * The #SpiceUsbDeviceWidget::connect-failed signal is emitted whenever
     * the user has requested for a device to be redirected and this has
     * failed.
     **/
    signals[CONNECT_FAILED] =
        g_signal_new("connect-failed",
                    G_OBJECT_CLASS_TYPE(gobject_class),
                    G_SIGNAL_RUN_FIRST,
                    G_STRUCT_OFFSET(SpiceUsbDeviceWidgetClass, connect_failed),
                    NULL, NULL,
                    g_cclosure_user_marshal_VOID__BOXED_BOXED,
                    G_TYPE_NONE,
                    2,
                    SPICE_TYPE_USB_DEVICE,
                    G_TYPE_ERROR);
}

static void spice_usb_device_widget_init(SpiceUsbDeviceWidget *self)
{
    self->priv = SPICE_USB_DEVICE_WIDGET_GET_PRIVATE(self);
}

/* ------------------------------------------------------------------ */
/* public api                                                         */

/**
 * spice_usb_device_widget_new:
 * @session: #SpiceSession for which to widget will control USB redirection
 * @device_format_string: (allow-none): String passed to
 * spice_usb_device_get_description()
 *
 * Creates a new widget to control USB redirection.
 *
 * Returns: a new #SpiceUsbDeviceWidget instance
 */
GtkWidget *spice_usb_device_widget_new(SpiceSession    *session,
                                       const gchar     *device_format_string)
{
    return g_object_new(SPICE_TYPE_USB_DEVICE_WIDGET,
                        "orientation", GTK_ORIENTATION_VERTICAL,
                        "session", session,
                        "device-format-string", device_format_string,
                        "spacing", 6,
                        NULL);
}

/* ------------------------------------------------------------------ */
/* callbacks                                                          */

static gboolean usb_widget_tree_store_check_redirect_foreach_cb(GtkTreeModel *tree_model,
                                                                GtkTreePath *path, GtkTreeIter *iter,
                                                                gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    SpiceUsbDevice *usb_device;
    gboolean is_lun_item;

    gtk_tree_model_get(tree_model, iter,
                       COL_LUN_ITEM, &is_lun_item,
                       COL_ITEM_DATA, (gpointer *)&usb_device,
                       -1);
    if (!is_lun_item) {
        gboolean can_redirect;

        if (spice_usb_device_manager_is_redirecting(priv->manager)) {
            can_redirect = FALSE;
        } else {
            GError *err = NULL;

            can_redirect = spice_usb_device_manager_can_redirect_device(priv->manager,
                                                                        usb_device, &err);

            /* If we cannot redirect this device, append the error message to
               err_msg, but only if it is *not* already there! */
            if (!can_redirect) {
                if (priv->err_msg) {
                    if (!strstr(priv->err_msg, err->message)) {
                        gchar *old_err_msg = priv->err_msg;
                        priv->err_msg = g_strdup_printf("%s\n%s", priv->err_msg,
                                                        err->message);
                        g_free(old_err_msg);
                    }
                } else {
                    priv->err_msg = g_strdup(err->message);
                }
            }
            g_clear_error(&err);
        }
        gtk_tree_store_set(priv->tree_store, iter,
                           COL_CAN_REDIRECT, TRUE, //can_redirect,
                           -1);
    }
    return FALSE; /* continue iterating */
}

static gboolean spice_usb_device_widget_update_status(gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    gchar *str, *markup_str;
    const gchar *free_channels_str;
    int free_channels;

    g_object_get(priv->manager, "free-channels", &free_channels, NULL);
    free_channels_str = ngettext(_("Select USB devices to redirect (%d free channel)"),
                                 _("Select USB devices to redirect (%d free channels)"),
                                 free_channels);
    str = g_strdup_printf(free_channels_str, free_channels);
    markup_str = g_strdup_printf("<b>%s</b>", str);
    gtk_label_set_markup(GTK_LABEL (priv->label), markup_str);
    g_free(markup_str);
    g_free(str);

    gtk_tree_model_foreach(GTK_TREE_MODEL(priv->tree_store),
                           usb_widget_tree_store_check_redirect_foreach_cb, self);

    gtk_widget_show_all(GTK_WIDGET(priv->tree_view));

    /* Show messages in the info, if necessary */
    if (priv->err_msg) {
        spice_usb_device_widget_show_info_bar(self, priv->err_msg,
                                              GTK_MESSAGE_INFO, priv->icon_warning);
        g_free(priv->err_msg);
        priv->err_msg = NULL;
    } else if ( spice_usb_device_manager_is_redirecting(priv->manager)) {
        spice_usb_device_widget_show_info_bar(self, _("Redirecting USB Device..."),
                                              GTK_MESSAGE_INFO, priv->icon_info);
    } else {
        spice_usb_device_widget_hide_info_bar(self);
    }

    if (priv->device_count == 0)
        spice_usb_device_widget_show_info_bar(self, _("No USB devices detected"),
                                              GTK_MESSAGE_INFO, priv->icon_info);

    return FALSE;
}

#if 0
static void checkbox_usb_device_destroy_notify(gpointer user_data)
{
    g_boxed_free(spice_usb_device_get_type(), user_data);
}
#endif

