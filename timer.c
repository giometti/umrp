// Copyright (c) 2022 Rodolfo Giometti <giometti@enneenne.com>
// Copyright (c) 2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: (GPL-2.0)

#include <stdio.h>
#include <errno.h>

#include "ifdriver.h"
#include "state_machine.h"
#include "cfm_netlink.h"

static void mrp_clear_fdb_expired(struct ev_loop *loop,
				  ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, clear_fdb_work);

	ifdriver_flush(mrp);

	mrp_clear_fdb_stop(mrp);
}

static void mrp_mrm_ring_test_expired(struct mrp *mrp)
{
        switch (mrp->mrm_state) {
        case MRP_MRM_STATE_AC_STAT1:
                /* Ignore */
                break;
        case MRP_MRM_STATE_PRM_UP:
        case MRP_MRM_STATE_CHK_RO:
		mrp->add_test = false;
		mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);
		break;
	case MRP_MRM_STATE_CHK_RC:
		if (mrp->ring_test_curr >= mrp->ring_test_curr_max) {
			mrp_port_set_state(mrp->s_port,
                                           BR_MRP_PORT_STATE_FORWARDING);
			mrp->ring_test_curr_max = mrp->ring_test_conf_max - 1;
			mrp->ring_test_curr = 0;
			mrp->add_test = false;
			if (!mrp->no_tc)
				mrp_ring_topo_req(mrp,
						mrp->ring_topo_conf_interval);
			mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);

			mrp->ring_transitions++;
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_CHK_RO);
		} else {
			mrp->ring_test_curr++;
			mrp->add_test = false;
			mrp_ring_test_req(mrp, mrp->ring_test_conf_interval);
		}
                break;
        }
}

static void mrp_mrc_ring_test_expired(struct mrp *mrp)
{
	if (mrp->ring_mon_curr <= mrp->ring_mon_curr_max) {
		mrp->ring_mon_curr++;
		mrp_ring_test_start(mrp, mrp->ring_test_conf_short);
	} else {
		mrp_ring_test_start(mrp, mrp->ring_test_conf_short);
		mrp_set_mrm_init(mrp);

		switch (mrp->mrc_state) {
		case MRP_MRC_STATE_DE_IDLE:
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_PRM_UP);
			mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRM);
			break;
		case MRP_MRC_STATE_PT:
	                mrp_set_mrm_state(mrp, MRP_MRM_STATE_CHK_RC);
			mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRM);
			break;
		case MRP_MRC_STATE_DE:
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_PRM_UP);
			mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRM);
			break;
		case MRP_MRC_STATE_PT_IDLE:
			mrp_set_mrm_state(mrp, MRP_MRM_STATE_CHK_RO);
			mrp_set_ring_role(mrp, BR_MRP_RING_ROLE_MRM);
		default:
			break;
		}
	}
}

static void mrp_ring_test_expired(struct ev_loop *loop,
				  ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, ring_test_work);

	pthread_mutex_lock(&mrp->lock);

	if (mrp->mra_support && mrp->ring_role == BR_MRP_RING_ROLE_MRC)
		mrp_mrc_ring_test_expired(mrp);
	else if (mrp->ring_role == BR_MRP_RING_ROLE_MRM)
		mrp_mrm_ring_test_expired(mrp);

	pthread_mutex_unlock(&mrp->lock);
}

static void mrp_ring_topo_expired(struct ev_loop *loop,
				  ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, ring_topo_work);

	pr_debug("ring topo expired: ring_topo_curr_max: %d",
	        mrp->ring_topo_curr_max);

	pthread_mutex_lock(&mrp->lock);

	if (mrp->ring_topo_curr_max > 0) {
		mrp_ring_topo_send(mrp, mrp->ring_topo_curr_max *
					mrp->ring_topo_conf_interval);

		mrp->ring_topo_curr_max--;
	} else {
		mrp->ring_topo_curr_max = mrp->ring_topo_conf_max - 1;

		ifdriver_flush(mrp);
		mrp_ring_topo_send(mrp, 0);

		mrp_ring_topo_stop(mrp);
	}

	pthread_mutex_unlock(&mrp->lock);
}

