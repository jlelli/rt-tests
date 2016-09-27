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
#include <signal.h>

#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#include <linux/unistd.h>
#include <linux/magic.h>

#ifdef __i386__
#ifndef __NR_sched_setattr
#define __NR_sched_setattr		351
#endif
#ifndef __NR_sched_getattr
#define __NR_sched_getattr		352
#endif
#ifndef __NR_getcpu
#define __NR_getcpu			309
#endif
#else /* x86_64 */
#ifndef __NR_sched_setattr
#define __NR_sched_setattr		314
#endif
#ifndef __NR_sched_getattr
#define __NR_sched_getattr		315
#endif
#ifndef __NR_getcpu
#define __NR_getcpu			309
#endif
#endif /* i386 or x86_64 */
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE		6
#endif

#define _STR(x) #x
#define STR(x) _STR(x)
#ifndef MAXPATH
#define MAXPATH 1024
#endif

#define CPUSET_ALL	"my_cpuset_all"
#define CPUSET_LOCAL	"my_cpuset"

#define gettid() syscall(__NR_gettid)
#define sched_setattr(pid, attr, flags) syscall(__NR_sched_setattr, pid, attr, flags)
#define sched_getattr(pid, attr, size, flags) syscall(__NR_sched_getattr, pid, attr, size, flags)
#define getcpu(cpup, nodep, unused) syscall(__NR_getcpu, cpup, nodep, unused)

typedef unsigned long long u64;
typedef unsigned int u32;
typedef int s32;

/* Struct to transfer parameters to the thread */
struct thread_param {
	u64 runtime_us;
	u64 deadline_us;

	int mode;
	int timermode;
	int signal;
	int clock;
	unsigned long max_cycles;
	struct thread_stat *stats;
	unsigned long interval;
	int cpu;
	int node;
	int tnum;
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
	long *outliers;
	pthread_t thread;
	int threadstarted;
	int tid;
	long reduce;
	long redmax;
	long cycleofmax;
	long hist_overflow;
	long num_outliers;
};

struct sched_data {
	u64 runtime_us;
	u64 deadline_us;

	int bufmsk;

	struct thread_stat stat;

	char buff[BUFSIZ+1];
};

struct sched_attr {
	u32 size;

	u32 sched_policy;
	u64 sched_flags;

	/* SCHED_NORMAL, SCHED_BATCH */
	s32 sched_nice;

	/* SCHED_FIFO, SCHED_RR */
	u32 sched_priority;

	/* SCHED_DEADLINE */
	u64 sched_runtime;
	u64 sched_deadline;
	u64 sched_period;
};

static int shutdown;

static pthread_barrier_t barrier;

static int cpu_count;
static int all_cpus;

static int nr_threads;
static int use_nsecs;

static int mark_fd;

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

