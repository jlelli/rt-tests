/*
 * Copyright (C) 2016 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not,  see <http://www.gnu.org/licenses>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * deadline_test.c
 *
 * This program is used to test the deadline scheduler (SCHED_DEADLINE tasks).
 * It is broken up into various degrees of complexity that can be set with
 * options.
 *
 * Here are the test cases:
 *
 * 1) Simplest - create one deadline task that can migrate across all CPUS.
 *    Look for "simple_test"
 *
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sched.h>
#include <ctype.h>
#include <errno.h>

#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#include <linux/unistd.h>
#include <linux/magic.h>

#include <rt-utils.h>
#include <rt-sched.h>

/**
 * usage - show the usage of the program and exit.
 * @argv: The program passed in args
 *
 * This is defined here to show people looking at this code how
 * to use this program as well. 
 */
static void usage(char **argv)
{
	char *arg = argv[0];
	char *p = arg+strlen(arg);

	while (p >= arg && *p != '/')
		p--;
	p++;

	printf("usage: %s [options]\n"
	       " -h - Show this help menu\n"
	       " -b - Bind on the last cpu. (shortcut for -c <lastcpu>)\n"
	       " -r prio - Add an RT task with given prio to stress system\n"
	       " -c cpulist - Comma/hyphen separated list of CPUs to run deadline tasks on\n"
	       " -i interval - The shortest deadline for the tasks\n"
	       " -p percent - The percent of bandwidth to use (1-90%%)\n"
	       " -P percent - The percent of runtime for execution completion\n"
	       "              (Default 100%%)\n"
	       " -t threads - The number of threads to run as deadline (default 1)\n"
	       " -s step(us) - The amount to increase the deadline for each task (default 500us)\n"
	       "\n", p);
	exit(-1);
}

#define _STR(x) #x
#define STR(x) _STR(x)

/* Max path for cpuset path names. 1K should be enough */
#ifndef MAXPATH
#define MAXPATH 1024
#endif

/*
 * "my_cpuset" is the cpuset that will hold the SCHED_DEADLINE tasks that
 * want to limit their affinity.
 *
 * "my_cpuset_all" is the cpuset that will have the affinity of all the
 * other CPUs outside the ones for SCHED_DEADLINE threads. It will hold
 * all other tasks.
 */
#define CPUSET_ALL	"my_cpuset_all"
#define CPUSET_LOCAL	"my_cpuset"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef int s32;

/**
 * struct sched_data - the descriptor for the threads.
 *
 * This is the descriptor that will be passed as the thread data.
 * It is used as both input to the thread, as well as output to
 * the main program.
 *
 * @runtime_us: The runtime for sched_deadline tasks in microseconds
 * @deadline_us: The deadline for sched_deadline tasks in microseconds
 * @loops_per_period: The amount of loops to run for the runtime
 * @max_time: Recorded max time to complete loops
 * @min_time: Recorded min time to complete loops
 * @total_time: The total time of all periods to perform the loops
 * @nr_periods: The number of periods executed
 * @prime: Calculating a prime number.
 * @missed_periods: The number of periods that were missed (started late)
 * @missed_deadlines: The number of deadlines that were missed (ended late)
 * @total_adjust: The time in microseconds adjusted for starting early
 * @nr_adjust: The number of times adjusted for starting early
 * @last_time: Last runtime of loops (used to calculate runtime to give)
 * @prio: The priority for SCHED_FIFO threads (uses same descriptor)
 * @tid: Stores the thread ID of the thread.
 * @vol: The number of voluntary schedules the thread made
 * @nonvol: The number of non-voluntary schedules the thread made (preempted)
 * @migrate: The number of migrations the thread made.
 * @buff: A string buffer to store data to write to ftrace
 *
 */
struct sched_data {
	u64 runtime_us;
	u64 deadline_us;

	u64 loops_per_period;

	u64 max_time;
	u64 min_time;
	u64 total_time;
	u64 nr_periods;

	u64 prime;

	int missed_periods;
	int missed_deadlines;
	u64 total_adjust;
	u64 nr_adjust;

	u64 last_time;

	int prio;
	int tid;

	int vol;
	int nonvol;
	int migrate;

	char buff[BUFSIZ+1];

	/* Try to keep each sched_data out of cache lines */
	char padding[256];
};

/* Barrier to synchronize the threads for initialization */
static pthread_barrier_t barrier;

/* cpu_count is the number of detected cpus on the running machine */
static int cpu_count;

/*
 * cpusetp and cpuset_size is for cpumasks, in case we run on a machine with
 * more than 64 CPUs.
 */
static cpu_set_t *cpusetp;
static int cpuset_size;

/* Number of threads to create to run deadline scheduler with (default two) */
static int nr_threads = 2;

/**
 * find_mount - Find if a file system type is already mounted
 * @mount: The type of files system to find
 * @debugfs: Where to place the path to the found file system.
 *
 * Returns 1 if found and sets @debugfs.
 * Returns 0 otherwise.
 */
static int find_mount(const char *mount, char *debugfs)
{
	char type[100];
	FILE *fp;

	if ((fp = fopen("/proc/mounts","r")) == NULL)
		return 0;

	while (fscanf(fp, "%*s %"
		      STR(MAXPATH)
		      "s %99s %*s %*d %*d\n",
		      debugfs, type) == 2) {
		if (strcmp(type, mount) == 0)
			break;
	}
	fclose(fp);

	if (strcmp(type, mount) != 0)
		return 0;
	return 1;
}

/**
 * find_debugfs - Search for where debugfs is found
 *
 * Finds where debugfs is mounted and returns the path.
 * The returned string is static and should not be modified.
 */
static const char *find_debugfs(void)
{
	static int debugfs_found;
	static char debugfs[MAXPATH+1];

	if (debugfs_found)
		return debugfs;

	if (!find_mount("debugfs", debugfs))
		return "";
	
	debugfs_found = 1;

	return debugfs;
}

/**
 * my_vsprintf - simplified vsprintf()
 * @buf: The buffer to write the string to
 * @size: The allocated size of @buf
 * @fmmt: The format to parse
 * @ap: The variable arguments
 *
 * Because there's no real way to prevent glibc's vsprintf from
 * allocating more memory, or doing any type of system call,
 * This is a simple version of the function that is under
 * our control, to make sure we stay in userspace when creating
 * a ftrace_write string, and only do a system call for the
 * actual ftrace_write.
 */
static int my_vsprintf(char *buf, int size, const char *fmt, va_list ap)
{
	const char *p;
	char tmp[100];
	char *s = buf;
	char *end = buf + size;
	char *str;
	long long lng;
	int l;
	int i;

	end[-1] = 0;

	for (p = fmt; *p && s < end; p++) {
		if (*p == '%') {
			l = 0;
 again:
			p++;
			switch (*p) {
			case 's':
				if (l) {
					fprintf(stderr, "Illegal print format l used with %%s\n");
					exit(-1);
				}
				str = va_arg(ap, char *);
				l = strlen(str);
				strncpy(s, str, end - s);
				s += l;
				break;
			case 'l':
				l++;
				goto again;
			case 'd':
				if (l == 1) {
					if (sizeof(long) == 8)
						l = 2;
				}
				if (l == 2)
					lng = va_arg(ap, long long);
				else if (l > 2) {
					fprintf(stderr, "Illegal print format l=%d\n", l);
					exit(-1);
				} else
					lng = va_arg(ap, int);
				i = 0;
				while (lng > 0) {
					tmp[i++] = (lng % 10) + '0';
					lng /= 10;
				}
				tmp[i] = 0;
				l = strlen(tmp);
				if (!l) {
					*s++ = '0';
				} else {
					while (l)
						*s++ = tmp[--l];
				}
				break;
			default:
				fprintf(stderr, "Illegal print format '%c'\n", *p);
				exit(-1);
			}
			continue;
		}
		*s++ = *p;
	}

	return s - buf;
}

