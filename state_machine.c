// Copyright (c) 2022 Rodolfo Giometti <giometti@enneenne.com>
// Copyright (c) 2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: (GPL-2.0)

#include <asm/byteorder.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <stdio.h>
#include <errno.h>

#include "state_machine.h"
#include "server_cmds.h"
#include "utils.h"
#include "ifdriver.h"
#include "packet.h"
#include "pdu.h"
#include "cfm_netlink.h"
#include "dbus.h"

static LIST_HEAD(mrp_instances);

const uint8_t mrp_test_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x1 };
const uint8_t mrp_control_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x2 };
const uint8_t mrp_itest_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x3 };
const uint8_t mrp_icontrol_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x4 };

struct mrp_port *mrp_get_port(uint32_t ifindex)
{
	struct mrp *mrp;

	list_for_each_entry(mrp, &mrp_instances, list) {
		if (mrp->p_port && mrp->p_port->ifindex == ifindex)
			return mrp->p_port;
		if (mrp->s_port && mrp->s_port->ifindex == ifindex)
			return mrp->s_port;
		if (mrp->i_port && mrp->i_port->ifindex == ifindex)
			return mrp->i_port;
	}

	return NULL;
}

static struct mrp *mrp_get_mrp(uint32_t ifindex, uint32_t peer_mepid)
{
	struct mrp *mrp;

	list_for_each_entry(mrp, &mrp_instances, list) {
		if (mrp->ifindex == ifindex &&
		    mrp->cfm_peer_mepid == peer_mepid)
			return mrp;
	}

	return NULL;
}

int mrp_port_set_state(struct mrp_port *p, enum br_mrp_port_state_type state)
{
	int ret;

        p->state = state;

        ret = ifdriver_port_set_state(p, state);
	if (ret)
		pr_warn("cannot set state %d for port %s", state, p->ifname);

	dbus_port_state_changed(p, state);

	return ret;
}

static bool mrp_is_port_up(const struct mrp_port *p)
{
	return if_get_link(p->ifindex);
}

static bool mrp_is_ring_port(const struct mrp_port *p)
{
	return p->role == BR_MRP_PORT_ROLE_PRIMARY ||
	       p->role == BR_MRP_PORT_ROLE_SECONDARY;
}

static bool mrp_is_in_port(const struct mrp_port *p)
{
	return p->role == BR_MRP_PORT_ROLE_INTER;
}

/* Determins if a port is part of a MRP instance */
static bool mrp_is_mrp_port(const struct mrp_port *p)
{
	if (!p->mrp)
		return false;

	return true;
}

static void mrp_reset_ring_state(struct mrp *mrp)
{
	mrp_timer_stop(mrp);
	mrp->mrm_state = MRP_MRM_STATE_AC_STAT1;
	mrp->mrc_state = MRP_MRC_STATE_AC_STAT1;
}

char *mrp_get_mrm_state(enum mrp_mrm_state_type state)
{
	switch (state) {
	case MRP_MRM_STATE_AC_STAT1: return "AC_STAT1";
	case MRP_MRM_STATE_PRM_UP: return "PRM_UP";
	case MRP_MRM_STATE_CHK_RO: return "CHK_RO";
	case MRP_MRM_STATE_CHK_RC: return "CHK_RC";
	default: return "Unknown MRM state";
	}
}

char *mrp_get_mrc_state(enum mrp_mrc_state_type state)
{
	switch (state) {
	case MRP_MRC_STATE_AC_STAT1: return "AC_STAT1";
	case MRP_MRC_STATE_DE_IDLE: return "DE_IDLE";
	case MRP_MRC_STATE_PT: return "PT";
	case MRP_MRC_STATE_DE: return "DE";
	case MRP_MRC_STATE_PT_IDLE: return "PT_IDLE";
	default: return "Unknown MRC state";
	}
}

char *mrp_get_mim_state(enum mrp_mim_state_type state)
{
	switch (state) {
	case MRP_MIM_STATE_AC_STAT1: return "AC_STAT1";
	case MRP_MIM_STATE_CHK_IO: return "CHK_IO";
	case MRP_MIM_STATE_CHK_IC: return "CHK_IC";
	default: return "Unknown MIM state";
	}
}

char *mrp_get_mic_state(enum mrp_mic_state_type state)
{
	switch (state) {
	case MRP_MIC_STATE_AC_STAT1: return "AC_STAT1";
	case MRP_MIC_STATE_PT: return "PT";
	case MRP_MIC_STATE_IP_IDLE: return "IP_IDLE";
	default: return "Unknown MIC state";
	}
}

int mrp_set_ring_role(struct mrp *mrp, enum br_mrp_ring_role_type role)
{
	int ret;

        mrp->ring_role = role;

        ret = ifdriver_set_ring_role(mrp, role);
	if (ret)
		pr_warn("cannot set state %d for bridge %s", role, mrp->ifname);
	pr_debug("role: %s", ring_role_str(role));
	return ret;
}
int mrp_set_in_role(struct mrp *mrp, enum br_mrp_in_role_type role)
{
	int ret;

	mrp->in_role = role;

        ret = ifdriver_set_in_role(mrp, role);
	if (ret)
		pr_warn("cannot set state %d for bridge %s", role, mrp->ifname);
	pr_debug("role: %s", in_role_str(role));
	return ret;
}

void mrp_set_mrm_init(struct mrp *mrp)
{
	mrp->add_test = false;
	mrp->no_tc = false;
	mrp->ring_test_curr = 0;
}

void mrp_set_mrc_init(struct mrp *mrp)
{
	mrp->ring_link_curr_max = mrp->ring_link_conf_max;
	mrp->ring_mon_curr = 0;
}

void mrp_set_mrm_state(struct mrp *mrp, enum mrp_mrm_state_type state)
{
	pr_debug("bridge: %s, mrm_state: %s", mrp->ifname,
						mrp_get_mrm_state(state));
	mrp->mrm_state = state;
	mrp->no_tc = false;
}

void mrp_set_mrc_state(struct mrp *mrp, enum mrp_mrc_state_type state)
{
	pr_debug("bridge: %s, mrc_state: %s", mrp->ifname,
						mrp_get_mrc_state(state));
	mrp->mrc_state = state;
}

void mrp_set_mim_state(struct mrp *mrp, enum mrp_mim_state_type state)
{
	pr_debug("bridge: %s, mim_state: %s", mrp->ifname,
						mrp_get_mim_state(state));
	mrp->mim_state = state;
}

void mrp_set_mic_state(struct mrp *mrp, enum mrp_mic_state_type state)
{
	pr_debug("bridge: %s, mic_state: %s", mrp->ifname,
						mrp_get_mic_state(state));
	mrp->mic_state = state;
}

static int mrp_set_mra_role(struct mrp *mrp)
{
	int err;

	/* If MRP instance doesn't have set both ports, then it can't have a
	 * role
	 */
	if (!mrp->p_port || !mrp->s_port)
		return -EINVAL;

	mrp->mra_support = true;

	/* When changing the role everything is reset */
	mrp_reset_ring_state(mrp);
	mrp_set_mrm_init(mrp);
	mrp_set_mrc_init(mrp);

	mrp_set_mrm_state(mrp, MRP_MRM_STATE_AC_STAT1);

	mrp_port_set_state(mrp->p_port, BR_MRP_PORT_STATE_BLOCKED);
	mrp_port_set_state(mrp->s_port, BR_MRP_PORT_STATE_BLOCKED);
	err = mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRM);
	if (err)
		return err;

	if (mrp_is_port_up(mrp->p_port))
		mrp_port_link_change(mrp->p_port, true);

	if (mrp_is_port_up(mrp->s_port))
		mrp_port_link_change(mrp->s_port, true);

	return 0;
}

static int mrp_set_mrm_role(struct mrp *mrp)
{
	int err;

	/* If MRP instance doesn't have set both ports, then it can't have a
	 * role
	 */
	if (!mrp->p_port || !mrp->s_port)
		return -EINVAL;

	/* When changing the role everything is reset */
	mrp_reset_ring_state(mrp);
	mrp_set_mrm_init(mrp);

	mrp_set_mrm_state(mrp, MRP_MRM_STATE_AC_STAT1);

	mrp_port_set_state(mrp->p_port, BR_MRP_PORT_STATE_BLOCKED);
	mrp_port_set_state(mrp->s_port, BR_MRP_PORT_STATE_BLOCKED);
	err = mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRM);
	if (err)
		return err;

	if (mrp_is_port_up(mrp->p_port))
		mrp_port_link_change(mrp->p_port, true);

	if (mrp_is_port_up(mrp->s_port))
		mrp_port_link_change(mrp->s_port, true);

	return 0;
}

static int mrp_set_mrc_role(struct mrp *mrp)
{
	int err;

	/* If MRP instance doesn't have set both ports, then it can't have a
	 * role
	 */
	if (!mrp->p_port || !mrp->s_port)
		return -EINVAL;

	/* When changing the role everything is reset */
	mrp_reset_ring_state(mrp);
	mrp_set_mrc_init(mrp);

	mrp_set_mrc_state(mrp, MRP_MRC_STATE_AC_STAT1);

	mrp_port_set_state(mrp->p_port, BR_MRP_PORT_STATE_BLOCKED);
	mrp_port_set_state(mrp->s_port, BR_MRP_PORT_STATE_BLOCKED);
	err = mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRC);
	if (err)
		return err;

	if (mrp_is_port_up(mrp->p_port))
		mrp_port_link_change(mrp->p_port, true);

	if (mrp_is_port_up(mrp->s_port))
		mrp_port_link_change(mrp->s_port, true);

	return 0;
}

static int mrp_set_mim_role(struct mrp *mrp)
{
	int err;

	if (!mrp->i_port)
		return -EINVAL;

	/* Reset the states but don't reset the timers */
	mrp->mim_state = MRP_MIM_STATE_AC_STAT1;
	mrp->mic_state = MRP_MIC_STATE_AC_STAT1;

	err = mrp_set_in_role(mrp, BR_MRP_IN_ROLE_MIM);
	if (err)
		return err;

	mrp_set_mim_state(mrp, MRP_MIM_STATE_AC_STAT1);
	mrp_port_set_state(mrp->i_port, BR_MRP_PORT_STATE_BLOCKED);

	if (mrp_is_port_up(mrp->i_port)) {
		if (mrp->in_mode == MRP_IN_MODE_RC)
			mrp_port_link_change(mrp->i_port, true);
		else
			mrp_cfm_link_change(mrp->ifindex, mrp->cfm_peer_mepid,
					    false);
	}

	return 0;
}

static int mrp_set_mic_role(struct mrp *mrp)
{
	int err;

	if (!mrp->i_port)
		return -EINVAL;

	/* Reset the states but don't reset the timers */
	mrp->mim_state = MRP_MIM_STATE_AC_STAT1;
	mrp->mic_state = MRP_MIC_STATE_AC_STAT1;

	mrp_set_mic_state(mrp, MRP_MIC_STATE_AC_STAT1);

	err = mrp_set_in_role(mrp, BR_MRP_IN_ROLE_MIC);
	if (err)
		return err;

	mrp_port_set_state(mrp->i_port, BR_MRP_PORT_STATE_BLOCKED);

	if (mrp_is_port_up(mrp->i_port)) {
		if (mrp->in_mode == MRP_IN_MODE_RC)
			mrp_port_link_change(mrp->i_port, true);
		else
			mrp_cfm_link_change(mrp->ifindex, mrp->cfm_peer_mepid,
					    false);
	}

	return 0;
}

/* According to the standard each frame has a different sequence number. If it
 * is MRP_Test, MRP_TopologyChange or MRP_LinkChange
 */
static uint16_t mrp_next_seq(struct mrp *mrp)
{
	mrp->seq_id++;
	return mrp->seq_id;
}

/* Returns the MRP_TLVHeader */
static struct br_mrp_tlv_hdr *mrp_get_tlv_hdr(unsigned char *buf)
{
	/* First 2 bytes in each MRP frame is the version and after that
	 * is the tlv header, therefor skip the version
	 */
	return (struct br_mrp_tlv_hdr *) (buf + sizeof(uint16_t));
}

/* Returns the MRP_InTestHeader */
static struct br_mrp_in_test_hdr *mrp_get_in_test_hdr(unsigned char *buf)
{
	/* Remove MRP version, tlv header and get InTest header */
	buf += sizeof(int16_t) + sizeof(struct br_mrp_tlv_hdr);
	return (struct br_mrp_in_test_hdr *) buf;
}

/* Allocates MRP frame and set head part of the frames. This is the ethernet
 * and the MRP version
 */
static struct frame_buf *mrp_fb_alloc(void)
{
	struct frame_buf *fb;
	uint16_t *version;

	fb = fb_alloc(MRP_MAX_FRAME_LENGTH);
	if (!fb)
		return NULL;

	version = fb_put(fb, sizeof(*version));
	*version = __cpu_to_be16(MRP_VERSION);

	return fb;
}

