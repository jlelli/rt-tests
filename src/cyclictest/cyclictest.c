/*
 * High resolution timer test software
 *
 * (C) 2008-2009 Clark Williams <williams@redhat.com>
 * (C) 2005-2007 Thomas Gleixner <tglx@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version
 * 2 as published by the Free Software Foundation.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define __USE_GNU
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <linux/unistd.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/mman.h>

#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifndef SCHED_NORMAL
#define SCHED_NORMAL SCHED_OTHER
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* Ugly, but .... */
#define gettid() syscall(__NR_gettid)
#define sigev_notify_thread_id _sigev_un._tid

#ifdef __UCLIBC__
#define MAKE_PROCESS_CPUCLOCK(pid, clock) \
	((~(clockid_t) (pid) << 3) | (clockid_t) (clock))
#define CPUCLOCK_SCHED          2

static int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *req,
		struct timespec *rem)
{
	if (clock_id == CLOCK_THREAD_CPUTIME_ID)
		return -EINVAL;
	if (clock_id == CLOCK_PROCESS_CPUTIME_ID)
		clock_id = MAKE_PROCESS_CPUCLOCK (0, CPUCLOCK_SCHED);

	return syscall(__NR_clock_nanosleep, clock_id, flags, req, rem);
}

static int sched_setaffinity(pid_t pid, unsigned int cpusetsize,
		cpu_set_t *mask)
{
	return -EINVAL;
}

static void CPU_SET(int cpu, cpu_set_t *set) { }
static void CPU_ZERO(cpu_set_t *set) { }
#else
extern int clock_nanosleep(clockid_t __clock_id, int __flags,
			   __const struct timespec *__req,
			   struct timespec *__rem);
#endif

#define USEC_PER_SEC		1000000
#define NSEC_PER_SEC		1000000000

#define HIST_MAX		1000000

#define MODE_CYCLIC		0
#define MODE_CLOCK_NANOSLEEP	1
#define MODE_SYS_ITIMER		2
#define MODE_SYS_NANOSLEEP	3
#define MODE_SYS_OFFSET		2

#define TIMER_RELTIME		0

/* Must be power of 2 ! */
#define VALBUF_SIZE		16384

#define KVARS			32
#define KVARNAMELEN		32
#define KVALUELEN		32

enum {
	NOTRACE,
	EVENTS,
	CTXTSWITCH,
	IRQSOFF,
	PREEMPTOFF,
	IRQPREEMPTOFF,
	WAKEUP,
	WAKEUPRT,
};

/* Struct to transfer parameters to the thread */
struct thread_param {
	int prio;
	int policy;
	int mode;
	int timermode;
	int signal;
	int clock;
	unsigned long max_cycles;
	struct thread_stat *stats;
	int bufmsk;
	unsigned long interval;
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
	long *hist_array;
	pthread_t thread;
	int threadstarted;
	int tid;
	long reduce;
	long redmax;
	long cycleofmax;
};

static int shutdown;
static int tracelimit = 0;
static int ftrace = 0;
static int kernelversion;
static int verbose = 0;
static int oscope_reduction = 1;
static int lockall = 0;
static int tracetype = NOTRACE;
static int histogram = 0;
static int histogram_limit_exceeded = 0;
static int duration = 0;
static int use_nsecs = 0;


/* Backup of kernel variables that we modify */
static struct kvars {
	char name[KVARNAMELEN];
	char value[KVALUELEN];
} kv[KVARS];

#define _STR(x) #x
#define STR(x) _STR(x)
#define MAX_PATH 256

static char *procfileprefix = "/proc/sys/kernel/";
static char debugfileprefix[MAX_PATH];
static char *fileprefix;
static char tracer[MAX_PATH];
static char **traceptr;
static int traceopt_count;
static int traceopt_size;

enum kernelversion {
	KV_NOT_26,
	KV_26_LT18,
	KV_26_LT24,
	KV_26_CURR
};

enum {
	ERROR_GENERAL	= -1,
	ERROR_NOTFOUND	= -2,
};

static char functiontracer[MAX_PATH];
static char traceroptions[MAX_PATH];

/*
 * Finds the tracing directory in a mounted debugfs
 */
static int set_debugfileprefix(void)
{
	char type[100];
	FILE *fp;
	int size;

	if ((fp = fopen("/proc/mounts","r")) == NULL)
		return ERROR_GENERAL;

	while (fscanf(fp, "%*s %"
		      STR(MAX_PATH)
		      "s %99s %*s %*d %*d\n",
		      debugfileprefix, type) == 2) {
		if (strcmp(type, "debugfs") == 0)
			break;
	}
	fclose(fp);

	if (strcmp(type, "debugfs") != 0)
		return ERROR_NOTFOUND;

	size = strlen(debugfileprefix);
	size = MAX_PATH - size;

	strncat(debugfileprefix, "/tracing/", size);

	return 0;
}

