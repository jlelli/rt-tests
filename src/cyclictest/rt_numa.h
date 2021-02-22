// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A numa library for cyclictest.
 *
 * (C) 2010 John Kacur <jkacur@redhat.com>
 * (C) 2010 Clark Williams <williams@redhat.com>
 *
 */

#ifndef _RT_NUMA_H
#define _RT_NUMA_H

#include "rt-utils.h"
#include "rt-error.h"

static int numa = 0;

#include <numa.h>

static void *
threadalloc(size_t size, int node)
{
	if (node == -1)
		return malloc(size);
	return numa_alloc_onnode(size, node);
}

static void
threadfree(void *ptr, size_t size, int node)
{
	if (node == -1)
		free(ptr);
	else
		numa_free(ptr, size);
}

static void rt_numa_set_numa_run_on_node(int node, int cpu)
{
	int res;
	res = numa_run_on_node(node);
	if (res)
		warn("Could not set NUMA node %d for thread %d: %s\n",
				node, cpu, strerror(errno));
	return;
}

static void *rt_numa_numa_alloc_onnode(size_t size, int node, int cpu)
{
	void *stack;
	stack = numa_alloc_onnode(size, node);
	if (stack == NULL)
		fatal("failed to allocate %d bytes on node %d for cpu %d\n",
				size, node, cpu);
	return stack;
}

/*
 * Use new bit mask CPU affinity behavior
 */
static int rt_numa_numa_node_of_cpu(int cpu)
{
	int node;
	node = numa_node_of_cpu(cpu);
	if (node == -1)
		fatal("invalid cpu passed to numa_node_of_cpu(%d)\n", cpu);
	return node;
}

static inline unsigned int rt_numa_bitmask_isbitset( const struct bitmask *mask,
	unsigned long i)
{
	return numa_bitmask_isbitset(mask,i);
}

static inline struct bitmask* rt_numa_parse_cpustring(const char* s,
	int max_cpus)
{
	return numa_parse_cpustring_all(s);
}

static inline void rt_bitmask_free(struct bitmask *mask)
{
	numa_bitmask_free(mask);
}

#endif	/* _RT_NUMA_H */
