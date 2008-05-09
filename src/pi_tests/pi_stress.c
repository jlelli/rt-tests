/*
   pi_stress - Priority Inheritance stress test
  
   Copyright (C) 2006, 2007 Clark Williams <williams@redhat.com>
  
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
   USA */

/* This program stress tests pthreads priority inheritance mutexes
  
   The logic is built upon the state machine that performs the "classic_pi"
   deadlock scenario. A state machine or "inversion group" is a group of three
   threads as described below.

   The basic premise here is to set up a deadlock scenario and confirm that PI
   mutexes resolve the situation. Three worker threads will be created from the
   main thread: low, medium and high priority threads that use SCHED_FIFO as 
   their scheduling policy. The low priority thread claims a mutex and then
   starts "working". The medium priority thread starts and preempts the low 
   priority thread. Then the high priority thread runs and attempts to claim
   the mutex owned by the low priority thread. Without priority inheritance, 
   this will deadlock the program. With priority inheritance, the low priority 
   thread receives a priority boost, finishes it's "work" and releases the mutex,
   which allows the high priority thread to run and finish and then the medium
   priority thread finishes. 

   That's the theory, anyway... 
   
   CW - 2006  */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>

// version
const char *version = "pi_stress v" VERSION_STRING " (" __DATE__ " " __TIME__ ")";

// conversions
#define USEC_PER_SEC 	1000000
#define NSEC_PER_SEC 	1000000000
#define USEC_TO_NSEC(u) ((u) * 1000)
#define USEC_TO_SEC(u) 	((u) / USEC_PER_SEC)
#define NSEC_TO_USEC(n) ((n) / 1000)
#define SEC_TO_NSEC(s) 	((s) * NSEC_PER_SEC)
#define SEC_TO_USEC(s) 	((s) * USEC_PER_SEC)

/* test timeout */
#define TIMEOUT 2

/* determine if the C library supports Priority Inheritance mutexes */
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT != -1
#define HAVE_PI_MUTEX 1
#else
#define HAVE_PI_MUTEX 0
#endif

#if HAVE_PI_MUTEX == 0
#error "Can't run this test without PI Muxtex support"
#endif

#define SUCCESS 0
#define FAILURE 1

// cursor control
#define UP_ONE "\033[1A"
#define DOWN_ONE "\033[1B" 

// the length of the test
// default is infinite
int duration = -1;

// times for starting and finishing the stress test
time_t start, finish;

// the number of groups to create
int ngroups = 10;

// the number of times a group causes a priority inversion situation 
// default to infinite
int inversions = -1;

// turn on lots of prints
int verbose = 0;

// turn on debugging prints
int debugging = 0;

// turn off all prints
int quiet = 0;

// prompt to start test
int prompt = 0;

// report interval
unsigned long report_interval = (unsigned long) SEC_TO_USEC(0.75);

// global that indicates we should shut down
volatile int shutdown = 0;

// reader-writer lock to protect shutdown 
pthread_rwlock_t shutdown_lock;

// indicate if errors have occured
int have_errors = 0;

// force running on one cpu
int uniprocessor = 0;

// lock all memory
int lockall = 0;

// command line options
struct option options [] = {
	{ "duration", required_argument, NULL, 't' },
	{ "verbose", no_argument, NULL, 'v' },
	{ "quiet", no_argument, NULL, 'q' },
	{ "groups", required_argument, NULL, 'g'},
	{ "inversions" , required_argument, NULL, 'i'},
	{ "rr", no_argument, NULL, 'r'},
	{ "signal", no_argument, NULL, 's'},
	{ "uniprocessor", no_argument, NULL, 'u'},
	{ "prompt", no_argument, NULL, 'p'},
	{ "debug", no_argument, NULL, 'd'},
	{ "version", no_argument, NULL, 'V'},
	{ "mlockall", no_argument, NULL, 'm'},
	{ "help", no_argument, NULL, 'h'},
	{ NULL, 0, NULL, 0},
};

// max priority for the scheduling policy
int prio_min;

