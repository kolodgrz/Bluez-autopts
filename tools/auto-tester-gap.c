/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2016  Intel Corporation. All rights reserved.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "gdbus/gdbus.h"
#include "auto-tester.h"
#include "src/shared/util.h"

#define CONTROLLER_INDEX 0

#define CHECK_BIT(var,pos) ((var) & (1<<(pos)))

#define ADVERTISE_PATH "/org/bluez/tester"
#define ADVERTISEMENT_BASE_NAME "org.bluez.LEAdvertisement"

#define AD_TYPE_BROADCAST 0
#define AD_TYPE_PERIPHERAL 1

struct advertise {
	char *name;
	uint8_t type;
	bool incl_tx;
};

static DBusConnection *dbus_conn;

static GSList *advertises = NULL;

static uint32_t gap_settings;

static void supported_commands()
{
	uint8_t cmds[1];
	struct gap_read_supported_commands_rp *rp = (void *) &cmds;

	memset(cmds, 0, sizeof(cmds));

	tester_set_bit(cmds, GAP_READ_SUPPORTED_COMMANDS);

	send_msg(BTP_SERVICE_ID_GAP, GAP_READ_SUPPORTED_COMMANDS,
				CONTROLLER_INDEX, sizeof(cmds), (uint8_t *) rp);
}

static void verify_err(const DBusError *error, void *user_data)
{
	uint8_t op = PTR_TO_UINT(user_data);

	if (dbus_error_is_set(error))
		send_status(BTP_SERVICE_ID_GAP, op, CONTROLLER_INDEX,
							BTP_STATUS_FAILED);
}

static void set_powered_cb()
{
	struct gap_set_powered_rp rp;

	rp.current_settings = gap_settings | get_current_settings();

	send_msg(BTP_SERVICE_ID_GAP, GAP_SET_POWERED, CONTROLLER_INDEX,
						sizeof(rp), (uint8_t *) &rp);
}

static void set_powered(GDBusProxy *adapter_proxy, uint8_t *data, uint16_t len)
{
	struct gap_set_powered_cmd *cmd = (void *) data;
	struct gap_set_powered_rp rp;
	const char *interface;
	dbus_bool_t powered;

	interface = g_dbus_proxy_get_interface(adapter_proxy);
	if (!interface) {
		send_status(BTP_SERVICE_ID_CORE, BTP_STATUS, CONTROLLER_INDEX,
							BTP_STATUS_FAILED);
		return;
	}

	if (cmd->powered)
		powered = TRUE;
	else
		powered = FALSE;

	rp.current_settings = gap_settings | get_current_settings();

	if (CHECK_BIT(rp.current_settings, GAP_SETTINGS_POWERED) == powered) {
		send_msg(BTP_SERVICE_ID_GAP, GAP_SET_POWERED, CONTROLLER_INDEX,
						sizeof(rp), (uint8_t *) &rp);
		return;
	}

	register_prop_cb(interface, "Powered", set_powered_cb);

	g_dbus_proxy_set_property_basic(adapter_proxy, "Powered",
					DBUS_TYPE_BOOLEAN, &powered, verify_err,
					UINT_TO_PTR(GAP_SET_POWERED), NULL);
}

static void adv_reg_setup(DBusMessageIter *iter, void *user_data)
{
	DBusMessageIter opt;
	const char *path = ADVERTISE_PATH;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);

	/* TODO Options are not handled yet */
	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
					&opt);
	dbus_message_iter_close_container(iter, &opt);
}

