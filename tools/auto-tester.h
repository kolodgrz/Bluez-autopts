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
#define BTP_MTU 1024

#define BTP_INDEX_NONE		0xff

#define BTP_SERVICE_ID_CORE	0
#define BTP_SERVICE_ID_GAP	1
#define BTP_SERVICE_ID_GATT	2
#define BTP_SERVICE_ID_L2CAP	3

#define BTP_STATUS_SUCCESS	0x00
#define BTP_STATUS_FAILED	0x01
#define BTP_STATUS_UNKNOWN_CMD	0x02
#define BTP_STATUS_NOT_READY	0x03

struct btp_hdr {
	uint8_t  service;
	uint8_t  opcode;
	uint8_t  index;
	uint16_t len;
} __attribute__((packed));

#define BTP_STATUS			0x00
struct btp_status {
	uint8_t code;
} __attribute__((packed));

/* Core Service */
#define CORE_READ_SUPPORTED_COMMANDS	0x01
struct core_read_supported_commands_rp {
	uint8_t data[0];
} __attribute__((packed));

#define CORE_READ_SUPPORTED_SERVICES	0x02
struct core_read_supported_services_rp {
	uint8_t data[0];
} __attribute__((packed));

#define CORE_REGISTER_SERVICE		0x03
struct core_register_service_cmd {
	uint8_t id;
} __attribute__((packed));

/* events */
#define CORE_EV_IUT_READY		0x80

/* GAP Service */
#define GAP_READ_SUPPORTED_COMMANDS	0x01
struct gap_read_supported_commands_rp {
	uint8_t data[0];
} __attribute__((packed));

#define GAP_SETTINGS_POWERED		0
#define GAP_SETTINGS_CONNECTABLE	1
#define GAP_SETTINGS_FAST_CONNECTABLE	2
#define GAP_SETTINGS_DISCOVERABLE	3
#define GAP_SETTINGS_BONDABLE		4
#define GAP_SETTINGS_LINK_SEC_3		5
#define GAP_SETTINGS_SSP		6
#define GAP_SETTINGS_BREDR		7
#define GAP_SETTINGS_HS			8
#define GAP_SETTINGS_LE			9
#define GAP_SETTINGS_ADVERTISING	10
#define GAP_SETTINGS_SC			11
#define GAP_SETTINGS_DEBUG_KEYS		12
#define GAP_SETTINGS_PRIVACY		13
#define GAP_SETTINGS_CONTROLLER_CONFIG	14
#define GAP_SETTINGS_STATIC_ADDRESS	15

#define GAP_READ_CONTROLLER_INFO	0x03
struct gap_read_controller_info_rp {
	uint8_t  address[6];
	uint32_t supported_settings;
	uint32_t current_settings;
	uint8_t  cod[3];
	uint8_t  name[249];
	uint8_t  short_name[11];
} __packed;

#define GAP_SET_POWERED			0x05
struct gap_set_powered_cmd {
	uint8_t powered;
} __attribute__((packed));
struct gap_set_powered_rp {
	uint32_t current_settings;
} __attribute__((packed));

#define GAP_START_ADVERTISING	0x0a
struct gap_start_advertising_cmd {
	uint8_t adv_data_len;
	uint8_t scan_rsp_len;
	uint8_t adv_data[0];
	uint8_t scan_rsp[0];
} __attribute__((packed));
struct gap_start_advertising_rp {
	uint32_t current_settings;
} __attribute__((packed));

#define GAP_STOP_ADVERTISING		0x0b
struct gap_stop_advertising_rp {
	uint32_t current_settings;
} __attribute__((packed));

#define GAP_DISCOVERY_FLAG_LE			0x01
#define GAP_DISCOVERY_FLAG_BREDR		0x02
#define GAP_DISCOVERY_FLAG_LIMITED		0x04
#define GAP_DISCOVERY_FLAG_LE_ACTIVE_SCAN	0x08

#define GAP_START_DISCOVERY		0x0c
struct gap_start_discovery_cmd {
	uint8_t flags;
} __attribute__((packed));

#define GAP_STOP_DISCOVERY		0x0d

#define GAP_CONNECT			0x0e
struct gap_connect_cmd {
	uint8_t address_type;
	uint8_t address[6];
} __attribute__((packed));

#define GAP_DISCONNECT			0x0f
struct gap_disconnect_cmd {
	uint8_t  address_type;
	uint8_t  address[6];
} __attribute__((packed));

/* GAP Events*/

#define GAP_EV_DEVICE_CONNECTED		0x82
struct gap_device_connected_ev {
	uint8_t address[6];
	uint8_t address_type;
} __attribute__((packed));

#define GAP_EV_DEVICE_DISCONNECTED	0x83
struct gap_device_disconnected_ev {
	uint8_t address[6];
	uint8_t address_type;
} __attribute__((packed));

static inline void tester_set_bit(uint8_t *addr, unsigned int bit)
{
	uint8_t *p = addr + (bit / 8);

	*p |= (bit % 8);
}

bool verbose;

void send_msg(uint8_t service, uint8_t opcode, uint8_t index, uint16_t len,
								uint8_t *data);
void send_status(uint8_t service, uint8_t opcode, uint8_t index,
								uint8_t status);
void register_prop_cb(const char *iface, char *prop, void cb(void));
uint8_t handle_gap_register(DBusConnection *dbus_conn);
void handle_gap(GDBusProxy *adapter_proxy, GDBusProxy *adv_proxy,
		GSList *dev_list, uint8_t op, uint8_t *data, uint16_t len);

uint32_t get_current_settings();