/* define priorities for the threads */
#define MAIN_PRIO() (prio_min + 3)
#define HIGH_PRIO() (prio_min + 2)
#define MED_PRIO()  (prio_min + 1)
#define LOW_PRIO()  (prio_min + 0)

#define NUM_TEST_THREADS 3
#define NUM_ADMIN_THREADS 1

#define TIMER_SIGNAL	(SIGRTMIN+1)

pthread_barrier_t all_threads_ready;
pthread_barrier_t all_threads_done;

cpu_set_t test_cpu_mask, admin_cpu_mask;

int policy = SCHED_FIFO;

struct group_parameters {

	// group id (index)
	int id;

	// threads in the group
	pthread_t low_tid;
	pthread_t med_tid;
	pthread_t high_tid;

	// number of machine iterations to perform
	int inversions;

	// group mutex
	pthread_mutex_t mutex;

        // state barriers
	pthread_barrier_t start_barrier;
	pthread_barrier_t locked_barrier;
	pthread_barrier_t elevate_barrier;
	pthread_barrier_t finish_barrier;

	// state variables
	volatile int high_has_run;
	volatile int low_unlocked;
	volatile int watchdog;

	// total number of inversions performed
	unsigned long total;
} *groups;

/* forward prototypes */
void *low_priority(void *arg);
void *med_priority(void *arg);
void *high_priority(void *arg);
void *reporter(void *arg);
void *watchdog(void *arg);
int setup_thread_attr(pthread_attr_t *attr, int prio, cpu_set_t *mask, int schedpolicy);
int set_cpu_affinity(cpu_set_t *test_mask, cpu_set_t *admin_mask);
void error(char *, ...);
void info(char *, ...);
void debug(char *, ...);
void process_command_line(int argc, char **argv);
void usage(void);
int block_signals(void);
int allow_sigterm(void);
void cleanup(void);
int initialize_group(struct group_parameters *group);
int create_group(struct group_parameters *group, cpu_set_t *mask);
unsigned long total_inversions(void);
void banner(void);
void summary(void);
void wait_for_termination(void);

int
main (int argc, char **argv)
{
	int status;
	struct sched_param thread_param;
	int i;
	int retval = FAILURE;

	/* Make sure we see all message, even those on stdout.  */
	setvbuf (stdout, NULL, _IONBF, 0);

	/* process command line arguments */
	process_command_line(argc, argv);

	/* lock memory */
	if (lockall)
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			error("mlockall failed\n");
			return FAILURE;
		}

	// boost main's priority (so we keep running) :)
	prio_min = sched_get_priority_min(policy);
	thread_param.sched_priority = MAIN_PRIO();
	status = pthread_setschedparam(pthread_self(), SCHED_FIFO, &thread_param);
	if (status) {
		error("main: boosting to max priority: 0x%x\n", status);
		return FAILURE;
	}

	// block unwanted signals
	block_signals();

	// allocate our groups array
	groups = calloc(ngroups, sizeof(struct group_parameters));
	if (groups == NULL) {
		error("main: failed to allocate %d groups\n", ngroups);
		return FAILURE;
	}

	// set up CPU affinity masks
	if (set_cpu_affinity(&test_cpu_mask, &admin_cpu_mask))
		return FAILURE;
		
	// set up our ready barrier
	status = pthread_barrier_init(&all_threads_ready, NULL, 
				      (ngroups * NUM_TEST_THREADS) + NUM_ADMIN_THREADS);
	if (status) {
		error("initialize_barriers: failed to initialize all_threads_ready\n");
		return FAILURE;
	}

	// set up our done barrier
	status = pthread_barrier_init(&all_threads_done, NULL, 
				      (ngroups * NUM_TEST_THREADS) + NUM_ADMIN_THREADS);
	if (status) {
		error("initialize_barriers: failed to initialize all_threads_done\n");
		return FAILURE;
	}

	// set up our rwlock for shutdown flag
	status = pthread_rwlock_init(&shutdown_lock, NULL);
	if (status) {
		error("initializing rwlock: %s\n", strerror(status));
		return FAILURE;
	}

	// create the groups
	info("Creating %d test groups\n", ngroups);
	for (i = 0; i < ngroups; i++) {
		groups[i].id = i;
		if (create_group(&groups[i], &test_cpu_mask))
			return FAILURE;
	}

	// prompt if requested
	if (prompt) {
		printf("Press return to start test: ");
		getchar();
	}

	// report
	banner();
	start = time(NULL);

	// turn loose the threads
	info("Releasing all threads\n");
	status = pthread_barrier_wait(&all_threads_ready);
	if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
		error("main: pthread_barrier_wait(all_threads_ready): 0x%x\n", status);
		cleanup();
		return FAILURE;
	}

	reporter(NULL);

	if (!quiet) {
		fputs(DOWN_ONE, stdout);
		printf("Stopping test\n");
	}
	cleanup();

	// wait for all threads to notice the shutdown flag
	if (have_errors == 0) {
		info("waiting for all threads to complete\n");
		status = pthread_barrier_wait(&all_threads_done);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("main: pthread_barrier_wait(all_threads_ready): 0x%x\n", status);
			return FAILURE;
		}
		info("All threads terminated!\n");
		retval = SUCCESS;
	}
	finish = time(NULL);
	summary();
	if (lockall)
		munlockall();
	exit(retval);
}