/* The ftrace tracing_marker file descriptor to write to ftrace */
static int mark_fd;

/**
 * ftrace_write - write a string to ftrace tracing_marker
 * @buf: A BUFSIZ + 1 allocated scratch pad
 * @fmt: The format of the sting to write
 * @va_arg: The arguments for @fmt
 *
 * If mark_fd is not less than zero, format the input
 * and write it out to trace_marker (where mark_fd is a file
 * descriptor of).
 */
static void ftrace_write(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (mark_fd < 0)
		return;

	va_start(ap, fmt);
	n = my_vsprintf(buf, BUFSIZ, fmt, ap);
	va_end(ap);

	write(mark_fd, buf, n);
}

/**
 * setup_ftrace_marker - Check if trace_marker exists and open if it does
 *
 * Tests if debugfs is mounted, and if it is, it tests to see if the
 * trace_marker exists. If it does, it opens trace_marker and sets
 * mark_fd to the file descriptor. Then ftrace_write() will be able
 * to write to the ftrace marker, otherwise ftrace_write() becomes
 * a nop.
 *
 * Failure to open the trace_marker file will not stop this application
 * from executing. Only ftrace writes will not be performed.
 */
static void setup_ftrace_marker(void)
{
	struct stat st;
	const char *debugfs = find_debugfs();
	char files[strlen(debugfs) + strlen("/tracing/trace_marker") + 1];
	int ret;

	if (strlen(debugfs) == 0)
		return;

	sprintf(files, "%s/tracing/trace_marker", debugfs);
	ret = stat(files, &st);
	if (ret >= 0)
		goto found;
	/* Do nothing if not mounted */
	return;
found:
	mark_fd = open(files, O_WRONLY);
}

/**
 * setup_hr_tick - Enable the HRTICK in sched_features (if available)
 *
 * SCHED_DEADLINE tasks are based on HZ, which could be as slow as
 * 100 times a second (10ms). Which is incredibly slow for scheduling.
 * For SCHED_DEADLINE to have finer resolution, HRTICK feature must be
 * set. That's located in the debugfs/sched_features directory.
 *
 * This will not mount debugfs. If debugfs is not mounted, this simply
 * will fail.
 */
static int setup_hr_tick(void)
{
	const char *debugfs = find_debugfs();
	char files[strlen(debugfs) + strlen("/sched_features") + 1];
	char buf[500];
	struct stat st;
	static int set = 0;
	char *p;
	int ret;
	int len;
	int fd;

	if (set)
		return 1;

	set = 1;

	if (strlen(debugfs) == 0)
		return 0;

	sprintf(files, "%s/sched_features", debugfs);
	ret = stat(files, &st);
	if (ret < 0)
		return 0;

	fd = open(files, O_RDWR);
	if (fd < 0) {
		perror(files);
		return 0;
	}

	len = sizeof(buf);

	ret = read(fd, buf, len);
	if (ret < 0) {
		perror(files);
		close(fd);
		return 0;
	}
	if (ret >= len)
		ret = len - 1;
	buf[ret] = 0;

	ret = 1;

	p = strstr(buf, "HRTICK");
	if (p + 3 >= buf) {
		p -= 3;
		if (strncmp(p, "NO_HRTICK", 9) == 0) {
			ret = write(fd, "HRTICK", 6);
			if (ret != 6)
				ret = 0;
			else
				ret = 1;
		}
	}

	close(fd);
	return ret;
}

/**
 * mounted - test if a path is mounted via the given mount type
 * @path: The path to check is mounted
 * @magic: The magic number of the mount type.
 *
 * Returns -1 if the path does not exist.
 * Returns 0 if it is mounted but not of the given @magic type.
 * Returns 1 if mounted and the @magic type matches.
 */
static int mounted(const char *path, long magic)
{
	struct statfs st_fs;

	if (statfs(path, &st_fs) < 0)
		return -1;
	if ((long)st_fs.f_type != magic)
		return 0;
	return 1;
}

#define CGROUP_PATH "/sys/fs/cgroup"
#define CPUSET_PATH CGROUP_PATH "/cpuset"

/**
 * open_cpuset - open a file (usually a cpuset file)
 * @path: The path of the directory the file is in
 * @name: The name of the file in the path to open.
 *
 * Open a file, used to open cpuset files. This function simply is
 * made to open many files in the same directory.
 *
 * Returns the file descriptor of the opened file or less than zero
 * on error.
 */
static int open_cpuset(const char *path, const char *name)
{
	char buf[MAXPATH];
	struct stat st;
	int ret;
	int fd;

	buf[MAXPATH - 1] = 0;
	snprintf(buf, MAXPATH - 1, "%s/%s", path, name);

	ret = stat(buf, &st);
	if (ret < 0)
		return ret;

	fd = open(buf, O_WRONLY);
	return fd;
}

/**
 * mount_cpuset - Inialize the cpuset system
 *
 * Looks to see if cgroups are mounted, if it is not, then it mounts
 * the cgroup_root to /sys/fs/cgroup. Then the directory cpuset exists
 * and is mounted in that directory. If it is not, it is created and
 * mounted.
 *
 * The toplevel cpuset "cpu_exclusive" flag is set, this allows child
 * cpusets to set the flag too.
 *
 * The toplevel cpuset "load_balance" flag is cleared, letting the
 * child cpusets take over load balancing.
 */
static int mount_cpuset(void)
{
	struct stat st;
	int ret;
	int fd;

	/* Check if cgroups is already mounted. */
	ret = mounted(CGROUP_PATH, TMPFS_MAGIC);
	if (ret < 0)
		return ret;
	if (!ret) {
		ret = mount("cgroup_root", CGROUP_PATH, "tmpfs", 0, NULL);
		if (ret < 0)
			return ret;
	}
	ret = stat(CPUSET_PATH, &st);
	if (ret < 0) {
		ret = mkdir(CPUSET_PATH, 0755);
		if (ret < 0)
			return ret;
	}
	ret = mounted(CPUSET_PATH, CGROUP_SUPER_MAGIC);
	if (ret < 0)
		return ret;
	if (!ret) {
		ret = mount("cpuset", CPUSET_PATH, "cgroup", 0, "cpuset");
		if (ret < 0)
			return ret;
	}

	fd = open_cpuset(CPUSET_PATH, "cpuset.cpu_exclusive");
	if (fd < 0)
		return fd;
	ret = write(fd, "1", 2);
	close(fd);

	fd = open_cpuset(CPUSET_PATH, "cpuset.sched_load_balance");
	if (fd < 0)
		return fd;
	ret = write(fd, "0", 2);
	close(fd);

	return 0;
}