static void mrp_ring_link_up_expired(struct ev_loop *loop,
				     ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, ring_link_up_work);
	uint32_t interval;
	uint32_t delay;

	pr_debug("ring link up expired: ring_link_curr_max: %d",
	        mrp->ring_link_curr_max);

	pthread_mutex_lock(&mrp->lock);

	delay = mrp->ring_link_conf_interval;

	if (mrp->ring_link_curr_max > 0) {
		mrp->ring_link_curr_max--;

		mrp_ring_link_up_start(mrp, delay);

		interval = mrp->ring_link_curr_max * delay;

		mrp_ring_link_req(mrp->p_port, true, interval);
	} else {
		mrp->ring_link_curr_max = mrp->ring_link_conf_max;
		mrp_port_set_state(mrp->s_port,
					   BR_MRP_PORT_STATE_FORWARDING);
		mrp_set_mrc_state(mrp, MRP_MRC_STATE_PT_IDLE);

		mrp_ring_link_up_stop(mrp);
	}

	pthread_mutex_unlock(&mrp->lock);
}

static void mrp_ring_link_down_expired(struct ev_loop *loop,
				       ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, ring_link_down_work);
	uint32_t interval;
	uint32_t delay;

	pr_debug("ring link down expired: ring_link_curr_max: %d",
	        mrp->ring_link_curr_max);

	pthread_mutex_lock(&mrp->lock);

	delay = mrp->ring_link_conf_interval;

	if (mrp->ring_link_curr_max > 0) {
		mrp->ring_link_curr_max--;

		mrp_ring_link_down_start(mrp, delay);

		interval = mrp->ring_link_curr_max * delay;

		mrp_ring_link_req(mrp->p_port, false, interval);
	} else {
		mrp->ring_link_curr_max = mrp->ring_link_conf_max;

		mrp_set_mrc_state(mrp, MRP_MRC_STATE_DE_IDLE);

		mrp_ring_link_down_stop(mrp);
	}

	pthread_mutex_unlock(&mrp->lock);
}

static void mrp_in_test_expired(struct ev_loop *loop,
				ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, in_test_work);

	pthread_mutex_lock(&mrp->lock);

        switch (mrp->mim_state) {
        case MRP_MIM_STATE_AC_STAT1:
                /* Ignore */
                break;
        case MRP_MIM_STATE_CHK_IO:
		mrp_in_test_req(mrp, mrp->in_test_conf_interval);
		break;
	case MRP_MIM_STATE_CHK_IC:
		if (mrp->in_test_curr >= mrp->in_test_curr_max) {
			mrp_port_set_state(mrp->i_port,
                                           BR_MRP_PORT_STATE_FORWARDING);
			mrp->in_test_curr_max = mrp->in_test_conf_max - 1;
			mrp->in_test_curr = 0;
			mrp_in_topo_req(mrp, mrp->in_topo_conf_interval);
			mrp_in_test_req(mrp, mrp->in_test_conf_interval);

			mrp->in_transitions++;
			mrp_set_mrm_state(mrp, MRP_MIM_STATE_CHK_IO);
		} else {
			mrp->in_test_curr++;
			mrp_in_test_req(mrp, mrp->in_test_conf_interval);
		}
                break;
        }

	pthread_mutex_unlock(&mrp->lock);
}

static void mrp_in_topo_expired(struct ev_loop *loop,
				ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, in_topo_work);

	pr_debug("int topo expired: in_topo_curr_max: %d",
	        mrp->in_topo_curr_max);

	pthread_mutex_lock(&mrp->lock);

	if (mrp->in_topo_curr_max > 0) {
		mrp_in_topo_send(mrp, mrp->in_topo_curr_max *
				 mrp->in_topo_conf_interval);

		mrp->in_topo_curr_max--;
	} else {
		mrp->in_topo_curr_max = mrp->in_topo_conf_max - 1;

		ifdriver_flush(mrp);
		mrp_in_topo_send(mrp, 0);

		mrp_in_topo_stop(mrp);
	}

	pthread_mutex_unlock(&mrp->lock);
}