int
setup_thread_attr(pthread_attr_t *attr, int prio, cpu_set_t *mask, int schedpolicy)
{
	int status;
	struct sched_param thread_param;

	status = pthread_attr_init(attr);
	if (status) {
		error("setup_thread_attr: initializing thread attribute: 0x%x\n", status);
		return FAILURE;
	}
	status = pthread_attr_setschedpolicy(attr, schedpolicy);
	if (status) {
		error("setup_thread_attr: setting attribute policy to %s: 0x%x\n", 
		      schedpolicy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_RR", status);
		return FAILURE;
	}
	status = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
	if (status) {
		error("setup_thread_attr: setting explicit scheduling inheritance: 0x%x\n", status);
		return FAILURE;
	}
	thread_param.sched_priority = prio;
	status = pthread_attr_setschedparam(attr, &thread_param);
	if (status) {
		error("setup_thread_attr: setting scheduler param: 0x%x\n", status);
		return FAILURE;
	}
	status = pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), mask);
	if (status) {
		error("setup_thread_attr: setting affinity attribute: 0x%x\n", status);
		return FAILURE;
	}
	return SUCCESS;
}

int
set_cpu_affinity(cpu_set_t *test_mask, cpu_set_t *admin_mask)
{
	int status, i, admin_proc;
	cpu_set_t current_mask;
	int max_processors = sizeof(cpu_set_t) * 8;

	// first set our main thread to run on the first
	// scheduleable processor we can find
	status = sched_getaffinity(0, sizeof(cpu_set_t), &current_mask);
	if (status) {
		error("failed getting CPU affinity mask: 0x%x\n", status);
		return FAILURE;
	}
	for (i = 0; i < max_processors; i++) {
		if (CPU_ISSET(i, &current_mask))
		break;
	}
	if (i >= max_processors) {
		error("No schedulable CPU found for main!\n");
		return FAILURE;
	}
	admin_proc = i;
	CPU_ZERO(admin_mask);
	CPU_SET(admin_proc, admin_mask);
	status = sched_setaffinity(0, sizeof(cpu_set_t), admin_mask);
	if (status) {
		error("set_cpu_affinity: setting CPU affinity mask: 0x%x\n", status);
		return FAILURE;
	}
	if (!quiet)
		printf("Admin threads running on processor: %d\n", i);

	/* Now set up a CPU affinity mask so that tests only run on one processor */
	if (uniprocessor == 0) {
		for (i = max_processors - 1; i >= 0; i--) {
			if (CPU_ISSET(i, &current_mask))
				break;
		}
		if (i < 0) {
			error("No schedulable CPU found for tests!\n");
			return FAILURE;
		}
	}
	else
		i = admin_proc;
	CPU_ZERO(test_mask);
	CPU_SET(i, test_mask);
	if (!quiet)
		printf("Test threads running on processor:  %d\n", i);
	return SUCCESS;
}

// clear all watchdog counters
void
watchdog_clear(void)
{
	int i;
	for (i = 0; i < ngroups; i++)
		groups[i].watchdog = 0;
}

