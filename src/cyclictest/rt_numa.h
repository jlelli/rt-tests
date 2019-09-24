/*
 * A numa library for cyclictest.
 * The functions here are designed to work whether cyclictest has been
 * compiled with numa support or not.
 *
 * (C) 2010 John Kacur <jkacur@redhat.com>
 * (C) 2010 Clark Williams <williams@redhat.com>
 *
 */

#ifndef _RT_NUMA_H
#define _RT_NUMA_H

#include "rt-utils.h"
#include "error.h"

static int numa = 0;

#ifdef NUMA
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
#ifdef HAVE_PARSE_CPUSTRING_ALL		/* Currently not defined anywhere.  No
					   autotools build. */
	return numa_parse_cpustring_all(s);
#else
	/* We really need numa_parse_cpustring_all(), so we can assign threads
	 * to cores which are part of an isolcpus set, but early 2.x versions of
	 * libnuma do not have this function.  A work around should be to run
	 * your command with e.g. taskset -c 9-15 <command>
	 */
	return numa_parse_cpustring((char *)s);
#endif
}

static inline void rt_bitmask_free(struct bitmask *mask)
{
	numa_bitmask_free(mask);
}


#else /* ! NUMA */

struct bitmask {
    unsigned long size; /* number of bits in the map */
    unsigned long *maskp;
};
#define BITS_PER_LONG    (8*sizeof(long))

static inline void *threadalloc(size_t size, int n) { return malloc(size); }
static inline void threadfree(void *ptr, size_t s, int n) { free(ptr); }
static inline void rt_numa_set_numa_run_on_node(int n, int c) { }
static inline int rt_numa_numa_node_of_cpu(int cpu) { return -1; }
static void *rt_numa_numa_alloc_onnode(size_t s, int n, int c) { return NULL; }

/*
 * Map legacy CPU affinity behavior onto bit mask infrastructure
 */
static inline unsigned int rt_numa_bitmask_isbitset( const struct bitmask *mask,
	unsigned long i)
{
	long bit = mask->maskp[i/BITS_PER_LONG] & (1<<(i % BITS_PER_LONG));
	return (bit != 0);
}

static inline struct bitmask* rt_numa_parse_cpustring(const char* s,
	int max_cpus)
{
	int cpu;
	struct bitmask *mask = NULL;
	cpu = atoi(s);
	if (0 <= cpu && cpu < max_cpus) {
		mask = malloc(sizeof(*mask));
		if (mask) {
			/* Round up to integral number of longs to contain
			 * max_cpus bits */
			int nlongs = (max_cpus+BITS_PER_LONG-1)/BITS_PER_LONG;

			mask->maskp = calloc(nlongs, sizeof(unsigned long));
			if (mask->maskp) {
				mask->maskp[cpu/BITS_PER_LONG] |=
					(1UL << (cpu % BITS_PER_LONG));
				mask->size = max_cpus;
			} else {
				free(mask);
				mask = NULL;
			}
		}
	}
	return mask;
}

static inline void rt_bitmask_free(struct bitmask *mask)
{
	free(mask->maskp);
	free(mask);
}

#endif	/* NUMA */

/*
 * Any behavioral differences above are transparent to these functions
 */

/** Returns number of bits set in mask. */
static inline unsigned int rt_numa_bitmask_count(const struct bitmask *mask)
{
	unsigned int num_bits = 0, i;
	for (i = 0; i < mask->size; i++) {
		if (rt_numa_bitmask_isbitset(mask, i))
			num_bits++;
	}
	/* Could stash this instead of recomputing every time. */
	return num_bits;
}

#endif	/* _RT_NUMA_H */