/*
 * CPUSET flags: used for creating cpusets
 *
 *  CPU_EXCLUSIVE - Set the cpu exclusive flag
 *  MEM_EXCLUSIVE - Set the mem exclusive flag
 *  ALL_TASKS - Move all tasks from the toplevel cpuset to this one
 *  TASKS - Supply a list of thread IDs to move to this cpuset
 *  CLEAR_LOADBALANCE - clear the loadbalance flag
 *  SET_LOADBALANCE - set the loadbalance flag
 *  CLONE_CHILDREN - set the clone_children flag
 */
enum {
	CPUSET_FL_CPU_EXCLUSIVE		= (1 << 0),
	CPUSET_FL_MEM_EXCLUSIVE		= (1 << 1),
	CPUSET_FL_ALL_TASKS		= (1 << 2),
	CPUSET_FL_TASKS			= (1 << 3),
	CPUSET_FL_CLEAR_LOADBALANCE	= (1 << 4),
	CPUSET_FL_SET_LOADBALANCE	= (1 << 5),
	CPUSET_FL_CLONE_CHILDREN	= (1 << 6),
};

/**
 * make_cpuset - create a cpuset
 * @name: The name of the cpuset
 * @cpus: A string list of cpus this set is for e.g. "1,3,4-7"
 * @mems: The memory nodes to use (usually just "0") (set to NULL to ignore)
 * @flags: See the CPUSET_FL_* flags above for information
 * @va_args: An array of tasks to move if TASKS flag is set.
 *
 * Creates a cpuset.
 *
 * If TASKS is set, then @va_args will be an array of PIDs to move from
 * the main cpuset, to this cpuset. The last element of the array must
 * be a zero, to stop the processing of arrays.
 *
 * Returns NULL on success, and a string to describe what went wrong on error.
 */
static const char *make_cpuset(const char *name, const char *cpus,
			       const char *mems, unsigned flags, ...)
{
	struct stat st;
	char path[MAXPATH];
	char buf[100];
	va_list ap;
	int ret;
	int fd;

	printf("Creating cpuset '%s'\n", name);
	snprintf(path, MAXPATH - 1, "%s/%s", CPUSET_PATH, name);
	path[MAXPATH - 1] = 0;

	ret = mount_cpuset();
	if (ret < 0)
		return "mount_cpuset";

	/* Only create the new cpuset directory if it does not yet exist */
	ret = stat(path, &st);
	if (ret < 0) {
		ret = mkdir(path, 0755);
		if (ret < 0)
			return "mkdir";
	}

	/* Assign the CPUs */
	fd = open_cpuset(path, "cpuset.cpus");
	if (fd < 0)
		return "cset";
	ret = write(fd, cpus, strlen(cpus));
	close(fd);
	if (ret < 0)
		return "write cpus";

	/* Assign the "mems" if it exists */
	if (mems) {
		fd = open_cpuset(path, "cpuset.mems");
		if (fd < 0)
			return "open mems";
		ret = write(fd, mems, strlen(mems));
		close(fd);
		if (ret < 0)
			return "write mems";
	}

	if (flags & CPUSET_FL_CPU_EXCLUSIVE) {
		fd = open_cpuset(path, "cpuset.cpu_exclusive");
		if (fd < 0)
			return "open cpu_exclusive";
		ret = write(fd, "1", 2);
		close(fd);
		if (ret < 0)
			return "write cpu_exclusive";
	}

	if (flags & (CPUSET_FL_CLEAR_LOADBALANCE | CPUSET_FL_SET_LOADBALANCE)) {
		fd = open_cpuset(path, "cpuset.sched_load_balance");
		if (fd < 0)
			return "open sched_load_balance";
		if (flags & CPUSET_FL_SET_LOADBALANCE)
			ret = write(fd, "1", 2);
		else
			ret = write(fd, "0", 2);
		close(fd);
		if (ret < 0)
			return "write sched_load_balance";
	}

	if (flags & CPUSET_FL_CLONE_CHILDREN) {
		fd = open_cpuset(path, "cgroup.clone_children");
		if (fd < 0)
			return "open clone_children";
		ret = write(fd, "1", 2);
		close(fd);
		if (ret < 0)
			return "write clone_children";
	}


	/* If TASKS flag is set, then an array of tasks is passed it */
	if (flags & CPUSET_FL_TASKS) {
		int *pids;
		int i;

		va_start(ap, flags);

		fd = open_cpuset(path, "tasks");
		if (fd < 0)
			return "open tasks";

		ret = 0;
		pids = va_arg(ap, int *);

		/* The array ends with pids[i] == 0 */
		for (i = 0; pids[i]; i++) {
			sprintf(buf, "%d ", pids[i]);
			ret = write(fd, buf, strlen(buf));
			if (ret < 0)
				break;
		}
		va_end(ap);
		close(fd);
		if (ret < 0) {
			fprintf(stderr, "Failed on task %d\n", pids[i]);
			return "write tasks";
		}
	}

	/* If ALL_TASKS flag is set, move all tasks from the top level cpuset */
	if (flags & CPUSET_FL_ALL_TASKS) {
		FILE *fp;
		int pid;

		fd = open_cpuset(path, "tasks");

		snprintf(path, MAXPATH - 1, "%s/tasks", CPUSET_PATH);
		if ((fp = fopen(path,"r")) == NULL) {
			close (fd);
			return "opening cpuset tasks";
		}

		while (fscanf(fp, "%d", &pid) == 1) {
			sprintf(buf, "%d", pid);
			ret = write(fd, buf, strlen(buf));
			/*
			 * Tasks can come and go, and some tasks are kernel
			 * threads that cannot be moved. The only error we care
			 * about is ENOSPC, as that means something went
			 * wrong that we did not expect.
			 */
			if (ret < 0 && errno == ENOSPC) {
				fclose(fp);
				close(fd);
				return "Can not move tasks";
			}
		}
		fclose(fp);
		close(fd);
	}

	return NULL;
}

/**
 * destroy_cpuset - tear down a cpuset that was created
 * @name: The name of the cpuset to destroy
 * @print: If the tasks being moved should be displayed
 *
 * Reads the tasks in the cpuset and moves them to the top level cpuset
 * then destroys the @name cpuset.
 */
