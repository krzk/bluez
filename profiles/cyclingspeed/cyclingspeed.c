/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2012 Tieto Poland
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gdbus.h>
#include <errno.h>
#include <stdbool.h>
#include <glib.h>
#include <bluetooth/uuid.h>

#include "plugin.h"
#include "adapter.h"
#include "device.h"
#include "profile.h"
#include "dbus-common.h"
#include "error.h"
#include "attrib/gattrib.h"
#include "attrib/att.h"
#include "attrib/gatt.h"
#include "attio.h"
#include "log.h"

#define CYCLINGSPEED_INTERFACE		"org.bluez.CyclingSpeed"
#define CYCLINGSPEED_MANAGER_INTERFACE	"org.bluez.CyclingSpeedManager"

struct csc_adapter {
	struct btd_adapter	*adapter;
	GSList			*devices;	/* list of registered devices */
	GSList			*watchers;
};

struct csc {
	struct btd_device	*dev;
	struct csc_adapter	*cadapter;

	GAttrib			*attrib;
	guint			attioid;

	struct att_range	*svc_range;

	uint16_t		measurement_ccc_handle;
	uint16_t		controlpoint_val_handle;

	uint16_t		feature;
	gboolean		has_location;
	uint8_t			location;
};

struct watcher {
	struct csc_adapter	*cadapter;
	guint			id;
	char			*srv;
	char			*path;
};

struct characteristic {
	struct csc	*csc;
	char		uuid[MAX_LEN_UUID_STR + 1];
};

static GSList *csc_adapters = NULL;

static gint cmp_adapter(gconstpointer a, gconstpointer b)
{
	const struct csc_adapter *cadapter = a;
	const struct btd_adapter *adapter = b;

	if (adapter == cadapter->adapter)
		return 0;

	return -1;
}

static gint cmp_device(gconstpointer a, gconstpointer b)
{
	const struct csc *csc = a;
	const struct btd_device *dev = b;

	if (dev == csc->dev)
		return 0;

	return -1;
}

static gint cmp_watcher(gconstpointer a, gconstpointer b)
{
	const struct watcher *watcher = a;
	const struct watcher *match = b;
	int ret;

	ret = g_strcmp0(watcher->srv, match->srv);
	if (ret != 0)
		return ret;

	return g_strcmp0(watcher->path, match->path);
}

static struct csc_adapter *find_csc_adapter(struct btd_adapter *adapter)
{
	GSList *l = g_slist_find_custom(csc_adapters, adapter, cmp_adapter);

	if (!l)
		return NULL;

	return l->data;
}

static void destroy_watcher(gpointer user_data)
{
	struct watcher *watcher = user_data;

	g_free(watcher->path);
	g_free(watcher->srv);
	g_free(watcher);
}

static struct watcher *find_watcher(GSList *list, const char *sender,
							const char *path)
{
	struct watcher *match;
	GSList *l;

	match = g_new0(struct watcher, 1);
	match->srv = g_strdup(sender);
	match->path = g_strdup(path);

	l = g_slist_find_custom(list, match, cmp_watcher);
	destroy_watcher(match);

	if (l != NULL)
		return l->data;

	return NULL;
}

static void remove_watcher(gpointer user_data)
{
	struct watcher *watcher = user_data;

	g_dbus_remove_watch(btd_get_dbus_connection(), watcher->id);
}

static void destroy_csc_adapter(gpointer user_data)
{
	struct csc_adapter *cadapter = user_data;

	g_slist_free_full(cadapter->watchers, remove_watcher);

	g_free(cadapter);
}

static void destroy_csc(gpointer user_data)
{
	struct csc *csc = user_data;

	if (csc->attioid > 0)
		btd_device_remove_attio_callback(csc->dev, csc->attioid);

	if (csc->attrib != NULL)
		g_attrib_unref(csc->attrib);

	btd_device_unref(csc->dev);
	g_free(csc->svc_range);
	g_free(csc);
}

static void read_feature_cb(guint8 status, const guint8 *pdu,
						guint16 len, gpointer user_data)
{
	struct csc *csc = user_data;
	uint8_t value[2];
	ssize_t vlen;

	if (status) {
		error("CSC Feature read failed: %s", att_ecode2str(status));
		return;
	}

	vlen = dec_read_resp(pdu, len, value, sizeof(value));
	if (vlen < 0) {
		error("Protocol error");
		return;
	}

	if (vlen != sizeof(value)) {
		error("Invalid value length for CSC Feature");
		return;
	}

	csc->feature = att_get_u16(value);
}

static void read_location_cb(guint8 status, const guint8 *pdu,
						guint16 len, gpointer user_data)
{
	struct csc *csc = user_data;
	uint8_t value;
	ssize_t vlen;

	if (status) {
		error("Sensor Location read failed: %s", att_ecode2str(status));
		return;
	}

	vlen = dec_read_resp(pdu, len, &value, sizeof(value));
	if (vlen < 0) {
		error("Protocol error");
		return;
	}

	if (vlen != sizeof(value)) {
		error("Invalid value length for Sensor Location");
		return;
	}

	csc->has_location = TRUE;
	csc->location = value;

	g_dbus_emit_property_changed(btd_get_dbus_connection(),
					device_get_path(csc->dev),
					CYCLINGSPEED_INTERFACE, "Location");
}

