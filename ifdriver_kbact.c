// Copyright (c) 2022 Rodolfo Giometti <giometti@enneenne.com>
// SPDX-License-Identifier: (GPL-2.0)

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <stdarg.h>
#include <netinet/ether.h>
#include <linux/types.h>
#include <net/if.h>
#include <errno.h>

#include "ifdriver.h"
#include "libnetlink.h"
#include "state_machine.h"
#include "utils.h"

/*
 * Private data & functions
 */

static FILE *file;

__printf(1, 2) static int exec_cmd(const char *fmt, ...)
{
        char *cmd;
        va_list args;
        int n, ret;

        va_start(args, fmt);
        n = vasprintf(&cmd, fmt, args);
        va_end(args); 

	pr_debug("executing: %s", cmd);
        ret = fwrite(cmd, n, 1, file);
        free(cmd);

	if (ret < n)
		return -1;

	return 0;
}

/*
 * Public data & functions
 */

int kbact_port_set_state(struct mrp_port *p,
			       enum br_mrp_port_state_type state)
{
	int s;

	pr_debug("port: %s, state: %d", p->ifname, state);

        switch (state) {
        case BR_MRP_PORT_STATE_BLOCKED:    s = 1;   break;
        case BR_MRP_PORT_STATE_DISABLED:   s = 0;   break;
        case BR_MRP_PORT_STATE_FORWARDING: s = 3; break;
        default:
                BUG();
        }

        exec_cmd("cswtool -setstpstatus %s %d\n", p->ifname, s);

	return 0;
}
alias_ifdriver_port_set_state(kbact_port_set_state);

int kbact_set_ring_role(struct mrp *mrp, enum br_mrp_ring_role_type role)
{
	switch (role) {
	case BR_MRP_RING_ROLE_MRM:
		pr_debug("bridge: %s role: MRM", mrp->ifname);
		break;
	case BR_MRP_RING_ROLE_MRC:
		pr_debug("bridge: %s role: MRC", mrp->ifname);
		break;
	default:
               BUG();
	}

	return 0;
}
alias_ifdriver_set_ring_role(kbact_set_ring_role);

int kbact_set_in_role(struct mrp *mrp, enum br_mrp_in_role_type role)
{
	switch (role) {
	case BR_MRP_IN_ROLE_MIM:
		pr_debug("bridge: %s role: MIM", mrp->ifname);
		break;
	case BR_MRP_IN_ROLE_MIC:
		pr_debug("bridge: %s role: MIC", mrp->ifname);
		break;
	default:
               BUG();
	}
	return 0;
}
alias_ifdriver_set_in_role(kbact_set_in_role);

int kbact_flush(struct mrp *mrp)
{
	pr_debug("bridge: %s", mrp->ifname);

        exec_cmd("cswtool -atuflushport %s\n", mrp->p_port->ifname);
        exec_cmd("cswtool -atuflushport %s\n", mrp->s_port->ifname);

	if (!mrp->i_port)
		return 0;

        exec_cmd("cswtool -atuflushport %s\n", mrp->i_port->ifname);

	return 0;
}
alias_ifdriver_flush(kbact_flush);

/* INIT & UNINIT functions */

int kbact_init(void)
{
	file = popen("cswtool I", "w");
	if (file == NULL)
		return -1;
	setlinebuf(file);

	exec_cmd("cswtool -atuadd cpu %s 1 6 1\n",
				ether_ntoa((void *) mrp_test_dmac));
	exec_cmd("cswtool -atuadd cpu %s 1 6 1\n",
				ether_ntoa((void *) mrp_control_dmac));
	exec_cmd("cswtool -atuadd cpu %s 1 6 1\n",
				ether_ntoa((void *) mrp_itest_dmac));
	exec_cmd("cswtool -atuadd cpu %s 1 6 1\n",
				ether_ntoa((void *) mrp_icontrol_dmac));

	pr_debug("kbact ifdriver done");
        return 0;
}
alias_ifdriver_init(kbact_init);

void kbact_uninit(void)
{
	exec_cmd("cswtool -atudel cpu %s\n",
				ether_ntoa((void *) mrp_test_dmac));
	exec_cmd("cswtool -atudel cpu %s\n",
                               ether_ntoa((void *) mrp_control_dmac));
	exec_cmd("cswtool -atudel cpu %s\n",
				ether_ntoa((void *) mrp_itest_dmac));
	exec_cmd("cswtool -atudel cpu %s\n",
                               ether_ntoa((void *) mrp_icontrol_dmac));

	pclose(file);
}
alias_ifdriver_uninit(kbact_uninit);