static void destroy_cpuset(const char *name, int print)
{
	struct stat st;
	char path[MAXPATH];
	char buf[100];
	FILE *fp;
	int pid;
	int ret;
	int fd;
	int retry = 0;

	printf("Removing %s\n", name);

	/* Set path to the cpuset name that we will destroy */
	snprintf(path, MAXPATH - 1, "%s/%s", CPUSET_PATH, name);
	path[MAXPATH - 1] = 0;

	/* Make sure it exists! */
	ret = stat(path, &st);
	if (ret < 0)
		return;

 again:
	/*
	 * Append "/tasks" to the cpuset name, to read the tasks that are
	 * in this cpuset, that must be moved before destroying the cpuset.
	 */
	strncat(path, "/tasks", MAXPATH - 1);
	if ((fp = fopen(path,"r")) == NULL) {
		fprintf(stderr, "Failed opening %s\n", path);
		perror("fopen");
		return;
	}
	/* Set path to the toplevel cpuset tasks file */
	snprintf(path, MAXPATH - 1, "%s/tasks", CPUSET_PATH);
	path[MAXPATH - 1] = 0;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fclose(fp);
		fprintf(stderr, "Failed opening %s\n", path);
		perror("open");
		return;
	}

	/*
	 * Now fp points to the destroying cpuset tasks file, and
	 * fd is the toplevel cpuset file descriptor. Scan in the
	 * tasks that are in the cpuset that is being destroyed and
	 * write their pids into the toplevel cpuset.
	 */
	while (fscanf(fp, "%d", &pid) == 1) {
		sprintf(buf, "%d", pid);
		if (print)
			printf("Moving %d out of %s\n", pid, name);
		write(fd, buf, strlen(buf));
	}
	fclose(fp);
	close(fd);

	/* Reset the path name back to the cpuset to destroy */
	snprintf(path, MAXPATH - 1, "%s/%s", CPUSET_PATH, name);
	path[MAXPATH - 1] = 0;

	/* Sleep a bit to let all tasks migrate out of this cpuset. */
	sleep(1);

	ret = rmdir(path);
	if (ret < 0) {
		/*
		 * Sometimes there appears to be a delay, and tasks don't
		 * always move when you expect them to. Try 5 times, and
		 * give up after that.
		 */
		if (retry++ < 5)
			goto again;
		fprintf(stderr, "Failed to remove %s\n", path);
		perror("rmdir");
		if (retry++ < 5) {
			fprintf(stderr, "Trying again\n");
			goto again;
		}
	}
}

/**
 * teardown - Called atexit() to reset the system back to normal
 *
 * If cpusets were created, this destroys them and puts all tasks
 * back to the main cgroup.
 */
static void teardown(void)
{
	int fd;

	fd = open_cpuset(CPUSET_PATH, "cpuset.cpu_exclusive");
	if (fd >= 0) {
		write(fd, "0", 2);
		close(fd);
	}

	fd = open_cpuset(CPUSET_PATH, "cpuset.sched_load_balance");
	if (fd >= 0) {
		write(fd, "1", 2);
		close(fd);
	}

	destroy_cpuset(CPUSET_ALL, 0);
	destroy_cpuset(CPUSET_LOCAL, 1);
}

/**
 * bind_cpu - Set the affinity of a thread to a specific CPU.
 * @cpu: The CPU to bind to.
 *
 * Sets the current thread to have an affinity of a sigle CPU.
 * Does not work on SCHED_DEADLINE tasks.
 */
static void bind_cpu(int cpu)
{
	int ret;

	CPU_ZERO_S(cpuset_size, cpusetp);
	CPU_SET_S(cpu, cpuset_size, cpusetp);

	ret = sched_setaffinity(0, cpuset_size, cpusetp);
	if (ret < 0)
		perror("sched_setaffinity bind");
}

/**
 * unbind_cpu - Set the affinity of a task to all CPUs
 *
 * Sets the current thread to have an affinity for all CPUs.
 * Does not work on SCHED_DEADLINE tasks.
 */
static void unbind_cpu(void)
{
	int cpu;
	int ret;

	for (cpu = 0; cpu < cpu_count; cpu++)
		CPU_SET_S(cpu, cpuset_size, cpusetp);

	ret = sched_setaffinity(0, cpuset_size, cpusetp);
	if (ret < 0)
		perror("sched_setaffinity unbind");
}

/*
 * Used by set_prio, but can be used for any task not just current.
 */
static int set_thread_prio(pid_t pid, int prio)
{
	struct sched_param sp = { .sched_priority = prio };
	int policy = SCHED_FIFO;

	if (!prio)
		policy = SCHED_OTHER;

	/* set up our priority */
	return sched_setscheduler(pid, policy, &sp);
}

/**
 * set_prio - Set the SCHED_FIFO priority of a thread
 * @prio: The priority to set a thread to
 *
 * Converts a SCHED_OTHER task into a SCHED_FIFO task and sets
 * its priority to @prio. If @prio is zero, then it converts
 * a SCHED_FIFO task back to a SCHED_OTHER task.
 *
 * Returns 0 on success, otherwise it failed.
 */
static int set_prio(int prio)
{
	return set_thread_prio(0, prio);
}

/* done - set when the test is complete to have all threads stop */
static int done;

/* fail - set during setup if any thread fails to initialize. */
static int fail;

/**
 * get_time_us - Git the current clock time in microseconds
 *
 * Returns the current clock time in microseconds.
 */
static u64 get_time_us(void)
{
	struct timespec ts;
	u64 time;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	time = ts.tv_sec * 1000000;
	time += ts.tv_nsec / 1000;

	return time;
}

/**
 * run_loops - execute a number of loops to perform
 * @loops: The number of loops to execute.
 *
 * Calculates prime numbers, because what else should we do?
 */
static u64 run_loops(struct sched_data *data, u64 loops)
{
	u64 start = get_time_us();
	u64 end;
	u64 i;
	u64 prime;
	u64 cnt = 2;
	u64 result;

	prime = data->prime;

	for (i = 0; i < loops; i++) {
		if (cnt > prime / 2) {
			data->prime = prime;
			prime++;
			cnt = 2;
		}
		result = prime / cnt;
		if (result * cnt == prime) {
			prime++;
			cnt = 2;
		} else
			cnt++;
	}

	/* Memory barrier */
	asm("":::"memory");

	end = get_time_us();
	return end - start;
}

/* Helper function for read_ctx_switchs */
static int get_value(const char *line)
{
	const char *p;

	for (p = line; isspace(*p); p++)
		;
	if (*p != ':')
		return -1;
	p++;
	for (; isspace(*p); p++)
		;
	return atoi(p);
}

/* Helper function for read_ctx_switchs */
static int update_value(const char *line, int *val, const char *name)
{
	int ret;

	if (strncmp(line, name, strlen(name)) == 0) {
		ret = get_value(line + strlen(name));
		if (ret < 0)
			return 0;
		*val = ret;
		return 1;
	}
	return 0;
}

/**
 * read_ctx_switches - read the scheduling information of a task
 * @vol: Output to place number of voluntary schedules
 * @nonvol: Output to place number of non-voluntary schedules (preemption)
 * @migrate: Output to place the number of times the task migrated
 *
 * Reads /proc/<pid>/sched to get the statistics of the thread.
 *
 * For info only.
 */
