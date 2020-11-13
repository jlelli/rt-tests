// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef __RT_NUMA_H
#define __RT_NUMA_H

#include <numa.h>

int parse_cpumask(char *str, int max_cpus, struct bitmask **cpumask);

#endif
