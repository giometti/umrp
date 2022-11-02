// Copyright (c) 2022 Rodolfo Giometti <giometti@enneenne.com>
// SPDX-License-Identifier: (GPL-2.0)

#ifndef _IFDRIVER_H
#define _IFDRIVER_H

#include "state_machine.h"

extern int ifdriver_port_set_state(struct mrp_port *p,
				   enum br_mrp_port_state_type state);
#define alias_ifdriver_port_set_state(f)				\
	int ifdriver_port_set_state(struct mrp_port *p,			\
				   enum br_mrp_port_state_type state)	\
		__alias(stringify(f))
extern int ifdriver_set_ring_role(struct mrp *mrp,
				   enum br_mrp_ring_role_type role);
#define alias_ifdriver_set_ring_role(f)					\
	int ifdriver_set_ring_role(struct mrp *mrp,			\
				   enum br_mrp_ring_role_type role)	\
		__alias(stringify(f))
extern int ifdriver_set_in_role(struct mrp *mrp,
				   enum br_mrp_in_role_type role);
#define alias_ifdriver_set_in_role(f)					\
	int ifdriver_set_in_role(struct mrp *mrp,			\
				   enum br_mrp_in_role_type role)	\
		__alias(stringify(f))
extern int ifdriver_flush(struct mrp *mrp);
#define alias_ifdriver_flush(f)						\
	int ifdriver_flush(struct mrp *mrp)				\
		__alias(stringify(f))

extern int ifdriver_init(void);
#define alias_ifdriver_init(f)		int ifdriver_init(void)		\
		__alias(stringify(f))
extern void ifdriver_uninit(void);
#define alias_ifdriver_uninit(f)	void ifdriver_uninit(void)	\
		__alias(stringify(f))

#endif /* _IFDRIVER_H */
