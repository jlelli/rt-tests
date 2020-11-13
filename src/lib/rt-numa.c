// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2020 Daniel Wagner <dwagner@suse.de>
 * Copyright 2020 John Kacur <jkacur@redhat.com>
 */

#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include "rt-numa.h"

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