static int kernvar(int mode, const char *name, char *value, size_t sizeofvalue)
{
	char filename[128];
	int retval = 1;
	int path;

	strncpy(filename, fileprefix, sizeof(filename));
	strncat(filename, name, sizeof(filename) - strlen(fileprefix));
	path = open(filename, mode);
	if (path >= 0) {
		if (mode == O_RDONLY) {
			int got;
			if ((got = read(path, value, sizeofvalue)) > 0) {
				retval = 0;
				value[got-1] = '\0';
			}
		} else if (mode == O_WRONLY) {
			if (write(path, value, sizeofvalue) == sizeofvalue)
				retval = 0;
		}
		close(path);
	}
	return retval;
}

static void setkernvar(const char *name, char *value)
{
	int i;
	char oldvalue[KVALUELEN];

	if (kernelversion != KV_26_CURR) {
		if (kernvar(O_RDONLY, name, oldvalue, sizeof(oldvalue)))
			fprintf(stderr, "could not retrieve %s\n", name);
		else {
			for (i = 0; i < KVARS; i++) {
				if (!strcmp(kv[i].name, name))
					break;
				if (kv[i].name[0] == '\0') {
					strncpy(kv[i].name, name,
						sizeof(kv[i].name));
					strncpy(kv[i].value, oldvalue,
					    sizeof(kv[i].value));
					break;
				}
			}
			if (i == KVARS)
				fprintf(stderr, "could not backup %s (%s)\n",
					name, oldvalue);
		}
	}
	if (kernvar(O_WRONLY, name, value, strlen(value)))
		fprintf(stderr, "could not set %s to %s\n", name, value);

}

static void restorekernvars(void)
{
	int i;

	for (i = 0; i < KVARS; i++) {
		if (kv[i].name[0] != '\0') {
			if (kernvar(O_WRONLY, kv[i].name, kv[i].value,
			    strlen(kv[i].value)))
				fprintf(stderr, "could not restore %s to %s\n",
					kv[i].name, kv[i].value);
		}
	}
}

static inline void tsnorm(struct timespec *ts)
{
	while (ts->tv_nsec >= NSEC_PER_SEC) {
		ts->tv_nsec -= NSEC_PER_SEC;
		ts->tv_sec++;
	}
}

static inline long long calcdiff(struct timespec t1, struct timespec t2)
{
	long long diff;
	diff = USEC_PER_SEC * (long long)((int) t1.tv_sec - (int) t2.tv_sec);
	diff += ((int) t1.tv_nsec - (int) t2.tv_nsec) / 1000;
	return diff;
}

static inline long long calcdiff_ns(struct timespec t1, struct timespec t2)
{
	long long diff;
	diff = NSEC_PER_SEC * (long long)((int) t1.tv_sec - (int) t2.tv_sec);
	diff += ((int) t1.tv_nsec - (int) t2.tv_nsec);
	return diff;
}

void traceopt(char *option)
{
	char *ptr;
	if (traceopt_count + 1 > traceopt_size) {
		traceopt_size += 16;
		printf("expanding traceopt buffer to %d entries\n", traceopt_size);
		traceptr = realloc(traceptr, sizeof(char*) * traceopt_size);
		if (traceptr == NULL) {
			fprintf(stderr, "Error allocating space for %d trace options\n",
				traceopt_count+1);
			exit(EXIT_FAILURE);
		}
	}
	ptr = malloc(strlen(option)+1);
	if (ptr == NULL) {
		fprintf(stderr, "error allocating space for trace option %s\n", option);
		exit(EXIT_FAILURE);
	}
	printf("adding traceopt %s\n", option);
	strcpy(ptr, option);
	traceptr[traceopt_count++] = ptr;
}


void tracing(int on)
{
	if (on) {
		switch (kernelversion) {
		case KV_26_LT18: gettimeofday(0,(struct timezone *)1); break;
		case KV_26_LT24: prctl(0, 1); break;
		case KV_26_CURR: setkernvar("tracing_enabled", "1"); break;
		default:	 break;
		}
	} else {
		switch (kernelversion) {
		case KV_26_LT18: gettimeofday(0,0); break;
		case KV_26_LT24: prctl(0, 0); break;
		case KV_26_CURR: setkernvar("tracing_enabled", "0"); break;
		default:	 break;
		}
	}
}

static int settracer(char *tracer)
{
	char filename[MAX_PATH];
	char tracers[MAX_PATH];
	char *name;
	FILE *fp;
	int ret = -1;
	int len;
	const char *delim = " \t\n";

	/* Make sure tracer is available */
	strncpy(filename, debugfileprefix, sizeof(filename));
	strncat(filename, "available_tracers", 
		sizeof(filename) - strlen(debugfileprefix));

	fp = fopen(filename, "r");
	if (!fp)
		return -1;

	if (!(len = fread(tracers, 1, sizeof(tracers), fp))) {
		fclose(fp);
		return -1;
	}
	tracers[len] = '\0';
	fclose(fp);

	name = strtok(tracers, delim);
	while (name) {
		if (strcmp(name, tracer) == 0) {
			ret = 0;
			break;
		}
		name = strtok(NULL, delim);
	}

	if (!ret)
		setkernvar("current_tracer", tracer);

	return ret;
}