#if 0
static int my_sprintf(char *buf, int size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
#endif

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

static void setup_ftrace_marker(void)
{
	struct stat st;
	const char *debugfs = find_debugfs();
	char files[strlen(debugfs) + 14];
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
	perror(files);
	if (fd < 0)
		return 0;

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

enum {
	CPUSET_FL_CPU_EXCLUSIVE		= (1 << 0),
	CPUSET_FL_MEM_EXCLUSIVE		= (1 << 1),
	CPUSET_FL_ALL_TASKS		= (1 << 2),
	CPUSET_FL_TASKS			= (1 << 3),
	CPUSET_FL_CLEAR_LOADBALANCE	= (1 << 4),
	CPUSET_FL_SET_LOADBALANCE	= (1 << 5),
	CPUSET_FL_CLONE_CHILDREN	= (1 << 6),
};

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

	ret = stat(path, &st);
	if (ret < 0) {
		ret = mkdir(path, 0755);
		if (ret < 0)
			return "mkdir";
	}

	fd = open_cpuset(path, "cpuset.cpus");
	if (fd < 0)
		return "cset";
	ret = write(fd, cpus, strlen(cpus));
	close(fd);
	if (ret < 0)
		return "write cpus";

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


	if (flags & CPUSET_FL_TASKS) {
		int *pids;
		int i;

		va_start(ap, flags);

		fd = open_cpuset(path, "tasks");
		if (fd < 0)
			return "open tasks";

		ret = 0;
		pids = va_arg(ap, int *);
		for (i = 0; pids[i]; i++) {
			sprintf(buf, "%d ", pids[i]);
			ret = write(fd, buf, strlen(buf));
		}
		va_end(ap);
		close(fd);
		if (ret < 0) {
			fprintf(stderr, "Failed on task %d\n", pids[i]);
			return "write tasks";
		}
	}

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
			 * Tasks can come and go, the only error we care
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
	snprintf(path, MAXPATH - 1, "%s/%s", CPUSET_PATH, name);
	path[MAXPATH - 1] = 0;

	ret = stat(path, &st);
	if (ret < 0)
		return;

 again:
	strncat(path, "/tasks", MAXPATH - 1);
	if ((fp = fopen(path,"r")) == NULL) {
		fprintf(stderr, "Failed opening %s\n", path);
		perror("fopen");
		return;
	}
	snprintf(path, MAXPATH - 1, "%s/tasks", CPUSET_PATH);
	path[MAXPATH - 1] = 0;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fclose(fp);
		fprintf(stderr, "Failed opening %s\n", path);
		perror("open");
		return;
	}

	while (fscanf(fp, "%d", &pid) == 1) {
		sprintf(buf, "%d", pid);
		if (print)
			printf("Moving %d out of %s\n", pid, name);
		write(fd, buf, strlen(buf));
	}
	fclose(fp);
	close(fd);

	snprintf(path, MAXPATH - 1, "%s/%s", CPUSET_PATH, name);
	path[MAXPATH - 1] = 0;

//	return;
	sleep(1);
	ret = rmdir(path);
	if (ret < 0) {
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

static void teardown(void)
{
	int fd;

	if (all_cpus)
		return;

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

static void usage(char **argv)
{
	char *arg = argv[0];
	char *p = arg+strlen(arg);

	while (p >= arg && *p != '/')
		p--;
	p++;

	printf("usage: %s\n"
	       "\n",p);
	exit(-1);
}

static int fail;

static u64 get_time_us(void)
{
	struct timespec ts;
	u64 time;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	time = ts.tv_sec * 1000000;
	time += ts.tv_nsec / 1000;

	return time;
}

static void print_stat(FILE *fp, struct sched_data *sd, int index, int verbose, int quiet)
{
	struct thread_stat *stat = &sd->stat;

	if (!verbose) {
		if (quiet != 1) {
			char *fmt;
			if (use_nsecs)
				fmt = "T:%2d (%5d) I:%ld C:%7lu "
					"Min:%7ld Act:%8ld Avg:%8ld Max:%8ld\n";
			else
				fmt = "T:%2d (%5d) I:%ld C:%7lu "
					"Min:%7ld Act:%5ld Avg:%5ld Max:%8ld\n";
			fprintf(fp, fmt, index, stat->tid,
				sd->deadline_us, stat->cycles, stat->min, stat->act,
				stat->cycles ?
				(long)(stat->avg/stat->cycles) : 0, stat->max);
		}
	} else {
		while (stat->cycles != stat->cyclesread) {
			long diff = stat->values
			    [stat->cyclesread & sd->bufmsk];

			if (diff > stat->redmax) {
				stat->redmax = diff;
				stat->cycleofmax = stat->cyclesread;
			}
			stat->cyclesread++;
		}
	}
}

static u64 do_runtime(long tid, struct sched_data *sd, u64 period)
{
	struct thread_stat *stat = &sd->stat;
	u64 next_period = period + sd->deadline_us;
	u64 now = get_time_us();
	u64 diff;

	if (now < period) {
		u64 delta = period - now;
		/*
		 * The period could be off due to other deadline tasks
		 * preempting us when we started. If that's the case then
		 * adjust the current period.
		 */
		ftrace_write(sd->buff,
			     "Adjusting period: now: %lld period: %lld delta:%lld%s\n",
			     now, period, delta, delta > sd->deadline_us / 2 ?
			     " HUGE ADJUSTMENT" : "");
		period = now;
		next_period = period + sd->deadline_us;
	}

	ftrace_write(sd->buff, "start at %lld off=%lld (period=%lld next=%lld)\n",
		     now, now - period, period, next_period);


	diff = now - period;
	if (diff > stat->max)
		stat->max = diff;
	if (!stat->min || diff < stat->min)
		stat->min = diff;
	stat->act = diff;
	stat->avg += (double) diff;

	stat->cycles++;

	return next_period;
}

void *run_deadline(void *data)
{
	struct sched_data *sd = data;
	struct thread_stat *stat = &sd->stat;
	struct sched_attr attr;
	long tid = gettid();
	u64 period;
	int ret;

	printf("deadline thread %ld\n", tid);

	stat->tid = tid;

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
	attr.sched_runtime = sd->runtime_us * 1000;
	attr.sched_deadline = sd->deadline_us * 1000;

	printf("thread[%ld] runtime=%lldus deadline=%lldus\n",
	       gettid(), sd->runtime_us, sd->deadline_us);

	pthread_barrier_wait(&barrier);

	ret = sched_setattr(0, &attr, 0);
	if (ret < 0) {
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
	
	while (!shutdown) {
		period = do_runtime(tid, sd, period);
		sched_yield();
	}
	ret = sched_getattr(0, &attr, sizeof(attr), 0);
	if (ret < 0) {
		perror("sched_getattr");
		pthread_exit("Failed second sched_getattr");
	}

	return NULL;
}

struct cpu_list {
	struct cpu_list	*next;
	int		start_cpu;
	int		end_cpu;
};

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

static void sighand(int sig)
{
	shutdown = 1;
}

static const char *join_thread(pthread_t *thread)
{
	void *result;

	pthread_join(*thread, &result);
	return result;
}

static void loop(struct sched_data *sched_data, int nr_threads)
{
	int i;

	while (!shutdown) {
		for (i = 0; i < nr_threads; i++) {
			print_stat(stdout, &sched_data[i], i, 0, 0);
		}
		usleep(10000);
		printf("\033[%dA", nr_threads);
	}
	usleep(10000);
	for (i = 0; i < nr_threads; i++) {
		printf("\n");
	}
}

int main (int argc, char **argv)
{
	struct sched_data *sched_data;
	struct sched_data *sd;
	const char *res;
	const char *setcpu = NULL;
	char *setcpu_buf = NULL;
	char *allcpu_buf = NULL;
	pthread_t *thread;
	unsigned int interval = 1000;
	unsigned int step = 500;
	int percent = 60;
	u64 runtime;
	u64 start_period;
	u64 end_period;
	int nr_cpus;
	int i;
	int c;

	cpu_count = sysconf(_SC_NPROCESSORS_CONF);
	if (cpu_count < 1) {
		fprintf(stderr, "Can not calculate number of CPUS\n");
		exit(-1);
	}

	while ((c = getopt(argc, argv, "+hac:t:")) >= 0) {
		switch (c) {
		case 'a':
			all_cpus = 1;
			if (!nr_threads)
				nr_threads = cpu_count;
			break;
		case 'c':
			setcpu = optarg;
			break;
		case 'i':
			interval = atoi(optarg);
			break;
		case 's':
			step = atoi(optarg);
			break;
		case 't':
			nr_threads = atoi(optarg);
			break;
		case 'h':
		default:
			usage(argv);
		}
	}

	if (!nr_threads)
		nr_threads = 1;

	if (setcpu) {
		nr_cpus = calc_nr_cpus(setcpu, &setcpu_buf);
		if (nr_cpus < 0 || nr_cpus > cpu_count) {
			fprintf(stderr, "Invalid cpu input '%s'\n", setcpu);
			exit(-1);
		}
	} else
		nr_cpus = cpu_count;

	if (!all_cpus && cpu_count == nr_cpus) {
		printf("Using all CPUS\n");
		all_cpus = 1;
	}

	/* Default cpu to use is the last one */
	if (!all_cpus && !setcpu) {
		setcpu_buf = malloc(10);
		if (!setcpu_buf) {
			perror("malloc");
			exit(-1);
		}
		sprintf(setcpu_buf, "%d", cpu_count - 1);
	}

	setcpu = setcpu_buf;

	if (setcpu)
		make_other_cpu_list(setcpu, &allcpu_buf);

	if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
		perror("mlockall");
	}

	setup_ftrace_marker();

	thread = calloc(nr_threads, sizeof(*thread));
	sched_data = calloc(nr_threads, sizeof(*sched_data));
	if (!thread || !sched_data) {
		perror("allocating threads");
		exit(-1);
	}

	if (nr_threads > nr_cpus) {
		/*
		 * More threads than CPUs, then have the total be
		 * no more than 80 percent.
		 */
		percent = nr_cpus * 80 / nr_threads;
	}

	/* Set up the data while sill in SCHED_FIFO */
	for (i = 0; i < nr_threads; i++) {
		sd = &sched_data[i];
		/*
		 * Interval is the deadline/period
		 * The runtime is the percentage of that period.
		 */
		runtime = interval * percent / 100;

		if (runtime < 2000) {
			/*
			 * If the runtime is less than 2ms, then we better
			 * have HRTICK enabled.
			 */
			if (!setup_hr_tick()) {
				fprintf(stderr, "For less than 2ms run times, you need to\n"
					"have HRTICK enabled in debugfs/sched_features\n");
				exit(-1);
			}
		}
		sd->runtime_us = runtime;
		sd->deadline_us = interval;

		printf("interval: %lld:%lld\n", sd->runtime_us, sd->deadline_us);

		/* Make sure that we can make our deadlines */
		start_period = get_time_us();
		do_runtime(gettid(), sd, start_period);
		end_period = get_time_us();
		if (end_period - start_period > sd->runtime_us) {
			fprintf(stderr, "Failed to perform task within runtime: Missed by %lld us\n",
				end_period - start_period - sd->runtime_us);
			exit(-1);
		}

		printf("  Tested at %lldus of %lldus\n",
		       end_period - start_period, sd->runtime_us);

		interval += step;
	}


	pthread_barrier_init(&barrier, NULL, nr_threads + 1);

	for (i = 0; i < nr_threads; i++) {
		sd = &sched_data[i];
		pthread_create(&thread[i], NULL, run_deadline, sd);
	}

	atexit(teardown);

	pthread_barrier_wait(&barrier);

	if (fail) {
		printf("fail 1\n");
		exit(-1);
	}

	all_cpus = 1;
	if (!all_cpus) {
		int *pids;

		res = make_cpuset(CPUSET_ALL, allcpu_buf, "0",
				  CPUSET_FL_SET_LOADBALANCE |
				  CPUSET_FL_CLONE_CHILDREN |
				  CPUSET_FL_ALL_TASKS);
		if (res) {
			perror(res);
			exit(-1);
		}

		pids = calloc(nr_threads + 1, sizeof(int));
		if (!pids) {
			perror("Allocating pids");
			exit(-1);
		}

		for (i = 0; i < nr_threads; i++)
			pids[i] = sched_data[i].stat.tid;

		res = make_cpuset(CPUSET_LOCAL, setcpu, "0",
				  CPUSET_FL_CPU_EXCLUSIVE |
				  CPUSET_FL_SET_LOADBALANCE |
				  CPUSET_FL_CLONE_CHILDREN |
				  CPUSET_FL_TASKS, pids);
		free(pids);
		if (res) {
			perror(res);
			exit(-1);
		}

		system("cat /sys/fs/cgroup/cpuset/my_cpuset/tasks");
	}

	printf("main thread %ld\n", gettid());

	pthread_barrier_wait(&barrier);
	printf("fail 2 %d\n", fail);

	if (fail)
		exit(-1);

	pthread_barrier_wait(&barrier);

	signal(SIGINT, sighand);
	signal(SIGTERM, sighand);

	if (!fail)
		loop(sched_data, nr_threads);

	for (i = 0; i < nr_threads; i++) {

		sd = &sched_data[i];

		res = join_thread(&thread[i]);
		if (res) {
			printf("Thread %d failed: %s\n", i, res);
			continue;
		}
	}

	free(setcpu_buf);
	return 0;
}
