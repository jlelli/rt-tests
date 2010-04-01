/*
 * A numa library for cyclictest.
 * The functions here are designed to work whether cyclictest has been
 * compiled with numa support or not, and whether the user uses the --numa
 * option or not.
 * They should also work correctly with older versions of the numactl lib
 * such as the one found on RHEL5, or with the newer version 2 and above.
 *
 * (C) 2010 John Kacur <jkacur@redhat.com>
 * (C) 2010 Clark Williams <williams@redhat.com>
 *
 */

#ifndef _RT_NUMA_H
#define _RT_NUMA_H

#include "rt-utils.h"

static int numa = 0;

#ifdef NUMA
#include <numa.h>

#ifndef LIBNUMA_API_VERSION
#define LIBNUMA_API_VERSION 1
#endif

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

static void numa_on_and_available()
{
	if (numa && numa_available() == -1)
		fatal("--numa specified and numa functions not available.\n");
}

#if LIBNUMA_API_VERSION >= 2
static int rt_numa_numa_node_of_cpu(int cpu)
{
	int node;
	node = numa_node_of_cpu(cpu);
	if (node == -1)
		fatal("invalid cpu passed to numa_node_of_cpu(%d)\n", cpu);
	return node;
}

#else	/* LIBNUMA_API_VERSION == 1 */

static int rt_numa_numa_node_of_cpu(int cpu)
{
	unsigned char cpumask[256];
	int node, idx, bit;
	int max_node, max_cpus;

	max_node = numa_max_node();
	max_cpus = sysconf(_SC_NPROCESSORS_CONF);

	if (cpu > max_cpus) {
		errno = EINVAL;
		return -1;
	}

	/* calculate bitmask index and relative bit position of cpu */
	idx = cpu / 8;
	bit = cpu % 8;

	for (node = 0; node <= max_node; node++) {
		if (numa_node_to_cpus(node, (void *) cpumask, sizeof(cpumask)))
			return -1;

		if (cpumask[idx] & (1<<bit))
			return node;
	}
	errno = EINVAL;
	return -1;
}

#endif	/* LIBNUMA_API_VERSION */

static void *rt_numa_numa_alloc_onnode(size_t size, int node, int cpu)
{
	void *stack;
	stack = numa_alloc_onnode(size, node);
	if (stack == NULL)
		fatal("failed to allocate %d bytes on node %d for cpu %d\n",
				size, node, cpu);
	return stack;
}

#else

static inline void *threadalloc(size_t size, int n) { return malloc(size); }
static inline void threadfree(void *ptr, size_t s, int n) { free(ptr); }
static inline void rt_numa_set_numa_run_on_node(int n, int c) { }
static inline void numa_on_and_available() { };
static inline int rt_numa_numa_node_of_cpu(int cpu) { return -1; }
static void *rt_numa_numa_alloc_onnode(size_t s, int n, int c) { return NULL; }

#endif	/* NUMA */

#endif	/* _RT_NUMA_H */