static void adv_reg_reply(DBusMessage *message, void *user_data)
{
	struct gap_start_advertising_rp rp;
	struct advertise *adv = user_data;
	DBusError error;

	dbus_error_init(&error);

	if (dbus_set_error_from_message(&error, message) == FALSE) {
		rp.current_settings = gap_settings | get_current_settings();
		send_msg(BTP_SERVICE_ID_GAP, GAP_START_ADVERTISING,
				CONTROLLER_INDEX, sizeof(rp), (uint8_t *) &rp);
		return;
	}

	dbus_error_free(&error);
	if (g_dbus_unregister_interface(dbus_conn, ADVERTISE_PATH,
							adv->name) == FALSE) {
		advertises = g_slist_remove(advertises, adv);
		free(adv->name);
		free(adv);

		if (verbose)
			printf("D-Bus failed to unreg %s interface\n",
								adv->name);

		send_status(BTP_SERVICE_ID_GAP, GAP_START_ADVERTISING,
				CONTROLLER_INDEX, BTP_STATUS_FAILED);
	}
}

static gboolean get_adv_type(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct advertise *adv = (struct advertise*) user_data;
	const char *type;

	if (adv->type == AD_TYPE_BROADCAST)
		type = "broadcast";
	else
		type = "peripheral";

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &type);

	return TRUE;
}

/* TODO Implement advertising data getters */
static gboolean ex_svc_uuids(const GDBusPropertyTable *property, void *data)
{
	return FALSE;
}

static gboolean get_svc_uuids(const GDBusPropertyTable *property,
				DBusMessageIter *iter, void *user_data)
{
	return TRUE;
}

static gboolean ex_manf_data(const GDBusPropertyTable *property, void *data)
{
	return FALSE;
}

static gboolean get_manf_data(const GDBusPropertyTable *property,
				DBusMessageIter *iter, void *user_data)
{
	return TRUE;
}

static gboolean ex_sol_uuids(const GDBusPropertyTable *property, void *data)
{
	return FALSE;
}

static gboolean get_sol_uuids(const GDBusPropertyTable *property,
				DBusMessageIter *iter, void *user_data)
{
	return TRUE;
}

static gboolean ex_svc_data(const GDBusPropertyTable *property, void *data)
{
	return FALSE;
}

static gboolean get_svc_data(const GDBusPropertyTable *property,
				DBusMessageIter *iter, void *user_data)
{
	return TRUE;
}

static gboolean ex_incl_tx(const GDBusPropertyTable *property, void *data)
{
	return FALSE;
}

static gboolean get_incl_tx(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	return TRUE;
}

static const GDBusPropertyTable adv_properties[] = {
	{ "Type", "s", get_adv_type },
	{ "ServiceUUIDs", "as", get_svc_uuids, NULL, ex_svc_uuids },
	{ "ManufacturerData", "a{sv}", get_manf_data, NULL, ex_manf_data},
	{ "SolicitUUIDs", "as", get_sol_uuids, NULL, ex_sol_uuids},
	{ "ServiceData", "a{sv}", get_svc_data, NULL, ex_svc_data},
	{ "IncludeTxPower", "b", get_incl_tx, NULL, ex_incl_tx},
	{ }
};