// check for zero watchdog counters
int
watchdog_check(void)
{
	int i;
	int failures = 0;
	struct group_parameters *g;

	for (i = 0; i < ngroups; i++) {
		g = &groups[i];
		if (g->watchdog == 0) {
			// don't report deadlock if group is finished
			if (g->inversions == g->total)
				continue;
			error("WATCHDOG triggered: group %d is deadlocked!\n", i);
			failures++;
		}
	}
	return failures ? FAILURE : SUCCESS;
}	

int
pending_interrupt(void)
{
	sigset_t pending;

	if (sigpending(&pending) < 0) {
		error("from sigpending: %s\n", strerror(errno));
		return 0;
	}
	
	return sigismember(&pending, SIGINT);
}
	
static inline void tsnorm(struct timespec *ts)
{
	while (ts->tv_nsec >= NSEC_PER_SEC) {
		ts->tv_nsec -= NSEC_PER_SEC;
		ts->tv_sec++;
	}
}

//
// this routine serves two purposes:
//   1. report progress
//   2. check for deadlocks
//
void *
reporter(void *arg)
{
	int status;
	struct timespec ts;

	ts.tv_sec = 0;
	ts.tv_nsec = USEC_TO_NSEC(report_interval);

	tsnorm(&ts);

	// sleep initially to let everything get up and running
	status = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
	if (status) {
		error("from clock_nanosleep: %s\n", strerror(status));
		return NULL;
	}

	debug("reporter: starting report loop\n");

	if (!quiet)
		printf("Press Control-C to stop test\nCurrent Inversions: \n");

	while (shutdown == 0) {

		// check for a pending SIGINT
		if (pending_interrupt()) {
			if (!quiet)
				printf("Keyboard Interrupt!\n");
			break;
		}

		// clear watchdog counters
		watchdog_clear();
		
		// wait for our reporting interval
		status = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
		if (status) {
			error("from clock_nanosleep: %s\n", strerror(status));
			break;
		}
		if (shutdown == 0) {
			if(!quiet) {
				fputs(UP_ONE, stdout);
				printf("Current Inversions: %lu\n", total_inversions());
			}
		}

		// check watchdog stuff
		if ((watchdog_check())) {
			error("reporter stopping due to watchdog event\n");
			cleanup();
			break;
		}
			
	}
	debug("reporter: finished\n");
	return NULL;
}


