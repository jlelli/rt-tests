// SPDX-License-Identifier: GPL-3.0-only
/*
 * oslat - OS latency detector
 *
 * Copyright 2020 Red Hat Inc.
 *
 * Authors: Peter Xu <peterx@redhat.com>
 *
 * Some of the utility code based on sysjitter-1.3:
 * Copyright 2010-2015 David Riddoch <david@riddoch.org.uk>
 */

#include <assert.h>
#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <numa.h>
#include <math.h>
#include <limits.h>
#include <linux/unistd.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "rt-utils.h"
#include "rt-numa.h"
#include "error.h"

#ifdef __GNUC__
# define atomic_inc(ptr)   __sync_add_and_fetch((ptr), 1)
# if defined(__x86_64__)
#  define relax()          __asm__ __volatile__("pause" ::: "memory")
static inline void frc(uint64_t *pval)
{
	uint32_t low, high;
	/* See rdtsc_ordered() of Linux */
	__asm__ __volatile__("lfence");
	__asm__ __volatile__("rdtsc" : "=a" (low), "=d" (high));
	*pval = ((uint64_t) high << 32) | low;
}
# elif defined(__i386__)
#  define relax()          __asm__ __volatile__("pause" ::: "memory")
static inline void frc(uint64_t *pval)
{
	__asm__ __volatile__("rdtsc" : "=A" (*pval));
}
# elif defined(__PPC64__)
#  define relax()          do { } while (0)
static inline void frc(uint64_t *pval)
{
	__asm__ __volatile__("mfspr %0, 268\n" : "=r" (*pval));
}
# else
#  define relax()          do { } while (0)
#  define frc(x)
#  define FRC_MISSING
# endif
#else
# error Need to add support for this compiler.
#endif

typedef uint64_t stamp_t;   /* timestamp */
typedef uint64_t cycles_t;  /* number of cycles */

#define  true   1
#define  false  0

enum command {
	WAIT,
	GO,
	STOP
};

enum workload_type {
	WORKLOAD_NONE = 0,
	WORKLOAD_MEMMOVE,
	WORKLOAD_NUM,
};

/* This workload needs pre-allocated memory */
#define  WORK_NEED_MEM  (1UL << 0)

typedef void (*workload_fn)(char *src, char *dst, size_t size);

struct workload {
	const char *w_name;
	uint64_t w_flags;
	workload_fn w_fn;
};

/* We'll have buckets 1us, 2us, ..., (BUCKET_SIZE) us. */
#define  BUCKET_SIZE  (32)

/* Default size of the workloads per thread (in bytes, which is 16KB) */
#define  WORKLOAD_MEM_SIZE  (16UL << 10)

/* By default, no workload */
#define  WORKLOAD_DEFAULT  WORKLOAD_NONE

struct thread {
	int                  core_i;
	pthread_t            thread_id;

	/* NOTE! this is also how many ticks per us */
	unsigned int         cpu_mhz;
	cycles_t             int_total;
	stamp_t              frc_start;
	stamp_t              frc_stop;
	cycles_t             runtime;
	stamp_t              *buckets;
	uint64_t             minlat;
	/* Maximum latency detected */
	uint64_t             maxlat;
	/*
	 * The extra part of the interruptions that cannot be put into even the
	 * biggest bucket.  We'll use this to calculate a more accurate average at
	 * the end of the tests.
	 */
	uint64_t             overflow_sum;
	int                  memory_allocated;

	/* Buffers used for the workloads */
	char                 *src_buf;
	char                 *dst_buf;

	/* These variables are calculated after the test */
	double               average;
};

struct global {
	/* Configuration. */
	unsigned int          runtime_secs;
	/*
	 * Number of threads running for current test
	 * (either pre heat or real run)
	 */
	unsigned int          n_threads;
	/* Number of threads to test for the real run */
	unsigned int          n_threads_total;
	struct timeval        tv_start;
	int                   rtprio;
	int                   bucket_size;
	int                   trace_threshold;
	int                   runtime;
	/* The core that we run the main thread.  Default is cpu0 */
	int                   cpu_main_thread;
	char                  *cpu_list;
	char                  *app_name;
	struct workload       *workload;
	uint64_t              workload_mem_size;
	int                   enable_bias;
	uint64_t              bias;
	int                   single_preheat_thread;
	int                   output_omit_zero_buckets;

