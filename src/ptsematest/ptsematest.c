// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ptsematest.c
 * Copyright (C) 2009 Carsten Emde <C.Emde@osadl.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <linux/unistd.h>
#include <utmpx.h>
#include <pthread.h>
#include <inttypes.h>

#include "rt-utils.h"
#include "rt-get_cpu.h"
#include "rt-error.h"

enum {
	AFFINITY_UNSPECIFIED,
	AFFINITY_SPECIFIED,
	AFFINITY_USEALL
};

static pthread_mutex_t *testmutex;
static pthread_mutex_t *syncmutex;

struct params {
	int num;
	int cpu;
	int priority;
	int affinity;
	int sender;
	int samples;
	int max_cycles;
	int tracelimit;
	int tid;
	int shutdown;
	int stopped;
	struct timespec delay;
	unsigned int mindiff, maxdiff;
	double sumdiff;
	struct timeval unblocked, received, diff;
	pthread_t threadid;
	struct params *neighbor;
};

void *semathread(void *param)
{
	int mustgetcpu = 0;
	struct params *par = param;
	cpu_set_t mask;
	int policy = SCHED_FIFO;
	struct sched_param schedp;

	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = par->priority;
	sched_setscheduler(0, policy, &schedp);

	if (par->cpu != -1) {
		CPU_ZERO(&mask);
		CPU_SET(par->cpu, &mask);
		if (sched_setaffinity(0, sizeof(mask), &mask) == -1)
			fprintf(stderr,	"WARNING: Could not set CPU affinity "
				"to CPU #%d\n", par->cpu);
	} else
		mustgetcpu = 1;

	par->tid = gettid();

	while (!par->shutdown) {
		if (par->sender) {
			pthread_mutex_lock(&syncmutex[par->num]);

			/* Release lock: Start of latency measurement ... */
			gettimeofday(&par->unblocked, NULL);
			pthread_mutex_unlock(&testmutex[par->num]);
			par->samples++;
			if (par->max_cycles && par->samples >= par->max_cycles)
				par->shutdown = 1;
			if (mustgetcpu)
				par->cpu = get_cpu();
		} else {
			pthread_mutex_lock(&testmutex[par->num]);

			/* ... Got the lock: End of latency measurement */
			gettimeofday(&par->received, NULL);
			par->samples++;
			timersub(&par->received, &par->neighbor->unblocked,
			    &par->diff);

			if (par->diff.tv_usec < par->mindiff)
				par->mindiff = par->diff.tv_usec;
			if (par->diff.tv_usec > par->maxdiff)
				par->maxdiff = par->diff.tv_usec;
			par->sumdiff += (double) par->diff.tv_usec;
			if (par->tracelimit && par->maxdiff > par->tracelimit) {
				char tracing_enabled_file[MAX_PATH];

				strcpy(tracing_enabled_file, get_debugfileprefix());
				strcat(tracing_enabled_file, "tracing_on");
				int tracing_enabled =
				    open(tracing_enabled_file, O_WRONLY);
				if (tracing_enabled >= 0) {
					write(tracing_enabled, "0", 1);
					close(tracing_enabled);
				} else
					fatal("Could not access %s\n",
					      tracing_enabled_file);
				par->shutdown = 1;
				par->neighbor->shutdown = 1;
			}

			if (par->max_cycles && par->samples >= par->max_cycles)
				par->shutdown = 1;
			if (mustgetcpu)
				par->cpu = get_cpu();
			nanosleep(&par->delay, NULL);
			pthread_mutex_unlock(&syncmutex[par->num]);
		}
	}
	par->stopped = 1;
	return NULL;
}


