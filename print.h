// Copyright (c) 2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: (GPL-2.0)

#ifndef PRINT_H
#define PRINT_H

#include <syslog.h>

#include "utils.h"

void print_set_level(int level);

#ifdef __GNUC__
__printf(2, 3)
#endif
void print(int level, char const *format, ...);

#define pr_alert(x...)   print(LOG_ALERT, x)
#define pr_err(x...)     print(LOG_ERR, x)
#define pr_info(x...)    print(LOG_INFO, x)

#endif /* PRINT_H */