static int read_ctx_switches(int *vol, int *nonvol, int *migrate)
{
	static int vol_once, nonvol_once;
	const char *vol_name = "nr_voluntary_switches";
	const char *nonvol_name = "nr_involuntary_switches";
	const char *migrate_name = "se.nr_migrations";
	char file[1024];
	char buf[1024];
	char *pbuf;
	int pid;
	size_t *pn;
	size_t n;
	FILE *fp;
	int r;

	pid = gettid();
	snprintf(file, 1024, "/proc/%d/sched", pid);
	fp = fopen(file, "r");
	if (!fp) {
		snprintf(file, 1024, "/proc/%d/status", pid);
		fp = fopen(file, "r");
		if (!fp) {
			fprintf(stderr, "could not open %s", file);
			return -1;
		}
		vol_name = "voluntary_ctxt_switches";
		nonvol_name = "nonvoluntary_ctxt_switches";
	}

	*vol = *nonvol = *migrate = -1;

	n = 1024;
	pn = &n;
	pbuf = buf;

	while ((r = getline(&pbuf, pn, fp)) >= 0) {

		if (update_value(buf, vol, vol_name))
			continue;

		if (update_value(buf, nonvol, nonvol_name))
			continue;

		if (update_value(buf, migrate, migrate_name))
			continue;
	}
	fclose(fp);

	if (!vol_once && *vol == -1) {
		vol_once++;
		fprintf(stderr, "Warning, could not find voluntary ctx switch count\n");
	}
	if (!nonvol_once && *nonvol == -1) {
		nonvol_once++;
		fprintf(stderr, "Warning, could not find nonvoluntary ctx switch count\n");
	}

	return 0;
}

/**
 * do_runtime - Run a loop to simulate a specific task
 * @tid: The thread ID
 * @data: The sched_data descriptor
 * @period: The time of the last period.
 *
 * Returns the expected next period.
 *
 * This simulates some task that needs to be completed within the deadline.
 *
 * Input:
 *  @data->deadline_us - to calculate next peroid
 *  @data->loops_per_peroid - to loop this amount of time
 *
 * Output:
 *  @data->total_adjust - Time adjusted for starting a period early
 *  @data->nr_adjusted - Number of times adjusted
 *  @data->missed_deadlines - Counter of missed deadlines
 *  @data->missed_periods - Counter of missed periods (started late)
 *  @data->max_time - Maximum time it took to complete the loops
 *  @data->min_time - Minimum time it took to complete the loops
 *  @data->last_time - How much time it took to complete loops this time
 *  @data->total_time - Total time it took to complete all loops
 *  @data->nr_periods - Number of periods that were executed.
 */
static u64 do_runtime(long tid, struct sched_data *data, u64 period)
{
	u64 next_period = period + data->deadline_us;
	u64 now = get_time_us();
	u64 end;
	u64 diff;
	u64 time;

	/*
	 * next_period is our new deadline. If now is passed that point
	 * we missed a period.
	 */
	if (now > next_period) {
		ftrace_write(data->buff,
			     "Missed a period start: %lld next: %lld now: %lld\n",
			     period, next_period, now);
		/* See how many periods were missed. */
		while (next_period < now) {
			next_period += data->deadline_us;
			data->missed_periods++;
		}
	} else if (now < period) {
		u64 delta = period - now;
		/*
		 * Currently, there's no way to find when the period actually
		 * does begin. If the first runtime starts late, due to another
		 * deadline task with a shorter deadline running, then it is
		 * possible that the next period comes in quicker than we
		 * expect it to.
		 *
		 * Adjust the period to start at now, and record the shift.
		 */
		ftrace_write(data->buff,
			     "Adjusting period: now: %lld period: %lld delta:%lld%s\n",
			     now, period, delta, delta > data->deadline_us / 2 ?
			     " HUGE ADJUSTMENT" : "");
		data->total_adjust += delta;
		data->nr_adjust++;
		period = now;
		next_period = period + data->deadline_us;
	}

	ftrace_write(data->buff, "start at %lld off=%lld (period=%lld next=%lld)\n",
		     now, now - period, period, next_period);

	/* Run the simulate task (loops) */
	time = run_loops(data, data->loops_per_period);

	end = get_time_us();

	/* Did we make our deadline? */
	if (end > next_period) {
		ftrace_write(data->buff,
			     "Failed runtime by %lld\n", end - next_period);
		data->missed_deadlines++;
		/*
		 * We missed our deadline, which means we entered the
		 * next period. Move it forward one, if we moved it too
		 * much, then the next interation will adjust.
		 */
		next_period += data->deadline_us;
	}


	diff = end - now;
	if (diff > data->max_time)
		data->max_time = diff;
	if (!data->min_time || diff < data->min_time)
		data->min_time = diff;

	data->last_time = time;
	data->total_time += diff;
	data->nr_periods++;
	ftrace_write(data->buff,
		     "end at %lld diff: %lld run loops: %lld us\n", end, diff, time);

	return next_period;
}

/**
 * run_deadline - Run deadline thread
 * @data: sched_data descriptor
 *
 * This is called by pthread_create() and executes the sched deadline
 * task. @data has the following:
 *
 * Input:
 *  @data->runtime_us: The amount of requested runtime in microseconds
 *  @data->deadline_us: The requested deadline in microseconds
 *  @data->loops_per_period: The number of loops to make during its runtime
 *
 * Output:
 *  @data->tid: The thread ID
 *  @data->vol: The number of times the thread voluntarily scheduled out
 *  @data->nonvol: The number of times the thread non-voluntarily scheduled out
 *  @data->migrate: The number of times the thread migrated across CPUs.
 */
void *run_deadline(void *data)
{
	struct sched_data *sched_data = data;
	struct sched_attr attr;
	int vol, nonvol, migrate;
	long tid = gettid();
	void *heap;
	u64 period;
	int ret;

	/*
	 * The internal glibc vsnprintf() used by ftrace_write()
	 * may alloc more space to do conversions. Alloc a bunch of
	 * memory and free it, and hopefully glibc doesn't return that
	 * back to the system (we did do an mlockall after all).
	 */
	heap = malloc(1000000);
	if (!heap) {
		perror("malloc");
		fail = 1;
		pthread_barrier_wait(&barrier);
		pthread_exit("Failed to alloc heap");
		return NULL;
	}
	free(heap);

	printf("deadline thread %ld\n", tid);

	sched_data->tid = tid;
	sched_data->prime = 2;

	ret = sched_getattr(0, &attr, sizeof(attr), 0);
	if (ret < 0) {
		fprintf(stderr, "[%ld]", tid);
		perror("sched_getattr");
		fail = 1;
		pthread_barrier_wait(&barrier);
		pthread_exit("Failed sched_getattr");
		return NULL;
	}

	pthread_barrier_wait(&barrier);

	if (fail)
		return NULL;

	attr.sched_policy = SCHED_DEADLINE;
	attr.sched_runtime = sched_data->runtime_us * 1000;
	attr.sched_deadline = sched_data->deadline_us * 1000;

	printf("thread[%d] runtime=%lldus deadline=%lldus loops=%lld\n",
	       gettid(), sched_data->runtime_us,
	       sched_data->deadline_us, sched_data->loops_per_period);

	pthread_barrier_wait(&barrier);

	ret = sched_setattr(0, &attr, 0);
	if (ret < 0) {
		done = 0;
		fprintf(stderr, "[%ld]", tid);
		perror("sched_setattr");
		fail = 1;
		pthread_barrier_wait(&barrier);
		pthread_exit("Failed sched_setattr");
		return NULL;
	}

	pthread_barrier_wait(&barrier);

	if (fail)
		return NULL;

	sched_yield();
	period = get_time_us();
	
	while (!done) {
		period = do_runtime(tid, sched_data, period);
		sched_yield();
	}
	ret = sched_getattr(0, &attr, sizeof(attr), 0);
	if (ret < 0) {
		perror("sched_getattr");
		pthread_exit("Failed second sched_getattr");
	}

	read_ctx_switches(&vol, &nonvol, &migrate);

	sched_data->vol = vol;
	sched_data->nonvol = nonvol;
	sched_data->migrate = migrate;

	return NULL;
}

