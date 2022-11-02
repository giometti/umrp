// Copyright (c) 2022 Rodolfo Giometti <giometti@enneenne.com>
// Copyright (c) 2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: (GPL-2.0)

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <linux/types.h>
#include <linux/if_bridge.h>
#include <net/if.h>
#include <errno.h>

#include "state_machine.h"
#include "utils.h"
#include "libnetlink.h"
#include "print.h"

static struct rtnl_handle rth = { .fd = -1 };

struct request {
	struct nlmsghdr		n;
	struct ifinfomsg	ifm;
	char			buf[1024];
};

static void mrp_nl_bridge_prepare(uint32_t ifindex, int cmd, struct request *req,
				  struct rtattr **afspec, struct rtattr **afmrp,
				  struct rtattr **af_submrp, int mrp_attr)
{
	req->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req->n.nlmsg_flags = NLM_F_REQUEST;
	req->n.nlmsg_type = cmd;
	req->ifm.ifi_family = PF_BRIDGE;

	req->ifm.ifi_index = ifindex;

	*afspec = addattr_nest(&req->n, sizeof(*req), IFLA_AF_SPEC);
	addattr16(&req->n, sizeof(*req), IFLA_BRIDGE_FLAGS, BRIDGE_FLAGS_SELF);

	*afmrp = addattr_nest(&req->n, sizeof(*req),
			      IFLA_BRIDGE_MRP | NLA_F_NESTED);
	*af_submrp = addattr_nest(&req->n, sizeof(*req),
				  mrp_attr | NLA_F_NESTED);
}

static void mrp_nl_port_prepare(struct mrp_port *port, int cmd,
				struct request *req, struct rtattr **afspec,
				struct rtattr **afmrp,
				struct rtattr **af_submrp, int mrp_attr)
{
	req->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req->n.nlmsg_flags = NLM_F_REQUEST;
	req->n.nlmsg_type = cmd;
	req->ifm.ifi_family = PF_BRIDGE;

	req->ifm.ifi_index = port->ifindex;

	*afspec = addattr_nest(&req->n, sizeof(*req), IFLA_AF_SPEC);
	*afmrp = addattr_nest(&req->n, sizeof(*req),
			      IFLA_BRIDGE_MRP | NLA_F_NESTED);
	*af_submrp = addattr_nest(&req->n, sizeof(*req),
				  mrp_attr | NLA_F_NESTED);
}

static int mrp_nl_terminate(struct request *req, struct rtattr *afspec,
			    struct rtattr *afmrp, struct rtattr *af_submrp)
{
	int err;

	addattr_nest_end(&req->n, af_submrp);
	addattr_nest_end(&req->n, afmrp);
	addattr_nest_end(&req->n, afspec);

	err = rtnl_talk(&rth, &req->n, NULL);
	if (err)
		return err;

	return 0;
}

int mrp_netlink_init(void)
{
	if (rtnl_open(&rth, 0) < 0) {
		pr_err("Cannot open rtnetlink");
		return EXIT_FAILURE;
	}

	return 0;
}

void mrp_netlink_uninit(void)
{
	rtnl_close(&rth);
}

int mrp_port_netlink_set_state(struct mrp_port *p,
			       enum br_mrp_port_state_type state)
{
	struct rtattr *afspec, *afmrp, *af_submrp;
	struct request req = { 0 };

	p->state = state;

	mrp_nl_port_prepare(p, RTM_SETLINK, &req, &afspec, &afmrp,
			    &af_submrp, IFLA_BRIDGE_MRP_PORT_STATE);

	addattr32(&req.n, sizeof(req), IFLA_BRIDGE_MRP_PORT_STATE_STATE, state);

	return mrp_nl_terminate(&req, afspec, afmrp, af_submrp);
}

int mrp_netlink_set_ring_role(struct mrp *mrp, enum br_mrp_ring_role_type role)
{
	struct rtattr *afspec, *afmrp, *af_submrp;
	struct request req = { 0 };

	mrp->ring_role = role;

	mrp_nl_bridge_prepare(mrp->ifindex, RTM_SETLINK, &req, &afspec, &afmrp,
			      &af_submrp, IFLA_BRIDGE_MRP_RING_ROLE);

	if (mrp->mra_support)
		role = BR_MRP_RING_ROLE_MRA;

	addattr32(&req.n, sizeof(req), IFLA_BRIDGE_MRP_RING_ROLE_RING_ID,
		  mrp->ring_nr);
	addattr32(&req.n, sizeof(req), IFLA_BRIDGE_MRP_RING_ROLE_ROLE,
		  role);

	return mrp_nl_terminate(&req, afspec, afmrp, af_submrp);
}

int mrp_netlink_set_in_role(struct mrp *mrp, enum br_mrp_in_role_type role)
{
	struct rtattr *afspec, *afmrp, *af_submrp;
	struct request req = { 0 };

	mrp->in_role = role;

	mrp_nl_bridge_prepare(mrp->ifindex, RTM_SETLINK, &req, &afspec, &afmrp,
			      &af_submrp, IFLA_BRIDGE_MRP_IN_ROLE);

	addattr32(&req.n, sizeof(req), IFLA_BRIDGE_MRP_IN_ROLE_RING_ID,
		  mrp->ring_nr);
	addattr16(&req.n, sizeof(req), IFLA_BRIDGE_MRP_IN_ROLE_IN_ID,
		  mrp->in_id);
	addattr32(&req.n, sizeof(req), IFLA_BRIDGE_MRP_IN_ROLE_I_IFINDEX,
		  mrp->i_port->ifindex);
	addattr32(&req.n, sizeof(req), IFLA_BRIDGE_MRP_IN_ROLE_ROLE,
		  role);

	return mrp_nl_terminate(&req, afspec, afmrp, af_submrp);
}

int mrp_netlink_flush(struct mrp *mrp)
{
	struct request req = { 0 };
	struct rtattr *protinfo;

	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_type = RTM_SETLINK;
	req.ifm.ifi_family = PF_BRIDGE;

	protinfo = addattr_nest(&req.n, sizeof(req),
				IFLA_PROTINFO | NLA_F_NESTED);
	addattr(&req.n, 1024, IFLA_BRPORT_FLUSH);

	addattr_nest_end(&req.n, protinfo);

	req.ifm.ifi_index = mrp->p_port->ifindex;
	if (rtnl_talk(&rth, &req.n, NULL) < 0)
		return -1;

	req.ifm.ifi_index = mrp->s_port->ifindex;
	if (rtnl_talk(&rth, &req.n, NULL) < 0)
		return -1;

	if (!mrp->i_port)
		return 0;

	req.ifm.ifi_index = mrp->i_port->ifindex;
	if (rtnl_talk(&rth, &req.n, NULL) < 0)
		return -1;

	return 0;
}