void *
low_priority(void *arg)
{
	int status;
	int unbounded;
	unsigned long count = 0;
	struct group_parameters *p = (struct group_parameters *)arg;

	allow_sigterm();

	debug("low_priority[%d]: entering ready state\n", p->id);
	/* wait for all threads to be ready */
	status = pthread_barrier_wait(&all_threads_ready);
	if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
		error("low_priority[%d]: pthread_barrier_wait(all_threads_ready): %x", p->id, status);
		return NULL;
	}

	unbounded = (p->inversions < 0);

	debug("low_priority[%d]: starting inversion loop\n", p->id);

	// grab the shutdown lock to hold off a writer
	status = pthread_rwlock_rdlock(&shutdown_lock);
	if (status) {
		error("aquiring initial rwlock: %s\n", strerror(status));
		return NULL;
	}
	while (!shutdown && (unbounded || (p->total < p->inversions))) {
		/* initial state */
		debug("low_priority[%d]: entering start wait (%d)\n", p->id, count++);
		status = pthread_barrier_wait(&p->start_barrier);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("low_priority[%d]: pthread_barrier_wait(start): %x\n", p->id, status);
			return NULL;
		}
		// release the shutdown lock while we know our entire group
		// is in the loop
		status = pthread_rwlock_unlock(&shutdown_lock);
		if (status) {
			error("low_priority[%d]: releasing (read) shutdown lock: %s\n", 
			      p->id, strerror(status));
			return NULL;
		}
		debug("low_priority[%d]: claiming mutex\n", p->id);
		pthread_mutex_lock(&p->mutex);
		debug("low_priority[%d]: mutex locked\n", p->id);

		debug("low_priority[%d]: entering locked wait\n", p->id);
		status = pthread_barrier_wait(&p->locked_barrier);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("low_priority[%d]: pthread_barrier_wait(locked): %x\n", p->id, status);
			return NULL;
		}

		// wait for priority boost
		debug("low_priority[%d]: entering elevated wait\n", p->id);
		p->low_unlocked = 0; /* prevent race with med_priority */
		status = pthread_barrier_wait(&p->elevate_barrier);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("low_priority[%d]: pthread_barrier_wait(elevate): %x\n", p->id, status);
			return NULL;
		}
		p->low_unlocked = 1;

		// release the mutex
		debug("low_priority[%d]: unlocking mutex\n", p->id);
		pthread_mutex_unlock(&p->mutex);

		// finish state 
		debug("low_priority[%d]: entering finish wait\n", p->id);
		status = pthread_barrier_wait(&p->finish_barrier);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("low_priority[%d]: pthread_barrier_wait(elevate): %x\n", p->id, status);
			return NULL;
		}
		// reacquire the shutdown lock so that we can safely
		// test the loop control
		status = pthread_rwlock_rdlock(&shutdown_lock);
		if (status) {
			error("low_priority[%d]: acquiring (read) shutdown lock: %s\n", 
			      p->id, strerror(status));
			return NULL;
		}
	}
	status = pthread_rwlock_unlock(&shutdown_lock);
	if (status)
		error("low_priority[%d]: releasing (read) shutdown lock: %s\n", 
			  p->id, strerror(status));
	cleanup();
	debug("low_priority[%d]: entering done barrier\n", p->id);
	/* wait for all threads to finish */
	status = pthread_barrier_wait(&all_threads_done);
	if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
		error("low_priority[%d]: pthread_barrier_wait(all_threads_done): %x", p->id, status);
		return NULL;
	}
	debug("low_priority[%d]: exiting\n", p->id);
	return NULL;
}

void *
med_priority(void *arg)
{
	int status;
	int unbounded;
	unsigned long count = 0;
	struct group_parameters *p = (struct group_parameters *)arg;

	allow_sigterm();
	debug("med_priority[%d]: entering ready state\n", p->id);
	/* wait for all threads to be ready */
	status = pthread_barrier_wait(&all_threads_ready);
	if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
		error("med_priority[%d]: pthread_barrier_wait(all_threads_ready): %x", p->id, status);
		return NULL;
	}

	unbounded = (p->inversions < 0);

	debug("med_priority[%d]: starting inversion loop\n", p->id);
	status = pthread_rwlock_rdlock(&shutdown_lock);
	if (status) {
		error("aquiring initial rwlock: %s\n", strerror(status));
		return NULL;
	}
	while (!shutdown && (unbounded || (p->total < p->inversions))) {
		/* start state */
		debug("med_priority[%d]: entering start state (%d)\n", p->id, count++);
		status = pthread_barrier_wait(&p->start_barrier);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("med_priority[%d]: pthread_barrier_wait(start): %x", p->id, status);
			return NULL;
		}
		status = pthread_rwlock_unlock(&shutdown_lock);
		if (status) {
			error("med_priority[%d]: releasing (read) shutdown lock: %s\n", 
			      p->id, strerror(status));
			return NULL;
		}
		debug("med_priority[%d]: entering elevate state\n", p->id);
		do {
			status = pthread_barrier_wait(&p->elevate_barrier);
			if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
				error("med_priority[%d]: pthread_barrier_wait(elevate): %x", p->id, status);
				return NULL;
			}
		} while (!p->high_has_run && !p->low_unlocked);
		debug("med_priority[%d]: entering finish state\n", p->id);
		status = pthread_barrier_wait(&p->finish_barrier);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("med_priority[%d]: pthread_barrier_wait(finished): %x", p->id, status);
			return NULL;
		}
		status = pthread_rwlock_rdlock(&shutdown_lock);
		if (status) {
			error("med_priority[%d]: acquiring (read) shutdown lock: %s\n", 
			      p->id, strerror(status));
			return NULL;
		}
	}
	status = pthread_rwlock_unlock(&shutdown_lock);
	if (status)
		error("med_priority[%d]: releasing (read) shutdown lock: %s\n", 
			  p->id, strerror(status));

	debug("med_priority[%d]: entering done barrier\n", p->id);

	/* wait for all threads to finish */
	status = pthread_barrier_wait(&all_threads_done);
	if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
		error("med_priority[%d]: pthread_barrier_wait(all_threads_done): %x", p->id, status);
		return NULL;
	}
	// exit
	debug("med_priority[%d]: exiting\n", p->id);
	return NULL;
}

