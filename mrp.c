// Copyright (c) 2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: (GPL-2.0)

#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <net/if.h>

#include "utils.h"
#include "linux.h"
#include "pdu.h"
#include <linux/mrp_bridge.h>

#include <linux/cfm_bridge.h>

#define MRP_MRA_PRIO		0xa000

static void incomplete_command(void)
{
	printf("Command line is not complete. Try option \"help\"\n");
	exit(-1);
}

#define NEXT_ARG() do { argv++; if (--argc <= 0) incomplete_command(); } while(0)

static int fd = 1;

static int valid_ring_role(char *arg)
{
	if (strcmp(arg, "disabled") == 0 ||
	    strcmp(arg, "mrc") == 0 ||
	    strcmp(arg, "mrm") == 0 ||
	    strcmp(arg, "mra") == 0)
		return 1;
	return 0;
}

static enum br_mrp_ring_role_type ring_role_int(char *arg)
{
	if (strcmp(arg, "disabled") == 0)
		return BR_MRP_RING_ROLE_DISABLED;
	if (strcmp(arg, "mrc") == 0)
		return BR_MRP_RING_ROLE_MRC;
	if (strcmp(arg, "mrm") == 0)
		return BR_MRP_RING_ROLE_MRM;
	if (strcmp(arg, "mra") == 0)
		return BR_MRP_RING_ROLE_MRA;
	return BR_MRP_RING_ROLE_DISABLED;
}

static int valid_in_role(char *arg)
{
	if (strcmp(arg, "disabled") == 0 ||
	    strcmp(arg, "mic") == 0 ||
	    strcmp(arg, "mim") == 0)
		return 1;
	return 0;
}

static enum br_mrp_in_role_type in_role_int(char *arg)
{
	if (strcmp(arg, "disabled") == 0)
		return BR_MRP_IN_ROLE_DISABLED;
	if (strcmp(arg, "mic") == 0)
		return BR_MRP_IN_ROLE_MIC;
	if (strcmp(arg, "mim") == 0)
		return BR_MRP_IN_ROLE_MIM;
	return BR_MRP_IN_ROLE_DISABLED;
}

static int valid_in_mode(char *arg)
{
	if (strcmp(arg, "rc") == 0 ||
	    strcmp(arg, "lc") == 0)
		return 1;
	return 0;
}

static enum mrp_in_mode_type in_mode_int(char *arg)
{
	if (strcmp(arg, "rc") == 0)
		return MRP_IN_MODE_RC;
	if (strcmp(arg, "lc") == 0)
		return MRP_IN_MODE_LC;
	return MRP_IN_MODE_RC;
}

static char *in_mode_str(int in_mode)
{
	switch (in_mode) {
	case MRP_IN_MODE_RC: return "RC";
	case MRP_IN_MODE_LC: return "LC";
	default:
		return "Unknown in mode";
	}
}

static int valid_ring_recv(char *arg)
{
	if (strcmp(arg, "500") == 0 ||
	    strcmp(arg, "200") == 0 ||
	    strcmp(arg, "30") == 0 ||
	    strcmp(arg, "10") == 0)
		return 1;
	return 0;
}

static enum mrp_ring_recovery_type ring_recv_int(char *arg)
{
	if (strcmp(arg, "500") == 0)
		return MRP_RING_RECOVERY_500;
	if (strcmp(arg, "200") == 0)
		return MRP_RING_RECOVERY_200;
	if (strcmp(arg, "30") == 0)
		return MRP_RING_RECOVERY_30;
	if (strcmp(arg, "10") == 0)
		return MRP_RING_RECOVERY_10;
	return MRP_RING_RECOVERY_500;
}

static char* ring_recv_str(enum mrp_ring_recovery_type ring_recv)
{
	switch (ring_recv) {
	case MRP_RING_RECOVERY_500: return "500";
	case MRP_RING_RECOVERY_200: return "200";
	case MRP_RING_RECOVERY_30: return "30";
	case MRP_RING_RECOVERY_10: return "10";
	default:
		return "Unknown ring recovery";
	}
}

static int valid_in_recv(char *arg)
{
	if (strcmp(arg, "500") == 0 ||
	    strcmp(arg, "200") == 0)
		return 1;
	return 0;
}

