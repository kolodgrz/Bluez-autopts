/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation
 *  Copyright (C) 2010  Marcel Holtmann <marcel@holtmann.org>
 *
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

#include <errno.h>
#include <stdlib.h>
#include <glib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "adapter.h"
#include "device.h"
#include "log.h"
#include "gdbus.h"
#include "error.h"
#include "glib-helper.h"
#include "dbus-common.h"
#include "btio.h"
#include "storage.h"

#include "att.h"
#include "gattrib.h"
#include "gatt.h"
#include "client.h"

#define CHAR_INTERFACE "org.bluez.Characteristic"

struct gatt_service {
	struct btd_device *dev;
	bdaddr_t sba;
	bdaddr_t dba;
	char *path;
	GSList *primary;
	GAttrib *attrib;
	int psm;
	guint atid;
	gboolean listen;
};

struct format {
	guint8 format;
	guint8 exponent;
	guint16 unit;
	guint8 namespace;
	guint16 desc;
} __attribute__ ((packed));

struct primary {
	struct gatt_service *gatt;
	char *path;
	uuid_t uuid;
	uint16_t start;
	uint16_t end;
	GSList *chars;
	GSList *watchers;
};

struct characteristic {
	struct primary *prim;
	char *path;
	uint16_t handle;
	uint16_t end;
	uint8_t perm;
	uuid_t type;
	char *name;
	char *desc;
	struct format *format;
	uint8_t *value;
	size_t vlen;
};

struct query_data {
	struct primary *prim;
	struct characteristic *chr;
	uint16_t handle;
};

struct watcher {
	guint id;
	char *name;
	char *path;
	struct primary *prim;
};

static GSList *gatt_services = NULL;

static DBusConnection *connection;

static void characteristic_free(void *user_data)
{
	struct characteristic *chr = user_data;

	g_free(chr->path);
	g_free(chr->desc);
	g_free(chr->format);
	g_free(chr->value);
	g_free(chr->name);
	g_free(chr);
}

static void watcher_free(void *user_data)
{
	struct watcher *watcher = user_data;

	g_free(watcher->path);
	g_free(watcher->name);
	g_free(watcher);
}

static void primary_free(void *user_data)
{
	struct primary *prim = user_data;
	GSList *l;

	for (l = prim->watchers; l; l = l->next) {
		struct watcher *watcher = l->data;
		g_dbus_remove_watch(connection, watcher->id);
	}

	g_slist_foreach(prim->chars, (GFunc) characteristic_free, NULL);
	g_slist_free(prim->chars);
	g_free(prim->path);
	g_free(prim);
}

static void gatt_service_free(void *user_data)
{
	struct gatt_service *gatt = user_data;

	g_slist_foreach(gatt->primary, (GFunc) primary_free, NULL);
	g_slist_free(gatt->primary);
	g_attrib_unref(gatt->attrib);
	g_free(gatt->path);
	g_free(gatt);
}

static int gatt_dev_cmp(gconstpointer a, gconstpointer b)
{
	const struct gatt_service *gatt = a;
	const struct btd_device *dev = b;

	return gatt->dev == dev;
}

static int characteristic_handle_cmp(gconstpointer a, gconstpointer b)
{
	const struct characteristic *chr = a;
	uint16_t handle = GPOINTER_TO_UINT(b);

	return chr->handle - handle;
}

static int watcher_cmp(gconstpointer a, gconstpointer b)
{
	const struct watcher *watcher = a;
	const struct watcher *match = b;
	int ret;

	ret = g_strcmp0(watcher->name, match->name);
	if (ret != 0)
		return ret;

	return g_strcmp0(watcher->path, match->path);
}

static inline DBusMessage *invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".InvalidArguments",
			"Invalid arguments in method call");
}

static inline DBusMessage *not_authorized(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".NotAuthorized",
			"Not authorized");
}

static void append_char_dict(DBusMessageIter *iter, struct characteristic *chr)
{
	DBusMessageIter dict;
	const char *name = "";
	char *uuid;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	uuid = bt_uuid2string(&chr->type);
	dict_append_entry(&dict, "UUID", DBUS_TYPE_STRING, &uuid);
	g_free(uuid);

	/* FIXME: Translate UUID to name. */
	dict_append_entry(&dict, "Name", DBUS_TYPE_STRING, &name);

	if (chr->desc)
		dict_append_entry(&dict, "Description", DBUS_TYPE_STRING,
								&chr->desc);

	if (chr->value)
		dict_append_array(&dict, "Value", DBUS_TYPE_BYTE, &chr->value,
								chr->vlen);

	/* FIXME: Missing Format, Value and Representation */

	dbus_message_iter_close_container(iter, &dict);
}