/**
 * run_rt_spin - the Real-Time task spinner
 * @data: The sched_data descriptor
 *
 * This function is called as a thread function. It will read @data->prio
 * and set its priority base on that parameter. It sets @data->tid to the
 * thread ID. Then after waiting through pthread barriers to sync with
 * the main thread as well as with sched deadline threads, it will
 * run in a tight loop until the global variable "done" is set.
 */
void *run_rt_spin(void *data)
{
	struct sched_data *sched_data = data;
	long tid = gettid();

	sched_data->tid = tid;

	if (set_prio(sched_data->prio) < 0) {
		fail = 1;
		pthread_barrier_wait(&barrier);
		pthread_exit("Failed setting prio");
		return NULL;
	}

	pthread_barrier_wait(&barrier);

	if (fail)
		return NULL;

	pthread_barrier_wait(&barrier);

	if (fail)
		return NULL;

	pthread_barrier_wait(&barrier);

	if (fail)
		return NULL;

	while (!done) {
		get_time_us();
	}

	return NULL;
}

struct cpu_list {
	struct cpu_list	*next;
	int		start_cpu;
	int		end_cpu;
};

/**
 * add_cpus - Add cpus to cpu_list based on the passed in range
 * @cpu_list: The cpu list to add to
 * @start_cpu: The start of the range to add
 * @end_cpu: The end of the range to add.
 *
 * Adds a sorted unique item into @cpu_list based on @start_cpu and @end_cpu.
 * It removes duplicates in @cpu_list, and will even merge lists if a
 * new range is entered that will fill a gap. That is, if @cpu_list has
 * "1-3" and "6-7", and @start_cpu is 4 and @end_cpu is 5, it will combined
 * the two elements into a single list item of "1-7".
 */
static void add_cpus(struct cpu_list **cpu_list, int start_cpu, int end_cpu)
{
	struct cpu_list *list;

	while (*cpu_list && (*cpu_list)->end_cpu + 1 < start_cpu)
		cpu_list = &(*cpu_list)->next;

	if (!*cpu_list) {
		*cpu_list = malloc(sizeof(struct cpu_list));
		(*cpu_list)->start_cpu = start_cpu;
		(*cpu_list)->end_cpu = end_cpu;
		(*cpu_list)->next = NULL;
		return;
	}

	/* Look to concatinate */
	if (end_cpu > (*cpu_list)->start_cpu &&
	    start_cpu <= (*cpu_list)->end_cpu + 1) {
		if (start_cpu < (*cpu_list)->start_cpu)
			(*cpu_list)->start_cpu = start_cpu;
		list = (*cpu_list)->next;
		while (list && list->start_cpu <= end_cpu + 1) {
			(*cpu_list)->end_cpu = list->end_cpu;
			(*cpu_list)->next = list->next;
			free(list);
			list = (*cpu_list)->next;
		}
		if ((*cpu_list)->end_cpu < end_cpu)
			(*cpu_list)->end_cpu = end_cpu;
		return;
	}

	/* Check for overlaps */
	if (end_cpu >= (*cpu_list)->start_cpu - 1) {
		(*cpu_list)->start_cpu = start_cpu;
		return;
	}

	list = malloc(sizeof(struct cpu_list));
	list->start_cpu = start_cpu;
	list->end_cpu = end_cpu;
	list->next = (*cpu_list)->next;
	(*cpu_list)->next = list;
}

/**
 * count_cpus - Return the number of CPUs in a list
 * @cpu_list: The list of CPUs to count
 *
 * Reads the list of CPUs in @cpu_list. It als will free the
 * list as it reads it, so this can only be called once on @cpu_list.
 * It also checks if the CPUs in @cpu_list are less than cpu_count
 * (the number of discovered CPUs).
 *
 * Returns the number of CPUs in @cpu_list, or -1 if any CPU in
 * @cpu_list is greater or equal to cpu_count.
 */
static int count_cpus(struct cpu_list *cpu_list)
{
	struct cpu_list *list;
	int cpus = 0;
	int fail = 0;

	while (cpu_list) {
		list = cpu_list;
		cpus += (list->end_cpu - list->start_cpu) + 1;
		if (list->end_cpu >= cpu_count)
			fail = 1;
		cpu_list = list->next;
		free(list);
	}
	return fail ? -1 : cpus;
}

/**
 * append_cpus - Append a set of consecutive cpus to a string
 * @buf: The string to append to
 * @start: The cpu to start at.
 * @end: The cpu to end at.
 * @comma: The "," or "" to append before the cpu list.
 * @total: The total length of buf.
 *
 * Realloc @buf to include @comma@start-@end.
 * Updates @total to the new length of @buf.
 */
static char *append_cpus(char *buf, int start, int end,
			 const char *comma, int *total)
{
	int len;

	if (start == end) {
		len = snprintf(NULL, 0, "%s%d", comma, start);
		buf = realloc(buf, *total + len + 1);
		buf[*total] = 0;
		snprintf(buf + *total, len + 1, "%s%d", comma, start);
	} else {
		len = snprintf(NULL, 0, "%s%d-%d", comma, start, end);
		buf = realloc(buf, *total + len + 1);
		buf[*total] = 0;
		snprintf(buf + *total, len + 1, "%s%d-%d", comma,
			 start, end);
	}
	*total += len;
	return buf;
}

/**
 * make_new_list - convert cpu_list into a string
 * @cpu_list: The list of CPUs to include
 * @buf: The pointer to the allocated string to return
 *
 * Reads @cpu_list which contains a link list of consecutive
 * CPUs, and returns the combined list in @buf.
 * If cpu_list has "1", "3" and "6-8", buf would return
 * "1,3,6-8"
 */
static void make_new_list(struct cpu_list *cpu_list, char **buf)
{
	char *comma = "";
	int total = 0;

	while (cpu_list) {
		*buf = append_cpus(*buf, cpu_list->start_cpu, cpu_list->end_cpu,
				   comma, &total);
		comma = ",";
		cpu_list = cpu_list->next;
	}
}

/**
 * make_other_cpu_list - parse cpu list and return all other CPUs
 * @setcpu: string listing the CPUs to exclude
 * @cpus: The buffer to return the list of CPUs not in setcpu.
 *
 * @setcpu is expected to be compressed by calc_nr_cpus().
 *
 * Reads @setcpu and uses cpu_count (number of all CPUs), to return
 * a list of CPUs not included in @setcpu. For example, if
 * @setcpu is "1-5" and cpu_count is 8, then @cpus would contain
 * "0,6-7".
 */
