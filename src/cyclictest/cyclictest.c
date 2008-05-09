/*
 * High resolution timer test software
 *
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
	DEFAULTTRACE,
	IRQSOFF,
	PREEMPTOFF,
	IRQPREEMPTOFF,
};

/* Struct to transfer parameters to the thread */
struct thread_param {
	int prio;
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
static int tracetype;
static int lockall = 0;

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

/*
 * Finds the path to the debugfs/tracing
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

static inline long calcdiff(struct timespec t1, struct timespec t2)
{
	long diff;
	diff = USEC_PER_SEC * ((int) t1.tv_sec - (int) t2.tv_sec);
	diff += ((int) t1.tv_nsec - (int) t2.tv_nsec) / 1000;
	return diff;
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
	char name[100];
	FILE *fp;
	int ret;
	int i;

	/* Make sure tracer is available */
	strncpy(filename, debugfileprefix, sizeof(filename));
	strncat(filename, "available_tracers", sizeof(filename) - strlen(debugfileprefix));

	fp = fopen(filename, "r");
	if (!fp)
		return -1;

	for (i = 0; i < 10; i++) {
		ret = fscanf(fp, "%99s", name);
		if (!ret) {
			ret = -1;
			break;
		}
		if (strcmp(name, tracer) == 0) {
			ret = 0;
			break;
		}
	}
	fclose(fp);

	if (!ret)
		setkernvar("current_tracer", tracer);

	return ret;
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
	struct timespec now, next, interval;
	struct itimerval itimer;
	struct itimerspec tspec;
	struct thread_stat *stat = par->stats;
	int policy = par->prio ? SCHED_FIFO : SCHED_OTHER;
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

	if (tracelimit) {
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
			case IRQSOFF:
				ret = settracer("irqsoff");
				break;
			case PREEMPTOFF:
				ret = settracer("preemptoff");
				break;
			case IRQPREEMPTOFF:
				ret = settracer("preemptirqsoff");
				break;
			default:
				if ((ret = settracer("events"))) {
					if (ftrace)
						ret = settracer("ftrace");
					else
						ret = settracer("sched_switch");
				}
			}

			if (ret)
				fprintf(stderr, "Requested tracer not available\n");

			setkernvar("iter_ctrl", "print-parent");
			if (verbose) {
				setkernvar("iter_ctrl", "sym-offset");
				setkernvar("iter_ctrl", "sym-addr");
				setkernvar("iter_ctrl", "verbose");
			} else {
				setkernvar("iter_ctrl", "nosym-offset");
				setkernvar("iter_ctrl", "nosym-addr");
				setkernvar("iter_ctrl", "noverbose");
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
	}

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
	sched_setscheduler(0, policy, &schedp);

	/* Get current time */
	clock_gettime(par->clock, &now);
	next = now;
	next.tv_sec++;

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

	if (tracelimit)
		tracing(1);