void *
high_priority(void *arg)
{
	int status;
	int unbounded;
	unsigned long count = 0;
	struct group_parameters *p = (struct group_parameters *)arg;

	allow_sigterm();
	debug("high_priority[%d]: entering ready state\n", p->id);

	/* wait for all threads to be ready */
	status = pthread_barrier_wait(&all_threads_ready);
	if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
		error("high_priority[%d]: pthread_barrier_wait(all_threads_ready): %x", p->id, status);
		return NULL;
	}
	unbounded = (p->inversions < 0);
	debug("high_priority[%d]: starting inversion loop\n", p->id);
	status = pthread_rwlock_rdlock(&shutdown_lock);
	if (status) {
		error("aquiring initial rwlock: %s\n", strerror(status));
		return NULL;
	}
	while (!shutdown && (unbounded || (p->total < p->inversions))) {
		p->high_has_run = 0;
		debug("high_priority[%d]: entering start state (%d)\n", p->id, count++);
		status = pthread_barrier_wait(&p->start_barrier);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("high_priority[%d]: pthread_barrier_wait(start): %x", p->id, status);
			return NULL;
		}
		status = pthread_rwlock_unlock(&shutdown_lock);
		if (status) {
			error("high_priority[%d]: releasing (read) shutdown lock: %s\n", 
			      p->id, strerror(status));
			return NULL;
		}
		debug("high_priority[%d]: entering running state\n", p->id);
		status = pthread_barrier_wait(&p->locked_barrier);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("high_priority[%d]: pthread_barrier_wait(running): %x", p->id, status);
			return NULL;
		}
		debug("high_priority[%d]: locking mutex\n", p->id);
		pthread_mutex_lock(&p->mutex);
		debug("high_priority[%d]: got mutex\n", p->id);
		p->high_has_run = 1;
		debug("high_priority[%d]: unlocking mutex\n", p->id);
		pthread_mutex_unlock(&p->mutex);
		debug("high_priority[%d]: entering finish state\n", p->id);
		status = pthread_barrier_wait(&p->finish_barrier);
		if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
			error("high_priority[%d]: pthread_barrier_wait(finish): %x", status);
			return NULL;
		}

		// update the group stats
		p->total++;

		// update the watchdog counter
		p->watchdog++;

		status = pthread_rwlock_rdlock(&shutdown_lock);
		if (status) {
			error("high_priority[%d]: acquiring (read) shutdown lock: %s\n", 
			      p->id, strerror(status));
			return NULL;
		}
	}

	status = pthread_rwlock_unlock(&shutdown_lock);
	if (status)
		error("high_priority[%d]: releasing (read) shutdown lock: %s\n", 
			  p->id, strerror(status));
	debug("high_priority[%d]: entering done barrier\n", p->id);

	/* wait for all threads to finish */
	status = pthread_barrier_wait(&all_threads_done);
	if (status && status != PTHREAD_BARRIER_SERIAL_THREAD) {
		error("high_priority[%d]: pthread_barrier_wait(all_threads_done): %x", p->id, status);
		return NULL;
	}
       
	// exit
	debug("high_priority[%d]: exiting\n", p->id);
	return NULL;
}