static void discover_desc_cb(guint8 status, const guint8 *pdu,
					guint16 len, gpointer user_data)
{
	struct characteristic *ch = user_data;
	struct att_data_list *list = NULL;
	uint8_t format;
	int i;

	if (status != 0) {
		error("Discover %s descriptors failed: %s", ch->uuid,
							att_ecode2str(status));
		goto done;
	}

	list = dec_find_info_resp(pdu, len, &format);
	if (list == NULL)
		goto done;

	if (format != ATT_FIND_INFO_RESP_FMT_16BIT)
		goto done;

	for (i = 0; i < list->num; i++) {
		uint8_t *value;
		uint16_t handle, uuid;

		value = list->data[i];
		handle = att_get_u16(value);
		uuid = att_get_u16(value + 2);

		if (uuid != GATT_CLIENT_CHARAC_CFG_UUID)
			continue;

		if (g_strcmp0(ch->uuid, CSC_MEASUREMENT_UUID) == 0)
			ch->csc->measurement_ccc_handle = handle;

		/* We only want CCC, can break here */
		break;
	}

done:
	if (list)
		att_data_list_free(list);
	g_free(ch);
}

static void discover_desc(struct csc *csc, struct gatt_char *c,
						struct gatt_char *c_next)
{
	struct characteristic *ch;
	uint16_t start, end;

	start = c->value_handle + 1;

	if (c_next != NULL) {
		if (start == c_next->handle)
			return;
		end = c_next->handle - 1;
	} else if (c->value_handle != csc->svc_range->end) {
		end = csc->svc_range->end;
	} else {
		return;
	}

	ch = g_new0(struct characteristic, 1);
	ch->csc = csc;
	memcpy(ch->uuid, c->uuid, sizeof(c->uuid));

	gatt_find_info(csc->attrib, start, end, discover_desc_cb, ch);
}

static void discover_char_cb(GSList *chars, guint8 status, gpointer user_data)
{
	struct csc *csc = user_data;
	uint16_t feature_val_handle = 0;

	if (status) {
		error("Discover CSCS characteristics: %s",
							att_ecode2str(status));
		return;
	}

	for (; chars; chars = chars->next) {
		struct gatt_char *c = chars->data;
		struct gatt_char *c_next =
				(chars->next ? chars->next->data : NULL);

		if (g_strcmp0(c->uuid, CSC_MEASUREMENT_UUID) == 0) {
			discover_desc(csc, c, c_next);
		} else if (g_strcmp0(c->uuid, CSC_FEATURE_UUID) == 0) {
			feature_val_handle = c->value_handle;
		} else if (g_strcmp0(c->uuid, SENSOR_LOCATION_UUID) == 0) {
			DBG("Sensor Location supported");
			gatt_read_char(csc->attrib, c->value_handle,
							read_location_cb, csc);
		} else if (g_strcmp0(c->uuid, SC_CONTROL_POINT_UUID) == 0) {
			DBG("SC Control Point supported");
			csc->controlpoint_val_handle = c->value_handle;
			discover_desc(csc, c, c_next);
		}
	}

	if (feature_val_handle > 0)
		gatt_read_char(csc->attrib, feature_val_handle,
							read_feature_cb, csc);
}

static void attio_connected_cb(GAttrib *attrib, gpointer user_data)
{
	struct csc *csc = user_data;

	DBG("");

	csc->attrib = g_attrib_ref(attrib);

	gatt_discover_char(csc->attrib, csc->svc_range->start,
				csc->svc_range->end, NULL,
				discover_char_cb, csc);
}

static void attio_disconnected_cb(gpointer user_data)
{
	struct csc *csc = user_data;

	DBG("");

	g_attrib_unref(csc->attrib);
	csc->attrib = NULL;
}

static void watcher_exit_cb(DBusConnection *conn, void *user_data)
{
	struct watcher *watcher = user_data;
	struct csc_adapter *cadapter = watcher->cadapter;

	DBG("cycling watcher [%s] disconnected", watcher->path);

	cadapter->watchers = g_slist_remove(cadapter->watchers, watcher);
	g_dbus_remove_watch(conn, watcher->id);
}