static struct ethhdr *mrp_eth_alloc(const unsigned char *src,
				    const unsigned char *dst)
{
	struct ethhdr *hdr;

	hdr = malloc(sizeof(*hdr));
	if (!hdr)
		return NULL;

	memcpy(hdr->h_dest, dst, ETH_ALEN);
	memcpy(hdr->h_source, src, ETH_ALEN);
	hdr->h_proto = __cpu_to_be16(ETH_P_MRP);

	return hdr;
}

static void mrp_fb_tlv(struct frame_buf *fb, enum br_mrp_tlv_header_type type,
		       uint8_t length)
{
	struct br_mrp_tlv_hdr *hdr;

	hdr = fb_put(fb, sizeof(*hdr));
	hdr->type = type;
	hdr->length = length;
}

static void mrp_fb_sub_tlv(struct frame_buf *fb,
			   enum br_mrp_sub_tlv_header_type type, uint8_t length)
{
	struct br_mrp_sub_tlv_hdr *hdr;

	hdr = fb_put(fb, sizeof(*hdr));
	hdr->type = type;
	hdr->length = length;
}

static void mrp_fb_common(struct frame_buf *fb, struct mrp_port *p)
{
	struct br_mrp_common_hdr *hdr;

	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_COMMON, sizeof(*hdr));

	hdr = fb_put(fb, sizeof(*hdr));
	hdr->seq_id = __cpu_to_be16(mrp_next_seq(p->mrp));
	memcpy(hdr->domain, p->mrp->domain, MRP_DOMAIN_UUID_LENGTH);
}

static void mrp_forward(struct mrp_port *p, struct frame_buf *fb)
{
	if (fb->size < 60)
		fb->size += 60 - fb->size;

	struct iovec iov[1] =
	{
		{ .iov_base = fb->start, .iov_len = fb->size }
	};

	if (p->operstate == IF_OPER_UP)
		packet_send(p->ifindex, iov, 1, fb->size);
}

static void mrp_send(struct mrp_port *p, struct ethhdr *h, struct frame_buf *fb)
{
	if (sizeof(*h) + fb->size < 60)
		fb->size += 60 - fb->size - sizeof(*h);

	struct iovec iov[2] =
	{
		{ .iov_base = h, .iov_len = sizeof(*h) },
		{ .iov_base = fb->start, .iov_len = fb->size }
	};

	if (p->operstate == IF_OPER_UP)
		packet_send(p->ifindex, iov, 2, sizeof(*h) + fb->size);
}

/* Compose MRP_Test frame and forward the frame to the port p.
 * The MRP_Test frame has the following format:
 * MRP_Version, MRP_TLVHeader, MRP_Prio, MRP_SA, MRP_PortRole, MRP_RingState,
 * MRP_Transition, MRP_TimeStamp
 */
static void mrp_send_ring_test(struct mrp_port *p)
{
	struct br_mrp_ring_test_hdr *hdr = NULL;
	struct frame_buf *fb = NULL;
	struct mrp *mrp = p->mrp;
	struct ethhdr *h = NULL;
	struct timespec t;
	uint32_t time_ms;

	clock_gettime(CLOCK_MONOTONIC, &t);
	time_ms = t.tv_sec * 1000 + t.tv_nsec / 1000000;

	fb = mrp_fb_alloc();
	if (!fb)
		return;

	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_RING_TEST, sizeof(*hdr));
	hdr = fb_put(fb, sizeof(*hdr));

	hdr->prio = __cpu_to_be16(mrp->prio);
	ether_addr_copy(hdr->sa, mrp->macaddr);
	hdr->port_role = __cpu_to_be16(p->role);
	hdr->state = __cpu_to_be16(mrp->mrm_state == MRP_MRM_STATE_CHK_RC ?
			BR_MRP_RING_STATE_CLOSED : BR_MRP_RING_STATE_OPEN);
	hdr->transitions = __cpu_to_be16(mrp->ring_transitions);
	hdr->timestamp = __cpu_to_be32(time_ms);

	mrp_fb_common(fb, p);
	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_END, 0x0);

	h = mrp_eth_alloc(p->macaddr, mrp_test_dmac);
	if (!h)
		goto out;

	mrp_send(p, h, fb);

	free(h);
out:
	free(fb->start);
	free(fb);
}

void mrp_ring_test_send(struct mrp *mrp)
{
	mrp_send_ring_test(mrp->p_port);
	mrp_send_ring_test(mrp->s_port);
}

/* Send MRP_Test frames on both MRP ports and start a timer to send
 * continuously frames with specific interval.
 */
void mrp_ring_test_req(struct mrp *mrp, uint32_t interval)
{
	mrp_ring_test_send(mrp);
	mrp_ring_test_start(mrp, interval);
}

/* Compose MRP_TopologyChange frame and forward the frame to the port p.
 * The MRP_TopologyChange frame has the following format:
 * MRP_Version, MRP_TLVHeader, MRP_Prio, MRP_SA, MRP_Interval
 */
static void mrp_send_ring_topo(struct mrp_port *p, uint32_t interval)
{
	struct br_mrp_ring_topo_hdr *hdr = NULL;
	struct frame_buf *fb = NULL;
	struct mrp *mrp = p->mrp;
	struct ethhdr *h = NULL;

	fb = mrp_fb_alloc();
	if (!fb)
		return;

	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_RING_TOPO, sizeof(*hdr));
	hdr = fb_put(fb, sizeof(*hdr));

	hdr->prio = __cpu_to_be16(mrp->prio);
	ether_addr_copy(hdr->sa, mrp->macaddr);
	hdr->interval = interval == 0 ? 0 : __cpu_to_be16(interval / 1000);

	mrp_fb_common(fb, p);
	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_END, 0x0);

	h = mrp_eth_alloc(p->macaddr, mrp_control_dmac);
	if (!h)
		goto out;

	mrp_send(p, h, fb);

	free(h);
out:
	free(fb->start);
	free(fb);
}

void mrp_ring_topo_send(struct mrp *mrp, uint32_t time)
{
	mrp_send_ring_topo(mrp->p_port, time);
	mrp_send_ring_topo(mrp->s_port, time);
}

/* Send MRP_TopologyChange frames on both MRP ports and start a timer to send
 * continuously frames with specific interval. If the interval is 0, then the
 * FDB needs to be clear, meaning that there was a change in the topology of the
 * network.
 */
void mrp_ring_topo_req(struct mrp *mrp, uint32_t time)
{
	pr_debug("time: %d", time);

	mrp_ring_topo_send(mrp, time * mrp->ring_topo_conf_max);

	if (!time) {
		ifdriver_flush(mrp);
	} else {
		uint32_t delay = mrp->ring_topo_conf_interval;

		mrp_ring_topo_start(mrp, delay);
	}
}

/* Compose MRP_LinkChange frame and forward the frame to the port p.
 * The MRP_LinkChange frame has the following format:
 * MRP_Version, MRP_TLVHeader, MRP_SA, MRP_PortRole, MRP_Interval, MRP_Blocked
 */
static void mrp_send_ring_link(struct mrp_port *p, bool up, uint32_t interval)
{
	struct br_mrp_ring_link_hdr *hdr = NULL;
	struct frame_buf *fb = NULL;
	struct mrp *mrp = p->mrp;
	struct ethhdr *h = NULL;

	fb = mrp_fb_alloc();
	if (!fb)
		return;

	mrp_fb_tlv(fb, up ? BR_MRP_TLV_HEADER_RING_LINK_UP :
			    BR_MRP_TLV_HEADER_RING_LINK_DOWN,
		      sizeof(*hdr));
	hdr = fb_put(fb, sizeof(*hdr));

	ether_addr_copy(hdr->sa, mrp->macaddr);
	hdr->port_role = __cpu_to_be16(p->role);
	hdr->interval = interval == 0 ? 0 : __cpu_to_be16(interval / 1000);
	hdr->blocked = __cpu_to_be16(mrp->blocked);

	mrp_fb_common(fb, p);
	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_END, 0x0);

	h = mrp_eth_alloc(p->macaddr, mrp_control_dmac);
	if (!h)
		goto out;

	mrp_send(p, h, fb);

	free(h);
out:
	free(fb->start);
	free(fb);
}

/* Send MRP_LinkChange frames on one of MRP ports */
void mrp_ring_link_req(struct mrp_port *p, bool up, uint32_t interval)
{
	pr_debug("port: %s, link_req up: %d interval: %d",
				p->ifname, up, interval);

	mrp_send_ring_link(p, up, interval);
}

static void mrp_send_test_mgr_nack(struct mrp_port *p, uint8_t sa[ETH_ALEN])
{
	struct br_mrp_test_mgr_nack_hdr *nack_hdr = NULL;
	struct br_mrp_sub_opt_hdr *sub_opt_hdr = NULL;
	struct br_mrp_oui_hdr *oui_hdr = NULL;
	struct frame_buf *fb = NULL;
	struct mrp *mrp = p->mrp;
	struct ethhdr *h = NULL;

	fb = mrp_fb_alloc();
	if (!fb)
		return;

	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_OPTION,
		   sizeof(struct br_mrp_oui_hdr) +
		   sizeof(struct br_mrp_sub_opt_hdr) +
		   sizeof(struct br_mrp_sub_tlv_hdr) +
		   sizeof(struct br_mrp_test_mgr_nack_hdr));

	oui_hdr = fb_put(fb, sizeof(*oui_hdr));
	memset(oui_hdr->oui, 0x0, MRP_OUI_LENGTH);

	sub_opt_hdr = fb_put(fb, sizeof(*sub_opt_hdr));
	sub_opt_hdr->type = 0x0;
	memset(sub_opt_hdr->manufacture_data, 0x0, MRP_MANUFACTURE_DATA_LENGTH);

	/* The number 2 is for padding */
	mrp_fb_sub_tlv(fb, BR_MRP_SUB_TLV_HEADER_TEST_MGR_NACK,
		       sizeof(*nack_hdr) + 2);
	nack_hdr = fb_put(fb, sizeof(*nack_hdr));

	nack_hdr->prio = __cpu_to_be16(mrp->prio);
	ether_addr_copy(nack_hdr->sa, mrp->macaddr);
	nack_hdr->other_prio = 0;
	ether_addr_copy(nack_hdr->other_sa, sa);

	fb_put(fb, 2);

	mrp_fb_common(fb, p);
	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_END, 0x0);

	h = mrp_eth_alloc(p->macaddr, mrp_test_dmac);
	if (!h)
		goto out;

	mrp_send(p, h, fb);

	free(h);
out:
	free(fb->start);
	free(fb);
}

static void mrp_test_mgr_nack_req(struct mrp *mrp, uint8_t sa[ETH_ALEN])
{
	mrp_send_test_mgr_nack(mrp->p_port, sa);
	mrp_send_test_mgr_nack(mrp->s_port, sa);
}

static void mrp_send_test_prop(struct mrp_port *p)
{
	struct br_mrp_sub_opt_hdr *sub_opt_hdr = NULL;
	struct br_mrp_test_prop_hdr *prop_hdr = NULL;
	struct br_mrp_oui_hdr *oui_hdr = NULL;
	struct frame_buf *fb = NULL;
	struct mrp *mrp = p->mrp;
	struct ethhdr *h = NULL;

	fb = mrp_fb_alloc();
	if (!fb)
		return;

	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_OPTION,
		   sizeof(struct br_mrp_oui_hdr) +
		   sizeof(struct br_mrp_sub_opt_hdr) +
		   sizeof(struct br_mrp_sub_tlv_hdr) +
		   sizeof(struct br_mrp_test_prop_hdr));

	oui_hdr = fb_put(fb, sizeof(*oui_hdr));
	memset(oui_hdr->oui, 0x0, MRP_OUI_LENGTH);

	sub_opt_hdr = fb_put(fb, sizeof(*sub_opt_hdr));
	sub_opt_hdr->type = 0x0;
	memset(sub_opt_hdr->manufacture_data, 0x0, MRP_MANUFACTURE_DATA_LENGTH);

	/* The number 2 is for padding */
	mrp_fb_sub_tlv(fb, BR_MRP_SUB_TLV_HEADER_TEST_PROPAGATE,
		       sizeof(*prop_hdr) + 2);
	prop_hdr = fb_put(fb, sizeof(*prop_hdr));

	prop_hdr->prio = __cpu_to_be16(mrp->prio);
	ether_addr_copy(prop_hdr->sa, mrp->macaddr);
	prop_hdr->other_prio = __cpu_to_be16(mrp->ring_prio);
	ether_addr_copy(prop_hdr->other_sa, mrp->ring_mac);

	mrp_fb_common(fb, p);
	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_END, 0x0);

	h = mrp_eth_alloc(p->macaddr, mrp_test_dmac);
	if (!h)
		goto out;

	mrp_send(p, h, fb);

	free(h);
out:
	free(fb->start);
	free(fb);
}

static void mrp_test_prop_req(struct mrp *mrp)
{
	mrp_send_test_prop(mrp->p_port);
	mrp_send_test_prop(mrp->s_port);
}