static void setup_tracer(void)
{
	if (!tracelimit)
		return;

	if (kernelversion == KV_26_CURR) {
		char testname[MAX_PATH];

		set_debugfileprefix();
		fileprefix = debugfileprefix;

		strcpy(testname, debugfileprefix);
		strcat(testname, "tracing_enabled");
		if (access(testname, R_OK)) {
			fprintf(stderr, "ERROR: %s not found\n"
			    "debug fs not mounted, "
			    "TRACERs not configured?\n", testname);
		}
	} else
		fileprefix = procfileprefix;

	if (kernelversion == KV_26_CURR) {
		char buffer[32];
		int ret;

		sprintf(buffer, "%d", tracelimit);
		setkernvar("tracing_thresh", buffer);

		/* ftrace_enabled is a sysctl variable */
		fileprefix = procfileprefix;
		if (ftrace)
			setkernvar("ftrace_enabled", "1");
		else
			setkernvar("ftrace_enabled", "0");
		fileprefix = debugfileprefix;

		switch (tracetype) {
		case NOTRACE:
			if (ftrace)
				ret = settracer(functiontracer);
			else
				ret = 0;
			break;
		case IRQSOFF:
			ret = settracer("irqsoff");
			break;
		case PREEMPTOFF:
			ret = settracer("preemptoff");
			break;
		case IRQPREEMPTOFF:
			ret = settracer("preemptirqsoff");
			break;
		case EVENTS:
			ret = settracer("events");
			if (ftrace)
				ret = settracer(functiontracer);
			break;
		case CTXTSWITCH:
			ret = settracer("sched_switch");
			break;
               case WAKEUP:
                       ret = settracer("wakeup");
                       break;
               case WAKEUPRT:
                       ret = settracer("wakeup_rt");
                       break;
		default:
			if (strlen(tracer)) {
				ret = settracer(tracer);
				if (strcmp(tracer, "events") == 0 && ftrace)
					ret = settracer(functiontracer);
			}
			else {
				printf("cyclictest: unknown tracer!\n");
				ret = 0;
			}
			break;
		}

		if (ret)
			fprintf(stderr, "Requested tracer '%s' not available\n", tracer);

		setkernvar(traceroptions, "print-parent");
		if (verbose) {
			setkernvar(traceroptions, "sym-offset");
			setkernvar(traceroptions, "sym-addr");
			setkernvar(traceroptions, "verbose");
		} else {
			setkernvar(traceroptions, "nosym-offset");
			setkernvar(traceroptions, "nosym-addr");
			setkernvar(traceroptions, "noverbose");
		}
		if (traceopt_count) {
			int i;
			for (i = 0; i < traceopt_count; i++)
				setkernvar(traceroptions, traceptr[i]);
		}
		setkernvar("tracing_max_latency", "0");
		setkernvar("latency_hist/wakeup_latency/reset", "1");
	} else {
		setkernvar("trace_all_cpus", "1");
		setkernvar("trace_freerunning", "1");
		setkernvar("trace_print_on_crash", "0");
		setkernvar("trace_user_triggered", "1");
		setkernvar("trace_user_trigger_irq", "-1");
		setkernvar("trace_verbose", "0");
		setkernvar("preempt_thresh", "0");
		setkernvar("wakeup_timing", "0");
		setkernvar("preempt_max_latency", "0");
		if (ftrace)
			setkernvar("mcount_enabled", "1");
		setkernvar("trace_enabled", "1");
	}

	tracing(1);
}

/*
 * parse an input value as a base10 value followed by an optional
 * suffix. The input value is presumed to be in seconds, unless
 * followed by a modifier suffix: m=minutes, h=hours, d=days
 *
 * the return value is a value in seconds
 */
int
parse_time_string(char *val)
{
	char *end;
	int t = strtol(val, &end, 10);
	if (end) {
		switch (*end) {
		case 'm':
		case 'M':
			t *= 60;
			break;

		case 'h':
		case 'H':
			t *= 60*60;
			break;

		case 'd':
		case 'D':
			t *= 24*60*60;
			break;

		}
	}
	return t;
}

/*
 * timer thread
 *
 * Modes:
 * - clock_nanosleep based
 * - cyclic timer based
 *
 * Clock:
 * - CLOCK_MONOTONIC
 * - CLOCK_REALTIME
 * - CLOCK_MONOTONIC_HR
 * - CLOCK_REALTIME_HR
 *
 */
