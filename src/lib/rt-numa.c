// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2020 Daniel Wagner <dwagner@suse.de>
 * Copyright 2020 John Kacur <jkacur@redhat.com>
 */

#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>

#include "rt-error.h"
#include "rt-numa.h"

/*
 * numa_available() must be called before any other calls to the numa library
 * returns 0 if numa is available, or 1 if numa is not available
 */
int numa_initialize(void)
{
	static int is_initialized;	// Only call numa_available once
	static int numa;

	if (is_initialized == 1)
		return numa;

	if (numa_available() != -1)
		numa = 1;

	is_initialized = 1;

	return numa;
}

int get_available_cpus(struct bitmask *cpumask)
{
	if (cpumask)
		return numa_bitmask_weight(cpumask);

	return numa_num_task_cpus();
}

int cpu_for_thread_sp(int thread_num, int max_cpus, struct bitmask *cpumask)
{
	unsigned int m, cpu, i, num_cpus;

	num_cpus = numa_bitmask_weight(cpumask);

	if (num_cpus == 0)
		fatal("No allowable cpus to run on\n");

	m = thread_num % num_cpus;

	/* there are num_cpus bits set, we want position of m'th one */
	for (i = 0, cpu = 0; i < max_cpus; i++) {
		if (numa_bitmask_isbitset(cpumask, i)) {
			if (cpu == m)
				return i;
			cpu++;
		}
	}
	warn("Bug in cpu mask handling code.\n");
	return 0;
}

/* cpu_for_thread AFFINITY_USEALL */
int cpu_for_thread_ua(int thread_num, int max_cpus)
{
	int res, num_cpus, i, m, cpu;
	pthread_t thread;
	cpu_set_t cpuset;

	thread = pthread_self();
	CPU_ZERO(&cpuset);

	res = pthread_getaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
	if (res != 0)
		fatal("pthread_getaffinity_np failed: %s\n", strerror(res));

	num_cpus = CPU_COUNT(&cpuset);
	m = thread_num % num_cpus;

	for (i = 0, cpu = 0; i < max_cpus; i++) {
		if (CPU_ISSET(i, &cpuset)) {
			if (cpu == m)
				return i;
			cpu++;
		}
	}

	warn("Bug in cpu mask handling code.\n");
	return 0;
}

/*
 * After this function is called, affinity_mask is the intersection of
 * the user supplied affinity mask and the affinity mask from the run
 * time environment
 */
static void use_current_cpuset(int max_cpus, struct bitmask *cpumask)
{
	struct bitmask *curmask;
	int i;

	curmask = numa_allocate_cpumask();
	numa_sched_getaffinity(getpid(), curmask);

	/*
	 * Clear bits that are not set in both the cpuset from the
	 * environment, and in the user specified affinity.
	 */
	for (i = 0; i < max_cpus; i++) {
		if ((!numa_bitmask_isbitset(cpumask, i)) ||
		    (!numa_bitmask_isbitset(curmask, i)))
			numa_bitmask_clearbit(cpumask, i);
	}

	numa_bitmask_free(curmask);
}

int parse_cpumask(char *str, int max_cpus, struct bitmask **cpumask)
{
	struct bitmask *mask;

	mask = numa_parse_cpustring_all(str);
	if (!mask)
		return -ENOMEM;

	if (numa_bitmask_weight(mask) == 0) {
		numa_bitmask_free(mask);
		*cpumask = NULL;
		return 0;
	}

	use_current_cpuset(max_cpus, mask);
	*cpumask = mask;

	return 0;
}
