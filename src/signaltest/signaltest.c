// SPDX-License-Identifier: GPL-2.0-only

/*
 * RT signal roundtrip test software
 *
 * (C) 2007 Thomas Gleixner <tglx@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version
 * 2 as published by the Free Software Foundation;
 *
 */

#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <inttypes.h>

#include <linux/unistd.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>

#include "rt-error.h"
#include "rt-utils.h"
#include "rt-numa.h"

/* Must be power of 2 ! */
#define VALBUF_SIZE		16384

/* Struct to transfer parameters to the thread */
struct thread_param {
	int id;
	int prio;
	int signal;
	unsigned long max_cycles;
	struct thread_stat *stats;
	int bufmsk;
	int cpu;
};

/* Struct for statistics */
struct thread_stat {
	unsigned long cycles;
	unsigned long cyclesread;
	long min;
	long max;
	long act;
	double avg;
	long *values;
	pthread_t thread;
	pthread_t tothread;
	int threadstarted;
	int tid;
	int interrupted;
};

static int shutdown;
static int tracelimit;


/*
 * signal thread
 *
 */
void *signalthread(void *param)
{
	struct thread_param *par = param;
	struct sched_param schedp;
	sigset_t sigset;
	struct timespec before, after;
	struct thread_stat *stat = par->stats;
	int policy = par->prio ? SCHED_FIFO : SCHED_OTHER;
	int stopped = 0;
	int first = 1;
	pthread_t thread;
	cpu_set_t mask;

	stat->tid = gettid();

	if (par->cpu != -1) {
		CPU_ZERO(&mask);
		CPU_SET(par->cpu, &mask);
		thread = pthread_self();
		if (pthread_setaffinity_np(thread, sizeof(mask), &mask) != 0)
			warn("Could not set CPU affinity to CPU #%d\n",
			     par->cpu);
	}

	sigemptyset(&sigset);
	sigaddset(&sigset, par->signal);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = par->prio;
	sched_setscheduler(0, policy, &schedp);

	stat->threadstarted++;

	clock_gettime(CLOCK_MONOTONIC, &before);

	while (!shutdown) {
		struct timespec now;
		long diff;
		int sigs;

		if (sigwait(&sigset, &sigs) < 0)
			goto out;

		clock_gettime(CLOCK_MONOTONIC, &after);

		/*
		 * If it is the first thread, sleep after every 16
		 * round trips.
		 */
		if (!par->id && !(stat->cycles & 0x0F))
			usleep(10000);

		/* Get current time */
		clock_gettime(CLOCK_MONOTONIC, &now);
		pthread_kill(stat->tothread, SIGUSR1);

		/* Skip the first cycle */
		if (first) {
			first = 0;
			before = now;
			continue;
		}

		diff = calcdiff(after, before);
		before = now;

		if (diff < stat->min)
			stat->min = diff;
		if (diff > stat->max)
			stat->max = diff;
		stat->avg += (double) diff;

		if (!stopped && tracelimit && !par->id  && (diff > tracelimit)) {
			stat->act = diff;
			stat->interrupted = 1;
			stopped++;
			shutdown++;
		}
		stat->act = diff;
		stat->cycles++;

		if (par->bufmsk)
			stat->values[stat->cycles & par->bufmsk] = diff;

		if (par->max_cycles && par->max_cycles == stat->cycles)
			break;
	}

out:
	/* switch to normal */
	schedp.sched_priority = 0;
	sched_setscheduler(0, SCHED_OTHER, &schedp);

	stat->threadstarted = -1;

	return NULL;
}