static DBusMessage *release_advertisement(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	if (verbose)
		printf("Advertisement released\n");

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable adv_methods[] = {
	{ GDBUS_METHOD("Release", NULL, NULL, release_advertisement) },
	{ },
};

static void start_advertising(GDBusProxy *adv_proxy, uint8_t *data,
								uint16_t len)
{
	uint32_t s;
	int advertises_cnt;
	struct advertise *adv;
	char advertise_prefix[3], name[30];
	char *path;

	advertises_cnt = g_slist_length(advertises) + 1;
	path = ADVERTISE_PATH;

	/* Currently only one advertising could be registered */
	if (advertises_cnt > 1) {
		if (verbose)
			printf("Cannot alocate more advertisings\n");
		send_status(BTP_SERVICE_ID_GAP, GAP_START_ADVERTISING,
					CONTROLLER_INDEX, BTP_STATUS_FAILED);
		return;
	}

	sprintf(advertise_prefix, "%d", advertises_cnt);

	strcpy(name, ADVERTISEMENT_BASE_NAME);
	strcat(name, advertise_prefix);

	adv = malloc(sizeof(*adv));
	adv->name = strdup(name);

	if (!g_dbus_register_interface(dbus_conn, path, name, adv_methods, NULL,
						adv_properties, adv, NULL)) {
		if (verbose)
			printf("D-Bus failed to register %s interface\n",
								adv->name);
		goto failed;
	}

	/* TODO Add missing advertising data */
	s = gap_settings | get_current_settings();
	adv->type = CHECK_BIT(s, GAP_SETTINGS_CONNECTABLE) ?
					AD_TYPE_PERIPHERAL : AD_TYPE_BROADCAST;

	advertises = g_slist_append(advertises, adv);

	if (g_dbus_proxy_method_call(adv_proxy, "RegisterAdvertisement",
						adv_reg_setup, adv_reg_reply,
						adv, NULL) == FALSE)
		goto failed;

	return;

failed:
	advertises = g_slist_remove(advertises, adv);
	free(adv->name);
	free(adv);
	send_status(BTP_SERVICE_ID_GAP, GAP_START_ADVERTISING, CONTROLLER_INDEX,
							BTP_STATUS_FAILED);
}

static void adv_unreg_setup(DBusMessageIter *iter, void *user_data)
{
	const char *path = ADVERTISE_PATH;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);
}

static void adv_unreg_reply(DBusMessage *message, void *user_data)
{
	struct gap_stop_advertising_rp rp;
	struct advertise *adv = user_data;
	DBusError error;

	dbus_error_init(&error);

	if (dbus_set_error_from_message(&error, message) == FALSE) {
		if (g_dbus_unregister_interface(dbus_conn, ADVERTISE_PATH,
							adv->name) == FALSE)
			goto failed;

		free(adv->name);
		free(adv);
		advertises = g_slist_remove(advertises, adv);
		rp.current_settings = gap_settings | get_current_settings();

		send_msg(BTP_SERVICE_ID_GAP, GAP_STOP_ADVERTISING,
				CONTROLLER_INDEX, sizeof(rp), (uint8_t *) &rp);
		return;
	}

	dbus_error_free(&error);
	if (verbose)
		printf("D-Bus failed to unreg %s interface\n", adv->name);

failed:
	send_status(BTP_SERVICE_ID_GAP, GAP_STOP_ADVERTISING, CONTROLLER_INDEX,
							BTP_STATUS_FAILED);
}

static void stop_advertising(GDBusProxy *adv_proxy, uint8_t *data, uint16_t len)
{
	struct advertise *adv;

	/* TODO Currently only one advertising could be registered */
	adv = (struct advertise*) g_slist_last(advertises)->data;

	if (!adv)
		goto failed;

	if (g_dbus_proxy_method_call(adv_proxy, "UnregisterAdvertisement",
					adv_unreg_setup, adv_unreg_reply,
					adv, NULL) == FALSE)
		goto failed;

	return;

failed:
	send_status(BTP_SERVICE_ID_GAP, GAP_STOP_ADVERTISING,
					CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

uint8_t handle_gap_register(DBusConnection *conn)
{
	dbus_conn = conn;

	return BTP_STATUS_SUCCESS;
}

void handle_gap(GDBusProxy *adapter_proxy, GDBusProxy *adv_proxy, uint8_t op,
						uint8_t *data, uint16_t len)
{
	switch (op) {
	case GAP_READ_SUPPORTED_COMMANDS:
		supported_commands();
		break;
	case GAP_SET_POWERED:
		set_powered(adapter_proxy, data, len);
		break;
	case GAP_START_ADVERTISING:
		start_advertising(adv_proxy, data, len);
		break;
	case GAP_STOP_ADVERTISING:
		stop_advertising(adv_proxy, data, len);
		break;
	default:
		send_status(BTP_SERVICE_ID_GAP, op, CONTROLLER_INDEX,
							BTP_STATUS_UNKNOWN_CMD);
	}
}