static DBusMessage *register_watcher(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct csc_adapter *cadapter = data;
	struct watcher *watcher;
	const char *sender = dbus_message_get_sender(msg);
	char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	watcher = find_watcher(cadapter->watchers, sender, path);
	if (watcher != NULL)
		return btd_error_already_exists(msg);

	watcher = g_new0(struct watcher, 1);
	watcher->cadapter = cadapter;
	watcher->id = g_dbus_add_disconnect_watch(conn, sender, watcher_exit_cb,
						watcher, destroy_watcher);
	watcher->srv = g_strdup(sender);
	watcher->path = g_strdup(path);

	cadapter->watchers = g_slist_prepend(cadapter->watchers, watcher);

	DBG("cycling watcher [%s] registered", path);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *unregister_watcher(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct csc_adapter *cadapter = data;
	struct watcher *watcher;
	const char *sender = dbus_message_get_sender(msg);
	char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	watcher = find_watcher(cadapter->watchers, sender, path);
	if (watcher == NULL)
		return btd_error_does_not_exist(msg);

	cadapter->watchers = g_slist_remove(cadapter->watchers, watcher);
	g_dbus_remove_watch(conn, watcher->id);

	DBG("cycling watcher [%s] unregistered", path);

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable cyclingspeed_manager_methods[] = {
	{ GDBUS_METHOD("RegisterWatcher",
			GDBUS_ARGS({ "agent", "o" }), NULL,
			register_watcher) },
	{ GDBUS_METHOD("UnregisterWatcher",
			GDBUS_ARGS({ "agent", "o" }), NULL,
			unregister_watcher) },
	{ }
};

static int csc_adapter_probe(struct btd_profile *p, struct btd_adapter *adapter)
{
	struct csc_adapter *cadapter;

	cadapter = g_new0(struct csc_adapter, 1);
	cadapter->adapter = adapter;

	csc_adapters = g_slist_prepend(csc_adapters, cadapter);

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
						adapter_get_path(adapter),
						CYCLINGSPEED_MANAGER_INTERFACE,
						cyclingspeed_manager_methods,
						NULL, NULL, cadapter,
						destroy_csc_adapter)) {
		error("D-Bus failed to register %s interface",
						CYCLINGSPEED_MANAGER_INTERFACE);
		destroy_csc_adapter(cadapter);
		return -EIO;
	}

	return 0;
}

static void csc_adapter_remove(struct btd_profile *p,
						struct btd_adapter *adapter)
{
	struct csc_adapter *cadapter;

	cadapter = find_csc_adapter(adapter);
	if (cadapter == NULL)
		return;

	csc_adapters = g_slist_remove(csc_adapters, cadapter);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
					adapter_get_path(cadapter->adapter),
					CYCLINGSPEED_MANAGER_INTERFACE);
}

static gint cmp_primary_uuid(gconstpointer a, gconstpointer b)
{
	const struct gatt_primary *prim = a;
	const char *uuid = b;

	return g_strcmp0(prim->uuid, uuid);
}

static int csc_device_probe(struct btd_profile *p,
				struct btd_device *device, GSList *uuids)
{
	struct btd_adapter *adapter;
	struct csc_adapter *cadapter;
	struct csc *csc;
	struct gatt_primary *prim;
	GSList *primaries;
	GSList *l;

	primaries = btd_device_get_primaries(device);

	l = g_slist_find_custom(primaries, CYCLING_SC_UUID, cmp_primary_uuid);
	if (l == NULL)
		return -EINVAL;

	prim = l->data;

	adapter = device_get_adapter(device);

	cadapter = find_csc_adapter(adapter);
	if (cadapter == NULL)
		return -1;

	csc = g_new0(struct csc, 1);
	csc->dev = btd_device_ref(device);
	csc->cadapter = cadapter;

	csc->svc_range = g_new0(struct att_range, 1);
	csc->svc_range->start = prim->range.start;
	csc->svc_range->end = prim->range.end;

	cadapter->devices = g_slist_prepend(cadapter->devices, csc);

	csc->attioid = btd_device_add_attio_callback(device, attio_connected_cb,
						attio_disconnected_cb, csc);

	return 0;
}

static void csc_device_remove(struct btd_profile *p,
						struct btd_device *device)
{
	struct btd_adapter *adapter;
	struct csc_adapter *cadapter;
	struct csc *csc;
	GSList *l;

	adapter = device_get_adapter(device);

	cadapter = find_csc_adapter(adapter);
	if (cadapter == NULL)
		return;

	l = g_slist_find_custom(cadapter->devices, device, cmp_device);
	if (l == NULL)
		return;

	csc = l->data;

	cadapter->devices = g_slist_remove(cadapter->devices, csc);

	destroy_csc(csc);
}

static struct btd_profile cscp_profile = {
	.name		= "Cycling Speed and Cadence GATT Driver",
	.remote_uuids	= BTD_UUIDS(CYCLING_SC_UUID),

	.adapter_probe	= csc_adapter_probe,
	.adapter_remove	= csc_adapter_remove,

	.device_probe	= csc_device_probe,
	.device_remove	= csc_device_remove,
};

static int cyclingspeed_init(void)
{
	return btd_profile_register(&cscp_profile);
}

static void cyclingspeed_exit(void)
{
	btd_profile_unregister(&cscp_profile);
}

BLUETOOTH_PLUGIN_DEFINE(cyclingspeed, VERSION,
					BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
					cyclingspeed_init, cyclingspeed_exit)