static void display_help(int error)
{
	printf("ptsematest V %1.2f\n", VERSION);
	printf("Usage:\n"
	       "ptsematest <options>\n\n"
	       "Function: test POSIX threads mutex latency\n\n"
	       "Available options:\n"
	       "-a [NUM] --affinity        run thread #N on processor #N, if possible\n"
	       "                           with NUM pin all threads to the processor NUM\n"
	       "-b USEC  --breaktrace=USEC send break trace command when latency > USEC\n"
	       "-d DIST  --distance=DIST   distance of thread intervals in us default=500\n"
	       "-D       --duration=TIME   specify a length for the test run.\n"
	       "                           Append 'm', 'h', or 'd' to specify minutes, hours or\n"
	       "                           days.\n"
	       "-i INTV  --interval=INTV   base interval of thread in us default=1000\n"
	       "         --json=FILENAME   write final results into FILENAME, JSON formatted\n"
	       "-l LOOPS --loops=LOOPS     number of loops: default=0(endless)\n"
	       "-p PRIO  --prio=PRIO       priority\n"
	       "-q       --quiet           print a summary only on exit\n"
	       "-S       --smp             SMP testing: options -a -t and same priority\n"
	       "                           of all threads\n"
	       "-t       --threads         one thread per available processor\n"
	       "-t [NUM] --threads=NUM     number of threads:\n"
	       "                           without NUM, threads = max_cpus\n"
	       "                           without -t default = 1\n"
	       );
	exit(error);
}


static int setaffinity = AFFINITY_UNSPECIFIED;
static int affinity;
static int tracelimit;
static int priority;
static int num_threads = 1;
static int max_cycles;
static int duration;
static int interval = 1000;
static int distance = 500;
static int smp;
static int sameprio;
static int quiet;
static char jsonfile[MAX_PATH];

enum option_value {
	OPT_AFFINITY=1, OPT_BREAKTRACE, OPT_DISTANCE, OPT_DURATION,
	OPT_HELP, OPT_INTERVAL, OPT_JSON, OPT_LOOPS, OPT_PRIORITY,
	OPT_QUIET, OPT_SMP, OPT_THREADS
};

static void process_options(int argc, char *argv[])
{
	int error = 0;
	int max_cpus = sysconf(_SC_NPROCESSORS_CONF);

	for (;;) {
		int option_index = 0;
		/** Options for getopt */
		static struct option long_options[] = {
			{"affinity",	optional_argument,	NULL, OPT_AFFINITY},
			{"breaktrace",	required_argument,	NULL, OPT_BREAKTRACE},
			{"distance",	required_argument,	NULL, OPT_DISTANCE},
			{"duration",	required_argument,	NULL, OPT_DURATION},
			{"help",	no_argument,		NULL, OPT_HELP},
			{"interval",	required_argument,	NULL, OPT_INTERVAL},
			{"json",	required_argument,      NULL, OPT_JSON },
			{"loops",	required_argument,	NULL, OPT_LOOPS},
			{"priority",	required_argument,	NULL, OPT_PRIORITY},
			{"quiet",	no_argument	,	NULL, OPT_QUIET},
			{"smp",		no_argument,		NULL, OPT_SMP},
			{"threads",	optional_argument,	NULL, OPT_THREADS},
			{NULL, 0, NULL, 0}
		};
		int c = getopt_long (argc, argv, "a::b:d:i:l:D:p:qSt::h",
			long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case OPT_AFFINITY:
		case 'a':
			if (smp) {
				warn("-a ignored due to --smp\n");
				break;
			}
			if (optarg != NULL) {
				affinity = atoi(optarg);
				setaffinity = AFFINITY_SPECIFIED;
			} else if (optind < argc && atoi(argv[optind])) {
				affinity = atoi(argv[optind]);
				setaffinity = AFFINITY_SPECIFIED;
			} else {
				setaffinity = AFFINITY_USEALL;
			}
			break;
		case OPT_BREAKTRACE:
		case 'b':
			tracelimit = atoi(optarg);
			break;
		case OPT_DISTANCE:
		case 'd':
			distance = atoi(optarg);
			break;
		case OPT_DURATION:
		case 'D':
			duration = parse_time_string(optarg);
			break;
		case OPT_INTERVAL:
		case 'i':
			interval = atoi(optarg);
			break;
		case OPT_JSON:
			strncpy(jsonfile, optarg, strnlen(optarg, MAX_PATH-1));
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
			smp = 1;
			num_threads = max_cpus;
			setaffinity = AFFINITY_USEALL;
			break;
		case OPT_THREADS:
		case 't':
			if (smp) {
				warn("-t ignored due to --smp\n");
				break;
			}
			if (optarg != NULL)
				num_threads = atoi(optarg);
			else if (optind < argc && atoi(argv[optind]))
				num_threads = atoi(argv[optind]);
			else
				num_threads = max_cpus;
			break;
		default:
			display_help(1);
			break;
		}
	}

	if (setaffinity == AFFINITY_SPECIFIED) {
		if (affinity < 0)
			error = 1;
		if (affinity >= max_cpus) {
			fprintf(stderr, "ERROR: CPU #%d not found, only %d CPUs available\n",
			    affinity, max_cpus);
			error = 1;
		}
	}

	if (num_threads < 0 || num_threads > 255)
		error = 1;

	if (priority < 0 || priority > 99)
		error = 1;

	if (num_threads < 1)
		error = 1;

	if (duration < 0)
		error = 1;

	if (priority && smp)
		sameprio = 1;

	if (error)
		display_help(error);
}


