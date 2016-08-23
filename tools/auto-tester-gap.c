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

#define CONTROLLER_INDEX 0

static void supported_commands()
{
	uint8_t cmds[1];
	struct gap_read_supported_commands_rp *rp = (void *) &cmds;

	memset(cmds, 0, sizeof(cmds));

	tester_set_bit(cmds, GAP_READ_SUPPORTED_COMMANDS);

	send_msg(BTP_SERVICE_ID_GAP, GAP_READ_SUPPORTED_COMMANDS,
				CONTROLLER_INDEX, sizeof(cmds), (uint8_t *) rp);
}

void handle_gap(GDBusProxy *adapter_proxy, GDBusProxy *adv_proxy, uint8_t op,
						uint8_t *data, uint16_t len)
{
	switch (op) {
	case GAP_READ_SUPPORTED_COMMANDS:
		supported_commands();
		break;
	default:
		send_status(BTP_SERVICE_ID_GAP, op, CONTROLLER_INDEX,
							BTP_STATUS_UNKNOWN_CMD);
	}
}
