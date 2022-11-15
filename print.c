// Copyright (c) 2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: (GPL-2.0)

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "print.h"

static int print_level = LOG_INFO;

void print_set_level(int level)
{
	print_level = level;
}

void print(int level, char const *format, ...)
{
	struct timespec ts;
	char buf[1024];
	va_list ap;
	FILE *f;

	if (level > print_level)
		return;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	f = level >= LOG_NOTICE ? stdout : stderr;
	fprintf(f, "MRP[%lld.%03ld]: %s\n",
			(long long)ts.tv_sec, ts.tv_nsec / 1000000,
			buf);
	fflush(f);
}