/* Compose MRP_IntTest frame and send the frame to the port p.
 * The MRP_IntTest frame has the following format:
 * MRP_Version, MRP_TLVHeader, MRP_SA, MRP_IntId, MRP_PortRole, MRP_InState,
 * MRP_Transition, MRP_TimeStamp
 */
static void mrp_send_in_test(struct mrp_port *p)
{
	struct br_mrp_in_test_hdr *hdr = NULL;
	struct frame_buf *fb = NULL;
	struct mrp *mrp = p->mrp;
	struct ethhdr *h = NULL;
	struct timespec t;
	uint32_t time_ms;

	clock_gettime(CLOCK_MONOTONIC, &t);
	time_ms = t.tv_sec * 1000 + t.tv_nsec / 1000000;

	fb = mrp_fb_alloc();
	if (!fb)
		return;

	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_IN_TEST, sizeof(*hdr));
	hdr = fb_put(fb, sizeof(*hdr));

	ether_addr_copy(hdr->sa, mrp->macaddr);
	hdr->id = __cpu_to_be16(mrp->in_id);
	hdr->port_role = __cpu_to_be16(p->role);
	hdr->state = __cpu_to_be16(mrp->mim_state == MRP_MIM_STATE_CHK_IC ?
				BR_MRP_IN_STATE_CLOSED : BR_MRP_IN_STATE_OPEN);
	hdr->transitions = __cpu_to_be16(mrp->in_transitions);
	hdr->timestamp = __cpu_to_be32(time_ms);

	mrp_fb_common(fb, p);
	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_END, 0x0);

	h = mrp_eth_alloc(p->macaddr, mrp_itest_dmac);
	if (!h)
		goto out;

	mrp_send(p, h, fb);

	free(h);
out:
	free(fb->start);
	free(fb);
}

void mrp_in_test_send(struct mrp *mrp)
{
	mrp_send_in_test(mrp->p_port);
	mrp_send_in_test(mrp->s_port);
	mrp_send_in_test(mrp->i_port);
}

/* Send MRP_IntTopologyChange frames on all MRP ports and start a timer to send
 * continuously frames with specific interval.
 */
void mrp_in_test_req(struct mrp *mrp, uint32_t interval)
{
	mrp_in_test_send(mrp);
	mrp_in_test_start(mrp, interval);
}

/* Compose MRP_IntTopologyChange frame and send the frame to the port p.
 * The MRP_IntTopologyChange frame has the following format:
 * MRP_Version, MRP_TLVHeader, MRP_SA, MRP_IntId, MRP_Interval
 */
static void mrp_send_in_topo(struct mrp_port *p, uint32_t interval)
{
	struct br_mrp_in_topo_hdr *hdr = NULL;
	struct frame_buf *fb = NULL;
	struct mrp *mrp = p->mrp;
	struct ethhdr *h = NULL;

	fb = mrp_fb_alloc();
	if (!fb)
		return;

	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_IN_TOPO, sizeof(*hdr));
	hdr = fb_put(fb, sizeof(*hdr));

	ether_addr_copy(hdr->sa, mrp->macaddr);
	hdr->id = __cpu_to_be16(mrp->in_id);
	hdr->interval = interval == 0 ? 0 : __cpu_to_be16(interval / 1000);

	mrp_fb_common(fb, p);
	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_END, 0x0);

	h = mrp_eth_alloc(p->macaddr, mrp_icontrol_dmac);
	if (!h)
		goto out;

	mrp_send(p, h, fb);

	free(h);
out:
	free(fb->start);
	free(fb);
}

void mrp_in_topo_send(struct mrp *mrp, uint32_t interval)
{
	mrp_send_in_topo(mrp->p_port, interval);
	mrp_send_in_topo(mrp->s_port, interval);
	mrp_send_in_topo(mrp->i_port, interval);
}

/* Send MRP_IntTopologyChange frames on all MRP ports and start a timer to send
 * continuously frames with specific interval. If the interval is 0, then the
 * FDB needs to be clear, meaning that there was a change in the topology of the
 * network.
 */
void mrp_in_topo_req(struct mrp *mrp, uint32_t time)
{
	pr_debug("time: %d", time);

	mrp_in_topo_send(mrp, time * mrp->in_topo_conf_max);

	if (!time) {
		ifdriver_flush(mrp);
	} else {
		uint32_t delay = mrp->in_topo_conf_interval;

		mrp_in_topo_start(mrp, delay);
	}
}

/* Compose MRP_LinkChange frame and send the frame to the port p.
 * The MRP_LinkChange frame has the following format:
 * MRP_Version, MRP_TLVHeader, MRP_SA, MRP_IntId,  MRP_PortRole, MRP_Interval
 */
static void mrp_send_in_link(struct mrp_port *p, bool up, uint32_t interval)
{
	struct br_mrp_in_link_hdr *hdr = NULL;
	struct frame_buf *fb = NULL;
	struct mrp *mrp = p->mrp;
	struct ethhdr *h = NULL;

	fb = mrp_fb_alloc();
	if (!fb)
		return;

	mrp_fb_tlv(fb, up ? BR_MRP_TLV_HEADER_IN_LINK_UP:
			    BR_MRP_TLV_HEADER_IN_LINK_DOWN,
		       sizeof(*hdr));
	hdr = fb_put(fb, sizeof(*hdr));

	ether_addr_copy(hdr->sa, mrp->macaddr);
	hdr->port_role = __cpu_to_be16(p->role);
	hdr->id = __cpu_to_be16(mrp->in_id);
	hdr->interval = interval == 0 ? 0 : __cpu_to_be16(interval / 1000);

	mrp_fb_common(fb, p);
	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_END, 0x0);

	h = mrp_eth_alloc(p->macaddr, mrp_icontrol_dmac);
	if (!h)
		goto out;

	mrp_send(p, h, fb);

	free(h);
out:
	free(fb->start);
	free(fb);
}

/* Send MRP_IntLinkChange frames on all MRP ports */
void mrp_in_link_req(struct mrp *mrp, bool up, uint32_t  interval)
{
	pr_debug("up:%d, interval: %d", up, interval);

	mrp_send_in_link(mrp->p_port, up, interval);
	mrp_send_in_link(mrp->s_port, up, interval);
	mrp_send_in_link(mrp->i_port, up, interval);
}

/* Compose MRP_LinkStatusPoll frame and send the frame to the port p.
 * The MRP_LinkStatusPoll frame has the following format:
 * MRP_Version, MRP_TLVHeader, MRP_SA, MRP_IntId,  MRP_PortRole
 */
static void mrp_send_in_link_status(struct mrp_port *p)
{
	struct br_mrp_in_link_status_hdr *hdr = NULL;
	struct frame_buf *fb = NULL;
	struct mrp *mrp = p->mrp;
	struct ethhdr *h = NULL;

	fb = mrp_fb_alloc();
	if (!fb)
		return;

	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_IN_LINK_STATUS, sizeof(*hdr));
	hdr = fb_put(fb, sizeof(*hdr));

	ether_addr_copy(hdr->sa, mrp->macaddr);
	hdr->port_role = __cpu_to_be16(p->role);
	hdr->id = __cpu_to_be16(mrp->in_id);

	mrp_fb_common(fb, p);
	mrp_fb_tlv(fb, BR_MRP_TLV_HEADER_END, 0x0);

	h = mrp_eth_alloc(p->macaddr, mrp_icontrol_dmac);
	if (!h)
		goto out;

	mrp_send(p, h, fb);

	free(h);
out:
	free(fb->start);
	free(fb);
}

/* Send MRP_IntLinkStatusPoll frames on MRP ring ports */
void mrp_in_link_status_req(struct mrp *mrp, uint32_t interval)
{
	uint32_t delay = mrp->in_link_status_conf_interval;

	mrp_send_in_link_status(mrp->p_port);
	mrp_send_in_link_status(mrp->s_port);

	if (interval)
		mrp_in_link_status_start(mrp, delay);
}

/* Represents the state machine for when a MRP_Test frame was received on one
 * of the MRP ports and the MRP instance has the role MRM.
 */
static void mrp_mrm_recv_ring_test(struct mrp *mrp)
{
	uint32_t topo_interval = mrp->ring_topo_conf_interval;

	switch (mrp->mrm_state) {
	case MRP_MRM_STATE_AC_STAT1:
		/* Ignore */
		break;
	case MRP_MRM_STATE_PRM_UP:
		mrp->ring_test_curr_max = mrp->ring_test_conf_max - 1;
		mrp->ring_test_curr = 0;
		mrp->no_tc = false;

		mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);

		mrp_set_mrm_state(mrp, MRP_MRM_STATE_CHK_RC);
		break;
	case MRP_MRM_STATE_CHK_RO:
		mrp_port_set_state(mrp->s_port,
					   BR_MRP_PORT_STATE_BLOCKED);

		mrp->ring_test_curr_max = mrp->ring_test_conf_max - 1;
		mrp->ring_test_curr = 0;
		mrp->no_tc = false;

		mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);

		topo_interval = mrp->react_on_link_change ? 0 : topo_interval;
		mrp_ring_topo_req(mrp, topo_interval);

		mrp_set_mrm_state(mrp, MRP_MRM_STATE_CHK_RC);
		break;
	case MRP_MRM_STATE_CHK_RC:
		mrp->ring_test_curr_max = mrp->ring_test_conf_max - 1;
		mrp->ring_test_curr = 0;
		mrp->no_tc = false;

		break;
	}
}

static bool mrp_better_than_own(struct mrp *mrp,
				struct br_mrp_ring_test_hdr *hdr)
{
	uint16_t prio = __be16_to_cpu(hdr->prio);

	if (prio < mrp->prio ||
	    (prio == mrp->prio &&
	    ether_addr_to_u64(hdr->sa) < ether_addr_to_u64(mrp->macaddr)))
		return true;

	return false;
}

static void mrp_mra_recv_ring_test(struct mrp *mrp,
				   struct br_mrp_ring_test_hdr *hdr)
{
	if (mrp->ring_role == BR_MRP_RING_ROLE_MRM) {
		if (!mrp_better_than_own(mrp, hdr))
			mrp_test_mgr_nack_req(mrp, hdr->sa);

		return;
	}

	if (mrp->ring_role == BR_MRP_RING_ROLE_MRC) {
		if (ether_addr_equal(hdr->sa, mrp->ring_mac))
			return;

		if (mrp_better_than_own(mrp, hdr))
			mrp->ring_mon_curr = 0;

		mrp->ring_prio = hdr->prio;
	}
}

static void mrp_recv_ring_test(struct mrp_port *p, unsigned char *buf)
{
	struct br_mrp_ring_test_hdr *hdr;
	struct mrp *mrp = p->mrp;

	/* remove MRP version, tlv and get test header */
	buf += sizeof(int16_t) + sizeof(struct br_mrp_tlv_hdr);
	hdr = (struct br_mrp_ring_test_hdr*)buf;

	/* If the MRP_Test frames was not send by this instance process it
	 * if MRA support is enabled. Otherwise it's an error!
	 */
	if (!ether_addr_equal(hdr->sa, mrp->macaddr)) {
		if (!mrp->mra_support) {
			pr_warn_ratelimit("Received unexpected MRP Test frame");
			return;
		}

		mrp_mra_recv_ring_test(mrp, hdr);
		return;
	}

	mrp_mrm_recv_ring_test(mrp);
}

/* Represents the state machine for when a MRP_TopologyChange frame was
 * received on one of the MRP ports and the MRP instance has the role MRM and
 * has MRA support;
 */
static void mrp_mra_recv_ring_topo(struct mrp_port *p, unsigned char *buf)
{
	struct br_mrp_ring_topo_hdr *hdr;
	struct mrp *mrp = p->mrp;

	pr_debug_v("mrm state: %s", mrp_get_mrm_state(mrp->mrm_state));

	/* remove MRP version, tlv and get ring topo header */
	buf += sizeof(int16_t) + sizeof(struct br_mrp_tlv_hdr);
	hdr = (struct br_mrp_ring_topo_hdr *)buf;

	if (ether_addr_equal(hdr->sa, mrp->macaddr))
		return;


	mrp_clear_fdb_start(mrp, ntohs(hdr->interval) * 1000);
}

/* Represents the state machine for when a MRP_TopologyChange frame was
 * received on one of the MRP ports and the MRP instance has the role MRC
 */
