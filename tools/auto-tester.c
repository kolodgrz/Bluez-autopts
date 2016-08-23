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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "gdbus/gdbus.h"
#include "auto-tester.h"

#define BTP_SOCKET_NAME "/tmp/bt-stack-tester"

#define BTP_INDEX_NONE		0xff

static int btp_fd = 0;

static GMainLoop *main_loop;
static GDBusProxy *adapter_proxy;
static GDBusProxy *adv_proxy;
static DBusConnection *dbus_conn;

uint32_t current_settings;
static GSList *prop_cbs = NULL;

static void connect_handler(DBusConnection *connection, void *user_data)
{
	if (verbose)
		printf("Connected succesfully to DBUS\n");
}

static void disconnect_handler(DBusConnection *connection, void *user_data)
{
	if (verbose)
		printf("Disconnected from DBUS\n");
}

static void message_handler(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	if (verbose)
		printf("Got signal from DBUS\n");
}

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	const char *interface;

	interface = g_dbus_proxy_get_interface(proxy);

	if (!strcmp(interface, "org.bluez.Adapter1"))
		adapter_proxy = proxy;
	else if (!strcmp(interface, "org.bluez.LEAdvertisingManager1"))
		adv_proxy = proxy;

	if (verbose)
		printf("DBUS Proxy added: %s\n", interface);
}

static void proxy_removed(GDBusProxy *proxy, void *user_data)
{
	const char *interface;

	interface = g_dbus_proxy_get_interface(proxy);

	if (verbose)
		printf("DBUS Proxy removed: %s\n", interface);
}

struct prop_cb {
	char *iface;
	char *prop;
	void (*cb_func)(void);
};

void register_prop_cb(const char *iface, char *prop, void (*cb_func)(void))
{
	struct prop_cb *p;

	p = malloc(sizeof(*p));

	p->iface = strdup(iface);
	p->prop = strdup(prop);
	p->cb_func = cb_func;

	prop_cbs = g_slist_append(prop_cbs, p);
}

static void prop_cmp_foreach(gpointer data, gpointer user_data)
{
	struct prop_cb *a = (struct prop_cb*) data;
	struct prop_cb *b = (struct prop_cb*) user_data;

	if (strcmp(a->iface, b->iface))
		return;

	if (strcmp(a->prop, b->prop))
		return;

	a->cb_func();

	free(a->iface);
	free(a->prop);
	free(a);

	prop_cbs = g_slist_remove(prop_cbs, data);
}

static void change_current_setting(uint8_t bit, bool set)
{
	if (set)
		current_settings |= 1 << bit;
	else
		current_settings &= ~(1 << bit);
}

static void property_changed(GDBusProxy *proxy, const char *name,
					DBusMessageIter *iter, void *user_data)
{
	dbus_bool_t valbool;
	const char *interface;
	struct prop_cb p;

	interface = g_dbus_proxy_get_interface(proxy);

	if (!strcmp(interface, "org.bluez.Adapter1")) {
		if (!strcmp(name, "Powered")) {
			dbus_message_iter_get_basic(iter, &valbool);
			change_current_setting(GAP_SETTINGS_POWERED, valbool);
		}
	}

	p.iface = strdup(interface);
	p.prop = strdup(name);

	g_slist_foreach(prop_cbs, prop_cmp_foreach, &p);

	free(p.prop);
	free(p.iface);

	if (verbose)
		printf("DBUS Property changed, %s - %s\n", interface, name);
}

static void client_ready(GDBusClient *client, void *user_data)
{
	if (verbose)
		printf("DBUS Client ready\n");

	send_msg(BTP_SERVICE_ID_CORE, CORE_EV_IUT_READY, BTP_INDEX_NONE, 0,
									NULL);
}

void send_msg(uint8_t service, uint8_t opcode, uint8_t index, uint16_t len,
								uint8_t *data)
{
	struct btp_hdr hdr;
	uint8_t buf[BTP_MTU];
	int err, i;

	if (len > BTP_MTU - sizeof(hdr)) {
		printf("Too much data to send over btp\n");
		return;
	}

	hdr.service = service;
	hdr.opcode = opcode;
	hdr.index = index;
	hdr.len = len;

	memcpy(buf, &hdr, sizeof(hdr));

	if (hdr.len)
		memcpy(buf+sizeof(hdr), data, hdr.len);

	if (write(btp_fd, buf, sizeof(hdr) + hdr.len) < 0) {
		err = -errno;
		printf("write error, (%s - %d)\n", strerror(-err), -err);
	}

	if (verbose) {
		printf("< HDR: svc=0x%02x, op=0x%02x, index=0x%02x, dlen=%d\n",
				hdr.service, hdr.opcode, hdr.index, hdr.len);
		printf("< DATA: ");
		for (i = 0; i < hdr.len; i++)
			printf("%02x ", buf[i+sizeof(hdr)]);
		printf("\n");
	}
}