	while (!shutdown) {

		long diff;
		int sigs;

		/* Wait for next period */
		switch (par->mode) {
		case MODE_CYCLIC:
		case MODE_SYS_ITIMER:
			if (sigwait(&sigset, &sigs) < 0)
				goto out;
			break;

		case MODE_CLOCK_NANOSLEEP:
			if (par->timermode == TIMER_ABSTIME)
				clock_nanosleep(par->clock, TIMER_ABSTIME,
						&next, NULL);
			else {
				clock_gettime(par->clock, &now);
				clock_nanosleep(par->clock, TIMER_RELTIME,
						&interval, NULL);
				next.tv_sec = now.tv_sec + interval.tv_sec;
				next.tv_nsec = now.tv_nsec + interval.tv_nsec;
				tsnorm(&next);
			}
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

		diff = calcdiff(now, next);
		if (diff < stat->min)
			stat->min = diff;
		if (diff > stat->max)
			stat->max = diff;
		stat->avg += (double) diff;

		if (!stopped && tracelimit && (diff > tracelimit)) {
			stopped++;
			tracing(0);
			shutdown++;
		}
		stat->act = diff;
		stat->cycles++;

		if (par->bufmsk)
			stat->values[stat->cycles & par->bufmsk] = diff;

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
static void display_help(void)
{
	printf("cyclictest V %1.2f\n", VERSION_STRING);
	printf("Usage:\n"
	       "cyclictest <options>\n\n"
	       "-a [NUM] --affinity        run thread #N on processor #N, if possible\n"
	       "                           with NUM pin all threads to the processor NUM\n"
	       "-a PROC  --affinity=PROC   run all threads on processor #PROC\n"
	       "-b USEC  --breaktrace=USEC send break trace command when latency > USEC\n"
	       "-B       --preemptirqs     both preempt and irqsoff tracing (used with -b)\n"
	       "-c CLOCK --clock=CLOCK     select clock\n"
	       "                           0 = CLOCK_MONOTONIC (default)\n"
	       "                           1 = CLOCK_REALTIME\n"
	       "-d DIST  --distance=DIST   distance of thread intervals in us default=500\n"
	       "-f       --ftrace          function trace (when -b is active)\n"
	       "-i INTV  --interval=INTV   base interval of thread in us default=1000\n"
	       "-I       --irqsoff         Irqsoff tracing (used with -b)\n"
	       "-l LOOPS --loops=LOOPS     number of loops: default=0(endless)\n"
	       "-m       --mlockall        lock current and future memory allocations\n"
	       "-n       --nanosleep       use clock_nanosleep\n"
	       "-o RED   --oscope=RED      oscilloscope mode, reduce verbose output by RED\n"
	       "-p PRIO  --prio=PRIO       priority of highest prio thread\n"
	       "-P       --preemptoff      Preempt off tracing (used with -b)\n"
	       "-q       --quiet           print only a summary on exit\n"
	       "-r       --relative        use relative timer instead of absolute\n"
	       "-s       --system          use sys_nanosleep and sys_setitimer\n"
	       "-t       --threads         one thread per available processor\n"
	       "-t [NUM] --threads=NUM     number of threads:\n"
	       "                           without NUM, threads = max_cpus\n"
	       "                           without -t default = 1\n"
	       "-v       --verbose         output values on stdout for statistics\n"
	       "                           format: n:c:v n=tasknum c=count v=value in us\n");
	exit(0);
}

static int use_nanosleep;
static int timermode = TIMER_ABSTIME;
static int use_system;
static int priority;
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
			{"distance", required_argument, NULL, 'd'},
			{"ftrace", no_argument, NULL, 'f'},
			{"interval", required_argument, NULL, 'i'},
			{"irqsoff", no_argument, NULL, 'I'},
			{"loops", required_argument, NULL, 'l'},
			{"mlockall", no_argument, NULL, 'm' },
			{"nanosleep", no_argument, NULL, 'n'},
			{"oscope", required_argument, NULL, 'o'},
			{"priority", required_argument, NULL, 'p'},
			{"preemptoff", no_argument, NULL, 'P'},
			{"quiet", no_argument, NULL, 'q'},
			{"relative", no_argument, NULL, 'r'},
			{"system", no_argument, NULL, 's'},
			{"threads", optional_argument, NULL, 't'},
			{"verbose", no_argument, NULL, 'v'},
			{"help", no_argument, NULL, '?'},
			{NULL, 0, NULL, 0}
		};
		int c = getopt_long (argc, argv, "a::b:Bc:d:fi:Il:nmo:p:Pqrst::v",
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
		case 'd': distance = atoi(optarg); break;
		case 'f': ftrace = 1; break;
		case 'i': interval = atoi(optarg); break;
		case 'I': tracetype = IRQSOFF; break;
		case 'l': max_cycles = atoi(optarg); break;
		case 'n': use_nanosleep = MODE_CLOCK_NANOSLEEP; break;
		case 'o': oscope_reduction = atoi(optarg); break;
		case 'p': priority = atoi(optarg); break;
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
		case 'v': verbose = 1; break;
		case 'm': lockall = 1; break;
		case '?': error = 1; break;
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

	if (clocksel < 0 || clocksel > ARRAY_SIZE(clocksources))
		error = 1;

	if (oscope_reduction < 1)
		error = 1;

	if (oscope_reduction > 1 && !verbose) {
		fprintf(stderr, "ERROR: -o option only meaningful, if verbose\n");
		error = 1;
	}

	if (priority < 0 || priority > 99)
		error = 1;

	if (num_threads < 1)
		error = 1;

	if (error)
		display_help ();
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
		else
			kv = KV_26_CURR;
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

static void print_stat(struct thread_param *par, int index, int verbose)
{
	struct thread_stat *stat = par->stats;

	if (!verbose) {
		if (quiet != 1) {
			printf("T:%2d (%5d) P:%2d I:%ld C:%7lu "
			       "Min:%7ld Act:%5ld Avg:%5ld Max:%8ld\n",
			       index, stat->tid, par->prio, par->interval,
			       stat->cycles, stat->min, stat->act,
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
				printf("%8d:%8lu:%8ld\n", index, stat->cycleofmax,
				    stat->redmax);
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
	struct thread_param *par;
	struct thread_stat *stat;
	int max_cpus = sysconf(_SC_NPROCESSORS_CONF);
	int i, ret = -1;

	if (geteuid()) {
		fprintf(stderr, "cyclictest: need to run as root!\n");
		exit(-1);
	}

	process_options(argc, argv);

	/* lock all memory (prevent paging) */
	if (lockall)
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			perror("mlockall");
			goto out;
		}
		
	kernelversion = check_kernel();

	if (kernelversion == KV_NOT_26)
		fprintf(stderr, "WARNING: Most functions require kernel 2.6\n");

	if (tracelimit && kernelversion == KV_26_CURR) {
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
	} else if (tracelimit)
		fileprefix = procfileprefix;

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
		if (verbose) {
			stat[i].values = calloc(VALBUF_SIZE, sizeof(long));
			if (!stat[i].values)
				goto outall;
			par[i].bufmsk = VALBUF_SIZE - 1;
		}

		par[i].prio = priority;
		if (priority)
			priority--;
		par[i].clock = clocksources[clocksel];
		par[i].mode = mode;
		par[i].timermode = timermode;
		par[i].signal = signum;
		par[i].interval = interval;
		interval += distance;
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

		if (!verbose && !quiet) {
			fd = open("/proc/loadavg", O_RDONLY, 0666);
			len = read(fd, &lavg, 255);
			close(fd);
			lavg[len-1] = 0x0;
			printf("%s          \n\n", lavg);
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
	ret = 0;
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
			if (quiet)
				print_stat(&par[i], i, 0);
		}
		if (stat[i].values)
			free(stat[i].values);
	}
	free(stat);
 outpar:
	free(par);
 out:

	/* unlock everything */
	if (lockall)
		munlockall();

	/* Be a nice program, cleanup */
	if (kernelversion != KV_26_CURR)
		restorekernvars();

	exit(ret);
}