	/* Mutable state. */
	volatile enum command cmd;
	volatile unsigned int n_threads_started;
	volatile unsigned int n_threads_running;
	volatile unsigned int n_threads_finished;
};

static struct global g;

static void workload_nop(char *dst, char *src, size_t size)
{
	/* Nop */
}

static void workload_memmove(char *dst, char *src, size_t size)
{
	memmove(dst, src, size);
}

struct workload workload_list[WORKLOAD_NUM] = {
	{ "no", 0, workload_nop },
	{ "memmove", WORK_NEED_MEM, workload_memmove },
};

#define TEST(x)						\
	do {						\
		if (!(x))                             \
			test_fail(#x, __LINE__);	\
	} while (0)

#define TEST0(x)  TEST((x) == 0)

static void test_fail(const char *what, int line)
{
	fprintf(stderr, "ERROR:\n");
	fprintf(stderr, "ERROR: TEST(%s)\n", what);
	fprintf(stderr, "ERROR: at line %d\n", line);
	fprintf(stderr, "ERROR: errno=%d (%s)\n", errno, strerror(errno));
	fprintf(stderr, "ERROR:\n");
	exit(1);
}

static int move_to_core(int core_i)
{
	cpu_set_t cpus;

	CPU_ZERO(&cpus);
	CPU_SET(core_i, &cpus);
	return sched_setaffinity(0, sizeof(cpus), &cpus);
}

static cycles_t __measure_cpu_hz(void)
{
	struct timeval tvs, tve;
	stamp_t s, e;
	double sec;

	frc(&s);
	e = s;
	gettimeofday(&tvs, NULL);
	while (e - s < 1000000)
		frc(&e);
	gettimeofday(&tve, NULL);
	sec = tve.tv_sec - tvs.tv_sec + (tve.tv_usec - tvs.tv_usec) / 1e6;
	return (cycles_t) ((e - s) / sec);
}

static unsigned int measure_cpu_mhz(void)
{
	cycles_t m, mprev, d;

	mprev = __measure_cpu_hz();
	do {
		m = __measure_cpu_hz();
		if (m > mprev)
			d = m - mprev;
		else
			d = mprev - m;
		mprev = m;
	} while (d > m / 1000);

	return (unsigned int) (m / 1000000);
}

static void thread_init(struct thread *t)
{
	t->cpu_mhz = measure_cpu_mhz();
	t->maxlat = 0;
	t->overflow_sum = 0;
	t->minlat = (uint64_t)-1;

	/* NOTE: all the buffers are not freed until the process quits. */
	if (!t->memory_allocated) {
		TEST(t->buckets = calloc(1, sizeof(t->buckets[0]) * g.bucket_size));
		if (g.workload->w_flags & WORK_NEED_MEM) {
			TEST0(posix_memalign((void **)&t->src_buf, getpagesize(),
					     g.workload_mem_size));
			memset(t->src_buf, 0, g.workload_mem_size);
			TEST0(posix_memalign((void **)&t->dst_buf, getpagesize(),
					     g.workload_mem_size));
			memset(t->dst_buf, 0, g.workload_mem_size);
		}
		t->memory_allocated = 1;
	} else {
		/* Clear the buckets */
		memset(t->buckets, 0, sizeof(t->buckets[0]) * g.bucket_size);
	}
}

static float cycles_to_sec(const struct thread *t, uint64_t cycles)
{
	return cycles / (t->cpu_mhz * 1e6);
}

static void insert_bucket(struct thread *t, stamp_t value)
{
	int index, us;
	uint64_t extra;

	index = value / t->cpu_mhz;
	assert(index >= 0);
	us = index + 1;
	assert(us > 0);

	if (g.trace_threshold && us >= g.trace_threshold) {
		char *line = "%s: Trace threshold (%d us) triggered with %u us!\n"
		    "Stopping the test.\n";
		tracemark(line, g.app_name, g.trace_threshold, us);
		err_quit(line, g.app_name, g.trace_threshold, us);
	}

	/* Update max latency */
	if (us > t->maxlat)
		t->maxlat = us;

	if (us < t->minlat)
		t->minlat = us;

	if (g.bias) {
		/* t->bias will be set after pre-heat if user enabled it */
		us -= g.bias;
		/*
		 * Negative should hardly happen, but if it happens, we assume we're in
		 * the smallest bucket, which is 1us.  Same to index.
		 */
		if (us <= 0)
			us = 1;
		index -= g.bias;
		if (index < 0)
			index = 0;
	}

	/* Too big the jitter; put into the last bucket */
	if (index >= g.bucket_size) {
		/* Keep the extra bit (in us) */
		extra = index - g.bucket_size;
		if (t->overflow_sum + extra < t->overflow_sum) {
			/* The uint64_t even overflowed itself; bail out */
			printf("Accumulated overflow too much!\n");
			exit(1);
		}
		t->overflow_sum += extra;
		index = g.bucket_size - 1;
	}

	t->buckets[index]++;
	if (t->buckets[index] == 0) {
		printf("Bucket %d overflowed\n", index);
		exit(1);
	}
}

static void doit(struct thread *t)
{
	stamp_t ts1, ts2;
	workload_fn workload_fn = g.workload->w_fn;

	frc(&ts2);
	do {
		workload_fn(t->dst_buf, t->src_buf, g.workload_mem_size);
		frc(&ts1);
		insert_bucket(t, ts1 - ts2);
		ts2 = ts1;
	} while (g.cmd == GO);
}

static int set_fifo_prio(int prio)
{
	struct sched_param param;

	memset(&param, 0, sizeof(param));
	param.sched_priority = prio;
	return sched_setscheduler(0, SCHED_FIFO, &param);
}

static void *thread_main(void *arg)
{
	/* Important thing to note here is that once we start bashing the CPU, we
	 * need to keep doing so to prevent the core from changing frequency or
	 * dropping into a low power state.
	 */
	struct thread *t = arg;

	/* Alloc memory in the thread itself after setting affinity to get the
	 * best chance of getting numa-local memory.  Doesn't matter so much for
	 * the "struct thread" since we expect that to stay cache resident.
	 */
	TEST(move_to_core(t->core_i) == 0);
	if (g.rtprio)
		TEST(set_fifo_prio(g.rtprio) == 0);

	/* Don't bash the cpu until all threads have got going. */
	atomic_inc(&g.n_threads_started);
	while (g.cmd == WAIT)
		usleep(1000);

	thread_init(t);

	/* Ensure we all start at the same time. */
	atomic_inc(&g.n_threads_running);
	while (g.n_threads_running != g.n_threads)
		relax();

	frc(&t->frc_start);
	doit(t);
	frc(&t->frc_stop);

	t->runtime = t->frc_stop - t->frc_start;

	/* Wait for everyone to finish so we don't disturb them by exiting and
	 * waking the main thread.
	 */
	atomic_inc(&g.n_threads_finished);
	while (g.n_threads_finished != g.n_threads)
		relax();

	return NULL;
}

#define putfield(label, val, fmt, end) do {		\
		printf("%12s:\t", label);               \
		for (i = 0; i < g.n_threads; ++i)       \
			printf(" %"fmt, val);		\
		printf("%s\n", end);                    \
	} while (0)