/* Print usage information */
static void display_help(int error)
{
	printf("signaltest V %1.2f\n", VERSION);
	printf("Usage:\n"
		"signaltest <options>\n\n"
		"-a [NUM] --affinity        run thread #N on processor #N, if possible\n"
		"                           with NUM pin all threads to the processor NUM\n"
		"-b USEC  --breaktrace=USEC send break trace command when latency > USEC\n"
		"-D       --duration=TIME   specify a length for the test run.\n"
		"                           Append 'm', 'h', or 'd' to specify minutes, hours or\n"
		"                           days.\n"
		"-h       --help            display usage information\n"
		"-l LOOPS --loops=LOOPS     number of loops: default=0(endless)\n"
		"-m       --mlockall        lock current and future memory allocations\n"
		"-p PRIO  --prio=PRIO       priority of highest prio thread\n"
		"-q       --quiet           print a summary only on exit\n"
		"-t NUM   --threads=NUM     number of threads: default=2\n"
		"-v       --verbose         output values on stdout for statistics\n"
		"                           format: n:c:v n=tasknum c=count v=value in us\n"
		);
	exit(error);
}

static int priority;
static int num_threads = 2;
static int max_cycles;
static int duration;
static int verbose;
static int quiet;
static int lockall;
static struct bitmask *affinity_mask = NULL;
static int smp = 0;
static int numa = 0;
static int setaffinity = AFFINITY_UNSPECIFIED;
static char outfile[MAX_PATH];

enum option_values {
	OPT_AFFINITY=1, OPT_BREAKTRACE,
	OPT_DURATION, OPT_HELP, OPT_LOOPS,
	OPT_MLOCKALL, OPT_OUTPUT, OPT_PRIORITY,
	OPT_QUIET, OPT_SMP, OPT_THREADS, OPT_VERBOSE
};

/* Process commandline options */
static void process_options(int argc, char *argv[], unsigned int max_cpus)
{
	int option_affinity = 0;
	int error = 0;

	for (;;) {
		int option_index = 0;
		/** Options for getopt */
		static struct option long_options[] = {
			{"affinity",	optional_argument,	NULL, OPT_AFFINITY},
			{"breaktrace",	required_argument,	NULL, OPT_BREAKTRACE},
			{"duration",	required_argument,	NULL, OPT_DURATION},
			{"help",	no_argument,		NULL, OPT_HELP},
			{"loops",	required_argument,	NULL, OPT_LOOPS},
			{"mlockall",	no_argument,		NULL, OPT_MLOCKALL},
			{"output",	required_argument,	NULL, OPT_OUTPUT},
			{"priority",	required_argument,	NULL, OPT_PRIORITY},
			{"quiet",	no_argument,		NULL, OPT_QUIET},
			{"smp",		no_argument,		NULL, OPT_SMP},
			{"threads",	required_argument,	NULL, OPT_THREADS},
			{"verbose",	no_argument,		NULL, OPT_VERBOSE},
			{NULL, 0, NULL, 0}
		};
		int c = getopt_long(argc, argv, "a::b:D:hl:mp:qSt:v",
				long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case OPT_AFFINITY:
		case 'a':
			option_affinity = 1;
			/* smp sets AFFINITY_USEALL in OPT_SMP */
			if (smp)
				break;
			numa = numa_initialize();
			if (optarg) {
				parse_cpumask(optarg, max_cpus, &affinity_mask);
				setaffinity = AFFINITY_SPECIFIED;
			} else if (optind < argc &&
				   (atoi(argv[optind]) ||
				    argv[optind][0] == '0' ||
				    argv[optind][0] == '!')) {
				parse_cpumask(argv[optind], max_cpus, &affinity_mask);
				setaffinity = AFFINITY_SPECIFIED;
			} else {
				setaffinity = AFFINITY_USEALL;
			}

			if (setaffinity == AFFINITY_SPECIFIED && !affinity_mask)
				display_help(1);
			if (verbose)
				printf("Using %u cpus.\n",
					numa_bitmask_weight(affinity_mask));
			break;
		case OPT_BREAKTRACE:
		case 'b':
			tracelimit = atoi(optarg);
			break;
		case OPT_DURATION:
		case 'D':
			duration = parse_time_string(optarg);
			break;
		case OPT_HELP:
		case '?':
		case 'h':
			display_help(0);
			break;
		case OPT_LOOPS:
		case 'l':
			max_cycles = atoi(optarg);
			break;
		case OPT_MLOCKALL:
		case 'm':
			lockall = 1;
			break;
		case OPT_OUTPUT:
			strncpy(outfile, optarg, strnlen(optarg, MAX_PATH-1));
			break;
		case OPT_PRIORITY:
		case 'p':
			priority = atoi(optarg);
			break;
		case OPT_QUIET:
		case 'q':
			quiet = 1;
			break;
		case OPT_SMP:
		case 'S':
			if (numa)
				fatal("numa and smp options are mutually exclusive\n");
			smp = 1;
			num_threads = -1; /* update after parsing */
			setaffinity = AFFINITY_USEALL;
			break;
		case OPT_THREADS:
		case 't':
			num_threads = atoi(optarg);
			break;
		case OPT_VERBOSE:
		case 'v': verbose = 1;
			break;
		}
	}

	if (duration < 0)
		error = 1;

	if (priority < 0 || priority > 99)
		error = 1;

	if (num_threads == -1)
		num_threads = get_available_cpus(affinity_mask);

	if (num_threads < 2)
		error = 1;

	/* if smp wasn't requested, test for numa automatically */
	if (!smp) {
		numa = numa_initialize();
		if (setaffinity == AFFINITY_UNSPECIFIED)
			setaffinity = AFFINITY_USEALL;
	}

	if (option_affinity) {
		if (smp)
			warn("-a ignored due to smp mode\n");
	}

	if (error) {
		if (affinity_mask)
			numa_bitmask_free(affinity_mask);
		display_help(error);
	}
}