static void mrp_in_link_up_expired(struct ev_loop *loop,
				   ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, in_link_up_work);
	uint32_t interval;
	uint32_t delay;

	pr_debug("int link up expired: in_link_curr_max: %d",
	        mrp->in_link_curr_max);

	pthread_mutex_lock(&mrp->lock);

	delay = mrp->in_link_conf_interval;

	if (mrp->in_link_curr_max > 0) {
		mrp->in_link_curr_max--;

		mrp_in_link_up_start(mrp, delay);

		interval = mrp->in_link_curr_max * delay;

		mrp_in_link_req(mrp, true, interval);
	} else {
		mrp->in_link_curr_max = mrp->in_link_conf_max;
		mrp_port_set_state(mrp->i_port,
					   BR_MRP_PORT_STATE_FORWARDING);
		mrp_set_mic_state(mrp, MRP_MIC_STATE_IP_IDLE);

		mrp_in_link_up_stop(mrp);
	}

	pthread_mutex_unlock(&mrp->lock);
}

static void mrp_in_link_down_expired(struct ev_loop *loop,
				     ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, in_link_down_work);
	uint32_t interval;
	uint32_t delay;

	pr_debug("int link down expired: in_link_curr_max: %d",
	        mrp->in_link_curr_max);

	pthread_mutex_lock(&mrp->lock);

	delay = mrp->in_link_conf_interval;

	if (mrp->in_link_curr_max > 0) {
		mrp->in_link_curr_max--;

		mrp_in_link_down_start(mrp, delay);

		interval = mrp->in_link_curr_max * delay;

		mrp_in_link_req(mrp, false, interval);
	} else {
		mrp->in_link_curr_max = mrp->in_link_conf_max;

		mrp_in_link_down_stop(mrp);
	}

	pthread_mutex_unlock(&mrp->lock);
}

static void mrp_in_link_status_expired(struct ev_loop *loop,
				       ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, in_link_status_work);
	uint32_t interval;
	uint32_t delay;

	pr_debug("in link status expired: in_link_status_curr_max: %d",
	        mrp->in_link_status_curr_max);

	pthread_mutex_lock(&mrp->lock);

	delay = mrp->in_link_status_conf_interval;

	if (mrp->in_link_status_curr_max > 0) {
		mrp->in_link_status_curr_max--;

		interval = mrp->in_link_status_curr_max * delay;

		mrp_in_link_status_req(mrp,  interval);
	} else {
		mrp->in_link_status_curr_max = mrp->in_link_status_conf_max;

		mrp_in_link_status_stop(mrp);
	}

	pthread_mutex_unlock(&mrp->lock);
}

static void mrp_cfm_ccm_expired(struct ev_loop *loop,
				ev_timer *w, int revents)
{
	struct mrp *mrp = container_of(w, struct mrp, cfm_ccm_work);
	struct mac_addr dmac;

	memcpy(dmac.addr, mrp->cfm_ccm_dmac, ETH_ALEN);

	mrp->cfm_ccm_work.repeat = (ev_tstamp)mrp->cfm_ccm_period / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->cfm_ccm_work);

	cfm_offload_cc_ccm_tx(mrp->ifindex, mrp->cfm_instance, &dmac, 1,
			      mrp->cfm_ccm_period, 1, 100, 1, 200);
}

int mrp_ring_test_start(struct mrp *mrp, uint32_t interval)
{
	mrp->ring_test_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->ring_test_work);
	return 0;
}

void mrp_ring_test_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->ring_test_work);
}

void mrp_ring_topo_start(struct mrp *mrp, uint32_t interval)
{
	mrp->ring_topo_running = true;
	mrp->ring_topo_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->ring_topo_work);
}

void mrp_ring_topo_stop(struct mrp *mrp)
{
	mrp->ring_topo_running = false;
	ev_timer_stop(EV_DEFAULT, &mrp->ring_topo_work);
}