void calculate(struct thread *t)
{
	int i, j;
	double sum;
	uint64_t count;

	for (i = 0; i < g.n_threads; ++i) {
		/* Calculate average */
		sum = count = 0;
		for (j = 0; j < g.bucket_size; j++) {
			sum += 1.0 * t[i].buckets[j] * (g.bias+j+1);
			count += t[i].buckets[j];
		}
		/* Add the extra amount of huge spikes in */
		sum += t->overflow_sum;
		t[i].average = sum / count;
	}
}

static void write_summary(struct thread *t)
{
	int i, j, k, print_dotdotdot = 0;
	char bucket_name[64];

	calculate(t);

	putfield("Core", t[i].core_i, "d", "");
	putfield("CPU Freq", t[i].cpu_mhz, "u", " (Mhz)");

	for (j = 0; j < g.bucket_size; j++) {
		if (j < g.bucket_size-1 && g.output_omit_zero_buckets) {
			for (k = 0; k < g.n_threads; k++) {
				if (t[k].buckets[j] != 0)
					break;
			}
			if (k == g.n_threads) {
				print_dotdotdot = 1;
				continue;
			}
		}

		if (print_dotdotdot) {
			printf("    ...\n");
			print_dotdotdot = 0;
		}

		snprintf(bucket_name, sizeof(bucket_name), "%03"PRIu64
			 " (us)", g.bias+j+1);
		putfield(bucket_name, t[i].buckets[j], PRIu64,
			 (j == g.bucket_size - 1) ? " (including overflows)" : "");
	}

	putfield("Minimum", t[i].minlat, PRIu64, " (us)");
	putfield("Average", t[i].average, ".3lf", " (us)");
	putfield("Maximum", t[i].maxlat, PRIu64, " (us)");
	putfield("Max-Min", t[i].maxlat - t[i].minlat, PRIu64, " (us)");
	putfield("Duration", cycles_to_sec(&(t[i]), t[i].runtime),
		 ".3f", " (sec)");
	printf("\n");
}