static void watcher_exit(DBusConnection *conn, void *user_data)
{
	struct watcher *watcher = user_data;
	struct primary *prim = watcher->prim;
	struct gatt_service *gatt = prim->gatt;

	DBG("%s watcher %s exited", prim->path, watcher->name);

	prim->watchers = g_slist_remove(prim->watchers, watcher);

	g_attrib_unref(gatt->attrib);
}

static int characteristic_set_value(struct characteristic *chr,
					const uint8_t *value, size_t vlen)
{
	chr->value = g_try_realloc(chr->value, vlen);
	if (chr->value == NULL)
		return -ENOMEM;

	memcpy(chr->value, value, vlen);
	chr->vlen = vlen;

	return 0;
}

static void update_watchers(gpointer data, gpointer user_data)
{
	struct watcher *w = data;
	struct characteristic *chr = user_data;
	DBusMessage *msg;

	msg = dbus_message_new_method_call(w->name, w->path,
				"org.bluez.Watcher", "ValueChanged");
	if (msg == NULL)
		return;

	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &chr->path,
			DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			&chr->value, chr->vlen, DBUS_TYPE_INVALID);

	dbus_message_set_no_reply(msg, TRUE);
	g_dbus_send_message(connection, msg);
}

static void events_handler(const uint8_t *pdu, uint16_t len,
							gpointer user_data)
{
	struct gatt_service *gatt = user_data;
	struct characteristic *chr;
	struct primary *prim;
	GSList *lprim, *lchr;
	uint8_t opdu[ATT_MAX_MTU];
	guint handle = att_get_u16(&pdu[1]);
	uint16_t olen;

	for (lprim = gatt->primary, prim = NULL, chr = NULL; lprim;
						lprim = lprim->next) {
		prim = lprim->data;

		lchr = g_slist_find_custom(prim->chars,
			GUINT_TO_POINTER(handle), characteristic_handle_cmp);
		if (lchr) {
			chr = lchr->data;
			break;
		}
	}

	if (chr == NULL) {
		DBG("Attribute handle 0x%02x not found", handle);
		return;
	}

	switch (pdu[0]) {
	case ATT_OP_HANDLE_IND:
		olen = enc_confirmation(opdu, sizeof(opdu));
		g_attrib_send(gatt->attrib, opdu[0], opdu, olen,
						NULL, NULL, NULL);
	case ATT_OP_HANDLE_NOTIFY:
		if (characteristic_set_value(chr, &pdu[3], len - 3) < 0)
			DBG("Can't change Characteristic %0x02x", handle);

		g_slist_foreach(prim->watchers, update_watchers, chr);
		break;
	}
}

static void primary_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data);

static void attrib_destroy(gpointer user_data)
{
	struct gatt_service *gatt = user_data;

	gatt->attrib = NULL;
}

static void attrib_disconnect(gpointer user_data)
{
	struct gatt_service *gatt = user_data;

	/* Remote initiated disconnection only */
	g_attrib_unref(gatt->attrib);
}

static void connect_cb(GIOChannel *chan, GError *gerr, gpointer user_data)
{
	struct gatt_service *gatt = user_data;
	guint atid;

	if (gerr) {
		error("%s", gerr->message);
		goto fail;
	}

	if (gatt->attrib == NULL)
		return;

	/* Listen mode: used for notification and indication */
	if (gatt->listen == TRUE) {
		g_attrib_register(gatt->attrib,
					ATT_OP_HANDLE_NOTIFY,
					events_handler, gatt, NULL);
		g_attrib_register(gatt->attrib,
					ATT_OP_HANDLE_IND,
					events_handler, gatt, NULL);
		return;
	}

	atid = gatt_discover_primary(gatt->attrib, 0x0001, 0xffff, primary_cb,
									gatt);
	if (atid == 0)
		goto fail;

	gatt->atid = atid;

	return;
fail:
	g_attrib_unref(gatt->attrib);
}

