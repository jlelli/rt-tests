// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef __RT_NUMA_H
#define __RT_NUMA_H

#include <numa.h>

enum {
	AFFINITY_UNSPECIFIED,
	AFFINITY_SPECIFIED,
	AFFINITY_USEALL
};

int numa_initialize(void);

int get_available_cpus(struct bitmask *cpumask);
int cpu_for_thread_sp(int thread_num, int max_cpus, struct bitmask *cpumask);
int cpu_for_thread_ua(int thread_num, int max_cpus);

int parse_cpumask(char *str, int max_cpus, struct bitmask **cpumask);

#endif