static int volatile shutdown;

static void sighand(int sig)
{
	shutdown = 1;
}

static void print_stat(FILE *fp, struct params *receiver, struct params *sender,
		       int verbose, int quiet)
{
	int i;

	if (quiet)
		return;

	for (i = 0; i < num_threads; i++) {
		printf("#%1d: ID%d, P%d, CPU%d, I%ld; #%1d: ID%d, P%d, CPU%d, Cycles %d\n",
			i*2, receiver[i].tid, receiver[i].priority, receiver[i].cpu,
			receiver[i].delay.tv_nsec / 1000,
			i*2+1, sender[i].tid, sender[i].priority, sender[i].cpu,
			sender[i].samples);
	}
	for (i = 0; i < num_threads; i++) {
		printf("#%d -> #%d, Min %4d, Cur %4d, Avg %4d, Max %4d\n",
			i*2+1, i*2,
			receiver[i].mindiff, (int) receiver[i].diff.tv_usec,
			(int) ((receiver[i].sumdiff / receiver[i].samples) + 0.5),
			receiver[i].maxdiff);
	}
}

struct params_stats {
	struct params *receiver;
	struct params *sender;
};

static void write_stats(FILE *f, void *data)
{
	struct params_stats *ps = data;
	struct params *s, *r;
	unsigned int i;

	fprintf(f, "  \"num_threads\": %d,\n", num_threads);
	fprintf(f, "  \"thread\": {\n");
	for (i = 0; i < num_threads; i++) {
		s = &ps->sender[i];
		r = &ps->receiver[i];
		fprintf(f, "    \"%u\": {\n", i);
		fprintf(f, "      \"sender\": {\n");
		fprintf(f, "        \"cpu\": %d,\n", s->cpu);
		fprintf(f, "        \"priority\": %d,\n", s->priority);
		fprintf(f, "        \"samples\": %d,\n", s->samples);
		fprintf(f, "        \"interval\": %ld\n", r->delay.tv_nsec/1000);
		fprintf(f, "      },\n");
		fprintf(f, "      \"receiver\": {\n");
		fprintf(f, "        \"cpu\": %d,\n", r->cpu);
		fprintf(f, "        \"priority\": %d,\n", r->priority);
		fprintf(f, "        \"min\": %d,\n", r->mindiff);
		fprintf(f, "        \"avg\": %.2f,\n", r->sumdiff/r->samples);
		fprintf(f, "        \"max\": %d\n", r->maxdiff);
		fprintf(f, "      }\n");
		fprintf(f, "    }%s\n", i == num_threads - 1 ? "" : ",");
	}
	fprintf(f, "  }\n");
}