static DBusMessage *get_characteristics(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct primary *prim = data;
	DBusMessage *reply;
	DBusMessageIter iter, array;
	GSList *l;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_OBJECT_PATH_AS_STRING
			DBUS_TYPE_ARRAY_AS_STRING
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &array);

	for (l = prim->chars; l; l = l->next) {
		struct characteristic *chr = l->data;
		DBusMessageIter sub;

		DBG("path %s", chr->path);

		dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY,
								NULL, &sub);

		dbus_message_iter_append_basic(&sub, DBUS_TYPE_OBJECT_PATH,
								&chr->path);

		append_char_dict(&sub, chr);

		dbus_message_iter_close_container(&array, &sub);
	}

	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static int l2cap_connect(struct gatt_service *gatt, GError **gerr,
								gboolean listen)
{
	GIOChannel *io;

	if (gatt->attrib != NULL) {
		gatt->attrib = g_attrib_ref(gatt->attrib);
		return 0;
	}

	/*
	 * FIXME: If the service doesn't support Client Characteristic
	 * Configuration it is necessary to poll the server from time
	 * to time checking for modifications.
	 */
	io = bt_io_connect(BT_IO_L2CAP, connect_cb, gatt, NULL, gerr,
			BT_IO_OPT_SOURCE_BDADDR, &gatt->sba,
			BT_IO_OPT_DEST_BDADDR, &gatt->dba,
			BT_IO_OPT_PSM, gatt->psm,
			BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
			BT_IO_OPT_INVALID);
	if (!io)
		return -1;

	gatt->attrib = g_attrib_new(io);
	g_io_channel_unref(io);
	gatt->listen = listen;

	g_attrib_set_destroy_function(gatt->attrib, attrib_destroy, gatt);
	g_attrib_set_disconnect_function(gatt->attrib, attrib_disconnect,
									gatt);

	return 0;
}

static DBusMessage *register_watcher(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	struct primary *prim = data;
	struct watcher *watcher;
	GError *gerr = NULL;
	char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return invalid_args(msg);

	if (l2cap_connect(prim->gatt, &gerr, TRUE) < 0) {
		DBusMessage *reply;
		reply = g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
							"%s", gerr->message);
		g_error_free(gerr);

		return reply;
	}

	watcher = g_new0(struct watcher, 1);
	watcher->name = g_strdup(sender);
	watcher->prim = prim;
	watcher->path = g_strdup(path);
	watcher->id = g_dbus_add_disconnect_watch(conn, sender, watcher_exit,
							watcher, watcher_free);

	prim->watchers = g_slist_append(prim->watchers, watcher);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *unregister_watcher(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	struct primary *prim = data;
	struct watcher *watcher, *match;
	GSList *l;
	char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return invalid_args(msg);

	match = g_new0(struct watcher, 1);
	match->name = g_strdup(sender);
	match->path = g_strdup(path);
	l = g_slist_find_custom(prim->watchers, match, watcher_cmp);
	watcher_free(match);
	if (!l)
		return not_authorized(msg);

	watcher = l->data;
	g_dbus_remove_watch(conn, watcher->id);
	prim->watchers = g_slist_remove(prim->watchers, watcher);
	watcher_free(watcher);

	return dbus_message_new_method_return(msg);
}

static GDBusMethodTable prim_methods[] = {
	{ "GetCharacteristics",	"",	"a{oa{sv}}", get_characteristics},
	{ "RegisterCharacteristicsWatcher",	"o", "",
						register_watcher	},
	{ "UnregisterCharacteristicsWatcher",	"o", "",
						unregister_watcher	},
	{ }
};

static DBusMessage *set_value(DBusConnection *conn, DBusMessage *msg,
			DBusMessageIter *iter, struct characteristic *chr)
{
	struct gatt_service *gatt = chr->prim->gatt;
	DBusMessageIter sub;
	GError *gerr = NULL;
	uint8_t *value;
	int len;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY ||
			dbus_message_iter_get_element_type(iter) != DBUS_TYPE_BYTE)
		return invalid_args(msg);

	dbus_message_iter_recurse(iter, &sub);

	dbus_message_iter_get_fixed_array(&sub, &value, &len);

	if (l2cap_connect(gatt, &gerr, FALSE) < 0) {
		DBusMessage *reply;
		reply = g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
							"%s", gerr->message);
		g_error_free(gerr);

		return reply;
	}

	gatt_write_cmd(gatt->attrib, chr->handle, value, len, NULL, NULL);

	characteristic_set_value(chr, value, len);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *get_properties(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct characteristic *chr = data;
	DBusMessage *reply;
	DBusMessageIter iter;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	append_char_dict(&iter, chr);

	return reply;
}

