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

#include "gdbus/gdbus.h"
#include "auto-tester.h"
#include "src/shared/util.h"

#define CONTROLLER_INDEX 0

#define CHECK_BIT(var,pos) ((var) & (1<<(pos)))

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

	rp.current_settings = get_current_settings();

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

	rp.current_settings = get_current_settings();

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
	default:
		send_status(BTP_SERVICE_ID_GAP, op, CONTROLLER_INDEX,
							BTP_STATUS_UNKNOWN_CMD);
	}
}