void mrp_ring_link_up_start(struct mrp *mrp, uint32_t interval)
{
	mrp->ring_link_up_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->ring_link_up_work);
}

void mrp_ring_link_up_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->ring_link_up_work);
}

void mrp_ring_link_down_start(struct mrp *mrp, uint32_t interval)
{
	mrp->ring_link_down_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->ring_link_down_work);
}

void mrp_ring_link_down_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->ring_link_down_work);
}

int mrp_in_test_start(struct mrp *mrp, uint32_t interval)
{
	mrp->in_test_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->in_test_work);
	return 0;
}

void mrp_in_test_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->in_test_work);
}

void mrp_in_topo_start(struct mrp *mrp, uint32_t interval)
{
	mrp->in_topo_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->in_topo_work);
}

void mrp_in_topo_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->in_topo_work);
}

void mrp_in_link_up_start(struct mrp *mrp, uint32_t interval)
{
	mrp->in_link_up_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->in_link_up_work);
}

void mrp_in_link_up_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->in_link_up_work);
}

void mrp_in_link_down_start(struct mrp *mrp, uint32_t interval)
{
	mrp->in_link_down_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->in_link_down_work);
}

void mrp_in_link_down_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->in_link_down_work);
}

void mrp_in_link_status_start(struct mrp *mrp, uint32_t interval)
{
	mrp->in_link_status_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->in_link_status_work);
}

void mrp_in_link_status_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->in_link_status_work);
}

void mrp_clear_fdb_start(struct mrp *mrp, uint32_t interval)
{
	mrp->clear_fdb_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->clear_fdb_work);
	if (interval == 0)
		ifdriver_flush(mrp);
}

void mrp_clear_fdb_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->clear_fdb_work);
}

void mrp_cfm_ccm_start(struct mrp *mrp, uint32_t interval)
{
	mrp->cfm_ccm_work.repeat = (ev_tstamp)interval / 1000000;
	ev_timer_again(EV_DEFAULT, &mrp->cfm_ccm_work);
}

void mrp_cfm_ccm_stop(struct mrp *mrp)
{
	ev_timer_stop(EV_DEFAULT, &mrp->cfm_ccm_work);
}

/* Stops all the timers */
void mrp_timer_stop(struct mrp *mrp)
{
	mrp_clear_fdb_stop(mrp);
	mrp_ring_topo_stop(mrp);
	mrp_ring_link_up_stop(mrp);
	mrp_ring_link_down_stop(mrp);
	mrp_ring_test_stop(mrp);

	if (mrp->in_role != BR_MRP_IN_ROLE_DISABLED) {
		mrp_in_topo_stop(mrp);
		mrp_in_link_up_stop(mrp);
		mrp_in_link_down_stop(mrp);

		if (mrp->in_mode == MRP_IN_MODE_RC) {
			mrp_in_test_stop(mrp);
		} else {
			mrp_in_link_status_stop(mrp);
			mrp_cfm_ccm_stop(mrp);
		}
	}
}

void mrp_timer_init(struct mrp *mrp)
{
	ev_init(&mrp->clear_fdb_work, mrp_clear_fdb_expired);
	ev_init(&mrp->ring_topo_work, mrp_ring_topo_expired);
	ev_init(&mrp->ring_test_work, mrp_ring_test_expired);
	ev_init(&mrp->ring_link_up_work, mrp_ring_link_up_expired);
	ev_init(&mrp->ring_link_down_work, mrp_ring_link_down_expired);
	ev_init(&mrp->in_test_work, mrp_in_test_expired);
	ev_init(&mrp->in_topo_work, mrp_in_topo_expired);
	ev_init(&mrp->in_link_up_work, mrp_in_link_up_expired);
	ev_init(&mrp->in_link_down_work, mrp_in_link_down_expired);
	ev_init(&mrp->in_link_status_work, mrp_in_link_status_expired);
	ev_init(&mrp->cfm_ccm_work, mrp_cfm_ccm_expired);
}