static void sighand(int sig)
{
	shutdown = 1;
}

static void print_stat(struct thread_param *par, int index, int verbose)
{
	struct thread_stat *stat = par->stats;

	if (!verbose) {
		if (quiet != 1) {
			printf("T:%2d (%5d) P:%2d C:%7lu "
			       "Min:%7ld Act:%5ld Avg:%5ld Max:%8ld\n",
			       index, stat->tid, par->prio,
			       stat->cycles, stat->min, stat->act,
			       stat->cycles ?
			       (long)(stat->avg/stat->cycles) : 0, stat->max);
		}
	} else {
		while (stat->cycles != stat->cyclesread) {
			long diff = stat->values[stat->cyclesread & par->bufmsk];
			printf("%8d:%8lu:%8ld\n", index, stat->cyclesread, diff);
			stat->cyclesread++;
		}
	}
}

static void write_stats(FILE *f, void *data)
{
	struct thread_param *par = data;
	struct thread_stat *s;
	unsigned int i;

	fprintf(f, "  \"num_threads\": %d,\n", num_threads);
	fprintf(f, "  \"thread\": {\n");
	for (i = 0; i < num_threads; i++) {
		fprintf(f, "    \"%u\": {\n", i);
		s = &par->stats[i];
		fprintf(f, "      \"cycles\": %" PRIu64 ",\n", s->cycles);
		fprintf(f, "      \"min\": %" PRIu64 ",\n", s->min);
		fprintf(f, "      \"max\": %" PRIu64 ",\n", s->max);
		fprintf(f, "      \"avg\": %.2f,\n", s->avg/s->cycles);
		fprintf(f, "      \"cpu\": %d\n", par->cpu);
		fprintf(f, "    }%s\n", i == num_threads - 1 ? "" : ",");

	}
	fprintf(f, "  }\n");
}