static void make_other_cpu_list(const char *setcpu, char **cpus)
{
	const char *p = setcpu;
	const char *comma = "";
	int curr_cpu = 0;
	int cpu;
	int total = 0;

	while (*p && curr_cpu < cpu_count) {
		cpu = atoi(p);
		if (cpu > curr_cpu) {
			*cpus = append_cpus(*cpus, curr_cpu, cpu - 1,
					    comma, &total);
			comma = ",";
		}
		while (isdigit(*p))
			p++;
		if (*p == '-') {
			p++;
			cpu = atoi(p);
			while (isdigit(*p))
				p++;
		}
		curr_cpu = cpu + 1;
		if (*p)
			p++;
	}

	if (curr_cpu < cpu_count) {
		*cpus = append_cpus(*cpus, curr_cpu, cpu_count - 1,
				    comma, &total);
	}
}

/**
 * calc_nr_cpus - parse cpu list for list of cpus.
 * @setcpu: string listing the CPUs to include
 * @buf: The buffer to return as a compressed list.
 *
 * Reads @setcpu and removes duplicates, it also sets @buf to be
 * a consolidated list. For example, if @setcpu is "1,2,4,3-5"
 * @buf would become "1-5" and 5 would be returned.
 *
 * Returns the number of cpus listed in @setcpu.
 */
static int calc_nr_cpus(const char *setcpu, char **buf)
{
	struct cpu_list *cpu_list = NULL;
	const char *p;
	int end_cpu;
	int cpu;

	for (p = setcpu; *p; ) {
		cpu = atoi(p);
		if (cpu < 0 || (!cpu && *p != '0'))
			goto err;

		while (isdigit(*p))
			p++;
		if (*p == '-') {
			p++;
			end_cpu = atoi(p);
			if (end_cpu < cpu || (!end_cpu && *p != '0'))
				goto err;
			while (isdigit(*p))
				p++;
		} else
			end_cpu = cpu;

		add_cpus(&cpu_list, cpu, end_cpu);
		if (*p == ',')
			p++;
	}

	make_new_list(cpu_list, buf);
	return count_cpus(cpu_list);
 err:
	/* Frees the list */
	count_cpus(cpu_list);
	return -1;
}

static const char *join_thread(pthread_t *thread)
{
	void *result;

	pthread_join(*thread, &result);
	return result;
}

static void do_sleep(u64 next)
{
	struct timespec req;

	req.tv_nsec = next * 1000;
	req.tv_sec = 0;
	while (req.tv_nsec > 1000000000UL) {
		req.tv_nsec -= 1000000000UL;
		req.tv_sec++;
	}
	nanosleep(&req, NULL);
}

/**
 * calculate_loops_per_ms - calculate the number of loops per ms
 * @overhead: Return the overhead of the call around run_loops()
 *
 * Runs the do_runtime() to see how long it takes. That returns
 * how long the loops took (data->last_time), and the overhead can
 * be calculated by that diff.
 *
 * Returns the length of time it took to run for 1000 loops.
 */
static u64 calculate_loops_per_ms(u64 *overhead)
{
	struct sched_data sd = { };
	u64 test_loops = 100000;
	u64 loops;
	u64 diff;
	u64 odiff;
	u64 start;
	u64 end;

	sd.prime = 2;

	/* Sleep 1ms to help flush a bit of cache */
	do_sleep(1000);

	start = run_loops(&sd, test_loops);

	sd.deadline_us = start * 2;
	sd.runtime_us = start;
	sd.loops_per_period = test_loops;

	/* Again try to dirty some cache */
	do_sleep(1000);

	start = get_time_us();
	do_runtime(0, &sd, start + sd.deadline_us);
	end = get_time_us();

	diff = end - start;

	/*
	 * Based on the time it took to run test_loops, figure
	 * out how many loops it may take to run for 1000us.
	 *
	 * last_time / test_loops = 1000us / loops
	 *               or
	 * loops = test_loops * 1000us / last_time
	 */

	loops = 1000ULL * test_loops / sd.last_time;

	printf("%lld test loops took %lldus total (%lld internal)\n"
	       "calculated loops for 1000us=%lld\n",
	       test_loops, diff, sd.last_time, loops);

	sd.deadline_us = 2000;
	sd.runtime_us = 1000;
	sd.loops_per_period = loops;

	test_loops = loops;

	do_sleep(1000);

	start = get_time_us();
	do_runtime(0, &sd, start + sd.deadline_us);
	end = get_time_us();

	odiff = end - start;

	/*
	 * Use this new calcualtion to recalculate the number of loops
	 * for 1000us
	 */
	loops = 1000ULL * loops / sd.last_time;

	*overhead = odiff - sd.last_time;

	printf("%lld test loops took %lldus total (%lld internal)\n"
	       "New calculated loops for 1000us=%lld\n"
	       "Diff from last calculation: %lld loops\n",
	       test_loops, odiff, sd.last_time, loops, loops - test_loops);

	return loops;
}