static void mrp_mrc_recv_ring_topo(struct mrp_port *p, unsigned char *buf)
{
	struct br_mrp_ring_topo_hdr *hdr;
	struct mrp *mrp = p->mrp;

	pr_debug_v("port: %s, mrc state: %s", p->ifname,
	        mrp_get_mrc_state(mrp->mrc_state));

	/* remove MRP version, tlv and get ring topo header */
	buf += sizeof(int16_t) + sizeof(struct br_mrp_tlv_hdr);
	hdr = (struct br_mrp_ring_topo_hdr *)buf;

	switch (mrp->mrc_state) {
	case MRP_MRC_STATE_AC_STAT1:
		/* Ignore */
		break;
	case MRP_MRC_STATE_DE_IDLE:
		mrp_clear_fdb_start(mrp, ntohs(hdr->interval) * 1000);
		break;
	case MRP_MRC_STATE_PT:
		mrp->ring_link_curr_max = mrp->ring_link_conf_max;
		mrp_ring_link_up_stop(mrp);
		mrp_port_set_state(mrp->s_port,
					   BR_MRP_PORT_STATE_FORWARDING);
		mrp_clear_fdb_start(mrp, ntohs(hdr->interval) * 1000);
		mrp_set_mrc_state(mrp, MRP_MRC_STATE_PT_IDLE);
		break;
	case MRP_MRC_STATE_DE:
		mrp->ring_link_curr_max = mrp->ring_link_conf_max;
		mrp_ring_link_down_stop(mrp);
		mrp_clear_fdb_start(mrp, ntohs(hdr->interval) * 1000);
		mrp_set_mrc_state(mrp, MRP_MRC_STATE_DE_IDLE);
		break;
	case MRP_MRC_STATE_PT_IDLE:
		mrp_clear_fdb_start(mrp, ntohs(hdr->interval) * 1000);
		break;
	}
}

static void mrp_recv_ring_topo(struct mrp_port *p, unsigned char *buf)
{
	struct mrp *mrp = p->mrp;

	if (mrp->mra_support && mrp->ring_role == BR_MRP_RING_ROLE_MRM)
		return mrp_mra_recv_ring_topo(p, buf);

	return mrp_mrc_recv_ring_topo(p, buf);
}

/* Represents the state machine for when a MRP_LinkChange frame was
 * received on one of the MRP ports and the MRP instance has the role MRM. When
 * MRP instance has the role MRC it doesn't need to process the frame.
 */
static void mrp_recv_ring_link(struct mrp_port *p, unsigned char *buf)
{
	enum br_mrp_tlv_header_type type;
	struct mrp *mrp = p->mrp;
	struct br_mrp_tlv_hdr *tlv;

	pr_debug_v("port: %s, mrm state: %s",
	        p->ifname, mrp_get_mrm_state(mrp->mrm_state));

	/* remove MRP version to get the tlv */
	buf += sizeof(uint16_t);
	tlv = (struct br_mrp_tlv_hdr *)buf;

	type = tlv->type;

	switch (mrp->mrm_state) {
	case MRP_MRM_STATE_AC_STAT1:
		/* Ignore */
		break;
	case MRP_MRM_STATE_PRM_UP:
		if (mrp->blocked) {
			if (mrp->add_test)
				break;
			mrp->add_test = true;
			mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);
		} else {
			if (type == BR_MRP_TLV_HEADER_RING_LINK_DOWN)
				break;

			if (!mrp->add_test) {
				mrp->add_test = true;
				mrp_ring_test_req(mrp,
						  mrp->ring_test_conf_short);
			}
			mrp_ring_topo_req(mrp, 0);
		}
		break;
	case MRP_MRM_STATE_CHK_RO:
		if (!mrp->add_test && type == BR_MRP_TLV_HEADER_RING_LINK_UP &&
		    mrp->blocked) {
			mrp->add_test = true;
			mrp_ring_test_req(mrp, mrp->ring_test_conf_short);
			break;
		}

		if (mrp->add_test && type == BR_MRP_TLV_HEADER_RING_LINK_UP &&
		    mrp->blocked)
			break;

		if (mrp->add_test && type == BR_MRP_TLV_HEADER_RING_LINK_DOWN)
			break;

		if (!mrp->add_test && type == BR_MRP_TLV_HEADER_RING_LINK_DOWN) {
			mrp->add_test = true;
			mrp_ring_test_req(mrp, mrp->ring_test_conf_short);
			break;
		}

		if (type == BR_MRP_TLV_HEADER_RING_LINK_UP && !mrp->blocked) {
			mrp_port_set_state(mrp->s_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp->ring_test_curr_max = mrp->ring_test_conf_max - 1;
			mrp->ring_test_curr = 0;

			if (!mrp->add_test) {
				mrp_ring_test_req(mrp,
						  mrp->ring_test_conf_short);
				mrp->add_test = true;
			} else {
				mrp_ring_test_req(mrp,
						  mrp->ring_test_conf_interval);
			}

			mrp_ring_topo_req(mrp, 0);
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_CHK_RC);
			break;
		}
		break;
	case MRP_MRM_STATE_CHK_RC:
		if (mrp->add_test && !mrp->react_on_link_change &&
		    mrp->blocked)
			break;

		if (!mrp->add_test && !mrp->react_on_link_change &&
		    mrp->blocked) {
			mrp->add_test = true;
			mrp_ring_test_req(mrp, mrp->ring_test_conf_short);
			break;
		}

		if (type == BR_MRP_TLV_HEADER_RING_LINK_DOWN &&
		    mrp->react_on_link_change) {
			mrp_port_set_state(mrp->s_port,
						   BR_MRP_PORT_STATE_FORWARDING);
			mrp->ring_transitions++;
			mrp_ring_topo_req(mrp, 0);
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_CHK_RO);
			break;
		}

		if (type == BR_MRP_TLV_HEADER_RING_LINK_UP &&
		    mrp->react_on_link_change && !mrp->blocked) {
			mrp->ring_test_curr_max = mrp->ring_test_conf_max - 1;
			mrp_ring_topo_req(mrp, 0);
		}

		if (type == BR_MRP_TLV_HEADER_RING_LINK_UP &&
		    mrp->react_on_link_change && mrp->blocked) {
			mrp->ring_test_curr_max = mrp->ring_test_conf_max - 1;
			mrp_ring_topo_req(mrp, 0);
		}
		break;
	}
}

static bool mrp_better_than_host(struct mrp *mrp,
				 struct br_mrp_test_mgr_nack_hdr *hdr)
{
	uint16_t prio = __be16_to_cpu(hdr->prio);

	if (prio < mrp->ring_prio ||
	    (prio == mrp->ring_prio &&
	    ether_addr_to_u64(hdr->sa) < ether_addr_to_u64(mrp->ring_mac)))
		return true;

	return false;
}

static void mrp_recv_nack(struct mrp_port *p, unsigned char *buf)
{
	struct br_mrp_test_mgr_nack_hdr *hdr;
	struct mrp *mrp = p->mrp;

	buf += sizeof(struct br_mrp_sub_tlv_hdr);
	hdr = (struct br_mrp_test_mgr_nack_hdr *)buf;

	if (mrp->ring_role == BR_MRP_RING_ROLE_MRC)
		return;

	if (ether_addr_equal(hdr->sa, mrp->macaddr))
		return;

	if (!ether_addr_equal(hdr->other_sa, mrp->macaddr))
		return;

	if (mrp_better_than_host(mrp, hdr)) {
		mrp->ring_prio = __be16_to_cpu(hdr->prio);
		memcpy(mrp->ring_mac, hdr->sa, ETH_ALEN);
	}

	if (mrp->mrm_state == MRP_MRM_STATE_CHK_RC)
		mrp_port_set_state(mrp->s_port,
					   BR_MRP_PORT_STATE_FORWARDING);

	mrp_ring_topo_stop(mrp);
	mrp_set_mrc_init(mrp);
	mrp_test_prop_req(mrp);

	switch (mrp->mrm_state) {
	case MRP_MRM_STATE_PRM_UP:
		mrp_set_mrc_state(mrp, MRP_MRC_STATE_DE_IDLE);
		mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRC);
		break;
	case MRP_MRM_STATE_CHK_RO:
		mrp_set_mrc_state(mrp, MRP_MRC_STATE_PT_IDLE);
		mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRC);
		break;
	case MRP_MRM_STATE_CHK_RC:
		mrp_set_mrc_state(mrp, MRP_MRC_STATE_PT_IDLE);
		mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRC);
		break;
	default:
		break;
	}
}

static void mrp_recv_propagate(struct mrp_port *p, unsigned char *buf)
{
	struct br_mrp_test_prop_hdr *hdr;
	struct mrp *mrp;

	buf += sizeof(struct br_mrp_sub_tlv_hdr);
	mrp = p->mrp;

	hdr = (struct br_mrp_test_prop_hdr *)buf;

	if (mrp->ring_role == BR_MRP_RING_ROLE_MRM)
		return;

	if (!ether_addr_equal(hdr->sa, mrp->macaddr))
		return;

	if (hdr->other_prio != hdr->prio)
		return;

	mrp->ring_prio = __be16_to_cpu(hdr->other_prio);
	memcpy(mrp->ring_mac, hdr->other_sa, ETH_ALEN);
	mrp->ring_mon_curr = 0;
}

/* Represents the state machine for when a MRP_Option frame was
 * received on one of the MRP ports.
 */
static void mrp_recv_option(struct mrp_port *p, unsigned char *buf)
{
	struct br_mrp_sub_tlv_hdr *sub_tlv;
	struct mrp *mrp = p->mrp;

	pr_debug_v("port %s, mrm state: %s", p->ifname,
	        mrp_get_mrm_state(mrp->mrm_state));

	/* remove MRP version to get the tlv */
	buf += sizeof(uint16_t);
	/* remove also the tlv_hdr, mrp_oui, and sub_opt */
	buf += sizeof(struct br_mrp_tlv_hdr) +
	       sizeof(struct br_mrp_oui_hdr) +
	       sizeof(struct br_mrp_sub_opt_hdr);

	sub_tlv = (struct br_mrp_sub_tlv_hdr *)buf;
	if (sub_tlv->type == BR_MRP_SUB_TLV_HEADER_TEST_MGR_NACK)
		return mrp_recv_nack(p, buf);
	if (sub_tlv->type == BR_MRP_SUB_TLV_HEADER_TEST_PROPAGATE)
		return mrp_recv_propagate(p, buf);
}

static void mrp_mim_recv_in_test(struct mrp *mrp)
{
	switch (mrp->mim_state) {
	case MRP_MIM_STATE_AC_STAT1:
		mrp_port_set_state(mrp->i_port,
					   BR_MRP_PORT_STATE_BLOCKED);

		mrp->in_test_curr_max = mrp->in_test_conf_max - 1;
		mrp->in_test_curr = 0;

		mrp_in_test_req(mrp, mrp->in_test_conf_interval);

		mrp_set_mim_state(mrp, MRP_MIM_STATE_CHK_IC);
		break;
	case MRP_MIM_STATE_CHK_IO:
		mrp_port_set_state(mrp->i_port,
					   BR_MRP_PORT_STATE_BLOCKED);

		mrp->in_test_curr_max = mrp->in_test_conf_max - 1;
		mrp->in_test_curr = 0;

		mrp_in_topo_req(mrp, mrp->in_topo_conf_interval);
		mrp_in_test_req(mrp, mrp->in_test_conf_interval);

		mrp_set_mim_state(mrp, MRP_MIM_STATE_CHK_IC);
		break;
	case MRP_MIM_STATE_CHK_IC:
		mrp->in_test_curr_max = mrp->in_test_conf_max - 1;
		mrp->in_test_curr = 0;
		break;
	default:
		break;
		/* Ignore */
	}
}

static void mrp_recv_in_test(struct mrp_port *p, unsigned char *buf)
{
	struct br_mrp_in_test_hdr *hdr;
	struct mrp *mrp = p->mrp;

	/* remove MRP version, tlv and get int test header */
	buf += sizeof(int16_t) + sizeof(struct br_mrp_tlv_hdr);
	hdr = (struct br_mrp_in_test_hdr*)buf;

	if (mrp->in_id != ntohs(hdr->id))
		return;

	mrp_mim_recv_in_test(mrp);
}

/* Represents the state machine for when a MRP_IntTopologyChange frame was
 * received on one of the MRP ports.
 */
static void mrp_recv_in_topo(struct mrp_port *p, unsigned char *buf)
{
	struct br_mrp_in_topo_hdr *hdr;
	struct mrp *mrp = p->mrp;

	/* remove MRP version, tlv and get int topo change header */
	buf += sizeof(int16_t) + sizeof(struct br_mrp_tlv_hdr);
	hdr = (struct br_mrp_in_topo_hdr *)buf;

	if (mrp->ring_role == BR_MRP_RING_ROLE_MRM) {
		pr_debug_v("mrm state: %s", mrp_get_mrm_state(mrp->mrm_state));
		if (mrp->ring_topo_running == false)
			mrp_ring_topo_req(mrp, ntohs(hdr->interval) * 1000);
	}

	if (mrp->in_role == BR_MRP_IN_ROLE_MIM) {
		pr_debug_v("mim state: %s", mrp_get_mim_state(mrp->mim_state));

		/* If MRP_SA == MRP_TS_SA ignore */
		if (ether_addr_equal(hdr->sa, mrp->macaddr))
			return;

		mrp_clear_fdb_start(mrp, ntohs(hdr->interval) * 1000);
	}

	if (mrp->in_role == BR_MRP_IN_ROLE_MIC) {
		pr_debug_v("mic state: %s", mrp_get_mic_state(mrp->mic_state));

		switch (mrp->mic_state) {
		case MRP_MIC_STATE_AC_STAT1:
			if (ntohs(hdr->id) == mrp->in_id)
				mrp_in_link_down_stop(mrp);
			break;
		case MRP_MIC_STATE_PT:
			mrp->in_link_curr_max = mrp->in_link_conf_max;
			mrp_in_link_up_stop(mrp);
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_FORWARDING);
			mrp_set_mic_state(mrp, MRP_MIC_STATE_IP_IDLE);
			break;
		case MRP_MIC_STATE_IP_IDLE:
			/* Ignore */
			break;
		}
	}
}