int main(int argc, char **argv)
{
	sigset_t sigset;
	int signum = SIGUSR1;
	struct thread_param *par;
	struct thread_stat *stat;
	int i, ret = -1;
	int status, cpu;
	int max_cpus = sysconf(_SC_NPROCESSORS_ONLN);

	process_options(argc, argv, max_cpus);

	if (check_privs())
		exit(1);

	/* lock all memory (prevent paging) */
	if (lockall)
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			perror("mlockall");
			goto out;
		}

	/* Restrict the main pid to the affinity specified by the user */
	if (affinity_mask != NULL) {
		int res;

		errno = 0;
		res = numa_sched_setaffinity(getpid(), affinity_mask);
		if (res != 0)
			warn("Couldn't setaffinity in main thread: %s\n", strerror(errno));
	}

	sigemptyset(&sigset);
	sigaddset(&sigset, signum);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	signal(SIGINT, sighand);
	signal(SIGTERM, sighand);
	signal(SIGALRM, sighand);

	if (duration)
		alarm(duration);

	par = calloc(num_threads, sizeof(struct thread_param));
	if (!par)
		goto out;
	stat = calloc(num_threads, sizeof(struct thread_stat));
	if (!stat)
		goto outpar;

	for (i = 0; i < num_threads; i++) {
		if (verbose) {
			stat[i].values = calloc(VALBUF_SIZE, sizeof(long));
			if (!stat[i].values)
				goto outall;
			par[i].bufmsk = VALBUF_SIZE - 1;
		}

		switch (setaffinity) {
		case AFFINITY_UNSPECIFIED:
			cpu = -1;
			break;
		case AFFINITY_SPECIFIED:
			cpu = cpu_for_thread_sp(i, max_cpus, affinity_mask);
			if (verbose)
				printf("Thread %d using cpu %d.\n", i, cpu);
			break;
		case AFFINITY_USEALL:
			cpu = cpu_for_thread_ua(i, max_cpus);
			break;
		default:
			cpu = -1;
		}

		par[i].id = i;
		par[i].prio = priority;
#if 0
		if (priority)
			priority--;
#endif
		par[i].signal = signum;
		par[i].max_cycles = max_cycles;
		par[i].stats = &stat[i];
		par[i].cpu = cpu;
		stat[i].min = 1000000;
		stat[i].max = -1000000;
		stat[i].avg = 0.0;
		stat[i].threadstarted = 1;
		status = pthread_create(&stat[i].thread, NULL, signalthread,
					&par[i]);
		if (status)
			fatal("failed to create thread %d: %s\n", i,
			      strerror(status));
	}

	while (!shutdown) {
		int allstarted = 1;

		for (i = 0; i < num_threads; i++) {
			if (stat[i].threadstarted != 2)
				allstarted = 0;
		}
		if (!allstarted)
			continue;

		for (i = 0; i < num_threads - 1; i++)
			stat[i].tothread = stat[i+1].thread;
		stat[i].tothread = stat[0].thread;
		break;
	}
	pthread_kill(stat[0].thread, signum);

	while (!shutdown) {
		char lavg[256];
		int fd, len, allstopped = 0;

		if (!verbose && !quiet) {
			fd = open("/proc/loadavg", O_RDONLY, 0666);
			len = read(fd, &lavg, 255);
			close(fd);
			lavg[len-1] = 0x0;
			printf("%s          \n\n", lavg);
		}

		print_stat(&par[0], 0, verbose);
		if (max_cycles && stat[0].cycles >= max_cycles)
			allstopped++;

		usleep(10000);
		if (shutdown || allstopped)
			break;
		if (!verbose && !quiet)
			printf("\033[%dA", 3);
	}
	ret = 0;
 outall:
	shutdown = 1;
	usleep(50000);
	if (quiet)
		quiet = 2;
	for (i = 0; i < num_threads; i++) {
		if (stat[i].threadstarted > 0)
			pthread_kill(stat[i].thread, SIGUSR1);
		if (stat[i].interrupted)
			printf("Thread %d exceeded trace limit.\n", i);
		if (stat[i].threadstarted) {
			pthread_join(stat[i].thread, NULL);
			print_stat(&par[i], i, 0);
		}
		if (stat[i].values)
			free(stat[i].values);
	}
	if (strlen(outfile) != 0)
		rt_write_json(outfile, argc, argv, write_stats, par);

	free(stat);
 outpar:
	free(par);
 out:
	if (lockall)
		munlockall();

	exit(ret);
}
