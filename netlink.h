// Copyright (c) 2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: (GPL-2.0)

#ifndef MRP_NETLINK_H
#define MRP_NETLINK_H

#include "state_machine.h"

int mrp_netlink_set_ring_role(struct mrp *mrp, enum br_mrp_ring_role_type role);
int mrp_netlink_set_in_role(struct mrp *mrp, enum br_mrp_in_role_type role);

int mrp_port_netlink_set_state(struct mrp_port *p,
			       enum br_mrp_port_state_type state);
int mrp_netlink_flush(struct mrp *mrp);

int mrp_netlink_init(void);
void mrp_netlink_uninit(void);

#endif