int main (int argc, char **argv)
{
	struct sched_data *sched_data;
	struct sched_data *sd;
	struct sched_data rt_sched_data;
	const char *res;
	const char *setcpu = NULL;
	char *setcpu_buf = NULL;
	char *allcpu_buf = NULL;
	pthread_t *thread;
	pthread_t rt_thread;
	unsigned int interval = 1000;
	unsigned int step = 500;
	u64 loop_time;
	u64 loops;
	u64 runtime;
	u64 overhead;
	u64 start_period;
	u64 end_period;
	int nr_cpus;
	int all_cpus = 1;
	int run_percent = 100;
	int percent = 80;
	int rt_task = 0;
	int i;
	int c;

	cpu_count = sysconf(_SC_NPROCESSORS_CONF);
	if (cpu_count < 1) {
		fprintf(stderr, "Can not calculate number of CPUS\n");
		exit(-1);
	}

	while ((c = getopt(argc, argv, "+hbr:c:i:p:P:t:s:")) >= 0) {
		switch (c) {
		case 'b':
			all_cpus = 0;
			break;
		case 'c':
			all_cpus = 0;
			setcpu = optarg;
			break;
		case 'i':
			interval = atoi(optarg);
			break;
		case 'p':
			percent = atoi(optarg);
			break;
		case 'P':
			run_percent = atoi(optarg);
			break;
		case 's':
			step = atoi(optarg);
			break;
		case 't':
			nr_threads = atoi(optarg);
			break;
		case 'r':
			rt_task = atoi(optarg);
			break;
		case 'h':
		default:
			usage(argv);
		}
	}

	if (rt_task < 0 || rt_task > 98) {
		fprintf(stderr, "RT task can only be from 1 to 98\n");
		exit(-1);
	}

	if (percent < 1 || percent > 100 || run_percent < 1 || run_percent > 100) {
		fprintf(stderr, "Percent must be between 1 and 100\n");
		exit(-1);
	}

	if (setcpu) {
		nr_cpus = calc_nr_cpus(setcpu, &setcpu_buf);
		if (nr_cpus < 0) {
			fprintf(stderr, "Invalid cpu input '%s'\n", setcpu);
			exit(-1);
		}
	} else
		nr_cpus = 1;

	if (all_cpus)
		nr_cpus = cpu_count;

	if (cpu_count == nr_cpus)
		all_cpus = 1;

	/* -b has us bind to the last CPU. */
	if (!all_cpus && !setcpu) {
		setcpu_buf = malloc(12);
		if (!setcpu_buf) {
			perror("malloc");
			exit(-1);
		}
		sprintf(setcpu_buf, "%d", cpu_count - 1);
		setcpu = setcpu_buf;
	}

	/*
	 * Now the amount of bandwidth each tasks takes will be
	 * percent * nr_cpus / nr_threads. Now if nr_threads is
	 * But the amount of any one thread can not be more than
	 * 90 of the CPUs.
	 */
	percent = (percent * nr_cpus) / nr_threads;
	if (percent > 90)
		percent = 90;

	cpusetp = CPU_ALLOC(cpu_count);
	cpuset_size = CPU_ALLOC_SIZE(cpu_count);
	if (!cpusetp) {
		perror("allocating cpuset");
		exit(-1);
	}

	setup_ftrace_marker();

	thread = calloc(nr_threads, sizeof(*thread));
	sched_data = calloc(nr_threads, sizeof(*sched_data));
	if (!thread || !sched_data) {
		perror("allocating threads");
		exit(-1);
	}

	if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
		perror("mlockall");
	}

	/*
	 * Run at prio 99 bound to the last CPU, and try to calculate
	 * the time it takes to run the loops.
	 */
	set_prio(99);
	bind_cpu(cpu_count - 1);

	loops = calculate_loops_per_ms(&overhead);

	printf("Setup:\n");
	printf(" percent per task:%d", percent);
	if (run_percent < 100)
		printf(" run-percent:%d", run_percent);
	printf(" nr_cpus:%d", nr_cpus);
	if (setcpu)
		printf(" (%s)", setcpu);
	printf(" loops:%lld overhead:%lldus\n", loops, overhead);

 again:
	/* Set up the data while sill in SCHED_FIFO */
	for (i = 0; i < nr_threads; i++) {
		sd = &sched_data[i];
		/*
		 * Interval is the deadline/period
		 * The runtime is the percentage of that period.
		 */
		runtime = interval * percent / 100;
		if (runtime < overhead) {
			fprintf(stderr, "Run time too short: %lld us\n",
				runtime);
			fprintf(stderr, "Read context takes %lld us\n",
				overhead);
			exit(-1);
		}
		if (runtime < 2000) {
			/*
			 * If the runtime is less than 2ms, then we better
			 * have HRTICK enabled.
			 */
			if (!setup_hr_tick()) {
				fprintf(stderr, "For less that 2ms run times, you need to\n"
					"have HRTICK enabled in debugfs/sched_features\n");
				exit(-1);
			}
		}
		sd->runtime_us = runtime;
		/* Account for the reading of context switches */
		runtime -= overhead;
		/*
		 * loops is # of loops per ms, convert to us and
		 * take 5% off of it.
		 *  loops * %run_percent / 1000
		 */
		loop_time = runtime * run_percent / 100;
		sd->loops_per_period = loop_time * loops / 1000;

		sd->deadline_us = interval;

		/* Make sure that we can make our deadlines */
		start_period = get_time_us();
		do_runtime(gettid(), sd, start_period);
		end_period = get_time_us();
		if (end_period - start_period > sd->runtime_us) {
			printf("Failed to perform task within runtime: Missed by %lld us\n",
				end_period - start_period - sd->runtime_us);
			overhead += end_period - start_period - sd->runtime_us;
			printf("New overhead=%lldus\n", overhead);
			goto again;
		}

		printf("  Tested at %lldus of %lldus\n",
		       end_period - start_period, sd->runtime_us);

		interval += step;
	}

	set_prio(0);

	unbind_cpu();

	pthread_barrier_init(&barrier, NULL, nr_threads + 1 + !!rt_task);

	for (i = 0; i < nr_threads; i++) {
		sd = &sched_data[i];
		pthread_create(&thread[i], NULL, run_deadline, sd);
	}

	if (rt_task) {
		/* Make sure we are a higher priority than the spinner */
		set_prio(rt_task + 1);

		rt_sched_data.prio = rt_task;
		pthread_create(&rt_thread, NULL, run_rt_spin, &rt_sched_data);
	}

	pthread_barrier_wait(&barrier);

	if (fail) {
		exit(-1);
	}

	if (!all_cpus) {
		int *pids;

		atexit(teardown);

		make_other_cpu_list(setcpu, &allcpu_buf);

		res = make_cpuset(CPUSET_ALL, allcpu_buf, "0",
				  CPUSET_FL_SET_LOADBALANCE |
				  CPUSET_FL_CLONE_CHILDREN |
				  CPUSET_FL_ALL_TASKS);
		if (res) {
			perror(res);
			exit(-1);
		}

		pids = calloc(nr_threads + !!rt_task + 1, sizeof(int));
		if (!pids) {
			perror("Allocating pids");
			exit(-1);
		}

		for (i = 0; i < nr_threads; i++)
			pids[i] = sched_data[i].tid;
		if (rt_task)
			pids[i++] = rt_sched_data.tid;

		res = make_cpuset(CPUSET_LOCAL, setcpu, "0",
				  CPUSET_FL_CPU_EXCLUSIVE |
				  CPUSET_FL_SET_LOADBALANCE |
				  CPUSET_FL_CLONE_CHILDREN |
				  CPUSET_FL_TASKS, pids);
		free(pids);
		if (res) {
			perror(res);
			fprintf(stderr, "Check if other cpusets exist that conflict\n");
			exit(-1);
		}

		system("cat /sys/fs/cgroup/cpuset/my_cpuset/tasks");
	}

	pthread_barrier_wait(&barrier);

	if (fail)
		exit(-1);

	pthread_barrier_wait(&barrier);

	if (!fail)
		sleep(10);

	done = 1;
	if (rt_task) {
		res = join_thread(&rt_thread);
		if (res)
			printf("RT Thread failed: %s\n", res);
	}

	for (i = 0; i < nr_threads; i++) {

		sd = &sched_data[i];

		res = join_thread(&thread[i]);
		if (res) {
			printf("Thread %d failed: %s\n", i, res);
			continue;
		}

		printf("\n[%d]\n", sd->tid);
		printf("missed deadlines  = %d\n", sd->missed_deadlines);
		printf("missed periods    = %d\n", sd->missed_periods);
		printf("Total adjustments = %lld us\n", sd->total_adjust);
		printf("# adjustments = %lld avg: %lld us\n",
		       sd->nr_adjust, sd->total_adjust / sd->nr_adjust);
		printf("deadline   : %lld us\n", sd->deadline_us);
		printf("runtime    : %lld us\n", sd->runtime_us);
		printf("nr_periods : %lld\n", sd->nr_periods);
		printf("max_time: %lldus", sd->max_time);
		printf("\tmin_time: %lldus", sd->min_time);
		printf("\tavg_time: %lldus\n", sd->total_time / sd->nr_periods);
		printf("ctx switches vol:%d nonvol:%d migration:%d\n",
		       sd->vol, sd->nonvol, sd->migrate);
		printf("highes prime: %lld\n", sd->prime);
		printf("\n");
	}

	if (setcpu_buf)
		free(setcpu_buf);
	free(thread);
	free(sched_data);

	CPU_FREE(cpusetp);

	return 0;
}