static void run_expt(struct thread *threads, int runtime_secs)
{
	int i;

	g.runtime_secs = runtime_secs;
	g.n_threads_started = 0;
	g.n_threads_running = 0;
	g.n_threads_finished = 0;
	g.cmd = WAIT;

	for (i = 0; i < g.n_threads; ++i)
		TEST0(pthread_create(&(threads[i].thread_id), NULL,
				     thread_main, &(threads[i])));
	while (g.n_threads_started != g.n_threads)
		usleep(1000);

	gettimeofday(&g.tv_start, NULL);
	g.cmd = GO;

	alarm(runtime_secs);

	/* Go to sleep until the threads have done their stuff. */
	for (i = 0; i < g.n_threads; ++i)
		pthread_join(threads[i].thread_id, NULL);
}

static void handle_alarm(int code)
{
	g.cmd = STOP;
}

static void usage(int error)
{
	printf("oslat V %1.2f\n", VERSION);
	printf("Usage:\n"
	       "oslat <options>\n\n"
	       "This is an OS latency detector by running busy loops on specified cores.\n"
	       "Please run this tool using root.\n\n"
	       "Available options:\n\n"
	       "-b, --bucket-size      Specify the number of the buckets (4-1024)\n"
	       "-B, --bias             Add a bias to all the buckets using the estimated mininum\n"
	       "-c, --cpu-list         Specify CPUs to run on, e.g. '1,3,5,7-15'\n"
	       "-C, --cpu-main-thread  Specify which CPU the main thread runs on.  Default is cpu0.\n"
	       "-D, --duration         Specify test duration, e.g., 60, 20m, 2H\n"
	       "                       (m/M: minutes, h/H: hours, d/D: days)\n"
	       "-f, --rtprio           Using SCHED_FIFO priority (1-99)\n"
	       "-m, --workload-mem     Size of the memory to use for the workload (e.g., 4K, 1M).\n"
	       "                       Total memory usage will be this value multiplies 2*N,\n"
	       "                       because there will be src/dst buffers for each thread, and\n"
	       "                       N is the number of processors for testing.\n"
	       "-s, --single-preheat   Use a single thread when measuring latency at preheat stage\n"
	       "                       NOTE: please make sure the CPU frequency on all testing cores\n"
	       "                       are locked before using this parmater.  If you don't know how\n"
	       "                       to lock the freq then please don't use this parameter.\n"
	       "-T, --trace-threshold  Stop the test when threshold triggered (in us),\n"
	       "                       print a marker in ftrace and stop ftrace too.\n"
	       "-v, --version          Display the version of the software.\n"
	       "-w, --workload         Specify a kind of workload, default is no workload\n"
	       "                       (options: no, memmove)\n"
	       "-z, --zero-omit        Don't display buckets in the output histogram if all zeros.\n"
	       );
	exit(error);
}