static enum mrp_in_recovery_type in_recv_int(char *arg)
{
	if (strcmp(arg, "500") == 0)
		return MRP_IN_RECOVERY_500;
	if (strcmp(arg, "200") == 0)
		return MRP_IN_RECOVERY_200;
	return MRP_IN_RECOVERY_500;
}

static char* in_recv_str(enum mrp_in_recovery_type in_recv)
{
	switch (in_recv) {
	case MRP_IN_RECOVERY_500: return "500";
	case MRP_IN_RECOVERY_200: return "200";
	default:
		return "Unknown interconnect recovery";
	}
}

static char *mrm_state_str(int mrm_state)
{
	switch (mrm_state) {
	case MRP_MRM_STATE_AC_STAT1: return "AC_STAT1";
	case MRP_MRM_STATE_PRM_UP: return "PRM_UP";
	case MRP_MRM_STATE_CHK_RO: return "CHK_RO";
	case MRP_MRM_STATE_CHK_RC: return "CHK_RC";
	default:
		return "Unknown mrm_state";
	}
}

static char *mrc_state_str(int mrc_state)
{
	switch (mrc_state) {
	case MRP_MRC_STATE_AC_STAT1: return "AC_STAT1";
	case MRP_MRC_STATE_DE_IDLE: return "DE_IDLE";
	case MRP_MRC_STATE_PT: return "PT";
	case MRP_MRC_STATE_DE: return "DE";
	case MRP_MRC_STATE_PT_IDLE: return "PT_IDLE";
	default:
		return "Unknwon mrc_state";
	}
}

static char *mim_state_str(int mim_state)
{
	switch (mim_state) {
	case MRP_MIM_STATE_AC_STAT1: return "AC_STAT1";
	case MRP_MIM_STATE_CHK_IO: return "CHK_IO";
	case MRP_MIM_STATE_CHK_IC: return "CHK_IC";
	default:
		return "Unknown mim_state";
	}
}

static char *mic_state_str(int mic_state)
{
	switch (mic_state) {
	case MRP_MIC_STATE_AC_STAT1: return "AC_STAT1";
	case MRP_MIC_STATE_PT: return "PT";
	case MRP_MIC_STATE_IP_IDLE: return "IP_IDLE";
	default:
		return "Unknwon mic_state";
	}
}

static void cfm_dmac_get(char *argv, char *dmac)
{
	int values[ETH_ALEN];
	int i;

	sscanf(argv, "%x:%x:%x:%x:%x:%x%*c",
	       &values[0], &values[1], &values[2],
	       &values[3], &values[4], &values[5]);

	for(i = 0; i < ETH_ALEN; ++i)
		dmac[i] = (uint8_t) values[i];
}

static void cfm_maid_get(char *argv, char *maid)
{
	memset(maid, 0, CFM_MAID_LENGTH);
	maid[0] = 1;
	maid[1] = 2;
	maid[3] = strlen(argv);
	memcpy(&maid[3], argv, strlen(argv));
}

static int client_init(void)
{
	struct sockaddr_un sa_svr;
	struct sockaddr_un sa;
	char tmpname[64];
	int s;

	if (0 > (s = socket(PF_UNIX, SOCK_DGRAM, 0))) {
		printf("Couldn't open unix socket: %m");
		return -1;
	}

	set_socket_address(&sa_svr, MRP_SERVER_SOCK_NAME);

	sprintf(tmpname, "MRPCTL_%d", getpid());
	set_socket_address(&sa, tmpname);

	/* We need this bind. The autobind on connect isn't working properly.
	 * The server doesn't get a proper sockaddr in recvmsg if we don't do this.
	 */
	if (0 != bind(s, (struct sockaddr *)&sa, sizeof(sa))) {
	    printf("Couldn't bind socket: %m");
	    close(s);
	    return -1;
	}

	if (0 != connect(s, (struct sockaddr *)&sa_svr, sizeof(sa_svr))) {
	    printf("Couldn't connect to server");
	    close(s);
	    return -1;
	}
	fd = s;

	return 0;
}

static void client_cleanup(void)
{
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
}