/* Represents the state machine for when a MRP_IntLinkChange frame was
 * received on one of the MRP ports.
 */
static void mrp_recv_in_link(struct mrp_port *p, unsigned char *buf)
{
	enum br_mrp_tlv_header_type type;
	struct br_mrp_in_link_hdr *hdr;
	struct br_mrp_tlv_hdr *tlv;
	struct mrp *mrp = p->mrp;

	pr_debug_v("mim state: %s", mrp_get_mim_state(mrp->mim_state));

	/* remove MRP version to get the tlv */
	buf += sizeof(int16_t);
	tlv = (struct br_mrp_tlv_hdr *)buf;

	type = tlv->type;

	buf += sizeof(struct br_mrp_tlv_hdr);
	hdr = (struct br_mrp_in_link_hdr *)buf;

	switch (mrp->mim_state) {
	case MRP_MIM_STATE_AC_STAT1:
		/* Ignore */
		break;
	case MRP_MIM_STATE_CHK_IO:
		if (ntohs(hdr->id) == mrp->in_id &&
		    type == BR_MRP_TLV_HEADER_IN_LINK_UP)
			mrp_in_test_req(mrp, mrp->in_test_conf_interval);
		break;
	case MRP_MIM_STATE_CHK_IC:
		if (ntohs(hdr->id) == mrp->in_id &&
		    type == BR_MRP_TLV_HEADER_IN_LINK_UP) {
			mrp->in_test_curr_max = mrp->in_test_conf_max;
			mrp_in_topo_req(mrp, mrp->in_topo_conf_interval);
		}
		if (ntohs(hdr->id) == mrp->in_id &&
		    type == BR_MRP_TLV_HEADER_IN_LINK_DOWN) {
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_FORWARDING);
			mrp_in_topo_req(mrp, mrp->in_topo_conf_interval);
			mrp_set_mim_state(mrp, MRP_MIM_STATE_CHK_IO);
		}
		break;
	}
}

/* Represents the state machine for when a MRP_IntLinkStatus frame was
 * received on one of the MRP ports.
 */
static void mrp_recv_in_link_status(struct mrp_port *p, unsigned char *buf)
{
	struct br_mrp_in_link_status_hdr *hdr;
	struct mrp *mrp = p->mrp;

	if (mrp->in_role != BR_MRP_IN_ROLE_MIC)
		return;

	if (mrp->in_mode != MRP_IN_MODE_LC)
		return;

	pr_debug_v("mic state: %s", mrp_get_mic_state(mrp->mic_state));

	/* remove MRP version, tlv and get in link status header */
	buf += sizeof(int16_t) + sizeof(struct br_mrp_tlv_hdr);
	hdr = (struct br_mrp_in_link_status_hdr *)buf;

	if (ntohs(hdr->id) != mrp->in_id)
		return;

	switch (mrp->mic_state) {
	case MRP_MIC_STATE_AC_STAT1:
		mrp_in_link_req(mrp, false, 0);
		break;
	case MRP_MIC_STATE_PT:
		mrp_in_link_req(mrp, true, 0);
		break;
	case MRP_MIC_STATE_IP_IDLE:
		mrp_in_link_req(mrp, true, 0);
		break;
	}
}

/* Check if the MRP frame needs to be dropped */
static bool mrp_should_drop(const struct mrp_port *p,
			    enum br_mrp_tlv_header_type type)
{
	/* All frames should be dropped if the state of the port is disabled */
	if (p->state == BR_MRP_PORT_STATE_DISABLED)
		return true;

	/* If receiving a MRP frame on a port which is not in MRP ring
	 * then the frame should be drop
	 */
	if (!mrp_is_mrp_port(p))
		return true;

	/* In case the port is in blocked state then the kernel
	 * will drop all NON-MRP frames and it would send all
	 * MRP frames to the upper layer. So here is needed to drop MRP frames
	 * if the port is in blocked state.
	 */
	if (mrp_is_ring_port(p) && p->state == BR_MRP_PORT_STATE_BLOCKED &&
	    type != BR_MRP_TLV_HEADER_RING_TOPO &&
	    type != BR_MRP_TLV_HEADER_RING_TEST &&
	    type != BR_MRP_TLV_HEADER_RING_LINK_UP &&
	    type != BR_MRP_TLV_HEADER_RING_LINK_DOWN &&
	    type != BR_MRP_TLV_HEADER_IN_TOPO &&
	    type != BR_MRP_TLV_HEADER_IN_LINK_UP &&
	    type != BR_MRP_TLV_HEADER_IN_LINK_DOWN &&
	    type != BR_MRP_TLV_HEADER_OPTION)
		return true;

	if (mrp_is_in_port(p) && p->state == BR_MRP_PORT_STATE_BLOCKED &&
	    type != BR_MRP_TLV_HEADER_IN_TEST &&
	    type != BR_MRP_TLV_HEADER_IN_LINK_UP &&
	    type != BR_MRP_TLV_HEADER_IN_LINK_DOWN &&
	    type != BR_MRP_TLV_HEADER_IN_TOPO)
		return true;

	return false;
}

/* Check if the MRP frame needs to be process. It depends of the MRP instance
 * role and the frame type if the frame needs to be processed or not.
 */
static bool mrp_should_process(const struct mrp_port *p,
			       enum br_mrp_tlv_header_type type)
{
	struct mrp *mrp = p->mrp;

	switch (type) {
	case BR_MRP_TLV_HEADER_RING_TEST:
		if (mrp->ring_role == BR_MRP_RING_ROLE_MRM ||
		    (mrp->ring_role == BR_MRP_RING_ROLE_MRC && mrp->mra_support))
			return true;
		break;
	case BR_MRP_TLV_HEADER_RING_LINK_DOWN:
	case BR_MRP_TLV_HEADER_RING_LINK_UP:
		if (mrp->ring_role == BR_MRP_RING_ROLE_MRM)
			return true;
		break;
	case BR_MRP_TLV_HEADER_RING_TOPO:
		if (mrp->ring_role == BR_MRP_RING_ROLE_MRC ||
		    (mrp->ring_role == BR_MRP_RING_ROLE_MRM && mrp->mra_support))
			return true;
		break;
	case BR_MRP_TLV_HEADER_OPTION:
		if (mrp->mra_support)
			return true;
	case BR_MRP_TLV_HEADER_IN_TEST:
		if (mrp->in_role == BR_MRP_IN_ROLE_MIM)
			return true;
		break;
	case BR_MRP_TLV_HEADER_IN_TOPO:
		if (mrp->in_role == BR_MRP_IN_ROLE_MIC ||
		    mrp->in_role == BR_MRP_IN_ROLE_MIM ||
		    mrp->ring_role == BR_MRP_RING_ROLE_MRM)
			return true;
		break;
	case BR_MRP_TLV_HEADER_IN_LINK_UP:
	case BR_MRP_TLV_HEADER_IN_LINK_DOWN:
		if (mrp->in_role == BR_MRP_IN_ROLE_MIM)
			return true;
		break;
	default:
		break;
	}

	return false;
}

static bool mrp_is_ring_frame(enum br_mrp_tlv_header_type type)
{
        if (type == BR_MRP_TLV_HEADER_RING_TEST ||
            type == BR_MRP_TLV_HEADER_RING_TOPO ||
            type == BR_MRP_TLV_HEADER_RING_LINK_DOWN ||
            type == BR_MRP_TLV_HEADER_RING_LINK_UP ||
            type == BR_MRP_TLV_HEADER_OPTION)
                return true;

        return false;
}

static bool mrp_is_in_frame(enum br_mrp_tlv_header_type type)
{
        if (type == BR_MRP_TLV_HEADER_IN_TEST ||
            type == BR_MRP_TLV_HEADER_IN_TOPO ||
            type == BR_MRP_TLV_HEADER_IN_LINK_DOWN ||
            type == BR_MRP_TLV_HEADER_IN_LINK_UP ||
            type == BR_MRP_TLV_HEADER_IN_LINK_STATUS)
                return true;

        return false;
}

/* Check if the MRP frame needs to be forwarded and, if so, forward the frame.
 *
 * It depends of the MRP instance role and the frame type if the frame needs to
 * be forwarded or not.
 */
static void mrp_check_and_forward(const struct mrp_port *p,
				  struct frame_buf *fb,
				  enum br_mrp_tlv_header_type type)
{
	struct mrp *mrp = p->mrp;
	struct mrp_port *forward_p_port = NULL;
	struct mrp_port *forward_s_port = NULL;
	struct mrp_port *forward_i_port = NULL;

	/* Set the possible forwarding ways according to the receiving port */
	if (p == mrp->p_port) {
		forward_s_port = mrp->s_port;
		forward_i_port = mrp->i_port;
	} else if (p == mrp->s_port) {
		forward_p_port = mrp->p_port;
		forward_i_port = mrp->i_port;
	} else if (p == mrp->i_port) {
		forward_p_port = mrp->p_port;
		forward_s_port = mrp->s_port;
	}

        if (mrp_is_ring_frame(type)) {
		/* We should not forward ring frames received from the
		 * interconnection port.
		 */
		if (p == mrp->i_port)
			return;

		/* If the frame is a ring frame then it should not be forwarded
		 * to the interconnection port.
		 */
		forward_i_port = NULL;

		switch (mrp->ring_role) {
		case BR_MRP_RING_ROLE_MRM:
			/* If the role is MRM then don't forward the frames */
			return;

		case BR_MRP_RING_ROLE_MRC:
			/* If the role is MRC and MRA support is not enabled
			 * then don't forward Header Option frames.
			 */
			if (type == BR_MRP_TLV_HEADER_OPTION &&
			    !mrp->mra_support)
				return;
			goto forward;

		default:
			break;
		}
	}

        if (mrp_is_in_frame(type)) {
		struct br_mrp_in_test_hdr *hdr = mrp_get_in_test_hdr(fb->data);

		switch (mrp->ring_role) {
		case BR_MRP_RING_ROLE_MRM:
			/* Nodes that behaves as MRM needs to stop forwarding
			 * the frames in case the ring is closed, otherwise will			 * be a loop.
			 * In this case the frame is no forward between the
			 * ring ports, but the frame may still have a chance to
			 * go to through the interconnection port!
			 */
			if ((mrp->p_port->state !=
					BR_MRP_PORT_STATE_FORWARDING ||
			     mrp->s_port->state !=
					BR_MRP_PORT_STATE_FORWARDING) &&
			    mrp_is_ring_port(p)) {
				forward_p_port = NULL;
				forward_s_port = NULL;
			}
			break;

		case BR_MRP_RING_ROLE_MRC:
			/* A node that behaves as MRC should not forward
			 * interconnect frames between its ring ports if
			 * it has an interconnection roles (MIM or MIC)
			 * that matches the frame interconnection ID.
			 */
			if ((mrp->in_role != BR_MRP_IN_ROLE_DISABLED) &&
			    (mrp->in_id == ntohs(hdr->id)) &&
			    mrp_is_ring_port(p)) {
				forward_p_port = NULL;
				forward_s_port = NULL;
			}
			break;

		default:
			break;
		}

		switch (mrp->in_role) {
		case BR_MRP_IN_ROLE_MIM:
			if (type == BR_MRP_TLV_HEADER_IN_TEST) {
                                /* MIM should not forward it's own InTest
                                 * frames between its ports, but it should
				 * forward others InTest frames between its
				 * ring ports if they are not from the
				 * interconnection port.
                                 */
				if (ether_addr_equal(hdr->sa, mrp->macaddr))
					return;
				else {
	                                if (mrp_is_in_port(p))
	                                        return;
					else
	                                        forward_i_port = NULL;
				}
			} else {
                                /* MIM should forward IntLinkChange/Status and
                                 * IntTopoChange between ring ports, but MIM
                                 * should not forward IntLinkChange/Status and
                                 * IntTopoChange if the frame was received at
                                 * the interconnect port
                                 */
                                if (mrp_is_ring_port(p))
                                        forward_i_port = NULL;

                                if (mrp_is_in_port(p))
                                        return;
			}
			break;

		case BR_MRP_IN_ROLE_MIC:
                        /* MIC should forward InTest frames on all ports
                         * regardless of the received port
                         */
			if (type == BR_MRP_TLV_HEADER_IN_TEST)
                                goto forward;

                        /* MIC should forward IntLinkChange frames only if they
                         * are received on ring ports to all the ports
                         */
			if ((type == BR_MRP_TLV_HEADER_IN_LINK_UP ||
			     type == BR_MRP_TLV_HEADER_IN_LINK_DOWN) &&
			    mrp_is_ring_port(p))
				goto forward;

                        /* MIC should forward IntLinkStatus frames only to
                         * interconnect port if it was received on a ring port.
                         * If it is received on interconnect port then, it
                         * should be forward on both ring ports
                         */
			if (type == BR_MRP_TLV_HEADER_IN_LINK_STATUS &&
			    mrp_is_ring_port(p)) {
				forward_p_port = NULL;
				forward_s_port = NULL;
				goto forward;
			}

                        /* Should forward the InTopo frames only between the
                         * ring ports
                         */
			if (type == BR_MRP_TLV_HEADER_IN_TOPO) {
				forward_i_port = NULL;
				goto forward;
			}

			/* Otherwise don't forward the frames */
			return;

		default:
			break;
		}
	}

forward:
	if (forward_p_port)
		mrp_forward(forward_p_port, fb);
	if (forward_s_port)
		mrp_forward(forward_s_port, fb);
	if (forward_i_port)
		mrp_forward(forward_i_port, fb);
}

