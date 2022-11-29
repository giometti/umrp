// Copyright (c) 2022 Rodolfo Giometti <giometti@enneenne.com>
// SPDX-License-Identifier: (GPL-2.0)

#define DBUS_API_SUBJECT_TO_CHANGE
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>
#include <linux/mrp_bridge.h>
#include "utils.h"
#include "state_machine.h"
#include "dbus.h"

static DBusConnection *conn;

static const char *port_states[] =
{
	[BR_MRP_PORT_STATE_DISABLED]		= "Disabled",
	[BR_MRP_PORT_STATE_BLOCKED]		= "Blocking",
	[BR_MRP_PORT_STATE_FORWARDING]		= "Forwarding",
	[BR_MRP_PORT_STATE_NOT_CONNECTED]	= "Unconnected",
};

static int dbus_send(char *type, char *text)
{
	DBusMessage *msg;
	DBusMessageIter args;
	dbus_uint32_t serial = 0;

	/* Create a signal & check for errors */
	msg = dbus_message_new_signal(MRP_DBUS_PATH, MRP_DBUS_IFACE, type);
	if (NULL == msg) {
		pr_err("cannot create signal");
		return -1;
	}

	/* Append arguments onto signal */
	dbus_message_iter_init_append(msg, &args);
	if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &text)) {
		pr_err("cannot append message text");
		return -1;
	}

	/* Send the message and flush the connection */
	if (!dbus_connection_send(conn, msg, &serial)) {
		pr_err("cannot send signal");
		return -1;
	}
	dbus_connection_flush(conn);

	/* Free the message and close the connection */
	dbus_message_unref(msg);

	return 0;

}

static int dbus_message_valist(char *type,
			       char *source, char *event, va_list args)
{
	va_list a;
	char *s, *text;
	size_t len;
	int ret;

	/* Compute how much bytes we need to hold the message */
	va_copy(a, args);
	len = strlen(source) + 1;
	len += strlen(event) + 1;
	while ((s = va_arg(a, char *)) != NULL)
		len += strlen(s) + 1;
	va_end(a);

	/* Allocate the needed memory */
	text = malloc(len + 1);
	if (text == NULL) {
		pr_err("cannot allocate memory");
		return -1;
	}
	text[0] = '\0';

	/* Build the message */
	strcat(text, source);
	strcat(text, ":");
	strcat(text, event);
	strcat(text, ":");
	while ((s = va_arg(args, char *)) != NULL) {
		strcat(text, s);
		strcat(text, ":");
	}
	text[len - 1] = '\0';	/* drop the last ':' */

	/* Now send the message */
	ret = dbus_send(type, text);

	free(text);
	return ret;
}

static int dbus_port_event(char *prt, char *event, ...)
{
        va_list args;
        int ret;

        va_start(args, event);
        ret = dbus_message_valist("PortEvent", prt, event, args);
        va_end(args);

        return ret;
}

/*
 * Public functions
 */

int dbus_port_state_changed(struct mrp_port *p, int state)
{
	return dbus_port_event(p->ifname,
		"StateChanged", port_states[state], NULL);
}

int dbus_init(void)
{
	DBusError err;
	int ret;

	/* Initialise the error value */
	dbus_error_init(&err);

	/* Connect to the DBUS system bus, and check for errors */
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (dbus_error_is_set(&err)) {
		pr_err("connection Error (%s)", err.message);
		dbus_error_free(&err);
	}
	if (NULL == conn) {
		pr_err("cannot open dbus connection");
		return -1;
	}

	/* Register our name on the bus, and check for errors */
	ret = dbus_bus_request_name(conn, MRP_DBUS_IFACE,
				DBUS_NAME_FLAG_REPLACE_EXISTING , &err);
	if (dbus_error_is_set(&err)) {
		pr_err("name Error (%s)", err.message);
		dbus_error_free(&err);
	}
	if (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != ret) {
		pr_err("cannot request dbus name");
		return -1;
	}

	return 0;
}

void dbus_uninit(void)
{

}