static int client_send_message(int cmd, void *inbuf, int lin, void *outbuf,
			       int lout, int *res)
{
	struct ctl_msg_hdr mhdr;
	struct iovec iov[2];
	int timeout = 5000; /* 5 s */
	struct msghdr msg;
	struct pollfd pfd;
	int r;
	int l;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;

	mhdr.cmd = cmd;
	mhdr.lin = lin;
	mhdr.lout = lout;
	iov[0].iov_base = &mhdr;
	iov[0].iov_len = sizeof(mhdr);
	iov[1].iov_base = (void *)inbuf;
	iov[1].iov_len = lin;

	l = sendmsg(fd, &msg, 0);
	if(0 > l) {
		printf("Error sending message to server: %m");
		return -1;
	}

	if(l != sizeof(mhdr) + lin) {
		printf("Error sending message to server: Partial write");
		return -1;
	}

	iov[1].iov_base = outbuf;
	iov[1].iov_len = lout;

	pfd.fd = fd;
	pfd.events = POLLIN;
	do {
		if (0 == (r = poll(&pfd, 1, timeout))) {
			printf("Error getting message from server: Timeout");
			return -1;
		}
		if (0 > r) {
			printf("Error getting message from server: poll error: %m");
			return -1;
		}
	} while(0 == (pfd.revents & (POLLERR | POLLHUP | POLLNVAL | POLLIN)));

	l = recvmsg(fd, &msg, 0);
	if (0 > l) {
		printf("Error getting message from server: %m");
		return -1;
	}

	if ((sizeof(mhdr) > l) || (l != sizeof(mhdr) + mhdr.lout)
	   || (mhdr.cmd != cmd)) {
		printf("Error getting message from server: Bad format");
		return -1;
	}

	if (mhdr.lout != lout) {
		printf("Error, unexpected result length %d, expected %d\n",
		       mhdr.lout, lout);
		return -1;
	}

	if (res)
		*res = mhdr.res;

	return 0;
}

static int cmd_addmrp(int argc, char *const *argv)
{
	int br = 0, pport = 0, sport = 0, ring_nr = 0, ring_role = 0;
	uint16_t prio = MRP_DEFAULT_PRIO;
	bool prio_set = false;
	uint8_t ring_recv = MRP_RING_RECOVERY_500;
	uint8_t in_recv = MRP_IN_RECOVERY_500;
	uint8_t react_on_link_change = 1;
	int in_role = BR_MRP_IN_ROLE_DISABLED, iport = 0;
	int in_mode = MRP_IN_MODE_RC;
	uint16_t in_id = 0;
	uint32_t cfm_level = 0, cfm_mepid = 0, cfm_peer_mepid = 0, cfm_instance = 0;
	char cfm_dmac[ETH_ALEN];
	char cfm_maid[CFM_MAID_LENGTH];

	/* skip the command */
	argv++;
	argc -= 1;

	while (argc > 0) {
		if (strcmp(*argv, "bridge") == 0) {
			NEXT_ARG();
			br = if_nametoindex(*argv);
		} else if (strcmp(*argv, "ring_nr") == 0) {
			NEXT_ARG();
			ring_nr = atoi(*argv);
		} else if (strcmp(*argv, "pport") == 0) {
			NEXT_ARG();
			pport = if_nametoindex(*argv);
		} else if (strcmp(*argv, "sport") == 0) {
			NEXT_ARG();
			sport = if_nametoindex(*argv);
		} else if (strcmp(*argv, "ring_role") == 0) {
			NEXT_ARG();
			if (!valid_ring_role(*argv))
				return -1;
			ring_role = ring_role_int(*argv);
		} else if (strcmp(*argv, "prio") == 0) {
			NEXT_ARG();
			prio = atoi(*argv);
			prio_set = true;
		} else if (strcmp(*argv, "ring_recv") == 0) {
			NEXT_ARG();
			if (!valid_ring_recv(*argv))
				return -1;
			ring_recv = ring_recv_int(*argv);
		} else if (strcmp(*argv, "react_on_link_change") == 0) {
			NEXT_ARG();
			if (atoi(*argv))
				react_on_link_change = 1;
			else
				react_on_link_change = 0;
		} else if (strcmp(*argv, "in_role") == 0) {
			NEXT_ARG();
			if (!valid_in_role(*argv))
				return -1;
			in_role = in_role_int(*argv);
		} else if (strcmp(*argv, "in_recv") == 0) {
			NEXT_ARG();
			if (!valid_in_recv(*argv))
				return -1;
			in_recv = in_recv_int(*argv);
		} else if (strcmp(*argv, "in_id") == 0) {
			NEXT_ARG();
			in_id = atoi(*argv);
		} else if (strcmp(*argv, "iport") == 0) {
			NEXT_ARG();
			iport = if_nametoindex(*argv);
		} else if (strcmp(*argv, "in_mode") == 0) {
			NEXT_ARG();
			if (!valid_in_mode(*argv))
				return -1;
			in_mode = in_mode_int(*argv);
		} else if (strcmp(*argv, "cfm_instance") == 0) {
			NEXT_ARG();
			cfm_instance = atoi(*argv);
		} else if (strcmp(*argv, "cfm_level") == 0) {
			NEXT_ARG();
			cfm_level = atoi(*argv);
		} else if (strcmp(*argv, "cfm_mepid") == 0) {
			NEXT_ARG();
			cfm_mepid = atoi(*argv);
		} else if (strcmp(*argv, "cfm_maid") == 0) {
			NEXT_ARG();
			cfm_maid_get(*argv, cfm_maid);
		} else if (strcmp(*argv, "cfm_peer_mepid") == 0) {
			NEXT_ARG();
			cfm_peer_mepid = atoi(*argv);
		} else if (strcmp(*argv, "cfm_dmac") == 0) {
			NEXT_ARG();
			cfm_dmac_get(*argv, cfm_dmac);
		}

		argc--; argv++;
	}

	if (br == 0 || pport == 0 || sport == 0 || ring_nr == 0 ||
	    ring_role == 0)
		return -1;

	if (ring_role == BR_MRP_RING_ROLE_MRA && prio_set == false)
		prio = MRP_MRA_PRIO;

	return CTL_addmrp(br, ring_nr, pport, sport, ring_role, prio,
			  ring_recv, react_on_link_change, in_role,
			  in_id, iport, in_mode, in_recv, cfm_instance,
			  cfm_level, cfm_mepid, cfm_peer_mepid, cfm_maid,
			  cfm_dmac);
}