static int workload_select(char *name)
{
	int i = 0;

	for (i = 0; i < WORKLOAD_NUM; i++) {
		if (!strcmp(name, workload_list[i].w_name)) {
			g.workload = &workload_list[i];
			return 0;
		}
	}

	return -1;
}

/* Process commandline options */
static void parse_options(int argc, char *argv[])
{
	while (1) {
		static struct option options[] = {
			{ "bucket-size", required_argument, NULL, 'b' },
			{ "cpu-list", required_argument, NULL, 'c' },
			{ "cpu-main-thread", required_argument, NULL, 'C'},
			{ "duration", required_argument, NULL, 'D' },
			{ "rtprio", required_argument, NULL, 'f' },
			{ "help", no_argument, NULL, 'h' },
			{ "trace-threshold", required_argument, NULL, 'T' },
			{ "workload", required_argument, NULL, 'w'},
			{ "workload-mem", required_argument, NULL, 'm'},
			{ "bias", no_argument, NULL, 'B'},
			{ "single-preheat", no_argument, NULL, 's'},
			{ "zero-omit", no_argument, NULL, 'u'},
			{ "version", no_argument, NULL, 'v'},
			{ NULL, 0, NULL, 0 },
		};
		int i, c = getopt_long(argc, argv, "b:Bc:C:D:f:hm:sw:T:vz",
				       options, NULL);
		long ncores;

		if (c == -1)
			break;

		switch (c) {
		case 'b':
			g.bucket_size = strtol(optarg, NULL, 10);
			if (g.bucket_size > 1024 || g.bucket_size <= 4) {
				printf("Illegal bucket size: %s (should be: 4-1024)\n",
				       optarg);
				exit(1);
			}
			break;
		case 'B':
			g.enable_bias = 1;
			break;
		case 'c':
			g.cpu_list = strdup(optarg);
			break;
		case 'C':
			ncores = sysconf(_SC_NPROCESSORS_CONF);
			g.cpu_main_thread = strtol(optarg, NULL, 10);
			if (g.cpu_main_thread < 0 || g.cpu_main_thread > ncores) {
				printf("Illegal core for main thread: %s (should be: 0-%ld)\n",
				       optarg, ncores);
				exit(1);
			}
			break;
		case 'D':
			g.runtime = parse_time_string(optarg);
			if (!g.runtime) {
				printf("Illegal runtime: %s\n", optarg);
				exit(1);
			}
			break;
		case 'f':
			g.rtprio = strtol(optarg, NULL, 10);
			if (g.rtprio < 1 || g.rtprio > 99) {
				printf("Illegal RT priority: %s (should be: 1-99)\n", optarg);
				exit(1);
			}
			break;
		case 'T':
			g.trace_threshold = strtol(optarg, NULL, 10);
			if (g.trace_threshold <= 0) {
				printf("Parameter --trace-threshold needs to be positive\n");
				exit(1);
			}
			enable_trace_mark();
			break;
		case 'w':
			if (workload_select(optarg)) {
				printf("Unknown workload '%s'.  Please choose from: ", optarg);
				for (i = 0; i < WORKLOAD_NUM; i++) {
					printf("'%s'", workload_list[i].w_name);
					if (i != WORKLOAD_NUM - 1)
						printf(", ");
				}
				printf("\n\n");
				exit(1);
			}
			break;
		case 'm':
			if (parse_mem_string(optarg, &g.workload_mem_size)) {
				printf("Unknown workload memory size '%s'.\n\n", optarg);
				exit(1);
			}
			break;
		case 's':
			/*
			 * Only use one core for pre-heat.  Then if --bias is used, the
			 * bias will be exactly the min value of the pre-heat core.
			 */
			g.single_preheat_thread = true;
			break;
		case 'v':
			/*
			 * Because we always dump the version even before parsing options,
			 * what we need to do is to quit..
			 */
			exit(0);
			break;
		case 'z':
			g.output_omit_zero_buckets = 1;
			break;
		case 'h':
			usage(0);
			break;
		default:
			usage(1);
			break;
		}
	}
}