void
error(char *fmt, ...)
{
	va_list ap;
	fputs("ERROR: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	have_errors = 1;
}

void
info(char *fmt, ...)
{
	if (verbose) {
		va_list ap;
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
}

void 
debug(char *fmt, ...)
{
	if (debugging) {
		va_list ap;
		fputs("DEBUG: ", stderr);
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
}
	
void
usage(void)
{
	printf("usage: pi_stress <options>\n");
	printf("    options:\n");
	printf("\t--verbose\t- lots of output\n");
	printf("\t--duration=<n>- length of the test run in seconds [infinite]\n");
	printf("\t--groups=<n>\t- set the number of inversion groups [%d]\n", ngroups);
	printf("\t--inversions=<n>- number of inversions per group [infinite]\n");
	printf("\t--report=<path>\t- output to file [/dev/null]\n");
	printf("\t--rr\t\t- use SCHED_RR for test threads [SCHED_FIFO]\n");
	printf("\t--prompt\t- prompt before starting the test\n");
	printf("\t--uniprocessor\t- force all threads to run on one processor\n");
	printf("\t--mlockall\t- lock current and future memory\n");
	printf("\t--debug\t\t- turn on debug prints\n");
	printf("\t--version\t- print version number on output\n");
	printf("\t--help\t\t- print this message\n");
}

// block all signals (called from main)
int
block_signals(void)
{
	int status;
	sigset_t sigset;

	// mask off all signals
	status = sigfillset(&sigset);
	if (status) {
		error("setting up full signal set %s\n", strerror(status));
		return FAILURE;
	}
	status = pthread_sigmask(SIG_BLOCK, &sigset, NULL);
	if (status) {
		error("setting signal mask: %s\n", strerror(status));
		return FAILURE;
	}
	return SUCCESS;
}

// allow SIGTERM delivery (called from worker threads)
int
allow_sigterm(void)
{
	int status;
	sigset_t sigset;

	status = sigemptyset(&sigset);
	if (status) {
		error("creating empty signal set: %s\n", strerror(status));
		return FAILURE;
	}
	status = sigaddset(&sigset, SIGTERM);
	if (status) {
		error("adding SIGTERM to signal set: %s\n", strerror(status));
		return FAILURE;
	}
	status = pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
	if (status) {
		error("unblocking SIGTERM: %s\n", strerror(status));
		return FAILURE;
	}
	return SUCCESS;
}

// clean up before exiting
void
cleanup(void)
{
	int status;
	
	if (shutdown == 0) {
		// tell anyone that's looking that we're done
		debug("acquiring write lock on shutdown variable\n");
		status = pthread_rwlock_wrlock(&shutdown_lock);
		if (status) {
			error("acquiring (write) shutdown lock: %s\n", strerror(status));
			return;
		}
		info("setting shutdown flag\n");
		shutdown = 1;
		debug("releasing shutdown lock\n");
		status = pthread_rwlock_unlock(&shutdown_lock);
		if (status) {
			error("releasing (write) shutdown lock: %s\n", strerror(status));
			return;
		}
	}
}

// set up a test group
int
initialize_group(struct group_parameters *group)
{
	int status;
	pthread_mutexattr_t mutex_attr;

	group->inversions = inversions;

	// setup default attributes for the group mutex
	// (make it a PI mutex)
	status = pthread_mutexattr_init(&mutex_attr);
	if (status) {
		error("initializing mutex attribute: %s\n", strerror(status));
		return FAILURE;
	}

	/* set priority inheritance attribute for mutex */
	status = pthread_mutexattr_setprotocol(&mutex_attr, 
					       PTHREAD_PRIO_INHERIT);
	if (status) {
		error("setting mutex attribute policy: %s\n", strerror(status));
		return FAILURE;
	}

	// initialize the group mutex
	status = pthread_mutex_init(&group->mutex, &mutex_attr);
	if (status) {
		error("initializing mutex: %s\n", strerror(status));
		return FAILURE;
	}

	// initialize the group barriers
	status = pthread_barrier_init(&group->start_barrier, NULL, NUM_TEST_THREADS);
	if (status) {
		error("failed to initialize start_barrier\n");
		return FAILURE;
	}
	status = pthread_barrier_init(&group->locked_barrier, NULL, 2);
	if (status) {
		error("failed to intialize locked_barrier: %s\n", strerror(status));
		return FAILURE;
	}
	status = pthread_barrier_init(&group->elevate_barrier, NULL, 2);
	if (status) {
		error("failed to initialize elevate_barrier: %s\n", strerror(status));
		return FAILURE;
	}
	status = pthread_barrier_init(&group->finish_barrier, NULL, NUM_TEST_THREADS);
	if (status) {
		error("failed to initialize finish_barrier: %s\n", strerror(status));
		return FAILURE;
	}
	return SUCCESS;
}	
// setup and create a groups threads
int
create_group(struct group_parameters *group, cpu_set_t *mask)
{
	int status;
	pthread_attr_t thread_attr;

	// initialize group structure
	status = initialize_group(group);
	if (status) {
		error("initializing group %d\n", group->id);
		return FAILURE;
	}

	/* start the low priority thread */
	debug("creating low priority thread\n");
	if (setup_thread_attr(&thread_attr, LOW_PRIO(), mask, policy))
		return FAILURE;
	status = pthread_create(&group->low_tid, 
				&thread_attr, 
				low_priority, 
				group);
	if (status != 0) {
		error("creating low_priority thread: %s\n", strerror(status));
		return FAILURE;
	}

	/* create the medium priority thread */
	debug("creating medium priority thread\n");
	if (setup_thread_attr(&thread_attr, MED_PRIO(), mask, policy))
		return FAILURE;
	status = pthread_create(&group->med_tid,
				&thread_attr, 
				med_priority, 
				group);
	if (status != 0) {
		error("creating med_priority thread: %s\n", strerror(status));
		return FAILURE;
	}

	/* create the high priority thread */
	debug("creating high priority thread\n");
	if (setup_thread_attr(&thread_attr, HIGH_PRIO(), mask, policy))
		return FAILURE;
	status = pthread_create(&group->high_tid,
				&thread_attr, 
				high_priority, 
				group);
	if (status != 0) {
		error("creating high_priority thread: %s\n", strerror(status));
		cleanup();
		return FAILURE;
	}
	return SUCCESS;
}

void
process_command_line(int argc, char **argv)
{
	int opt;
	while ((opt = getopt_long(argc, argv, "+", options, NULL)) != -1) {
		switch (opt) {
		case '?':
		case 'h':
			usage();
			exit(0);
		case 't':
			duration = strtol(optarg, NULL, 10);
			break;
		case 'v':
			verbose = 1;
			quiet = 0;
			break;
		case 'q':
			verbose = 0;
			quiet = 1;
			break;
		case 'i':
			inversions = strtol(optarg, NULL, 10);
			info("doing %d inversion per group\n", inversions);
			break;
		case 'g':
			ngroups = strtol(optarg, NULL, 10);
			info("number of groups set to %d\n", ngroups);
			break;
		case 'r':
			policy = SCHED_RR;
			break;
		case 'p':
			prompt = 1;
			break;
		case 'd':
			debugging = 1;
			break;
		case 'V':
			puts(version);
			exit(0);
		case 'u':
			uniprocessor = 1;
			break;
		case 'm':
			lockall = 1;
			break;
		}
	}
}

// total the number of inversions that have been performed
unsigned long
total_inversions(void)
{
	int i;
	unsigned long total = 0;

	for (i = 0; i < ngroups; i++)
		total += groups[i].total;
	return total;
}

void banner(void)
{
	if (quiet)
		return;

	printf("Starting PI Stress Test\n");
	printf("Number of thread groups: %d\n", ngroups);
	if (inversions < 0)
		printf("Number of inversions per group: infinite\n");
	else
		printf("Number of inversions per group: %d\n", inversions);
	printf("Test threads using scheduler policy: %s\n", 
	     policy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_RR");
	printf("    Admin thread priority:  %d\n", MAIN_PRIO());
	printf("%d groups of 3 threads will be created\n", ngroups);
	printf("    High thread priority:   %d\n", HIGH_PRIO());
	printf("    Med  thread priority:   %d\n", MED_PRIO());
	printf("    Low thread priority:    %d\n", LOW_PRIO());
}


void summary(void)
{
	time_t interval = finish - start;
	struct tm *t = gmtime(&interval);
	
	printf("Total inversion performed: %lu\n", total_inversions());
	printf("Test Duration: %d days, %d hours, %d minutes, %d seconds\n",
		   t->tm_yday, t->tm_hour, t->tm_min, t->tm_sec);
}