static int cmd_delmrp(int argc, char *const *argv)
{
	int br = 0, ring_nr = 0;

	/* skip the command */
	argv++;
	argc -= 1;

	while (argc > 0) {
		if (strcmp(*argv, "bridge") == 0) {
			NEXT_ARG();
			br = if_nametoindex(*argv);
		} else if (strcmp(*argv, "ring_nr") == 0) {
			NEXT_ARG();
			ring_nr = atoi(*argv);
		}

		argc--; argv++;
	}

	if (br == 0 || ring_nr == 0)
		return -1;

	return CTL_delmrp(br, ring_nr);
}

static int cmd_getmrp(int argc, char *const *argv)
{
	struct mrp_status status[MAX_MRP_INSTANCES];
	char ifname[IF_NAMESIZE];
	int count = 0;
	int i;

	memset(ifname, 0, IF_NAMESIZE);

	if (CTL_getmrp(&count, status))
		return -1;

	for (i = 0; i < count; ++i) {
		printf("bridge: %s ", if_indextoname(status[i].br, ifname));
		printf("ring_nr: %d ", status[i].ring_nr);
		printf("pport: %s ", if_indextoname(status[i].pport, ifname));
		printf("sport: %s ", if_indextoname(status[i].sport, ifname));
		printf("mra_support: %d ", status[i].mra_support);
		printf("ring_role: %s ", ring_role_str(status[i].ring_role));
		printf("prio: %d ", status[i].prio);
		printf("ring_recv: %s \n", ring_recv_str(status[i].ring_recv));
		printf("react_on_link_change: %d ", status[i].react_on_link_change);
		if (status[i].ring_role == BR_MRP_RING_ROLE_MRM)
			printf("ring_state: %s \n", mrm_state_str(status[i].ring_state));
		if (status[i].ring_role == BR_MRP_RING_ROLE_MRC)
			printf("ring_state: %s \n", mrc_state_str(status[i].ring_state));

		if (status[i].in_role == BR_MRP_IN_ROLE_DISABLED)
			continue;

		printf("iport: %s ", if_indextoname(status[i].iport, ifname));
		printf("in_id: %d ", status[i].in_id);
		printf("in_role: %s ", in_role_str(status[i].in_role));
		printf("in_recv: %s \n", in_recv_str(status[i].in_recv));
		printf("in_mode: %s ", in_mode_str(status[i].in_mode));
		if (status[i].in_role == BR_MRP_IN_ROLE_MIM)
			printf("in_state: %s \n", mim_state_str(status[i].in_state));
		if (status[i].in_role == BR_MRP_IN_ROLE_MIC)
			printf("in_state: %s \n", mic_state_str(status[i].in_state));
	}

	return 0;
}