static DBusMessage *set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct characteristic *chr = data;
	DBusMessageIter iter;
	DBusMessageIter sub;
	const char *property;

	if (!dbus_message_iter_init(msg, &iter))
		return invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return invalid_args(msg);

	dbus_message_iter_recurse(&iter, &sub);

	if (g_str_equal("Value", property))
		return set_value(conn, msg, &sub, chr);

	return invalid_args(msg);
}

static GDBusMethodTable char_methods[] = {
	{ "GetProperties",	"",	"a{sv}", get_properties },
	{ "SetProperty",	"sv",	"",	set_property,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ }
};

static void register_primary(struct gatt_service *gatt)
{
	GSList *l;

	for (l = gatt->primary; l; l = l->next) {
		struct primary *prim = l->data;
		g_dbus_register_interface(connection, prim->path,
				CHAR_INTERFACE, prim_methods,
				NULL, NULL, prim, NULL);
		DBG("Registered: %s", prim->path);

		device_add_service(gatt->dev, prim->path);
	}
}

static char *characteristic_list_to_string(GSList *chars)
{
	GString *characteristics;
	GSList *l;

	characteristics = g_string_new(NULL);

	for (l = chars; l; l = l->next) {
		struct characteristic *chr = l->data;
		uuid_t *uuid128;
		char chr_str[64];
		char uuidstr[MAX_LEN_UUID_STR];

		memset(chr_str, 0, sizeof(chr_str));

		uuid128 = sdp_uuid_to_uuid128(&chr->type);
		sdp_uuid2strn(uuid128, uuidstr, MAX_LEN_UUID_STR);

		bt_free(uuid128);

		snprintf(chr_str, sizeof(chr_str), "%04X#%02X#%04X#%s ",
				chr->handle, chr->perm, chr->end, uuidstr);

		characteristics = g_string_append(characteristics, chr_str);
	}

	return g_string_free(characteristics, FALSE);
}

static void store_characteristics(struct gatt_service *gatt,
							struct primary *prim)
{
	char *characteristics;

	characteristics = characteristic_list_to_string(prim->chars);

	write_device_characteristics(&gatt->sba, &gatt->dba, prim->start,
							characteristics);

	g_free(characteristics);
}

static void register_characteristics(struct primary *prim)
{
	GSList *lc;

	for (lc = prim->chars; lc; lc = lc->next) {
		struct characteristic *chr = lc->data;
		g_dbus_register_interface(connection, chr->path,
				CHAR_INTERFACE, char_methods,
				NULL, NULL, chr, NULL);
		DBG("Registered: %s", chr->path);
	}
}

static GSList *string_to_characteristic_list(struct primary *prim,
							const char *str)
{
	GSList *l = NULL;
	char **chars;
	int i;

	if (str == NULL)
		return NULL;

	chars = g_strsplit(str, " ", 0);
	if (chars == NULL)
		return NULL;

	for (i = 0; chars[i]; i++) {
		struct characteristic *chr;
		char uuidstr[MAX_LEN_UUID_STR + 1];
		int ret;

		chr = g_new0(struct characteristic, 1);

		ret = sscanf(chars[i], "%04hX#%02hhX#%04hX#%s", &chr->handle,
				&chr->perm, &chr->end, uuidstr);
		if (ret < 4) {
			g_free(chr);
			continue;
		}

		chr->prim = prim;
		chr->path = g_strdup_printf("%s/characteristic%04x",
						prim->path, chr->handle);

		bt_string2uuid(&chr->type, uuidstr);

		l = g_slist_append(l, chr);
	}

	g_strfreev(chars);

	return l;
}

static void load_characteristics(gpointer data, gpointer user_data)
{
	struct primary *prim = data;
	struct gatt_service *gatt = user_data;
	GSList *chrs_list;
	char *str;

	if (prim->chars) {
		DBG("Characteristics already loaded");
		return;
	}

	str = read_device_characteristics(&gatt->sba, &gatt->dba, prim->start);
	if (str == NULL)
		return;

	chrs_list = string_to_characteristic_list(prim, str);

	free(str);

	if (chrs_list == NULL)
		return;

	prim->chars = chrs_list;
	register_characteristics(prim);

	return;
}

