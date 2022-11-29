// Copyright (c) 2022 Rodolfo Giometti <giometti@enneenne.com>
// SPDX-License-Identifier: (GPL-2.0)

#ifndef DBUS_H
#define DBUS_H

#include "state_machine.h"

#if defined(MRP_HAVE_DBUS)
int dbus_port_state_changed(struct mrp_port *p,
			    enum br_mrp_port_state_type state);
int dbus_init(void);
void dbus_uninit(void);
#else /* ! defined(MRP_HAVE_DBUS) */
static inline int dbus_port_state_changed(struct mrp_port *p,
			    enum br_mrp_port_state_type state)
{
	return 0;
}

static inline int dbus_init(void)
{
	return 0;
}

static inline void dbus_uninit(void)
{
	/* nop */
}
#endif /* defined(MRP_HAVE_DBUS) */

#endif /* DBUS_H */
