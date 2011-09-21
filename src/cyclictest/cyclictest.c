/*
 * High resolution timer test software
 *
 * (C) 2008-2011 Clark Williams <williams@redhat.com>
 * (C) 2005-2007 Thomas Gleixner <tglx@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version
 * 2 as published by the Free Software Foundation.
 *
 */

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
#include <limits.h>
#include <linux/unistd.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include "rt_numa.h"

#include "rt-utils.h"

#define DEFAULT_INTERVAL 1000
#define DEFAULT_DISTANCE 500

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

int sched_setaffinity (__pid_t __pid, size_t __cpusetsize,
                              __const cpu_set_t *__cpuset)
{
	return -EINVAL;
}

#undef CPU_SET
#undef CPU_ZERO
#define CPU_SET(cpu, cpusetp)
#define CPU_ZERO(cpusetp)

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

int enable_events;

enum {
	NOTRACE,
	CTXTSWITCH,
	IRQSOFF,
	PREEMPTOFF,
	PREEMPTIRQSOFF,
	WAKEUP,
	WAKEUPRT,
	LATENCY,
	FUNCTION,
	CUSTOM,
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
	int node;
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
	long hist_overflow;
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
static int histofall = 0;
static int duration = 0;
static int use_nsecs = 0;
static int refresh_on_max;
static int force_sched_other;

static pthread_cond_t refresh_on_max_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t refresh_on_max_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t break_thread_id_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t break_thread_id = 0;
static uint64_t break_thread_value = 0;

/* Backup of kernel variables that we modify */
static struct kvars {
	char name[KVARNAMELEN];
	char value[KVALUELEN];
} kv[KVARS];

static char *procfileprefix = "/proc/sys/kernel/";
static char *fileprefix;
static char tracer[MAX_PATH];
static char **traceptr;
static int traceopt_count;
static int traceopt_size;

static int latency_target_fd = -1;
static int32_t latency_target_value = 0;

/* Latency trick
 * if the file /dev/cpu_dma_latency exists,
 * open it and write a zero into it. This will tell 
 * the power management system not to transition to 
 * a high cstate (in fact, the system acts like idle=poll)
 * When the fd to /dev/cpu_dma_latency is closed, the behavior
 * goes back to the system default.
 * 
 * Documentation/power/pm_qos_interface.txt
 */
static void set_latency_target(void)
{
	struct stat s;
	int ret;

	if (stat("/dev/cpu_dma_latency", &s) == 0) {
		latency_target_fd = open("/dev/cpu_dma_latency", O_RDWR);
		if (latency_target_fd == -1)
			return;
		ret = write(latency_target_fd, &latency_target_value, 4);
		if (ret == 0) {
			printf("# error setting cpu_dma_latency to %d!: %s\n", latency_target_value, strerror(errno));
			close(latency_target_fd);
			return;
		}
		printf("# /dev/cpu_dma_latency set to %dus\n", latency_target_value);
	}
}


enum kernelversion {
	KV_NOT_SUPPORTED,
	KV_26_LT18,
	KV_26_LT24,
	KV_26_33,
	KV_30
};

enum {
	ERROR_GENERAL	= -1,
	ERROR_NOTFOUND	= -2,
};

static char functiontracer[MAX_PATH];
static char traceroptions[MAX_PATH];

static int trace_fd     = -1;

static int kernvar(int mode, const char *name, char *value, size_t sizeofvalue)
{
	char filename[128];
	int retval = 1;
	int path;
	size_t len_prefix = strlen(fileprefix), len_name = strlen(name);

	if (len_prefix + len_name + 1 > sizeof(filename)) {
		errno = ENOMEM;
		return 1;
	}

	memcpy(filename, fileprefix, len_prefix);
	memcpy(filename + len_prefix, name, len_name + 1);

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

	if (kernelversion < KV_26_33) {
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

static inline int64_t calcdiff(struct timespec t1, struct timespec t2)
{
	int64_t diff;
	diff = USEC_PER_SEC * (long long)((int) t1.tv_sec - (int) t2.tv_sec);
	diff += ((int) t1.tv_nsec - (int) t2.tv_nsec) / 1000;
	return diff;
}

static inline int64_t calcdiff_ns(struct timespec t1, struct timespec t2)
{
	int64_t diff;
	diff = NSEC_PER_SEC * (int64_t)((int) t1.tv_sec - (int) t2.tv_sec);
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
		if (traceptr == NULL)
			fatal ("Error allocating space for %d trace options\n",
			       traceopt_count+1);
	}
	ptr = malloc(strlen(option)+1);
	if (ptr == NULL)
		fatal("error allocating space for trace option %s\n", option);
	printf("adding traceopt %s\n", option);
	strcpy(ptr, option);
	traceptr[traceopt_count++] = ptr;
}

static int trace_file_exists(char *name)
{
	struct stat sbuf;
	char *tracing_prefix = get_debugfileprefix();
	char path[MAX_PATH];
	strcat(strcpy(path, tracing_prefix), name);
	return stat(path, &sbuf) ? 0 : 1;
}

void tracing(int on)
{
	if (on) {
		switch (kernelversion) {
		case KV_26_LT18: gettimeofday(0,(struct timezone *)1); break;
		case KV_26_LT24: prctl(0, 1); break;
		case KV_26_33: 
		case KV_30:
			write(trace_fd, "1", 1);
			break;
		default:	 break;
		}
	} else {
		switch (kernelversion) {
		case KV_26_LT18: gettimeofday(0,0); break;
		case KV_26_LT24: prctl(0, 0); break;
		case KV_26_33: 
		case KV_30:
			write(trace_fd, "0", 1);
			break;
		default:	break;
		}
	}
}

static int settracer(char *tracer)
{
	if (valid_tracer(tracer)) {
		setkernvar("current_tracer", tracer);
		return 0;
	}
	return -1;
}

static void setup_tracer(void)
{
	if (!tracelimit)
		return;

	if (mount_debugfs(NULL))
		fatal("could not mount debugfs");

	if (kernelversion >= KV_26_33) {
		char testname[MAX_PATH];

		fileprefix = get_debugfileprefix();
		if (!trace_file_exists("tracing_enabled") &&
		    !trace_file_exists("tracing_on"))
			warn("tracing_enabled or tracing_on not found\n"
			    "debug fs not mounted, "
			    "TRACERs not configured?\n", testname);
	} else
		fileprefix = procfileprefix;

	if (kernelversion >= KV_26_33) {
		int ret;

		if (trace_file_exists("tracing_enabled") &&
		    !trace_file_exists("tracing_on"))
			setkernvar("tracing_enabled", "1");

		/* ftrace_enabled is a sysctl variable */
		/* turn it on if you're doing anything but nop or event tracing */

		fileprefix = procfileprefix;
		if (tracetype)
			setkernvar("ftrace_enabled", "1");
		else
			setkernvar("ftrace_enabled", "0");
		fileprefix = get_debugfileprefix();

		/*
		 * Set default tracer to nop.
		 * this also has the nice side effect of clearing out
		 * old traces.
		 */
		ret = settracer("nop");

		switch (tracetype) {
		case NOTRACE:
			/* no tracer specified, use events */
			enable_events = 1;
			break;
		case FUNCTION:
			ret = settracer("function");
			break;
		case IRQSOFF:
			ret = settracer("irqsoff");
			break;
		case PREEMPTOFF:
			ret = settracer("preemptoff");
			break;
		case PREEMPTIRQSOFF:
			ret = settracer("preemptirqsoff");
			break;
		case CTXTSWITCH:
			if (valid_tracer("sched_switch"))
			    ret = settracer("sched_switch");
			else {
				if ((ret = event_enable("sched/sched_wakeup")))
					break;
				ret = event_enable("sched/sched_switch");
			}
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

		if (enable_events)
			/* turn on all events */
			event_enable_all();

		if (ret)
			fprintf(stderr, "Requested tracer '%s' not available\n", tracer);

		setkernvar(traceroptions, "print-parent");
		setkernvar(traceroptions, "latency-format");
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
		if (trace_file_exists("latency_hist"))
			setkernvar("latency_hist/wakeup/reset", "1");

		/* open the tracing on file descriptor */
		if (trace_fd == -1) {
			char path[MAX_PATH];
			strcpy(path, fileprefix);
			if (trace_file_exists("tracing_on"))
				strcat(path, "tracing_on");
			else
				strcat(path, "tracing_enabled");
			if ((trace_fd = open(path, O_WRONLY)) == -1)
				fatal("unable to open %s for tracing", path);
		}

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
		setkernvar("latency_hist/wakeup_latency/reset", "1");
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

	/* if we're running in numa mode, set our memory node */
	if (par->node != -1)
		rt_numa_set_numa_run_on_node(par->node, par->cpu);

	if (par->cpu != -1) {
		CPU_ZERO(&mask);
		CPU_SET(par->cpu, &mask);
		if(sched_setaffinity(0, sizeof(mask), &mask) == -1)
			warn("Could not set CPU affinity to CPU #%d\n", par->cpu);
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
	next.tv_sec += interval.tv_sec;
	next.tv_nsec += interval.tv_nsec;
	tsnorm(&next);

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

		uint64_t diff;
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
		if (diff > stat->max) {
			stat->max = diff;
			if (refresh_on_max)
				pthread_cond_signal(&refresh_on_max_cond);
		}
		stat->avg += (double) diff;

		if (duration && (calcdiff(now, stop) >= 0))
			shutdown++;

		if (!stopped && tracelimit && (diff > tracelimit)) {
			stopped++;
			tracing(0);
			shutdown++;
			pthread_mutex_lock(&break_thread_id_lock);
			if (break_thread_id == 0)
				break_thread_id = stat->tid;
			break_thread_value = diff;
			pthread_mutex_unlock(&break_thread_id_lock);
		}
		stat->act = diff;
		stat->cycles++;

		if (par->bufmsk)
			stat->values[stat->cycles & par->bufmsk] = diff;

		/* Update the histogram */
		if (histogram) {
			if (diff >= histogram)
				stat->hist_overflow++;
			else
				stat->hist_array[diff]++;
		}

		next.tv_sec += interval.tv_sec;
		next.tv_nsec += interval.tv_nsec;
		if (par->mode == MODE_CYCLIC) {
			int overrun_count = timer_getoverrun(timer);
			next.tv_sec += overrun_count * interval.tv_sec;
			next.tv_nsec += overrun_count * interval.tv_nsec;
		}
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
	char *prefix;

	prefix = get_debugfileprefix();
	if (prefix[0] == '\0')
		strcpy(tracers, "unavailable (debugfs not mounted)");
	else {
		fileprefix = prefix;
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
	       "-H       --histofall=US    same as -h except with an additional summary column\n"
	       "-i INTV  --interval=INTV   base interval of thread in us default=1000\n"
	       "-I       --irqsoff         Irqsoff tracing (used with -b)\n"
	       "-l LOOPS --loops=LOOPS     number of loops: default=0(endless)\n"
	       "-m       --mlockall        lock current and future memory allocations\n"
	       "-M       --refresh_on_max  delay updating the screen until a new max latency is hit\n" 
	       "-n       --nanosleep       use clock_nanosleep\n"
	       "-N       --nsecs           print results in ns instead of us (default us)\n"
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
	       "-u       --unbuffered      force unbuffered output for live processing\n"
	       "-v       --verbose         output values on stdout for statistics\n"
	       "                           format: n:c:v n=tasknum c=count v=value in us\n"
               "-w       --wakeup          task wakeup tracing (used with -b)\n"
               "-W       --wakeuprt        rt task wakeup tracing (used with -b)\n"
               "-y POLI  --policy=POLI     policy of realtime thread, POLI may be fifo(default) or rr\n"
               "                           format: --policy=fifo(default) or --policy=rr\n"
	       "-S       --smp             Standard SMP testing: options -a -t -n and\n"
               "                           same priority of all threads\n"
	       "-U       --numa            Standard NUMA testing (similar to SMP option)\n"
               "                           thread data structures allocated from local node\n",
	       tracers
		);
	if (error)
		exit(EXIT_FAILURE);
	exit(EXIT_SUCCESS);
}

static int use_nanosleep;
static int timermode = TIMER_ABSTIME;
static int use_system;
static int priority;
static int policy = SCHED_OTHER;	/* default policy if not specified */
static int num_threads = 1;
static int max_cycles;
static int clocksel = 0;
static int quiet;
static int interval = DEFAULT_INTERVAL;
static int distance = -1;
static int affinity = 0;
static int smp = 0;

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
	else	/* default policy if we don't recognize the request */
		policy = SCHED_OTHER;
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
			{"histofall", required_argument, NULL, 'H'},
			{"interval", required_argument, NULL, 'i'},
			{"irqsoff", no_argument, NULL, 'I'},
			{"loops", required_argument, NULL, 'l'},
			{"mlockall", no_argument, NULL, 'm' },
			{"refresh_on_max", no_argument, NULL, 'M' },
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
			{"unbuffered", no_argument, NULL, 'u'},
			{"verbose", no_argument, NULL, 'v'},
			{"duration",required_argument, NULL, 'D'},
                        {"wakeup", no_argument, NULL, 'w'},
                        {"wakeuprt", no_argument, NULL, 'W'},
			{"help", no_argument, NULL, '?'},
			{"tracer", required_argument, NULL, 'T'},
			{"traceopt", required_argument, NULL, 'O'},
			{"smp", no_argument, NULL, 'S'},
			{"numa", no_argument, NULL, 'U'},
			{"latency", required_argument, NULL, 'e'},
			{NULL, 0, NULL, 0}
		};
		int c = getopt_long(argc, argv, "a::b:Bc:Cd:Efh:H:i:Il:MnNo:O:p:PmqrsSt::uUvD:wWT:y:e:",
				    long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'a':
			if (smp) {
				warn("-a ignored due to --smp\n");
				break;
			}
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
		case 'B': tracetype = PREEMPTIRQSOFF; break;
		case 'c': clocksel = atoi(optarg); break;
		case 'C': tracetype = CTXTSWITCH; break;
		case 'd': distance = atoi(optarg); break;
		case 'E': enable_events = 1; break;
		case 'f': tracetype = FUNCTION; ftrace = 1; break;
		case 'H': histofall = 1; /* fall through */
		case 'h': histogram = atoi(optarg); break;
		case 'i': interval = atoi(optarg); break;
		case 'I':
			if (tracetype == PREEMPTOFF) {
				tracetype = PREEMPTIRQSOFF;
				strncpy(tracer, "preemptirqsoff", sizeof(tracer));
			} else {
				tracetype = IRQSOFF;
				strncpy(tracer, "irqsoff", sizeof(tracer));
			}
			break;
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
		case 'P':
			if (tracetype == IRQSOFF) {
				tracetype = PREEMPTIRQSOFF;
				strncpy(tracer, "preemptirqsoff", sizeof(tracer));
			} else {
				tracetype = PREEMPTOFF;
				strncpy(tracer, "preemptoff", sizeof(tracer));
			}
			break;
		case 'q': quiet = 1; break;
		case 'r': timermode = TIMER_RELTIME; break;
		case 's': use_system = MODE_SYS_OFFSET; break;
		case 't':
			if (smp) {
				warn("-t ignored due to --smp\n");
				break;
			}
			if (optarg != NULL)
				num_threads = atoi(optarg);
			else if (optind<argc && atoi(argv[optind]))
				num_threads = atoi(argv[optind]);
			else
				num_threads = max_cpus;
			break;
		case 'T': 
			tracetype = CUSTOM;
			strncpy(tracer, optarg, sizeof(tracer)); 
			break;
		case 'u': setvbuf(stdout, NULL, _IONBF, 0); break;
		case 'v': verbose = 1; break;
		case 'm': lockall = 1; break;
		case 'M': refresh_on_max = 1; break;
		case 'D': duration = parse_time_string(optarg);
			break;
                case 'w': tracetype = WAKEUP; break;
                case 'W': tracetype = WAKEUPRT; break;
                case 'y': handlepolicy(optarg); break;
		case 'S':  /* SMP testing */
			if (numa)
				fatal("numa and smp options are mutually exclusive\n");
			smp = 1;
			num_threads = max_cpus;
			setaffinity = AFFINITY_USEALL;
			use_nanosleep = MODE_CLOCK_NANOSLEEP;
			break;
		case 'U':  /* NUMA testing */
			if (smp)
				fatal("numa and smp options are mutually exclusive\n");
#ifdef NUMA
			numa = 1;
			num_threads = max_cpus;
			setaffinity = AFFINITY_USEALL;
			use_nanosleep = MODE_CLOCK_NANOSLEEP;
#else
			warn("cyclictest was not built with the numa option\n");
			warn("ignoring --numa or -U\n");
#endif
			break;
		case 'e': /* power management latency target value */
			  /* note: default is 0 (zero) */
			latency_target_value = atoi(optarg);
			if (latency_target_value < 0)
				latency_target_value = 0;
			break;

		case '?': display_help(0); break;
		}
	}

	if (setaffinity == AFFINITY_SPECIFIED) {
		if (affinity < 0)
			error = 1;
		if (affinity >= max_cpus) {
			warn("CPU #%d not found, only %d CPUs available\n",
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
		warn("-o option only meaningful, if verbose\n");
		error = 1;
	}

	if (histogram < 0)
		error = 1;

	if (histogram > HIST_MAX)
		histogram = HIST_MAX;

	if (histogram && distance != -1)
		warn("distance is ignored and set to 0, if histogram enabled\n");
	if (distance == -1)
		distance = DEFAULT_DISTANCE;

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
		return KV_NOT_SUPPORTED;
	}
	sscanf(kname.release, "%d.%d.%d", &maj, &min, &sub);
	if (maj == 2 && min == 6) {
		if (sub < 18)
			kv = KV_26_LT18;
		else if (sub < 24)
			kv = KV_26_LT24;
		else if (sub < 28) {
			kv = KV_26_33;
			strcpy(functiontracer, "ftrace");
			strcpy(traceroptions, "iter_ctrl");
		} else {
			kv = KV_26_33;
			strcpy(functiontracer, "function");
			strcpy(traceroptions, "trace_options");
		}
	} else if (maj == 3) {
		kv = KV_30;
		strcpy(functiontracer, "function");
		strcpy(traceroptions, "trace_options");
		
	} else
		kv = KV_NOT_SUPPORTED;

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
	if (refresh_on_max)
		pthread_cond_signal(&refresh_on_max_cond);
	if (tracelimit)
		tracing(0);
}

static void print_tids(struct thread_param *par[], int nthreads)
{
	int i;

	printf("# Thread Ids:");
	for (i = 0; i < nthreads; i++)
		printf(" %05d", par[i]->stats->tid);
	printf("\n");
}

static void print_hist(struct thread_param *par[], int nthreads)
{
	int i, j;
	unsigned long long int log_entries[nthreads+1];
	unsigned long maxmax, alloverflows;

	bzero(log_entries, sizeof(log_entries));

	printf("# Histogram\n");
	for (i = 0; i < histogram; i++) {
		unsigned long long int allthreads = 0;

		printf("%06d ", i);

		for (j = 0; j < nthreads; j++) {
			unsigned long curr_latency=par[j]->stats->hist_array[i];
			printf("%06lu", curr_latency);
			if (j < nthreads - 1)
				printf("\t");
			log_entries[j] += curr_latency;
			allthreads += curr_latency;
		}
		if (histofall && nthreads > 1) {
			printf("\t%06llu", allthreads);
			log_entries[nthreads] += allthreads;
		}
		printf("\n");
	}
	printf("# Total:");
	for (j = 0; j < nthreads; j++)
		printf(" %09llu", log_entries[j]);
	if (histofall && nthreads > 1)
		printf(" %09llu", log_entries[nthreads]);
	printf("\n");
	printf("# Min Latencies:");
	for (j = 0; j < nthreads; j++)
		printf(" %05lu", par[j]->stats->min);
	printf("\n");
	printf("# Avg Latencies:");
	for (j = 0; j < nthreads; j++)
		printf(" %05lu", par[j]->stats->cycles ?
		       (long)(par[j]->stats->avg/par[j]->stats->cycles) : 0);
	printf("\n");
	printf("# Max Latencies:");
	maxmax = 0;
	for (j = 0; j < nthreads; j++) {
 		printf(" %05lu", par[j]->stats->max);
		if (par[j]->stats->max > maxmax)
			maxmax = par[j]->stats->max;
	}
	if (histofall && nthreads > 1)
		printf(" %05lu", maxmax);
	printf("\n");
	printf("# Histogram Overflows:");
	alloverflows = 0;
	for (j = 0; j < nthreads; j++) {
 		printf(" %05lu", par[j]->stats->hist_overflow);
		alloverflows += par[j]->stats->hist_overflow;
	}
	if (histofall && nthreads > 1)
		printf(" %05lu", alloverflows);
	printf("\n");
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

int main(int argc, char **argv)
{
	sigset_t sigset;
	int signum = SIGALRM;
	int mode;
	struct thread_param **parameters;
	struct thread_stat **statistics;
	int max_cpus = sysconf(_SC_NPROCESSORS_CONF);
	int i, ret = -1;
	int status;

	process_options(argc, argv);

	if (check_privs())
		exit(EXIT_FAILURE);

	/* Checks if numa is on, program exits if numa on but not available */
	numa_on_and_available();

	/* lock all memory (prevent swapping) */
	if (lockall)
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			perror("mlockall");
			goto out;
		}

	/* use the /dev/cpu_dma_latency trick if it's there */
	set_latency_target();

	kernelversion = check_kernel();

	if (kernelversion == KV_NOT_SUPPORTED)
		warn("Running on unknown kernel version...YMMV\n");

	setup_tracer();

	if (check_timer())
		warn("High resolution timers not available\n");

	mode = use_nanosleep + use_system;

	sigemptyset(&sigset);
	sigaddset(&sigset, signum);
	sigprocmask (SIG_BLOCK, &sigset, NULL);

	signal(SIGINT, sighand);
	signal(SIGTERM, sighand);

	parameters = calloc(num_threads, sizeof(struct thread_param *));
	if (!parameters)
		goto out;
	statistics = calloc(num_threads, sizeof(struct thread_stat *));
	if (!statistics)
		goto outpar;

	for (i = 0; i < num_threads; i++) {
		pthread_attr_t attr;
		int node;
		struct thread_param *par;
		struct thread_stat *stat;

		status = pthread_attr_init(&attr);
		if (status != 0)
			fatal("error from pthread_attr_init for thread %d: %s\n", i, strerror(status));

		node = -1;
		if (numa) {
			void *stack;
			void *currstk;
			size_t stksize;

			/* find the memory node associated with the cpu i */
			node = rt_numa_numa_node_of_cpu(i);

			/* get the stack size set for for this thread */
			if (pthread_attr_getstack(&attr, &currstk, &stksize))
				fatal("failed to get stack size for thread %d\n", i);

			/* if the stack size is zero, set a default */
			if (stksize == 0)
				stksize = PTHREAD_STACK_MIN * 2;

			/*  allocate memory for a stack on appropriate node */
			stack = rt_numa_numa_alloc_onnode(stksize, node, i);

			/* set the thread's stack */
			if (pthread_attr_setstack(&attr, stack, stksize))
				fatal("failed to set stack addr for thread %d to 0x%x\n",
				      i, stack+stksize);
		}

		/* allocate the thread's parameter block  */
		parameters[i] = par = threadalloc(sizeof(struct thread_param), node);
		if (par == NULL)
			fatal("error allocating thread_param struct for thread %d\n", i);
		memset(par, 0, sizeof(struct thread_param));

		/* allocate the thread's statistics block */
		statistics[i] = stat = threadalloc(sizeof(struct thread_stat), node);
		if (stat == NULL)
			fatal("error allocating thread status struct for thread %d\n", i);
		memset(stat, 0, sizeof(struct thread_stat));

		/* allocate the histogram if requested */
		if (histogram) {
			int bufsize = histogram * sizeof(long);

			stat->hist_array = threadalloc(bufsize, node);
			if (stat->hist_array == NULL)
				fatal("failed to allocate histogram of size %d on node %d\n",
				      histogram, i);
			memset(stat->hist_array, 0, bufsize);
		}

		if (verbose) {
			int bufsize = VALBUF_SIZE * sizeof(long);
			stat->values = threadalloc(bufsize, node);
			if (!stat->values)
				goto outall;
			memset(stat->values, 0, bufsize);
			par->bufmsk = VALBUF_SIZE - 1;
		}

		par->prio = priority;
                if (priority && (policy == SCHED_FIFO || policy == SCHED_RR))
			par->policy = policy;
                else {
			par->policy = SCHED_OTHER;
			force_sched_other = 1;
		}
		if (priority && !histogram && !smp && !numa)
			priority--;
		par->clock = clocksources[clocksel];
		par->mode = mode;
		par->timermode = timermode;
		par->signal = signum;
		par->interval = interval;
		if (!histogram) /* same interval on CPUs */
			interval += distance;
		if (verbose)
			printf("Thread %d Interval: %d\n", i, interval);
		par->max_cycles = max_cycles;
		par->stats = stat;
		par->node = node;
		switch (setaffinity) {
		case AFFINITY_UNSPECIFIED: par->cpu = -1; break;
		case AFFINITY_SPECIFIED: par->cpu = affinity; break;
		case AFFINITY_USEALL: par->cpu = i % max_cpus; break;
		}
		stat->min = 1000000;
		stat->max = 0;
		stat->avg = 0.0;
		stat->threadstarted = 1;
		status = pthread_create(&stat->thread, &attr, timerthread, par);
		if (status)
			fatal("failed to create thread %d: %s\n", i, strerror(status));

	}

	while (!shutdown) {
		char lavg[256];
		int fd, len, allstopped = 0;
		static char *policystr = NULL;
		static char *slash = NULL;
		static char *policystr2;

		if (!policystr)
			policystr = policyname(policy);

		if (!slash) {
			if (force_sched_other) {
				slash = "/";
				policystr2 = policyname(SCHED_OTHER);
			} else
				slash = policystr2 = "";
		}
		if (!verbose && !quiet) {
			fd = open("/proc/loadavg", O_RDONLY, 0666);
			len = read(fd, &lavg, 255);
			close(fd);
			lavg[len-1] = 0x0;
			printf("policy: %s%s%s: loadavg: %s          \n\n",
			       policystr, slash, policystr2, lavg);
		}

		for (i = 0; i < num_threads; i++) {

			print_stat(parameters[i], i, verbose);
			if(max_cycles && statistics[i]->cycles >= max_cycles)
				allstopped++;
		}

		usleep(10000);
		if (shutdown || allstopped)
			break;
		if (!verbose && !quiet)
			printf("\033[%dA", num_threads + 2);

		if (refresh_on_max) {
			pthread_mutex_lock(&refresh_on_max_lock);
			pthread_cond_wait(&refresh_on_max_cond,
					  &refresh_on_max_lock);
			pthread_mutex_unlock(&refresh_on_max_lock);
		}
	}
	ret = EXIT_SUCCESS;

 outall:
	shutdown = 1;
	usleep(50000);

	if (quiet)
		quiet = 2;
	for (i = 0; i < num_threads; i++) {
		if (statistics[i]->threadstarted > 0)
			pthread_kill(statistics[i]->thread, SIGTERM);
		if (statistics[i]->threadstarted) {
			pthread_join(statistics[i]->thread, NULL);
			if (quiet && !histogram)
				print_stat(parameters[i], i, 0);
		}
		if (statistics[i]->values)
			threadfree(statistics[i]->values, VALBUF_SIZE*sizeof(long), parameters[i]->node);
	}

	if (histogram) {
		print_hist(parameters, num_threads);
		for (i = 0; i < num_threads; i++)
			threadfree(statistics[i]->hist_array, histogram*sizeof(long), parameters[i]->node);
	}

	if (tracelimit) {
		print_tids(parameters, num_threads);
		if (break_thread_id) {
			printf("# Break thread: %d\n", break_thread_id);
			printf("# Break value: %lu\n", break_thread_value);
		}
	}
	

	for (i=0; i < num_threads; i++) {
		if (!statistics[i])
			continue;
		threadfree(statistics[i], sizeof(struct thread_stat), parameters[i]->node);
	}

 outpar:
	for (i = 0; i < num_threads; i++) {
		if (!parameters[i])
			continue;
		threadfree(parameters[i], sizeof(struct thread_param), parameters[i]->node);
	}
 out:
	/* ensure that the tracer is stopped */
	if (tracelimit)
		tracing(0);


	/* close any tracer file descriptors */
	if (trace_fd >= 0)
		close(trace_fd);

	/* turn off all events */
	event_disable_all();

	/* turn off the function tracer */
	fileprefix = procfileprefix;
	if (tracetype)
		setkernvar("ftrace_enabled", "0");
	fileprefix = get_debugfileprefix();

	/* unlock everything */
	if (lockall)
		munlockall();

	/* Be a nice program, cleanup */
	if (kernelversion < KV_26_33)
		restorekernvars();

	/* close the latency_target_fd if it's open */
	if (latency_target_fd >= 0)
		close(latency_target_fd);

	exit(ret);
}