void *timerthread(void *param)
{
	struct thread_param *par = param;
	struct sched_param schedp;
	struct sigevent sigev;
	sigset_t sigset;
	timer_t timer;
	struct timespec now, next, interval, stop;
	struct itimerval itimer;
	struct itimerspec tspec;
	struct thread_stat *stat = par->stats;
	int stopped = 0;
	cpu_set_t mask;

	if (par->cpu != -1) {
		CPU_ZERO(&mask);
		CPU_SET(par->cpu, &mask);
		if(sched_setaffinity(0, sizeof(mask), &mask) == -1)
			fprintf(stderr,	"WARNING: Could not set CPU affinity "
				"to CPU #%d\n", par->cpu);
	}

	interval.tv_sec = par->interval / USEC_PER_SEC;
	interval.tv_nsec = (par->interval % USEC_PER_SEC) * 1000;

	stat->tid = gettid();

	sigemptyset(&sigset);
	sigaddset(&sigset, par->signal);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	if (par->mode == MODE_CYCLIC) {
		sigev.sigev_notify = SIGEV_THREAD_ID | SIGEV_SIGNAL;
		sigev.sigev_signo = par->signal;
		sigev.sigev_notify_thread_id = stat->tid;
		timer_create(par->clock, &sigev, &timer);
		tspec.it_interval = interval;
	}

	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = par->prio;
	sched_setscheduler(0, par->policy, &schedp);

	/* Get current time */
	clock_gettime(par->clock, &now);
	next = now;
	next.tv_sec++;

	if (duration) {
		memset(&stop, 0, sizeof(stop)); /* grrr */
		stop = now;
		stop.tv_sec += duration;
		tsnorm(&stop);
	}
	if (par->mode == MODE_CYCLIC) {
		if (par->timermode == TIMER_ABSTIME)
			tspec.it_value = next;
		else {
			tspec.it_value.tv_nsec = 0;
			tspec.it_value.tv_sec = 1;
		}
		timer_settime(timer, par->timermode, &tspec, NULL);
	}

	if (par->mode == MODE_SYS_ITIMER) {
		itimer.it_value.tv_sec = 1;
		itimer.it_value.tv_usec = 0;
		itimer.it_interval.tv_sec = interval.tv_sec;
		itimer.it_interval.tv_usec = interval.tv_nsec / 1000;
		setitimer (ITIMER_REAL, &itimer, NULL);
	}

	stat->threadstarted++;

	while (!shutdown) {

		long diff;
		int sigs, ret;

		/* Wait for next period */
		switch (par->mode) {
		case MODE_CYCLIC:
		case MODE_SYS_ITIMER:
			if (sigwait(&sigset, &sigs) < 0)
				goto out;
			break;

		case MODE_CLOCK_NANOSLEEP:
			if (par->timermode == TIMER_ABSTIME) {
				ret = clock_nanosleep(par->clock, TIMER_ABSTIME,
						&next, NULL);
			} else {
				clock_gettime(par->clock, &now);
				ret = clock_nanosleep(par->clock, TIMER_RELTIME,
						&interval, NULL);
				next.tv_sec = now.tv_sec + interval.tv_sec;
				next.tv_nsec = now.tv_nsec + interval.tv_nsec;
				tsnorm(&next);
			}

			/* Avoid negative calcdiff result if clock_nanosleep() 
			 * gets interrupted.
			 */
			if (ret == EINTR)
				goto out;

			break;

		case MODE_SYS_NANOSLEEP:
			clock_gettime(par->clock, &now);
			nanosleep(&interval, NULL);
			next.tv_sec = now.tv_sec + interval.tv_sec;
			next.tv_nsec = now.tv_nsec + interval.tv_nsec;
			tsnorm(&next);
			break;
		}
		clock_gettime(par->clock, &now);

		if (use_nsecs)
			diff = calcdiff_ns(now, next);
		else
			diff = calcdiff(now, next);
		if (diff < stat->min)
			stat->min = diff;
		if (diff > stat->max)
			stat->max = diff;
		stat->avg += (double) diff;

		if (duration && (calcdiff(now, stop) >= 0))
			shutdown++;

		if (!stopped && tracelimit && (diff > tracelimit)) {
			stopped++;
			tracing(0);
			shutdown++;
		}
		stat->act = diff;
		stat->cycles++;

		if (par->bufmsk)
			stat->values[stat->cycles & par->bufmsk] = diff;

		/* When histogram limit got exceed, mark limit as exceeded,
		 * and use last bucket to recored samples of, exceeding 
		 * latency spikes.
		 */
		if (histogram && diff >= histogram) {
			histogram_limit_exceeded = 1;
			diff = histogram - 1;
		}

		if (histogram)
			stat->hist_array[diff] += 1;

		next.tv_sec += interval.tv_sec;
		next.tv_nsec += interval.tv_nsec;
		tsnorm(&next);

		if (par->max_cycles && par->max_cycles == stat->cycles)
			break;
	}

out:
	if (par->mode == MODE_CYCLIC)
		timer_delete(timer);

	if (par->mode == MODE_SYS_ITIMER) {
		itimer.it_value.tv_sec = 0;
		itimer.it_value.tv_usec = 0;
		itimer.it_interval.tv_sec = 0;
		itimer.it_interval.tv_usec = 0;
		setitimer (ITIMER_REAL, &itimer, NULL);
	}

	/* switch to normal */
	schedp.sched_priority = 0;
	sched_setscheduler(0, SCHED_OTHER, &schedp);

	stat->threadstarted = -1;

	return NULL;
}