struct command
{
	const char *name;
	int (*func) (int argc, char *const *argv);
};

static const struct command commands[] =
{
	{"addmrp", cmd_addmrp},
	{"delmrp", cmd_delmrp},
	{"getmrp", cmd_getmrp},
};

static void help(void)
{
	printf("Usage: mrp [options] [commands]\n"
		"options:\n"
		"  -h | --help              Show this help text\n"
		"commands:\n\n"
		"addmrp: Create MRP instance\n"
		"Mandatory arguments:\n"
		"  bridge          [bridge]    Bridge name on which to create MRP instance\n"
		"  ring_nr         [id]        The ID of MRP instance\n"
		"  pport           [port]      Primary port name\n"
		"  sport           [port]      Secondary port name\n"
		"  ring_role       [role]      Ring role (mrm, mrc, mra)\n"
		"Optional arguments:\n"
		"  ring_recv       [recovery]    Ring recovery time(500,200,30,10)\n"
		"  prio            [priority]    Instance priority\n"
		"  react_on_link_change [status]    enable/disable property (1,0)\n"
		"  in_role         [in_role]     Interconnect role(mim, mic)\n"
		"  iport           [port]        Interconnect port name\n"
		"  in_id           [in_id]       Interconnect ring id\n"
		"  in_mode         [in_mode]     Interconnect operating mode(lc, rc)\n"
		"  in_recv         [in_recv]     Interconnect recovery time(500,200)\n"
		"  cfm_instance    [instance]    CFM instance ID\n"
		"  cfm_level       [level]       CFM level\n"
		"  cfm_mepid       [mepid]       CFM mepid\n"
		"  cfm_peer_mepid  [peer_mepid]  CFM peer mepid\n"
		"  cfm_maid        [maid]        CFM maid\n"
		"  cfm_dmac        [dmac]        CFM destination MAC\n\n"
		"delmrp: Delete MRP instance\n"
		"Mandatory arguments:\n"
		"  bridge          [bridge]    Bridge name on which the MRP instance exists\n"
		"  ring_nr         [id]        The ID of MRP instance\n\n"
		"getmrp: Show MRP instance\n\n");
}

static const struct command *command_lookup(const char *cmd)
{
	int i;

	for(i = 0; i < COUNT_OF(commands); ++i) {
		if(!strcmp(cmd, commands[i].name))
			return &commands[i];
	}

	return NULL;
}

static const struct command *command_lookup_and_validate(int argc,
							 char *const *argv,
							 int line_num)
{
	const struct command *cmd;

	cmd = command_lookup(argv[0]);
	if (!cmd) {
		if (line_num > 0)
			printf("Error on line %d:\n", line_num);

		printf("Unknown command [%s]\n", argv[0]);
		if (line_num == 0) {
			help();
			return NULL;
		}
	}

	return cmd;
}

int main (int argc, char *const *argv)
{
	const struct command *cmd;
	int f;
	int ret;

	static const struct option options[] =
	{
		{.name = "help",	.val = 'h'},
		{0}
	};

	while (EOF != (f = getopt_long(argc, argv, "h", options, NULL))) {
		switch (f) {
		case 'h':
			help();
			return 0;
		}
	}

	if (client_init()) {
		printf("can't setup control connection\n");
		return 1;
	}

	argc -= optind;
	argv += optind;

	if (argc == 0) {
		help();
		return 1;
	}

	cmd = command_lookup_and_validate(argc, argv, 0);
	if (!cmd)
		return 1;

	ret = cmd->func(argc, argv);
	client_cleanup();
	return ret;
}

CLIENT_SIDE_FUNCTION(addmrp);
CLIENT_SIDE_FUNCTION(delmrp);
CLIENT_SIDE_FUNCTION(getmrp);