void send_status(uint8_t service, uint8_t opcode, uint8_t index, uint8_t status)
{
	if (status == BTP_STATUS_SUCCESS) {
		send_msg(service, opcode, index, 0, NULL);
		return;
	}

	send_msg(service, BTP_STATUS, index, sizeof(status), &status);
}

/* Core cmds handling */
static void supported_commands(uint8_t *data, uint16_t len)
{
	uint8_t buf[1];
	struct core_read_supported_commands_rp *rp = (void *) buf;

	memset(buf, 0, sizeof(buf));

	tester_set_bit(buf, CORE_READ_SUPPORTED_COMMANDS);
	tester_set_bit(buf, CORE_READ_SUPPORTED_SERVICES);

	send_msg(BTP_SERVICE_ID_CORE, CORE_READ_SUPPORTED_COMMANDS,
				BTP_INDEX_NONE, sizeof(buf), (uint8_t *) rp);
}

static void supported_services(uint8_t *data, uint16_t len)
{
	uint8_t buf[1];
	struct core_read_supported_services_rp *rp = (void *) buf;

	memset(buf, 0, sizeof(buf));

	tester_set_bit(buf, BTP_SERVICE_ID_CORE);

	send_msg(BTP_SERVICE_ID_CORE, CORE_READ_SUPPORTED_SERVICES,
				BTP_INDEX_NONE, sizeof(buf), (uint8_t *) rp);
}

static int read_current_settings()
{
	DBusMessageIter iter;
	dbus_bool_t powered, discoverable, pairable;

	// TODO - Get rest of settings from stack

	if (!g_dbus_proxy_get_property(adapter_proxy, "Powered", &iter))
		return -1;
	dbus_message_iter_get_basic(&iter, &powered);

	if (!g_dbus_proxy_get_property(adapter_proxy, "Discoverable", &iter))
		return -1;
	dbus_message_iter_get_basic(&iter, &discoverable);

	if (!g_dbus_proxy_get_property(adapter_proxy, "Pairable", &iter))
		return -1;
	dbus_message_iter_get_basic(&iter, &pairable);

	change_current_setting(GAP_SETTINGS_POWERED, powered);
	change_current_setting(GAP_SETTINGS_DISCOVERABLE, discoverable);
	change_current_setting(GAP_SETTINGS_BONDABLE, pairable);

	return 0;
}

uint32_t get_current_settings()
{
	return current_settings;
}

static void core_reg_svc(uint8_t *data, uint16_t len)
{
	struct core_register_service_cmd *cmd = (void *) data;
	uint8_t status;

	switch (cmd->id) {
	case BTP_SERVICE_ID_GAP:
		if (read_current_settings()) {
			status = BTP_STATUS_FAILED;
			break;
		}
		status = handle_gap_register(dbus_conn);
		break;
	default:
		status = BTP_STATUS_FAILED;
	}

	if (status == BTP_STATUS_FAILED)
		send_status(BTP_SERVICE_ID_CORE, BTP_STATUS, BTP_INDEX_NONE,
									status);
	else
		send_status(BTP_SERVICE_ID_CORE, CORE_REGISTER_SERVICE,
						BTP_INDEX_NONE, status);
}

static void handle_core(uint8_t op, uint8_t *data, uint16_t len)
{
	switch (op) {
	case CORE_READ_SUPPORTED_COMMANDS:
		supported_commands(data, len);
		break;
	case CORE_READ_SUPPORTED_SERVICES:
		supported_services(data, len);
		break;
	case CORE_REGISTER_SERVICE:
		core_reg_svc(data, len);
		break;
	default:
		send_status(BTP_SERVICE_ID_CORE, BTP_STATUS, BTP_INDEX_NONE,
							BTP_STATUS_FAILED);
	}
}

static void handle_msg(struct btp_hdr *hdr, uint8_t *data, uint16_t len)
{
	switch (hdr->service) {
	case BTP_SERVICE_ID_CORE:
		handle_core(hdr->opcode, data, len);
		break;
	case BTP_SERVICE_ID_GAP:
		handle_gap(adapter_proxy, adv_proxy, hdr->opcode, data, len);
		break;
	default:
		send_status(hdr->service, BTP_STATUS, hdr->index,
							BTP_STATUS_FAILED);
	}
}