/* Print usage information */
static void display_help(int error)
{
	char tracers[MAX_PATH];

	if (set_debugfileprefix())
		strcpy(tracers, "unavailable (debugfs not mounted)");
	else {
		fileprefix = debugfileprefix;
		if (kernvar(O_RDONLY, "available_tracers", tracers, sizeof(tracers)))
			strcpy(tracers, "none");
	}
		
	printf("cyclictest V %1.2f\n", VERSION_STRING);
	printf("Usage:\n"
	       "cyclictest <options>\n\n"
	       "-a [NUM] --affinity        run thread #N on processor #N, if possible\n"
	       "                           with NUM pin all threads to the processor NUM\n"
	       "-b USEC  --breaktrace=USEC send break trace command when latency > USEC\n"
	       "-B       --preemptirqs     both preempt and irqsoff tracing (used with -b)\n"
	       "-c CLOCK --clock=CLOCK     select clock\n"
	       "                           0 = CLOCK_MONOTONIC (default)\n"
	       "                           1 = CLOCK_REALTIME\n"
	       "-C       --context         context switch tracing (used with -b)\n"
	       "-d DIST  --distance=DIST   distance of thread intervals in us default=500\n"
	       "-D       --duration=t      specify a length for the test run\n"
	       "                           default is in seconds, but 'm', 'h', or 'd' maybe added\n"
	       "                           to modify value to minutes, hours or days\n"
	       "-E       --event           event tracing (used with -b)\n"
	       "-f       --ftrace          function trace (when -b is active)\n"
	       "-h       --histogram=US    dump a latency histogram to stdout after the run\n"
               "                           (with same priority about many threads)\n"
	       "                           US is the max time to be be tracked in microseconds\n"
	       "-i INTV  --interval=INTV   base interval of thread in us default=1000\n"
	       "-I       --irqsoff         Irqsoff tracing (used with -b)\n"
	       "-l LOOPS --loops=LOOPS     number of loops: default=0(endless)\n"
	       "-m       --mlockall        lock current and future memory allocations\n"
	       "-n       --nanosleep       use clock_nanosleep\n"
	       "-N       --nsecs           print results in ns instead of ms (default ms)\n"
	       "-o RED   --oscope=RED      oscilloscope mode, reduce verbose output by RED\n"
	       "-O TOPT  --traceopt=TOPT   trace option\n"
	       "-p PRIO  --prio=PRIO       priority of highest prio thread\n"
	       "-P       --preemptoff      Preempt off tracing (used with -b)\n"
	       "-q       --quiet           print only a summary on exit\n"
	       "-r       --relative        use relative timer instead of absolute\n"
	       "-s       --system          use sys_nanosleep and sys_setitimer\n"
	       "-t       --threads         one thread per available processor\n"
	       "-t [NUM] --threads=NUM     number of threads:\n"
	       "                           without NUM, threads = max_cpus\n"
	       "                           without -t default = 1\n"
	       "-T TRACE --tracer=TRACER   set tracing function\n"
	       "    configured tracers: %s\n"
	       "-v       --verbose         output values on stdout for statistics\n"
	       "                           format: n:c:v n=tasknum c=count v=value in us\n"
               "-w       --wakeup          task wakeup tracing (used with -b)\n"
               "-W       --wakeuprt        rt task wakeup tracing (used with -b)\n"
               "-y POLI  --policy=POLI     policy of realtime thread (1:FIFO, 2:RR)\n"
               "                           format: --policy=fifo(default) or --policy=rr\n",
	       tracers
		);
	if (error)
		exit(-1);
	exit(0);
}

static int use_nanosleep;
static int timermode = TIMER_ABSTIME;
static int use_system;
static int priority;
static int policy = 0;
static int num_threads = 1;
static int max_cycles;
static int clocksel = 0;
static int quiet;
static int interval = 1000;
static int distance = 500;
static int affinity = 0;

enum {
	AFFINITY_UNSPECIFIED,
	AFFINITY_SPECIFIED,
	AFFINITY_USEALL
};
static int setaffinity = AFFINITY_UNSPECIFIED;

static int clocksources[] = {
	CLOCK_MONOTONIC,
	CLOCK_REALTIME,
};

static void handlepolicy(char *polname)
{
	if (strncasecmp(polname, "other", 5) == 0)
		policy = SCHED_OTHER;
	else if (strncasecmp(polname, "batch", 5) == 0)
		policy = SCHED_BATCH;
	else if (strncasecmp(polname, "idle", 4) == 0)
		policy = SCHED_IDLE;
	else if (strncasecmp(polname, "fifo", 4) == 0)
		policy = SCHED_FIFO;
	else if (strncasecmp(polname, "rr", 2) == 0)
		policy = SCHED_RR;

	if (policy == SCHED_FIFO || policy == SCHED_RR) {
		if (policy == 0)
			policy = 1;
	}
	else 
		policy = 0;
}