int main(int argc, char *argv[])
{
	int i;
	int max_cpus = sysconf(_SC_NPROCESSORS_CONF);
	int oldsamples = 1;
	struct params *receiver = NULL;
	struct params *sender = NULL;
	sigset_t sigset;
	struct timespec maindelay;

	rt_init(argc, argv);
	process_options(argc, argv);

	if (check_privs())
		return 1;

	if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
		perror("mlockall");
		return 1;
	}

	signal(SIGINT, sighand);
	signal(SIGTERM, sighand);
	signal(SIGALRM, sighand);

	if (duration)
		alarm(duration);

	receiver = calloc(num_threads, sizeof(struct params));
	sender = calloc(num_threads, sizeof(struct params));
	if (receiver == NULL || sender == NULL)
		goto nomem;

	testmutex = (pthread_mutex_t *) calloc(num_threads, sizeof(pthread_mutex_t));
	syncmutex = (pthread_mutex_t *) calloc(num_threads, sizeof(pthread_mutex_t));
	if (testmutex == NULL || syncmutex == NULL)
		goto nomem;

	for (i = 0; i < num_threads; i++) {
		receiver[i].mindiff = UINT_MAX;
		receiver[i].maxdiff = 0;
		receiver[i].sumdiff = 0.0;


		pthread_mutex_init(&testmutex[i], NULL);
		pthread_mutex_init(&syncmutex[i], NULL);

		/* Wait on first attempt */
		pthread_mutex_lock(&testmutex[i]);

		receiver[i].num = i;
		receiver[i].cpu = i;
		switch (setaffinity) {
		case AFFINITY_UNSPECIFIED: receiver[i].cpu = -1; break;
		case AFFINITY_SPECIFIED: receiver[i].cpu = affinity; break;
		case AFFINITY_USEALL: receiver[i].cpu = i % max_cpus; break;
		}
		receiver[i].priority = priority;
		receiver[i].tracelimit = tracelimit;
		if (priority > 1 && !sameprio)
			priority--;
		receiver[i].delay.tv_sec = interval / USEC_PER_SEC;
		receiver[i].delay.tv_nsec = (interval % USEC_PER_SEC) * 1000;
		interval += distance;
		receiver[i].max_cycles = max_cycles;
		receiver[i].sender = 0;
		receiver[i].neighbor = &sender[i];
		pthread_create(&receiver[i].threadid, NULL, semathread, &receiver[i]);
		memcpy(&sender[i], &receiver[i], sizeof(receiver[0]));
		sender[i].sender = 1;
		sender[i].neighbor = &receiver[i];
		pthread_create(&sender[i].threadid, NULL, semathread, &sender[i]);
	}

	maindelay.tv_sec = 0;
	maindelay.tv_nsec = 50000000; /* 50 ms */

	while (!shutdown) {
		for (i = 0; i < num_threads; i++)
			shutdown |= receiver[i].shutdown | sender[i].shutdown;

		if (receiver[0].samples > oldsamples || shutdown) {
			print_stat(stdout, receiver, sender, 0, quiet);
			if (!quiet)
				printf("\033[%dA", num_threads*2);
		}

		sigemptyset(&sigset);
		sigaddset(&sigset, SIGTERM);
		sigaddset(&sigset, SIGINT);
		pthread_sigmask(SIG_SETMASK, &sigset, NULL);

		nanosleep(&maindelay, NULL);

		sigemptyset(&sigset);
		pthread_sigmask(SIG_SETMASK, &sigset, NULL);
	}

	if (!quiet)
		printf("\033[%dB", num_threads*2 + 2);
	else
		print_stat(stdout, receiver, sender, 0, 0);

	for (i = 0; i < num_threads; i++) {
		receiver[i].shutdown = 1;
		sender[i].shutdown = 1;
		pthread_mutex_unlock(&testmutex[i]);
		pthread_mutex_unlock(&syncmutex[i]);
	}
	nanosleep(&receiver[0].delay, NULL);

	for (i = 0; i < num_threads; i++) {
		if (!receiver[i].stopped)
			pthread_kill(receiver[i].threadid, SIGTERM);
		if (!sender[i].stopped)
			pthread_kill(sender[i].threadid, SIGTERM);
	}

	for (i = 0; i < num_threads; i++) {
		pthread_mutex_destroy(&testmutex[i]);
		pthread_mutex_destroy(&syncmutex[i]);
	}

	if (strlen(jsonfile) != 0) {
		struct params_stats ps = {
			.receiver = receiver,
			.sender = sender,
		};
		rt_write_json(jsonfile, 0, write_stats, &ps);
	}

nomem:

	return 0;
}