static void mrp_process(struct mrp_port *p, unsigned char *buf,
			enum br_mrp_tlv_header_type type)
{
	switch (type) {
	case BR_MRP_TLV_HEADER_RING_TEST:
		mrp_recv_ring_test(p, buf);
		break;
	case BR_MRP_TLV_HEADER_RING_TOPO:
		mrp_recv_ring_topo(p, buf);
		break;
	case BR_MRP_TLV_HEADER_RING_LINK_DOWN:
	case BR_MRP_TLV_HEADER_RING_LINK_UP:
		mrp_recv_ring_link(p, buf);
		break;
	case BR_MRP_TLV_HEADER_OPTION:
		mrp_recv_option(p, buf);
		break;
	case BR_MRP_TLV_HEADER_IN_TEST:
		mrp_recv_in_test(p, buf);
		break;
	case BR_MRP_TLV_HEADER_IN_TOPO:
		mrp_recv_in_topo(p, buf);
		break;
	case BR_MRP_TLV_HEADER_IN_LINK_DOWN:
	case BR_MRP_TLV_HEADER_IN_LINK_UP:
		mrp_recv_in_link(p, buf);
		break;
	case BR_MRP_TLV_HEADER_IN_LINK_STATUS:
		mrp_recv_in_link_status(p, buf);
		break;
	default:
		pr_err("Unknown type: %d", type);
	}
}

/* All received MRP frames are added to a list of skbs and this function
 * pops the frame and process them. It decides if the MRP instance needs to
 * process it, forward it or dropp it
 */
static void mrp_process_frame(struct mrp_port *port, struct frame_buf *fb,
			      enum br_mrp_tlv_header_type type)
{
	struct mrp *mrp = port->mrp;

	pthread_mutex_lock(&mrp->lock);

	mrp_check_and_forward(port, fb, type);

	if (mrp_should_process(port, type)) {
		unsigned char nbuf[2048];

		/* Because there are cases when a frame needs to be
		 * proccesed and also forward, it is required to clone
		 * the frame for processing not to alter the original
		 * one.
		 */
		memcpy(nbuf, fb->data, fb->size - sizeof(struct ethhdr));

		mrp_process(port, nbuf, type);
	}

	pthread_mutex_unlock(&mrp->lock);
}

/* Receives all MRP frames and add them in a queue to be processed */
int mrp_recv(unsigned char *buf, int buf_len, struct sockaddr_ll *sl,
	     socklen_t salen)
{
	struct mrp_port *port;
	struct frame_buf fb;
	struct br_mrp_tlv_hdr *hdr;

	port = mrp_get_port(sl->sll_ifindex);
	if (!port)
		goto out;

	/* The buf contains also the link layer information. It is not possible
	 * to get rid completely of this because it is possible to forward the
	 * frame with this information therefor just putt it in fb and pass it
	 * arround
	 */
	fb.start = buf;
	fb.data = buf;
	fb.size = buf_len;
	fb.data += sizeof(struct ethhdr);

	hdr = mrp_get_tlv_hdr(fb.data);

	if (mrp_should_drop(port, hdr->type))
		goto out;

	mrp_process_frame(port, &fb, hdr->type);

out:
	return 0;
}

/* Represents the state machine for when MRP instance has the role MRM and the
 * link of one of the MRP ports is changed.
 */
static void mrp_mrm_port_link(struct mrp_port *p, bool up)
{
	struct mrp *mrp = p->mrp;
	uint32_t topo_interval = mrp->ring_topo_conf_interval;

	pr_debug("port: %s, up: %d, mrm_state: %s",
	        p->ifname, up, mrp_get_mrm_state(mrp->mrm_state));

	switch (mrp->mrm_state) {
	case MRP_MRM_STATE_AC_STAT1:
		if (up && p == mrp->p_port) {
			mrp_port_set_state(mrp->p_port,
						   BR_MRP_PORT_STATE_FORWARDING);
			mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_PRM_UP);
		}
		if (up && p != mrp->p_port) {
			mrp->s_port = mrp->p_port;
			mrp->p_port = p;
			mrp_port_set_state(mrp->p_port,
						   BR_MRP_PORT_STATE_FORWARDING);
			mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_PRM_UP);
		}
		break;
	case MRP_MRM_STATE_PRM_UP:
		if (!up && p == mrp->p_port) {
			mrp_ring_test_stop(mrp);
			mrp_port_set_state(mrp->p_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_AC_STAT1);
		}
		if (up && p != mrp->p_port) {
			mrp->ring_test_curr_max = mrp->ring_test_conf_max - 1;
			mrp->ring_test_curr = 0;
			mrp->no_tc = true;
			mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_CHK_RC);
		}
		break;
	case MRP_MRM_STATE_CHK_RO:
		if (!up && p == mrp->p_port) {
			mrp->p_port = mrp->s_port;
			mrp->s_port = p;
			mrp_port_set_state(mrp->s_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);
			mrp_ring_topo_req(mrp, topo_interval);
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_PRM_UP);
			break;
		}
		if (!up && p != mrp->p_port) {
			mrp_port_set_state(mrp->s_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_PRM_UP);
		}
		break;
	case MRP_MRM_STATE_CHK_RC:
		if (!up && p == mrp->p_port) {
			mrp->p_port = mrp->s_port;
			mrp->s_port = p;
			mrp_port_set_state(mrp->s_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_port_set_state(mrp->p_port,
						   BR_MRP_PORT_STATE_FORWARDING);
			mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);
			mrp_ring_topo_req(mrp, topo_interval);
			mrp->ring_transitions++;
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_PRM_UP);
			break;
		}
		if (!up && p != mrp->p_port) {
			mrp->ring_transitions++;
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_PRM_UP);
			break;
		}

		break;
	}
}

/* Represents the state machine for when MRP instance has the role MRC and the
 * link of one of the MRP ports is changed.
 */
static void mrp_mrc_port_link(struct mrp_port *p, bool up)
{
	struct mrp *mrp = p->mrp;

	pr_debug("port: %s up: %d, mrc_state: %s",
	        p->ifname, up, mrp_get_mrc_state(mrp->mrc_state));

	switch (mrp->mrc_state) {
	case MRP_MRC_STATE_AC_STAT1:
		if (up && p == mrp->p_port) {
			mrp_port_set_state(mrp->p_port,
						   BR_MRP_PORT_STATE_FORWARDING);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_DE_IDLE);
		}
		if (up && p != mrp->p_port) {
			mrp->s_port = mrp->p_port;
			mrp->p_port = p;
			mrp_port_set_state(mrp->p_port,
						   BR_MRP_PORT_STATE_FORWARDING);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_DE_IDLE);
		}
		break;
	case MRP_MRC_STATE_DE_IDLE:
		if (up && p != mrp->p_port) {
			mrp->ring_link_curr_max = mrp->ring_link_conf_max;
			mrp_ring_link_up_start(mrp,
					       mrp->ring_link_conf_interval);
			mrp_ring_link_req(mrp->p_port, up,
					  mrp->ring_link_curr_max *
					  mrp->ring_link_conf_interval);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_PT);
		}
		if (!up && p == mrp->p_port) {
			mrp_port_set_state(mrp->p_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_AC_STAT1);
		}
		break;
	case MRP_MRC_STATE_PT:
		if (!up && p != mrp->p_port) {
			mrp->ring_link_curr_max = mrp->ring_link_conf_max;
			mrp_ring_link_up_stop(mrp);
			mrp_port_set_state(mrp->s_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_ring_link_down_start(mrp,
						 mrp->ring_link_conf_interval);
			mrp_ring_link_req(mrp->p_port, up,
					  mrp->ring_link_curr_max *
					  mrp->ring_link_conf_interval);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_DE);
			break;
		}
		if (!up && p == mrp->p_port) {
			mrp->ring_link_curr_max = mrp->ring_link_conf_max;
			mrp_ring_link_up_stop(mrp);
			mrp->p_port = mrp->s_port;
			mrp->s_port = p;
			mrp_port_set_state(mrp->p_port,
						   BR_MRP_PORT_STATE_FORWARDING);
			mrp_port_set_state(mrp->s_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_ring_link_down_start(mrp,
						 mrp->ring_link_conf_interval);
			mrp_ring_link_req(mrp->p_port, up,
					  mrp->ring_link_curr_max *
					  mrp->ring_link_conf_interval);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_DE);
		}
		break;
	case MRP_MRC_STATE_DE:
		if (up && p != mrp->p_port) {
			mrp->ring_link_curr_max = mrp->ring_link_conf_max;
			mrp_ring_link_down_stop(mrp);
			mrp_ring_link_up_start(mrp,
					       mrp->ring_link_conf_interval);
			mrp_ring_link_req(mrp->p_port, up,
					  mrp->ring_link_curr_max *
					  mrp->ring_link_conf_interval);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_PT);
		}
		if (!up && p == mrp->p_port) {
			mrp->ring_link_curr_max = mrp->ring_link_conf_max;
			mrp_port_set_state(mrp->p_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_ring_link_down_stop(mrp);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_AC_STAT1);
		}
		break;
	case MRP_MRC_STATE_PT_IDLE:
		if (!up && p != mrp->p_port) {
			mrp->ring_link_curr_max = mrp->ring_link_conf_max;
			mrp_port_set_state(mrp->s_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_ring_link_down_start(mrp,
						 mrp->ring_link_conf_interval);
			mrp_ring_link_req(mrp->p_port, up,
					  mrp->ring_link_curr_max *
					  mrp->ring_link_conf_interval);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_DE);
		}
		if (!up && p == mrp->p_port) {
			mrp->ring_link_curr_max = mrp->ring_link_conf_max;
			mrp->p_port = mrp->s_port;
			mrp->s_port = p;
			mrp_port_set_state(mrp->s_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_ring_link_down_start(mrp,
						 mrp->ring_link_conf_interval);
			mrp_ring_link_req(mrp->p_port, up,
					  mrp->ring_link_curr_max *
					  mrp->ring_link_conf_interval);
			mrp_set_mrc_state(mrp, MRP_MRC_STATE_DE);
		}
		break;
	}
}

/* Represents the state machine for when MRP instance has the role MIM and the
 * link of one of the MRP ports is changed.
 */
static void mrp_mim_port_link(struct mrp_port *p, bool up)
{
	struct mrp *mrp = p->mrp;

	pr_debug("up: %d, mim_state: %s",
	        up, mrp_get_mim_state(mrp->mim_state));

	if (up && mrp->in_mode == MRP_IN_MODE_RC) {
		switch (mrp->mim_state) {
		case MRP_MIM_STATE_AC_STAT1:
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp->in_test_curr_max = mrp->in_test_conf_max - 1;
			mrp->in_test_curr = 0;
			mrp_in_test_req(mrp, mrp->in_test_conf_interval);
			mrp_set_mim_state(mrp, MRP_MIM_STATE_CHK_IC);
			break;
		case MRP_MIM_STATE_CHK_IO:
			/* Ignore */
			break;
		case MRP_MIM_STATE_CHK_IC:
			/* Ignore */
			break;
		}
	}

	if (!up && mrp->in_mode == MRP_IN_MODE_RC) {
		switch (mrp->mim_state) {
		case MRP_MIM_STATE_AC_STAT1:
			/* Ignore */
			break;
		case MRP_MIM_STATE_CHK_IO: /* fallthrough */
		case MRP_MIM_STATE_CHK_IC:
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_in_topo_req(mrp, mrp->in_topo_conf_interval);
			mrp_in_test_req(mrp, mrp->in_test_conf_interval);
			mrp_set_mim_state(mrp, MRP_MIM_STATE_AC_STAT1);
			break;
		}
	}

	if (up && mrp->in_mode == MRP_IN_MODE_LC) {
		switch (mrp->mim_state) {
		case MRP_MIM_STATE_AC_STAT1:
			mrp->in_link_status_curr_max = mrp->in_link_status_conf_max;
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_in_link_status_req(mrp,
					       mrp->in_link_status_conf_interval);
			mrp_set_mim_state(mrp, MRP_MIM_STATE_CHK_IC);
			break;
		case MRP_MIM_STATE_CHK_IO:
			/* Ignore */
			break;
		case MRP_MIM_STATE_CHK_IC:
			/* Ignore */
			break;
		}
	}

	if (!up && mrp->in_mode == MRP_IN_MODE_LC) {
		switch (mrp->mim_state) {
		case MRP_MIM_STATE_AC_STAT1:
			/* Ignore */
			break;
		case MRP_MIM_STATE_CHK_IO:
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_in_link_status_stop(mrp);
			mrp_set_mim_state(mrp, MRP_MIM_STATE_AC_STAT1);
			break;
		case MRP_MIM_STATE_CHK_IC:
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_set_mim_state(mrp, MRP_MIM_STATE_AC_STAT1);
			break;
		}
	}


	pr_debug("new mim_state: %s", mrp_get_mim_state(mrp->mim_state));
}