static void store_attribute(struct gatt_service *gatt, uint16_t handle,
				uint16_t type, uint8_t *value, gsize len)
{
	uuid_t uuid;
	char *str, *uuidstr, *tmp;
	guint i;

	str = g_malloc0(MAX_LEN_UUID_STR + len * 2 + 1);

	sdp_uuid16_create(&uuid, type);
	uuidstr = bt_uuid2string(&uuid);
	strcpy(str, uuidstr);
	g_free(uuidstr);

	str[MAX_LEN_UUID_STR - 1] = '#';

	for (i = 0, tmp = str + MAX_LEN_UUID_STR; i < len; i++, tmp += 2)
		sprintf(tmp, "%02X", value[i]);

	write_device_attribute(&gatt->sba, &gatt->dba, handle, str);
	g_free(str);
}

static void update_char_desc(guint8 status, const guint8 *pdu, guint16 len,
							gpointer user_data)
{
	struct query_data *current = user_data;
	struct gatt_service *gatt = current->prim->gatt;
	struct characteristic *chr = current->chr;

	if (status != 0)
		goto done;

	g_free(chr->desc);

	chr->desc = g_malloc(len);
	memcpy(chr->desc, pdu + 1, len - 1);
	chr->desc[len - 1] = '\0';

	store_attribute(gatt, current->handle, GATT_CHARAC_USER_DESC_UUID,
						(void *) chr->desc, len);
done:
	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static void update_char_format(guint8 status, const guint8 *pdu, guint16 len,
								gpointer user_data)
{
	struct query_data *current = user_data;
	struct gatt_service *gatt = current->prim->gatt;
	struct characteristic *chr = current->chr;

	if (status != 0)
		goto done;

	if (len < 8)
		goto done;

	g_free(chr->format);

	chr->format = g_new0(struct format, 1);
	memcpy(chr->format, pdu + 1, 7);

	store_attribute(gatt, current->handle, GATT_CHARAC_FMT_UUID,
				(void *) chr->format, sizeof(*chr->format));

done:
	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static void update_char_value(guint8 status, const guint8 *pdu,
					guint16 len, gpointer user_data)
{
	struct query_data *current = user_data;
	struct gatt_service *gatt = current->prim->gatt;
	struct characteristic *chr = current->chr;

	if (status == 0)
		characteristic_set_value(chr, pdu + 1, len - 1);

	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static int uuid_desc16_cmp(uuid_t *uuid, guint16 desc)
{
	uuid_t u16;

	sdp_uuid16_create(&u16, desc);

	return sdp_uuid_cmp(uuid, &u16);
}

static void descriptor_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	struct query_data *current = user_data;
	struct gatt_service *gatt = current->prim->gatt;
	struct att_data_list *list;
	guint8 format;
	int i;

	if (status != 0)
		goto done;

	DBG("Find Information Response received");

	list = dec_find_info_resp(pdu, plen, &format);
	if (list == NULL)
		goto done;

	for (i = 0; i < list->num; i++) {
		guint16 handle;
		uuid_t uuid;
		uint8_t *info = list->data[i];
		struct query_data *qfmt;

		handle = att_get_u16(info);

		if (format == 0x01) {
			sdp_uuid16_create(&uuid, att_get_u16(&info[2]));
		} else {
			/* Currently, only "user description" and "presentation
			 * format" descriptors are used, and both have 16-bit
			 * UUIDs. Therefore there is no need to support format
			 * 0x02 yet. */
			continue;
		}
		qfmt = g_new0(struct query_data, 1);
		qfmt->prim = current->prim;
		qfmt->chr = current->chr;
		qfmt->handle = handle;

		if (uuid_desc16_cmp(&uuid, GATT_CHARAC_USER_DESC_UUID) == 0) {
			gatt->attrib = g_attrib_ref(gatt->attrib);
			gatt_read_char(gatt->attrib, handle, update_char_desc,
									qfmt);
		} else if (uuid_desc16_cmp(&uuid, GATT_CHARAC_FMT_UUID) == 0) {
			gatt->attrib = g_attrib_ref(gatt->attrib);
			gatt_read_char(gatt->attrib, handle,
						update_char_format, qfmt);
		} else
			g_free(qfmt);
	}

	att_data_list_free(list);
done:
	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static void update_all_chars(gpointer data, gpointer user_data)
{
	struct query_data *qdesc, *qvalue;
	struct characteristic *chr = data;
	struct primary *prim = user_data;
	struct gatt_service *gatt = prim->gatt;

	qdesc = g_new0(struct query_data, 1);
	qdesc->prim = prim;
	qdesc->chr = chr;

	gatt->attrib = g_attrib_ref(gatt->attrib);
	gatt_find_info(gatt->attrib, chr->handle + 1, chr->end, descriptor_cb,
									qdesc);

	qvalue = g_new0(struct query_data, 1);
	qvalue->prim = prim;
	qvalue->chr = chr;

	gatt->attrib = g_attrib_ref(gatt->attrib);
	gatt_read_char(gatt->attrib, chr->handle, update_char_value, qvalue);
}

static void char_discovered_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	struct query_data *current = user_data;
	struct primary *prim = current->prim;
	struct gatt_service *gatt = prim->gatt;
	struct att_data_list *list;
	uint16_t last, *previous_end = NULL;
	int i;

	if (status == ATT_ECODE_ATTR_NOT_FOUND)
		goto done;

	if (status != 0) {
		DBG("Discover all characteristics failed: %s",
						att_ecode2str(status));

		goto fail;
	}

	DBG("Read by Type Response received");

	list = dec_read_by_type_resp(pdu, plen);
	if (list == NULL)
		goto fail;

	for (i = 0, last = 0; i < list->num; i++) {
		uint8_t *decl = list->data[i];
		struct characteristic *chr;

		chr = g_new0(struct characteristic, 1);
		chr->prim = prim;
		chr->perm = decl[2];
		chr->handle = att_get_u16(&decl[3]);
		chr->path = g_strdup_printf("%s/characteristic%04x",
						prim->path, chr->handle);
		if (list->len == 7) {
			sdp_uuid16_create(&chr->type,
					att_get_u16(&decl[5]));
		} else
			sdp_uuid128_create(&chr->type, &decl[5]);

		if (previous_end) {
			*previous_end = att_get_u16(decl);
		}

		last = chr->handle;
		previous_end = &chr->end;

		prim->chars = g_slist_append(prim->chars, chr);
	}

	if (previous_end)
		*previous_end = prim->end;

	att_data_list_free(list);

	if (last >= prim->end)
		goto done;

	/* Fetch remaining characteristics for the CURRENT primary service */
	gatt_discover_char(gatt->attrib, last + 1, prim->end,
						char_discovered_cb, current);

	return;

done:
	store_characteristics(gatt, prim);
	register_characteristics(prim);

	g_slist_foreach(prim->chars, update_all_chars, prim);

fail:
	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static void *attr_data_from_string(const char *str)
{
	uint8_t *data;
	int size, i;
	char tmp[3];

	size = strlen(str) / 2;
	data = g_try_malloc0(size);
	if (data == NULL)
		return NULL;

	tmp[2] = '\0';
	for (i = 0; i < size; i++) {
		memcpy(tmp, str + (i * 2), 2);
		data[i] = (uint8_t) strtol(tmp, NULL, 16);
	}

	return data;
}

static int find_primary(gconstpointer a, gconstpointer b)
{
	const struct primary *primary = a;
	uint16_t handle = GPOINTER_TO_UINT(b);

	if (handle < primary->start)
		return -1;

	if (handle > primary->end)
		return -1;

	return 0;
}

static int find_characteristic(gconstpointer a, gconstpointer b)
{
	const struct characteristic *chr = a;
	uint16_t handle = GPOINTER_TO_UINT(b);

	if (handle < chr->handle)
		return -1;

	if (handle > chr->end)
		return -1;

	return 0;
}

static void load_attribute_data(char *key, char *value, void *data)
{
	struct gatt_service *gatt = data;
	struct characteristic *chr;
	struct primary *primary;
	char addr[18], dst[18];
	uint16_t handle;
	uuid_t uuid;
	GSList *l;
	guint h;

	if (sscanf(key, "%17s#%04hX", addr, &handle) < 2)
		return;

	ba2str(&gatt->dba, dst);

	if (strcmp(addr, dst) != 0)
		return;

	h = handle;

	l = g_slist_find_custom(gatt->primary, GUINT_TO_POINTER(h),
								find_primary);
	if (!l)
		return;

	primary = l->data;

	l = g_slist_find_custom(primary->chars, GUINT_TO_POINTER(h),
							find_characteristic);
	if (!l)
		return;

	chr = l->data;

	/* value[] contains "<UUID>#<data>", but bt_string2uuid() expects a
	 * string containing only the UUID. To avoid creating a new buffer,
	 * "truncate" the string in place before calling bt_string2uuid(). */
	value[MAX_LEN_UUID_STR - 1] = '\0';
	if (bt_string2uuid(&uuid, value) < 0)
		return;

	/* Fill the characteristic field according to the attribute type. */
	if (uuid_desc16_cmp(&uuid, GATT_CHARAC_USER_DESC_UUID) == 0)
		chr->desc = attr_data_from_string(value + MAX_LEN_UUID_STR);
	else if (uuid_desc16_cmp(&uuid, GATT_CHARAC_FMT_UUID) == 0)
		chr->format = attr_data_from_string(value + MAX_LEN_UUID_STR);
}

static char *primary_list_to_string(GSList *primary_list)
{
	GString *services;
	GSList *l;

	services = g_string_new(NULL);

	for (l = primary_list; l; l = l->next) {
		struct primary *primary = l->data;
		uuid_t *uuid128;
		char service[64];
		char uuidstr[MAX_LEN_UUID_STR];

		memset(service, 0, sizeof(service));

		uuid128 = sdp_uuid_to_uuid128(&primary->uuid);
		sdp_uuid2strn(uuid128, uuidstr, MAX_LEN_UUID_STR);

		bt_free(uuid128);

		snprintf(service, sizeof(service), "%04X#%04X#%s ",
				primary->start, primary->end, uuidstr);

		services = g_string_append(services, service);
	}

	return g_string_free(services, FALSE);
}

static GSList *string_to_primary_list(struct gatt_service *gatt,
							const char *str)
{
	GSList *l = NULL;
	char **services;
	int i;

	if (str == NULL)
		return NULL;

	services = g_strsplit(str, " ", 0);
	if (services == NULL)
		return NULL;

	for (i = 0; services[i]; i++) {
		struct primary *prim;
		char uuidstr[MAX_LEN_UUID_STR + 1];
		int ret;

		prim = g_new0(struct primary, 1);
		prim->gatt = gatt;

		ret = sscanf(services[i], "%04hX#%04hX#%s", &prim->start,
							&prim->end, uuidstr);

		if (ret < 3) {
			g_free(prim);
			continue;
		}

		prim->path = g_strdup_printf("%s/service%04x", gatt->path,
								prim->start);

		bt_string2uuid(&prim->uuid, uuidstr);

		l = g_slist_append(l, prim);
	}

	g_strfreev(services);

	return l;
}

static void store_primary_services(struct gatt_service *gatt)
{
       char *services;

       services = primary_list_to_string(gatt->primary);

       write_device_services(&gatt->sba, &gatt->dba, services);

       g_free(services);
}

static gboolean load_primary_services(struct gatt_service *gatt)
{
	GSList *primary_list;
	char *str;

	if (gatt->primary) {
		DBG("Services already loaded");
		return FALSE;
	}

	str = read_device_services(&gatt->sba, &gatt->dba);
	if (str == NULL)
		return FALSE;

	primary_list = string_to_primary_list(gatt, str);

	free(str);

	if (primary_list == NULL)
		return FALSE;

	gatt->primary = primary_list;
	register_primary(gatt);

	g_slist_foreach(gatt->primary, load_characteristics, gatt);
	read_device_attributes(&gatt->sba, load_attribute_data, gatt);

	return TRUE;
}

static void discover_all_char(gpointer data, gpointer user_data)
{
	struct query_data *qchr;
	struct gatt_service *gatt = user_data;
	struct primary *prim = data;

	qchr = g_new0(struct query_data, 1);
	qchr->prim = prim;

	gatt->attrib = g_attrib_ref(gatt->attrib);
	gatt_discover_char(gatt->attrib, prim->start, prim->end,
						char_discovered_cb, qchr);
}

static void primary_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	struct gatt_service *gatt = user_data;
	struct att_data_list *list;
	unsigned int i;
	uint16_t end, start;

	if (status == ATT_ECODE_ATTR_NOT_FOUND) {
		if (gatt->primary == NULL)
			goto done;

		store_primary_services(gatt);
		register_primary(gatt);

		g_slist_foreach(gatt->primary, discover_all_char, gatt);
		goto done;
	}

	if (status != 0) {
		error("Discover all primary services failed: %s",
						att_ecode2str(status));
		goto done;
	}

	list = dec_read_by_grp_resp(pdu, plen);
	if (list == NULL) {
		error("Protocol error");
		goto done;
	}

	DBG("Read by Group Type Response received");

	for (i = 0, end = 0; i < list->num; i++) {
		struct primary *prim;
		uint8_t *info = list->data[i];

		/* Each element contains: attribute handle, end group handle
		 * and attribute value */
		start = att_get_u16(info);
		end = att_get_u16(&info[2]);

		prim = g_new0(struct primary, 1);
		prim->gatt = gatt;
		prim->start = start;
		prim->end = end;

		if (list->len == 6) {
			sdp_uuid16_create(&prim->uuid,
					att_get_u16(&info[4]));

		} else if (list->len == 20) {
			/* FIXME: endianness */
			sdp_uuid128_create(&prim->uuid, &info[4]);
		} else {
			DBG("ATT: Invalid Length field");
			g_free(prim);
			att_data_list_free(list);
			goto done;
		}

		prim->path = g_strdup_printf("%s/service%04x", gatt->path,
								prim->start);

		gatt->primary = g_slist_append(gatt->primary, prim);
	}

	att_data_list_free(list);

	if (end == 0) {
		DBG("ATT: Invalid PDU format");
		goto done;
	}

	/*
	 * Discover all primary services sub-procedure shall send another
	 * Read by Group Type Request until Error Response is received and
	 * the Error Code is set to Attribute Not Found.
	 */
	gatt->attrib = g_attrib_ref(gatt->attrib);
	gatt->atid = gatt_discover_primary(gatt->attrib,
				end + 1, 0xffff, primary_cb, gatt);
done:
	g_attrib_unref(gatt->attrib);
}

int attrib_client_register(struct btd_device *device, int psm)
{
	struct btd_adapter *adapter = device_get_adapter(device);
	const char *path = device_get_path(device);
	struct gatt_service *gatt;
	GError *gerr = NULL;
	GIOChannel *io;
	bdaddr_t sba, dba;

	/*
	 * Registering fake services/characteristics. The following
	 * paths/interfaces shall be registered after discover primary
	 * services only.
	 */

	adapter_get_address(adapter, &sba);
	device_get_address(device, &dba);

	gatt = g_new0(struct gatt_service, 1);
	gatt->dev = device;
	gatt->listen = FALSE;
	gatt->path = g_strdup(path);
	bacpy(&gatt->sba, &sba);
	bacpy(&gatt->dba, &dba);
	gatt->psm = psm;

	if (load_primary_services(gatt)) {
		DBG("Primary services loaded");
		goto done;
	}

	if (psm < 0) {
		io = bt_io_connect(BT_IO_L2CAP, connect_cb, gatt, NULL, &gerr,
					BT_IO_OPT_SOURCE_BDADDR, &sba,
					BT_IO_OPT_DEST_BDADDR, &dba,
					BT_IO_OPT_CID, GATT_CID,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
					BT_IO_OPT_INVALID);

			DBG("GATT over Low Energy");
	} else {
		io = bt_io_connect(BT_IO_L2CAP, connect_cb, gatt, NULL, &gerr,
					BT_IO_OPT_SOURCE_BDADDR, &sba,
					BT_IO_OPT_DEST_BDADDR, &dba,
					BT_IO_OPT_PSM, psm,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
					BT_IO_OPT_INVALID);

		DBG("GATT over Basic Rate");
	}

	if (!io) {
		error("%s", gerr->message);
		g_error_free(gerr);
		gatt_service_free(gatt);
		return -1;
	}

	gatt->attrib = g_attrib_new(io);
	g_io_channel_unref(io);

	g_attrib_set_destroy_function(gatt->attrib, attrib_destroy, gatt);
	g_attrib_set_disconnect_function(gatt->attrib, attrib_disconnect,
									gatt);

done:
	gatt_services = g_slist_append(gatt_services, gatt);

	return 0;
}

void attrib_client_unregister(struct btd_device *device)
{
	struct gatt_service *gatt;
	GSList *l, *lp, *lc;

	l = g_slist_find_custom(gatt_services, device, gatt_dev_cmp);
	if (!l)
		return;

	gatt = l->data;
	gatt_services = g_slist_remove(gatt_services, gatt);

	for (lp = gatt->primary; lp; lp = lp->next) {
		struct primary *prim = lp->data;
		for (lc = prim->chars; lc; lc = lc->next) {
			struct characteristic *chr = lc->data;
			g_dbus_unregister_interface(connection, chr->path,
								CHAR_INTERFACE);
		}
		g_dbus_unregister_interface(connection, prim->path,
								CHAR_INTERFACE);
	}

	gatt_service_free(gatt);
}

int attrib_client_init(DBusConnection *conn)
{

	connection = dbus_connection_ref(conn);

	/*
	 * FIXME: if the adapter supports BLE start scanning. Temporary
	 * solution, this approach doesn't allow to control scanning based
	 * on the discoverable property.
	 */

	return 0;
}

void attrib_client_exit(void)
{
	dbus_connection_unref(connection);
}