static int btp_connect()
{
	int fd;
	struct sockaddr_un addr;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, BTP_SOCKET_NAME, sizeof(addr.sun_path) - 1);

	if (connect(fd, (const struct sockaddr *) &addr,
						sizeof(struct sockaddr_un)))
		return -1;

	return fd;
}

static gboolean btp_rcv_cb(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	int len, err, i;
	struct btp_hdr hdr;
	uint8_t buf[BTP_MTU-sizeof(hdr)];

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP)) {
		if (verbose)
			printf("got btp rcv err\n");
		return FALSE;
	}

	len = read(btp_fd, buf, sizeof(hdr));

	if (len < 0) {
		err = -errno;
		printf("Reading hdr error, (%s - %d)\n", strerror(-err), -err);
		return FALSE;
	}

	memcpy(&hdr, buf, sizeof(struct btp_hdr));
	if (verbose)
		printf("> HDR: svc=0x%02x, op=0x%02x, index=0x%02x, dlen=%d\n",
				hdr.service, hdr.opcode, hdr.index, hdr.len);

	len = read(btp_fd, buf, hdr.len);

	if (len < 0) {
		err = -errno;
		printf("Reading msg data err, (%s-%d)\n", strerror(-err), -err);
		return FALSE;
	}

	if (verbose) {
		printf("> DATA: ");
		for (i = 0; i < len; i++)
			printf("%02x ", buf[i]);
		printf("\n");
	}

	handle_msg(&hdr, buf, hdr.len);

	return TRUE;
}

static void usage(void)
{
	printf("autopts-tester - Autopts IUT application\n"
		"Usage:\n");
	printf("\tautopts-tester [options]\n");
	printf("Options:\n"
		"\t-v, --version       Show version\n"
		"\t-d, --debug         Enable debugging output\n"
		"\t-h, --help          Show help options\n");
}

static const struct option main_options[] = {
	{ "debug",    no_argument,       NULL, 'd' },
	{ "version",  no_argument,       NULL, 'v' },
	{ "help",     no_argument,       NULL, 'h' },
	{ }
};

int main(int argc, char *argv[])
{
	int err;
	GDBusClient *client;
	GIOChannel *g_btp_chan;

	verbose = false;

	for (;;) {
		int opt;

		opt = getopt_long(argc, argv, "dvh", main_options, NULL);
		if (opt < 0)
			break;

		switch (opt) {
		case 'd':
			verbose = true;
			break;
		case 'v':
			printf("%s\n", VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage();
			return EXIT_SUCCESS;
		default:
			return EXIT_FAILURE;
		}
	}

	main_loop = g_main_loop_new(NULL, FALSE);

	dbus_conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, NULL);

	client = g_dbus_client_new(dbus_conn, "org.bluez", "/org/bluez");

	g_dbus_client_set_connect_watch(client, connect_handler, NULL);
	g_dbus_client_set_disconnect_watch(client, disconnect_handler, NULL);
	g_dbus_client_set_signal_watch(client, message_handler, NULL);
	g_dbus_client_set_proxy_handlers(client, proxy_added, proxy_removed,
							property_changed, NULL);
	g_dbus_client_set_ready_watch(client, client_ready, NULL);

	btp_fd = btp_connect();
	g_btp_chan = g_io_channel_unix_new(btp_fd);
	g_io_channel_set_close_on_unref(g_btp_chan, TRUE);
	g_io_channel_set_encoding(g_btp_chan, NULL, NULL);
	g_io_channel_set_buffered(g_btp_chan, FALSE);

	if (btp_fd < 0) {
		g_io_channel_unref(g_btp_chan);
		err = -errno;
		printf("Error while connecting to :%s (%s - %d)\n",
					BTP_SOCKET_NAME, strerror(-err), -err);
		return EXIT_FAILURE;
	}

	printf("Connected succesfully to: %s\n", BTP_SOCKET_NAME);
	g_io_add_watch(g_btp_chan, G_IO_IN | G_IO_HUP | G_IO_ERR, btp_rcv_cb,
									NULL);

	printf("Connecting to DBUS\n");

	g_main_loop_run(main_loop);

	g_io_channel_unref(g_btp_chan);
	g_dbus_client_unref(client);
	dbus_connection_unref(dbus_conn);

	return EXIT_SUCCESS;
}