static char *policyname(int policy)
{
	char *policystr = "";

	switch(policy) {
	case SCHED_OTHER:
		policystr = "other";
		break;
	case SCHED_FIFO:
		policystr = "fifo";
		break;
	case SCHED_RR:
		policystr = "rr";
		break;
	case SCHED_BATCH:
		policystr = "batch";
		break;
	case SCHED_IDLE:
		policystr = "idle";
		break;
	}
	return policystr;
}


/* Process commandline options */
static void process_options (int argc, char *argv[])
{
	int error = 0;
	int max_cpus = sysconf(_SC_NPROCESSORS_CONF);

	for (;;) {
		int option_index = 0;
		/** Options for getopt */
		static struct option long_options[] = {
			{"affinity", optional_argument, NULL, 'a'},
			{"breaktrace", required_argument, NULL, 'b'},
			{"preemptirqs", no_argument, NULL, 'B'},
			{"clock", required_argument, NULL, 'c'},
			{"context", no_argument, NULL, 'C'},
			{"distance", required_argument, NULL, 'd'},
			{"event", no_argument, NULL, 'E'},
			{"ftrace", no_argument, NULL, 'f'},
			{"histogram", required_argument, NULL, 'h'},
			{"interval", required_argument, NULL, 'i'},
			{"irqsoff", no_argument, NULL, 'I'},
			{"loops", required_argument, NULL, 'l'},
			{"mlockall", no_argument, NULL, 'm' },
			{"nanosleep", no_argument, NULL, 'n'},
			{"nsecs", no_argument, NULL, 'N'},
			{"oscope", required_argument, NULL, 'o'},
			{"priority", required_argument, NULL, 'p'},
                        {"policy", required_argument, NULL, 'y'},
			{"preemptoff", no_argument, NULL, 'P'},
			{"quiet", no_argument, NULL, 'q'},
			{"relative", no_argument, NULL, 'r'},
			{"system", no_argument, NULL, 's'},
			{"threads", optional_argument, NULL, 't'},
			{"verbose", no_argument, NULL, 'v'},
			{"duration",required_argument, NULL, 'D'},
                        {"wakeup", no_argument, NULL, 'w'},
                        {"wakeuprt", no_argument, NULL, 'W'},
			{"help", no_argument, NULL, '?'},
			{"tracer", required_argument, NULL, 'T'},
			{"traceopt", required_argument, NULL, 'O'},
			{NULL, 0, NULL, 0}
		};
                int c = getopt_long (argc, argv, "a::b:Bc:Cd:Efh:i:Il:nNo:O:p:Pmqrst::vD:wWTy:",
			long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'a':
			if (optarg != NULL) {
				affinity = atoi(optarg);
				setaffinity = AFFINITY_SPECIFIED;
			} else if (optind<argc && atoi(argv[optind])) {
				affinity = atoi(argv[optind]);
				setaffinity = AFFINITY_SPECIFIED;
			} else {
				setaffinity = AFFINITY_USEALL;
			}
			break;
		case 'b': tracelimit = atoi(optarg); break;
		case 'B': tracetype = IRQPREEMPTOFF; break;
		case 'c': clocksel = atoi(optarg); break;
		case 'C': tracetype = CTXTSWITCH; break;
		case 'd': distance = atoi(optarg); break;
		case 'E': tracetype = EVENTS; break;
		case 'f': ftrace = 1; break;
		case 'h': histogram = atoi(optarg); break;
		case 'i': interval = atoi(optarg); break;
		case 'I': tracetype = IRQSOFF; break;
		case 'l': max_cycles = atoi(optarg); break;
		case 'n': use_nanosleep = MODE_CLOCK_NANOSLEEP; break;
		case 'N': use_nsecs = 1; break;
		case 'o': oscope_reduction = atoi(optarg); break;
		case 'O': traceopt(optarg); break;
		case 'p': 
			priority = atoi(optarg); 
			if (policy != SCHED_FIFO && policy != SCHED_RR)
				policy = SCHED_FIFO;
			break;
		case 'P': tracetype = PREEMPTOFF; break;
		case 'q': quiet = 1; break;
		case 'r': timermode = TIMER_RELTIME; break;
		case 's': use_system = MODE_SYS_OFFSET; break;
		case 't':
			if (optarg != NULL)
				num_threads = atoi(optarg);
			else if (optind<argc && atoi(argv[optind]))
				num_threads = atoi(argv[optind]);
			else
				num_threads = max_cpus;
			break;
		case 'T': strncpy(tracer, optarg, sizeof(tracer)); break;
		case 'v': verbose = 1; break;
		case 'm': lockall = 1; break;
		case 'D': duration = parse_time_string(optarg);
			break;
                case 'w': tracetype = WAKEUP; break;
                case 'W': tracetype = WAKEUPRT; break;
                case 'y': handlepolicy(optarg); break;
		case '?': display_help(0); break;
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
	} else if (tracelimit)
		fileprefix = procfileprefix;

	if (clocksel < 0 || clocksel > ARRAY_SIZE(clocksources))
		error = 1;

	if (oscope_reduction < 1)
		error = 1;

	if (oscope_reduction > 1 && !verbose) {
		fprintf(stderr, "ERROR: -o option only meaningful, if verbose\n");
		error = 1;
	}

	if (histogram < 0)
		error = 1;

	if (histogram > HIST_MAX)
		histogram = HIST_MAX;

	if (priority < 0 || priority > 99)
		error = 1;

	if (priority && (policy != SCHED_FIFO && policy != SCHED_RR)) {
		fprintf(stderr, "policy and priority don't match: setting policy to SCHED_FIFO\n");
		policy = SCHED_FIFO;
	}

	if ((policy == SCHED_FIFO || policy == SCHED_RR) && priority == 0) {
		fprintf(stderr, "defaulting realtime priority to %d\n", 
			num_threads+1);
		priority = num_threads+1;
	}

	if (num_threads < 1)
		error = 1;


	if (error)
		display_help(1);
}

static int check_kernel(void)
{
	struct utsname kname;
	int maj, min, sub, kv, ret;

	ret = uname(&kname);
	if (ret) {
		fprintf(stderr, "uname failed: %s. Assuming not 2.6\n",
				strerror(errno));
		return KV_NOT_26;
	}
	sscanf(kname.release, "%d.%d.%d", &maj, &min, &sub);
	if (maj == 2 && min == 6) {
		if (sub < 18)
			kv = KV_26_LT18;
		else if (sub < 24)
			kv = KV_26_LT24;
		else if (sub < 28) {
			kv = KV_26_CURR;
			strcpy(functiontracer, "ftrace");
			strcpy(traceroptions, "iter_ctrl");
		} else {
			kv = KV_26_CURR;
			strcpy(functiontracer, "function");
			strcpy(traceroptions, "trace_options");
		}
	} else
		kv = KV_NOT_26;

	return kv;
}

static int check_timer(void)
{
	struct timespec ts;

	if (clock_getres(CLOCK_MONOTONIC, &ts))
		return 1;

	return (ts.tv_sec != 0 || ts.tv_nsec != 1);
}

static void sighand(int sig)
{
	shutdown = 1;
	if (tracelimit)
		tracing(0);
}

static void print_hist(struct thread_param *par, int nthreads)
{
	int i, j;
	unsigned long long log_entries[nthreads];
	unsigned long max_latency = 0;

	bzero(log_entries, sizeof(log_entries));

	printf("# Histogram\n");
	for (i = 0; i < histogram; i++) {

		printf("%06d ", i);

		for (j = 0; j < nthreads; j++) {
			unsigned long curr_latency=par[j].stats->hist_array[i];
			printf("%06lu\t", curr_latency);
			log_entries[j] += curr_latency;
			if (curr_latency && max_latency < i)
				max_latency = i;
		}
		printf("\n");
	}
	printf("# Total:");
	for (j = 0; j < nthreads; j++)
		printf(" %09llu", log_entries[j]);
	printf("\n");
	printf("# Max Latency: %lu / %d\n", max_latency, histogram);
}

static void print_stat(struct thread_param *par, int index, int verbose)
{
	struct thread_stat *stat = par->stats;

	if (!verbose) {
		if (quiet != 1) {
			char *fmt;
			if (use_nsecs)
                                fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
					"Min:%7ld Act:%8ld Avg:%8ld Max:%8ld\n";
			else
                                fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
					"Min:%7ld Act:%5ld Avg:%5ld Max:%8ld\n";
                        printf(fmt, index, stat->tid, par->prio, 
                               par->interval, stat->cycles, stat->min, stat->act,
			       stat->cycles ?
			       (long)(stat->avg/stat->cycles) : 0, stat->max);
		}
	} else {
		while (stat->cycles != stat->cyclesread) {
			long diff = stat->values
			    [stat->cyclesread & par->bufmsk];

			if (diff > stat->redmax) {
				stat->redmax = diff;
				stat->cycleofmax = stat->cyclesread;
			}
			if (++stat->reduce == oscope_reduction) {
				printf("%8d:%8lu:%8ld\n", index,
				       stat->cycleofmax, stat->redmax);
				stat->reduce = 0;
				stat->redmax = 0;
			}
			stat->cyclesread++;
		}
	}
}

static int
check_privs(void)
{
	int policy = sched_getscheduler(0);
	struct sched_param param;

	/* if we're already running a realtime scheduler
	 * then we *should* be able to change things later
	 */
	if (policy == SCHED_FIFO || policy == SCHED_RR)
		return 0;

	/* try to change to SCHED_FIFO */
	param.sched_priority = 1;
	if (sched_setscheduler(0, SCHED_FIFO, &param)) {
		fprintf(stderr, "Unable to change scheduling policy!\n");
		fprintf(stderr, "either run as root or join realtime group\n");
		return 1;
	}

	/* we're good; change back and return success */
	param.sched_priority = 0;
	sched_setscheduler(0, policy, &param);
	return 0;
}

int main(int argc, char **argv)
{
	sigset_t sigset;
	int signum = SIGALRM;
	int mode;
	struct thread_param *par;
	struct thread_stat *stat;
	int max_cpus = sysconf(_SC_NPROCESSORS_CONF);
	int i, ret = -1;

	process_options(argc, argv);

	if (check_privs())
		exit(-1);

	/* lock all memory (prevent paging) */
	if (lockall)
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			perror("mlockall");
			goto out;
		}

	kernelversion = check_kernel();

	if (kernelversion == KV_NOT_26)
		fprintf(stderr, "WARNING: Most functions require kernel 2.6\n");

	setup_tracer();

	if (check_timer())
		fprintf(stderr, "WARNING: High resolution timers not available\n");

	mode = use_nanosleep + use_system;

	sigemptyset(&sigset);
	sigaddset(&sigset, signum);
	sigprocmask (SIG_BLOCK, &sigset, NULL);

	signal(SIGINT, sighand);
	signal(SIGTERM, sighand);

	par = calloc(num_threads, sizeof(struct thread_param));
	if (!par)
		goto out;
	stat = calloc(num_threads, sizeof(struct thread_stat));
	if (!stat)
		goto outpar;

	for (i = 0; i < num_threads; i++) {
		if (histogram) {
			stat[i].hist_array = calloc(histogram, sizeof(long));
			if (!stat[i].hist_array) {
				fprintf(stderr, "Cannot allocate enough memory for histogram limit %d: %s",
						histogram, strerror(errno));
				exit(EXIT_FAILURE);
			}
		}

		if (verbose) {
			stat[i].values = calloc(VALBUF_SIZE, sizeof(long));
			if (!stat[i].values)
				goto outall;
			par[i].bufmsk = VALBUF_SIZE - 1;
		}

		par[i].prio = priority;
		if (priority && !histogram)
			priority--;
                if      (priority && policy <= 1) par[i].policy = SCHED_FIFO;
                else if (priority && policy == 2) par[i].policy = SCHED_RR;
                else                              par[i].policy = SCHED_OTHER;
		par[i].clock = clocksources[clocksel];
		par[i].mode = mode;
		par[i].timermode = timermode;
		par[i].signal = signum;
		par[i].interval = interval;
		if (!histogram) /* histogram requires same interval on CPUs*/
			interval += distance;
		if (verbose)
			printf("Thread %d Interval: %d\n", i, interval);
		par[i].max_cycles = max_cycles;
		par[i].stats = &stat[i];
		switch (setaffinity) {
		case AFFINITY_UNSPECIFIED: par[i].cpu = -1; break;
		case AFFINITY_SPECIFIED: par[i].cpu = affinity; break;
		case AFFINITY_USEALL: par[i].cpu = i % max_cpus; break;
		}
		stat[i].min = 1000000;
		stat[i].max = -1000000;
		stat[i].avg = 0.0;
		stat[i].threadstarted = 1;
		pthread_create(&stat[i].thread, NULL, timerthread, &par[i]);
	}

	while (!shutdown) {
		char lavg[256];
		int fd, len, allstopped = 0;
		char *policystr = NULL;

		if (!policystr)
			policystr = policyname(policy);

		if (!verbose && !quiet) {
			fd = open("/proc/loadavg", O_RDONLY, 0666);
			len = read(fd, &lavg, 255);
			close(fd);
			lavg[len-1] = 0x0;
			printf("policy: %s: loadavg: %s          \n\n", 
			       policystr, lavg);
		}

		for (i = 0; i < num_threads; i++) {

			print_stat(&par[i], i, verbose);
			if(max_cycles && stat[i].cycles >= max_cycles)
				allstopped++;
		}

		usleep(10000);
		if (shutdown || allstopped)
			break;
		if (!verbose && !quiet)
			printf("\033[%dA", num_threads + 2);
	}
	ret = EXIT_SUCCESS;

 outall:
	shutdown = 1;
	usleep(50000);

	if (quiet)
		quiet = 2;
	for (i = 0; i < num_threads; i++) {
		if (stat[i].threadstarted > 0)
			pthread_kill(stat[i].thread, SIGTERM);
		if (stat[i].threadstarted) {
			pthread_join(stat[i].thread, NULL);
			if (quiet && !histogram)
				print_stat(&par[i], i, 0);
		}
		if (stat[i].values)
			free(stat[i].values);
	}

	if (histogram) {
		print_hist(par, num_threads);
		for (i = 0; i < num_threads; i++)
			free (stat[i].hist_array);
	}

	free(stat);
 outpar:
	free(par);
 out:
	/* ensure that the tracer is stopped */
	if (tracelimit)
		tracing(0);

	/* unlock everything */
	if (lockall)
		munlockall();

	/* Be a nice program, cleanup */
	if (kernelversion != KV_26_CURR)
		restorekernvars();

	if (histogram && histogram_limit_exceeded) {
		ret = EXIT_FAILURE;
		fprintf(stderr, "ERROR: Histogram limit got exceeded at least once!\n"
				"Limit exceeding got sampled in last bucket.\n");

	}

	exit(ret);
}