void dump_globals(void)
{
	printf("Total runtime: \t\t%d seconds\n", g.runtime);
	printf("Thread priority: \t");
	if (g.rtprio)
		printf("SCHED_FIFO:%d\n", g.rtprio);
	else
		printf("default\n");
	printf("CPU list: \t\t%s\n", g.cpu_list ?: "(all cores)");
	printf("CPU for main thread: \t%d\n", g.cpu_main_thread);
	printf("Workload: \t\t%s\n", g.workload->w_name);
	printf("Workload mem: \t\t%"PRIu64" (KiB)\n",
	       (g.workload->w_flags & WORK_NEED_MEM) ?
	       (g.workload_mem_size / 1024) : 0);
	printf("Preheat cores: \t\t%d\n", g.single_preheat_thread ?
	       1 : g.n_threads_total);
	printf("\n");
}

static void record_bias(struct thread *t)
{
	int i;
	uint64_t bias = (uint64_t)-1;

	if (!g.enable_bias)
		return;

	/* Record the min value of minlat on all the threads */
	for (i = 0; i < g.n_threads; ++i) {
		if (t[i].minlat < bias)
			bias = t[i].minlat;
	}
	g.bias = bias;
	printf("Global bias set to %" PRId64 " (us)\n", bias);
}

int main(int argc, char *argv[])
{
	struct thread *threads;
	int i, n_cores;
	struct bitmask *cpu_set = NULL;
	int max_cpus = sysconf(_SC_NPROCESSORS_ONLN);

#ifdef FRC_MISSING
	printf("This architecture is not yet supported. "
	       "Please implement frc() function first for %s.\n", argv[0]);
	return 0;
#endif
	if (numa_available() == -1) {
		printf("ERROR: Could not initialize libnuma\n");
		exit(1);
	}

	g.app_name = argv[0];
	g.rtprio = 0;
	g.bucket_size = BUCKET_SIZE;
	g.runtime = 1;
	g.workload = &workload_list[WORKLOAD_DEFAULT];
	g.workload_mem_size = WORKLOAD_MEM_SIZE;
	/* Run the main thread on cpu0 by default */
	g.cpu_main_thread = 0;

	parse_options(argc, argv);

	TEST(mlockall(MCL_CURRENT | MCL_FUTURE) == 0);

	if (!g.cpu_list)
		g.cpu_list = strdup("all");
	if (parse_cpumask(g.cpu_list, max_cpus, &cpu_set))
		exit(1);
	n_cores = numa_bitmask_weight(cpu_set);

	TEST(threads = calloc(1, n_cores * sizeof(threads[0])));
	for (i = 0; i < n_cores; ++i)
		if (numa_bitmask_isbitset(cpu_set, i) && move_to_core(i) == 0)
			threads[g.n_threads_total++].core_i = i;

	if (numa_bitmask_isbitset(cpu_set, 0) && g.rtprio)
		printf("WARNING: Running SCHED_FIFO workload on CPU 0 may hang the thread\n");

	numa_bitmask_free(cpu_set);

	TEST(move_to_core(g.cpu_main_thread) == 0);

	signal(SIGALRM, handle_alarm);
	signal(SIGINT, handle_alarm);
	signal(SIGTERM, handle_alarm);

	dump_globals();

	printf("Pre-heat for 1 seconds...\n");
	if (g.single_preheat_thread)
		g.n_threads = 1;
	else
		g.n_threads = g.n_threads_total;
	run_expt(threads, 1);
	record_bias(threads);

	printf("Test starts...\n");
	/* Reset n_threads to always run on all the cores */
	g.n_threads = g.n_threads_total;
	run_expt(threads, g.runtime);

	printf("Test completed.\n\n");

	write_summary(threads);

	if (g.cpu_list) {
		free(g.cpu_list);
		g.cpu_list = NULL;
	}

	disable_trace_mark();

	return 0;
}