/* Represents the state machine for when MRP instance has the role MIC and the
 * link of one of the MRP ports is changed.
 */
static void mrp_mic_port_link(struct mrp_port *p, bool up)
{
	struct mrp *mrp = p->mrp;

	pr_debug_v("up: %d, mic_state: %s",
	        up, mrp_get_mic_state(mrp->mic_state));

	if (up && mrp->in_mode == MRP_IN_MODE_RC) {
		switch (mrp->mic_state) {
		case MRP_MIC_STATE_AC_STAT1:
			mrp->in_link_curr_max = mrp->in_link_conf_max;
			mrp_in_link_down_stop(mrp);
			mrp_in_link_up_start(mrp, mrp->in_link_conf_interval);
			mrp_in_link_req(mrp, up, mrp->in_link_conf_max *
					mrp->in_link_conf_interval);
			mrp_set_mic_state(mrp, MRP_MIC_STATE_PT);
			break;
		case MRP_MIC_STATE_PT: /* Fallthrough */
		case MRP_MIC_STATE_IP_IDLE:
			/* Ignore */
			break;
		}
	}

	if (!up && mrp->in_mode == MRP_IN_MODE_RC) {
		switch (mrp->mic_state) {
		case MRP_MIC_STATE_AC_STAT1:
			/* Ignore */
			break;
		case MRP_MIC_STATE_PT:
			mrp->in_link_curr_max = mrp->in_link_conf_max;
			mrp_in_link_up_stop(mrp);
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_in_link_down_start(mrp, mrp->in_link_conf_interval);
			mrp_in_link_req(mrp, up, mrp->in_link_conf_max *
					mrp->in_link_conf_interval);
			mrp_set_mic_state(mrp, MRP_MIC_STATE_AC_STAT1);
			break;
		case MRP_MIC_STATE_IP_IDLE:
			mrp->in_link_curr_max = mrp->in_link_conf_max;
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_in_link_down_start(mrp, mrp->in_link_conf_interval);
			mrp_in_link_req(mrp, up, mrp->in_link_conf_max *
					mrp->in_link_conf_interval);
			mrp_set_mic_state(mrp, MRP_MIC_STATE_AC_STAT1);
			break;
		}
	}

	if (up && mrp->in_mode == MRP_IN_MODE_LC) {
		switch (mrp->mic_state) {
		case MRP_MIC_STATE_AC_STAT1:
			mrp->in_link_curr_max = mrp->in_link_conf_max;
			mrp_in_link_down_stop(mrp);
			mrp_in_link_up_start(mrp, mrp->in_link_conf_interval);
			mrp_in_link_req(mrp, up,
					mrp->in_link_conf_max *
					mrp->in_link_conf_interval);
			mrp_set_mic_state(mrp, MRP_MIC_STATE_PT);
			break;
		case MRP_MIC_STATE_PT: /* Fallthrough */
		case MRP_MIC_STATE_IP_IDLE:
			/* Ignore */
			break;
		}
	}

	if (!up && mrp->in_mode == MRP_IN_MODE_LC) {
		switch (mrp->mic_state) {
		case MRP_MIC_STATE_AC_STAT1:
			/* Ignore */
			break;
		case MRP_MIC_STATE_PT: /* Fallthrough */
		case MRP_MIC_STATE_IP_IDLE:
			mrp->in_link_curr_max = mrp->in_link_conf_max;
			mrp_in_link_up_stop(mrp);
			mrp_port_set_state(mrp->i_port,
						   BR_MRP_PORT_STATE_BLOCKED);
			mrp_in_link_down_start(mrp,
					       mrp->in_link_conf_interval);
			mrp_in_link_req(mrp, up,
					mrp->in_link_conf_max *
					mrp->in_link_conf_interval);
			mrp_set_mic_state(mrp, MRP_MIC_STATE_AC_STAT1);
			break;
		}
	}

	pr_debug("new mic_state: %s", mrp_get_mic_state(mrp->mic_state));
}

/* Whenever the port link changes, this function is called */
void mrp_port_link_change(struct mrp_port *p, bool up)
{
	struct mrp *mrp;

	/* If the port which changed its status is not a ring port then
	 * nothing to do
	 */
	if (!mrp_is_mrp_port(p))
		return;

	mrp = p->mrp;

	/* If it is using CFM to notify when the link goes up/down on the
	 * interconnect port then the function mrp_cfm_link_change sets the
	 * correct port status
	 */
	if (mrp->in_mode == MRP_IN_MODE_LC &&
	    mrp_is_in_port(p))
		return;

	if (up)
		p->operstate = IF_OPER_UP;
	else
		p->operstate = IF_OPER_DOWN;

	if (mrp_is_ring_port(p)) {
		if (mrp->ring_role == BR_MRP_RING_ROLE_MRM)
			return mrp_mrm_port_link(p, up);

		if (mrp->ring_role == BR_MRP_RING_ROLE_MRC)
			return mrp_mrc_port_link(p, up);
	}

	if (mrp_is_in_port(p)) {
		if (mrp->in_role == BR_MRP_IN_ROLE_MIM)
			return mrp_mim_port_link(p, up);

		if (mrp->in_role == BR_MRP_IN_ROLE_MIC)
			return mrp_mic_port_link(p, up);
	}
}

void mrp_cfm_link_change(uint32_t br_ifindex, uint32_t peer_mepid,
			 uint32_t defect)
{
	struct mrp *mrp = NULL;

	mrp = mrp_get_mrp(br_ifindex, peer_mepid);
	if (!mrp)
		return;

	if (!mrp->i_port)
		return;

	if (!defect)
		mrp->i_port->operstate = IF_OPER_UP;
	else
		mrp->i_port->operstate = IF_OPER_DOWN;

	if (mrp->in_role == BR_MRP_IN_ROLE_MIM)
		mrp_mim_port_link(mrp->i_port, !defect);

	if (mrp->in_role == BR_MRP_IN_ROLE_MIC)
		mrp_mic_port_link(mrp->i_port, !defect);
}

void mrp_mac_change(uint32_t ifindex, unsigned char *mac)
{
	struct mrp_port *p;
	struct mrp *mrp;

	p = mrp_get_port(ifindex);
	if (p) {
		memcpy(p->macaddr, mac, ETH_ALEN);
		return;
	}

	list_for_each_entry(mrp, &mrp_instances, list) {
		if (mrp->ifindex == ifindex)
			memcpy(mrp->macaddr, mac, ETH_ALEN);
	}
}

/* There are 4 different recovery times in which an MRP ring can recover. Based
 * on the each time updates all the configuration variables. The interval are
 * represented in us.
 */
#define __us(v)		(v)
#define __ms(v)		__us(v) * 1000
#define __s(v)		__ms(v) * 1000
static void mrp_update_recovery(struct mrp *mrp,
				enum mrp_ring_recovery_type ring_recv,
				enum mrp_in_recovery_type in_recv)
{
	mrp->ring_recv = ring_recv;
	mrp->in_recv = in_recv;

	switch (ring_recv) {
	case MRP_RING_RECOVERY_500:
		mrp->ring_topo_conf_interval = __ms(20);
		mrp->ring_topo_conf_max = 3;
		mrp->ring_topo_curr_max = mrp->ring_topo_conf_max - 1;
		mrp->ring_test_conf_short = __ms(30);
		mrp->ring_test_conf_interval = __ms(50);
		mrp->ring_test_conf_max = 5;
		mrp->ring_test_curr_max = mrp->ring_test_conf_max;
		mrp->ring_mon_curr_max = mrp->ring_test_conf_max;
		mrp->ring_test_conf_ext_max = 15;
		mrp->ring_link_conf_interval = __ms(100);
		mrp->ring_link_conf_max = 4;
		mrp->ring_link_curr_max = 0;
		break;
	case MRP_RING_RECOVERY_200:
		mrp->ring_topo_conf_interval = __ms(10);
		mrp->ring_topo_conf_max = 3;
		mrp->ring_topo_curr_max = mrp->ring_topo_conf_max - 1;
		mrp->ring_test_conf_short = __ms(10);
		mrp->ring_test_conf_interval = __ms(20);
		mrp->ring_test_conf_max = 3;
		mrp->ring_test_curr_max = mrp->ring_test_conf_max;
		mrp->ring_mon_curr_max = mrp->ring_test_conf_max;
		mrp->ring_test_conf_ext_max = 15;
		mrp->ring_link_conf_interval = __ms(20);
		mrp->ring_link_conf_max = 4;
		mrp->ring_link_curr_max = 0;
		break;
	case MRP_RING_RECOVERY_30:
		mrp->ring_topo_conf_interval = __us(500);
		mrp->ring_topo_conf_max = 3;
		mrp->ring_topo_curr_max = mrp->ring_topo_conf_max - 1;
		mrp->ring_test_conf_short = __ms(1);
		mrp->ring_test_conf_interval = __ms(3.5);
		mrp->ring_test_conf_max = 3;
		mrp->ring_test_curr_max = mrp->ring_test_conf_max;
		mrp->ring_mon_curr_max = mrp->ring_test_conf_max;
		mrp->ring_test_conf_ext_max = 15;
		mrp->ring_link_conf_interval = __ms(1);
		mrp->ring_link_conf_max = 4;
		mrp->ring_link_curr_max = 0;
		break;
	case MRP_RING_RECOVERY_10:
		mrp->ring_topo_conf_interval = __us(500);
		mrp->ring_topo_conf_max = 3;
		mrp->ring_topo_curr_max = mrp->ring_topo_conf_max - 1;
		mrp->ring_test_conf_short = __us(500);
		mrp->ring_test_conf_interval = __ms(1);
		mrp->ring_test_conf_max = 3;
		mrp->ring_test_curr_max = mrp->ring_test_conf_max;
		mrp->ring_mon_curr_max = mrp->ring_test_conf_max;
		mrp->ring_test_conf_ext_max = 15;
		mrp->ring_link_conf_interval = __ms(1);
		mrp->ring_link_conf_max = 4;
		mrp->ring_link_curr_max = 0;
		break;
	default:
		break;
	}
	mrp->ring_topo_conf_interval *= time_factor;
	mrp->ring_test_conf_short *= time_factor;
	mrp->ring_test_conf_interval *= time_factor;
	mrp->ring_link_conf_interval *= time_factor;
	pr_debug("ring_topo_conf_interval: %dus", mrp->ring_topo_conf_interval);
	pr_debug("ring_topo_conf_max:      %d", mrp->ring_topo_conf_max);
	pr_debug("ring_test_conf_short:    %dus", mrp->ring_test_conf_short);
	pr_debug("ring_test_conf_interval: %dus", mrp->ring_test_conf_interval);
	pr_debug("ring_test_conf_max:      %d", mrp->ring_test_conf_max);
	pr_debug("ring_test_conf_ext_max:  %d", mrp->ring_test_conf_ext_max);
	pr_debug("ring_link_conf_interval: %dus", mrp->ring_link_conf_interval);
	pr_debug("ring_link_conf_max:      %d", mrp->ring_link_conf_max);

