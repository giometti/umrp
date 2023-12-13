// Copyright (c) 2022 Rodolfo Giometti <giometti@enneenne.com>
// Copyright (c) 2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: (GPL-2.0)


#include <stdio.h>
#include <stdbool.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <ev.h>
#include <unistd.h>

#include "server_socket.h"
#include "utils.h"
#include "packet.h"

int __debug_level;
volatile bool quit = false;
unsigned int time_factor = 1;

static void usage(void)
{
	printf("Usage:\n"
	       " -h        print this message and exit\n"
	       " -v        print server version and exit\n"
	       " -d        increase debugging level\n"
	       " -T <val>  use <val> as time factor to increase MRP timings " \
			"(for debugging ONLY!)\n");
}

static void pr_version(void)
{
	pr_info("UMRP ver. %s", VERSION);
}

static void handle_signal(int sig)
{
	ev_break(EV_DEFAULT, EVBREAK_ALL);;
}

int signal_init(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sa.sa_flags = 0;

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	return 0;
}

int main(int argc, char *argv[])
{
	int c;
	int ret;

	while ((c = getopt(argc, argv, "hvdT:")) != -1) {
		switch (c) {
		case 'T':
			time_factor = atoi(optarg);
			if (time_factor <= 0) {
				pr_err("invalid value for -T option argument");
				exit(EXIT_FAILURE);
			}
			break;
		case 'd':
			__debug_level++;
			break;
		case 'v':
			pr_version();
			return 0;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 0;
		}
	}
	pr_debug("time_factor: %d", time_factor);

	ret = ctl_socket_init();
	if (ret < 0) {
		pr_err("unable to init CTL socket layer");
		exit(EXIT_FAILURE);
	}
	ret = packet_socket_init();
	if (ret < 0) {
		pr_err("unable to init PACKET socket layer");
		exit(EXIT_FAILURE);
	}

	pr_version();
	ev_run(EV_DEFAULT, 0);

	packet_socket_cleanup();
	ctl_socket_cleanup();

	return 0;
}