	switch (in_recv) {
	case MRP_IN_RECOVERY_500:
		mrp->in_topo_conf_interval = __ms(20);
		mrp->in_topo_conf_max = 3;
		mrp->in_topo_curr_max = mrp->in_topo_conf_max - 1;
		mrp->in_test_conf_interval = __ms(50);
		mrp->in_test_conf_max = 8;
		mrp->in_test_curr_max = mrp->in_test_conf_max;
		mrp->in_link_conf_interval = __ms(20);
		mrp->in_link_conf_max = 4;
		mrp->in_link_curr_max = 0;
		mrp->in_link_status_conf_interval = __ms(20);
		mrp->in_link_status_conf_max = 8;
		mrp->in_link_status_curr_max = 0;
		mrp->cfm_ccm_period = __s(10);
		break;
	case MRP_IN_RECOVERY_200:
		mrp->in_topo_conf_interval = __ms(10);
		mrp->in_topo_conf_max = 3;
		mrp->in_topo_curr_max = mrp->in_topo_conf_max - 1;
		mrp->in_test_conf_interval = __ms(20);
		mrp->in_test_conf_max = 8;
		mrp->in_test_curr_max = mrp->in_test_conf_max;
		mrp->in_link_conf_interval = __ms(20);
		mrp->in_link_conf_max = 4;
		mrp->in_link_curr_max = 0;
		mrp->in_link_status_conf_interval = __ms(20);
		mrp->in_link_status_conf_max = 8;
		mrp->in_link_status_curr_max = 0;
		mrp->cfm_ccm_period = __s(10);
		break;
	default:
		break;
	}
	mrp->in_topo_conf_interval *= time_factor;
	mrp->in_test_conf_interval *= time_factor;
	mrp->in_link_conf_interval *= time_factor;
	mrp->in_link_status_conf_interval *= time_factor;
	pr_debug("in_topo_conf_interval:   %dus", mrp->in_topo_conf_interval);
	pr_debug("in_topo_conf_max:        %d", mrp->in_topo_conf_max);
	pr_debug("in_test_conf_interval:   %dus", mrp->in_test_conf_interval);
	pr_debug("in_test_conf_max:        %d", mrp->in_test_conf_max);
	pr_debug("in_link_conf_interval:   %dus", mrp->in_link_conf_interval);
	pr_debug("in_link_status_conf_int.:%d", mrp->in_link_status_conf_interval);
	pr_debug("in_link_status_conf_max: %d", mrp->in_link_status_conf_max);
	pr_debug("cfm_ccm_period: %d", mrp->cfm_ccm_period);
}

struct mrp *mrp_find(uint32_t br_ifindex, uint32_t ring_nr)
{
	struct mrp *mrp;

	list_for_each_entry(mrp, &mrp_instances, list) {
		if (mrp->ring_nr == ring_nr && mrp->ifindex == br_ifindex)
			return mrp;
	}

	return NULL;
}

/* Initialize an MRP port */
static int mrp_port_init(uint32_t p_ifindex, struct mrp *mrp,
			 enum br_mrp_port_role_type role)
{
	struct mrp_port *port;

	/* Allocate (and zeroed) the port data struct */
	port = malloc(sizeof(struct mrp_port));
	if (!port)
		return -ENOMEM;
	memset(port, 0x0, sizeof(struct mrp_port));

	port->mrp = mrp;
	port->ifindex = p_ifindex;
	if_indextoname(port->ifindex, port->ifname);
	BUG_ON(!port->ifname);
	port->role = role;
	if_get_mac(port->ifindex, port->macaddr);

	if (role == BR_MRP_PORT_ROLE_PRIMARY)
		mrp->p_port = port;
	if (role == BR_MRP_PORT_ROLE_SECONDARY)
		mrp->s_port = port;
	if (role == BR_MRP_PORT_ROLE_INTER)
		mrp->i_port = port;

	return 0;
}

/* Uninitialize MRP port */
static void mrp_port_uninit(struct mrp_port *port)
{
	struct mrp *mrp;

	if (!port || !port->mrp)
		return;

	mrp = port->mrp;

	pthread_mutex_lock(&mrp->lock);

	port->mrp = NULL;

	free(port);

	pthread_mutex_unlock(&mrp->lock);
}

/* Creates an MRP instance and initialize it */
static int mrp_create(uint32_t br_ifindex, uint32_t ring_nr, uint16_t in_id)
{
	struct mrp *mrp;

	mrp = malloc(sizeof(struct mrp));
	if (!mrp)
		return -ENOMEM;

	memset(mrp, 0x0, sizeof(struct mrp));

	pthread_mutex_init(&mrp->lock, NULL);

	mrp->ifindex = br_ifindex;
	mrp->p_port = NULL;
	mrp->s_port = NULL;
	mrp->i_port = NULL;
	mrp->ring_nr = ring_nr;
	mrp->in_id = in_id;

	mrp->ring_role = BR_MRP_RING_ROLE_MRC;
	mrp->in_role = BR_MRP_IN_ROLE_DISABLED;
	mrp->ring_transitions = 0;
	mrp->in_transitions = 0;

	mrp->seq_id = 0;
	mrp->prio = MRP_DEFAULT_PRIO;
	memset(mrp->domain, 0xFF, MRP_DOMAIN_UUID_LENGTH);

	mrp_update_recovery(mrp, MRP_RING_RECOVERY_500, MRP_IN_RECOVERY_500);

	mrp->blocked = 1;
	mrp->react_on_link_change = 1;

	mrp_timer_init(mrp);

	list_add_tail(&mrp->list, &mrp_instances);

	return 0;
}

static void mrp_delete_cfm(struct mrp *mrp)
{
	cfm_offload_mep_delete(mrp->ifindex, mrp->cfm_instance);
}

/* Uninitialize MRP instance and remove it */
void mrp_destroy(uint32_t br_ifindex, uint32_t ring_nr, bool offload)
{
	struct mrp *mrp = mrp_find(br_ifindex, ring_nr);

	if (!mrp)
		return;

	pthread_mutex_lock(&mrp->lock);

	mrp_reset_ring_state(mrp);

	if (mrp->in_mode == MRP_IN_MODE_LC)
		mrp_delete_cfm(mrp);

	if (mrp->p_port)
		free(mrp->p_port);

	if (mrp->s_port)
		free(mrp->s_port);

	if (mrp->i_port)
		free(mrp->i_port);

	pthread_mutex_unlock(&mrp->lock);

	list_del(&mrp->list);
	free(mrp);
}

int mrp_get(int *count, struct mrp_status *status)
{
	struct mrp *mrp;
	int i = 0;

	list_for_each_entry(mrp, &mrp_instances, list) {
		pthread_mutex_lock(&mrp->lock);

		status[i].br = mrp->ifindex;
		status[i].ring_nr = mrp->ring_nr;
		if (mrp->p_port)
			status[i].pport = mrp->p_port->ifindex;
		if (mrp->s_port)
			status[i].sport = mrp->s_port->ifindex;
		status[i].ring_role = mrp->ring_role;
		status[i].mra_support = mrp->mra_support;
		status[i].prio = mrp->prio;
		status[i].ring_recv = mrp->ring_recv;
		status[i].react_on_link_change = mrp->react_on_link_change;

		if (mrp->ring_role == BR_MRP_RING_ROLE_MRM)
			status[i].ring_state = mrp->mrm_state;
		if (mrp->ring_role == BR_MRP_RING_ROLE_MRC)
			status[i].ring_state = mrp->mrc_state;

		if (mrp->i_port)
			status[i].iport = mrp->i_port->ifindex;
		status[i].in_id = mrp->in_id;
		status[i].in_role = mrp->in_role;
		status[i].in_mode = mrp->in_mode;
		status[i].in_recv = mrp->in_recv;
		if (status[i].in_role == BR_MRP_IN_ROLE_MIM)
			status[i].in_state = mrp->mim_state;
		if (status[i].in_role == BR_MRP_IN_ROLE_MIC)
			status[i].in_state = mrp->mic_state;
		if (status[i].in_role == BR_MRP_IN_ROLE_DISABLED)
			status[i].in_state = -1;

		++i;

		pthread_mutex_unlock(&mrp->lock);
	}

	*count = i;

	return 0;
}

static void mrp_start_cfm(struct mrp *mrp, uint32_t cfm_instance,
			  uint32_t cfm_level, uint32_t cfm_mepid,
			  uint32_t cfm_peer_mepid, char *cfm_maid,
			  char *cfm_dmac)
{
	struct mac_addr smac;
	struct mac_addr dmac;
	struct maid_data maid;

	mrp->cfm_mepid = cfm_mepid;
	mrp->cfm_peer_mepid = cfm_peer_mepid;
	mrp->cfm_instance = cfm_instance;

	memcpy(smac.addr, mrp->i_port->macaddr, ETH_ALEN);
	memcpy(dmac.addr, cfm_dmac, ETH_ALEN);
	memcpy(mrp->cfm_ccm_dmac, cfm_dmac, ETH_ALEN);

	memset(maid.data, 0, sizeof(maid));
	memcpy(maid.data, cfm_maid, strlen(cfm_maid));

	cfm_offload_mep_create(mrp->ifindex, mrp->cfm_instance,
			       BR_CFM_PORT, BR_CFM_MEP_DIRECTION_DOWN,
			       mrp->i_port->ifindex);
	cfm_offload_mep_config(mrp->ifindex, mrp->cfm_instance,
			       &smac, cfm_level, cfm_mepid);
	cfm_offload_cc_config(mrp->ifindex, mrp->cfm_instance, 1,
			      BR_CFM_CCM_INTERVAL_3_3_MS, &maid);
	cfm_offload_cc_peer(mrp->ifindex, mrp->cfm_instance, 0,
			    cfm_peer_mepid);

	/* Start the transmision of the frames */
	cfm_offload_cc_ccm_tx(mrp->ifindex, mrp->cfm_instance, &dmac, 1,
			      mrp->cfm_ccm_period, 1, 100, 1, 200);
	mrp_cfm_ccm_start(mrp, mrp->cfm_ccm_period);
}

int mrp_add(uint32_t br_ifindex, uint32_t ring_nr, uint32_t pport,
	    uint32_t sport, uint32_t ring_role, uint16_t prio,
	    uint8_t ring_recv, uint8_t react_on_link_change,
	    uint32_t in_role, uint16_t in_id, uint32_t iport,
	    uint32_t in_mode, uint8_t in_recv, uint32_t cfm_instance,
	    uint32_t cfm_level, uint32_t cfm_mepid, uint32_t cfm_peer_mepid,
	    char *cfm_maid, char *cfm_dmac)
{
	struct mrp *mrp;
	int err;

	/* It is not possible to have MRP instances with the same ID */
	mrp = mrp_find(br_ifindex, ring_nr);
	if (mrp)
		return -EINVAL;

	/* Create the mrp instance */
	err = mrp_create(br_ifindex, ring_nr, in_id);
	if (err < 0)
		return err;

	mrp = mrp_find(br_ifindex, ring_nr);

	pthread_mutex_lock(&mrp->lock);

	mrp->ifindex = br_ifindex;
	if_indextoname(mrp->ifindex, mrp->ifname);
	BUG_ON(!mrp->ifname);
	mrp->prio = prio;
	mrp->ring_prio = prio;
	mrp_update_recovery(mrp, ring_recv, in_recv);
	mrp->react_on_link_change = react_on_link_change;
	mrp->in_mode = in_mode;

	if_get_mac(mrp->ifindex, mrp->macaddr);

	/* Initialize the ports */
	err = mrp_port_init(pport, mrp, BR_MRP_PORT_ROLE_PRIMARY);
	if (err < 0) {
		pthread_mutex_unlock(&mrp->lock);
		goto delete_mrp;
	}

	err = mrp_port_init(sport, mrp, BR_MRP_PORT_ROLE_SECONDARY);
	if (err < 0) {
		pthread_mutex_unlock(&mrp->lock);
		goto delete_port;
	}

	if (iport > 0) {
		err = mrp_port_init(iport, mrp, BR_MRP_PORT_ROLE_INTER);
		if (err < 0) {
			pthread_mutex_unlock(&mrp->lock);
			goto delete_ports;
		}
	}

	if (mrp->in_mode != MRP_IN_MODE_RC)
		mrp_start_cfm(mrp, cfm_instance, cfm_level, cfm_mepid,
			      cfm_peer_mepid, cfm_maid, cfm_dmac);

	if (ring_role == BR_MRP_RING_ROLE_MRM)
		err = mrp_set_mrm_role(mrp);
	if (ring_role == BR_MRP_RING_ROLE_MRC)
		err = mrp_set_mrc_role(mrp);
	if (ring_role == BR_MRP_RING_ROLE_MRA)
		err = mrp_set_mra_role(mrp);
	if (in_role == BR_MRP_IN_ROLE_MIM)
		err = mrp_set_mim_role(mrp);
	if (in_role == BR_MRP_IN_ROLE_MIC)
		err = mrp_set_mic_role(mrp);

	pthread_mutex_unlock(&mrp->lock);

	if (err)
		goto clear;

	return 0;

clear:
	mrp_port_uninit(mrp->i_port);
	mrp->i_port = NULL;

delete_ports:
	mrp_port_uninit(mrp->s_port);
	mrp->s_port = NULL;

delete_port:
	mrp_port_uninit(mrp->p_port);
	mrp->p_port = NULL;

delete_mrp:
	mrp_destroy(br_ifindex, ring_nr, true);
	return err;
}

int mrp_del(uint32_t br_ifindex, uint32_t ring_nr)
{
	struct mrp *mrp;

	mrp = mrp_find(br_ifindex, ring_nr);
	if (!mrp) {
		pr_err("invalid ring nr: %d", ring_nr);
		return -EINVAL;
	}

	mrp_destroy(br_ifindex, ring_nr, true);

	return 0;
}

void mrp_uninit(void)
{
	struct mrp *mrp, *tmp;

	/* The MRP ports are already uninitialized, therefore only
	 * destroy the MRP instances.
	 */
	list_for_each_entry_safe(mrp, tmp, &mrp_instances, list) {
		mrp_destroy(mrp->ifindex, mrp->ring_nr, true);
	}
}
