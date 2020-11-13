// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2009 Carsten Emde <carsten.emde@osadl.org>
 * Copyright (C) 2010 Clark Williams <williams@redhat.com>
 * Copyright (C) 2015 John Kacur <jkacur@redhat.com>
 *
 * based on functions from cyclictest that has
 * (C) 2008-2009 Clark Williams <williams@redhat.com>
 * (C) 2005-2007 Thomas Gleixner <tglx@linutronix.de>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h> /* For SYS_gettid definitions */
#include "rt-utils.h"
#include "rt-sched.h"
#include "error.h"

#define  TRACEBUFSIZ  1024

static char debugfileprefix[MAX_PATH];
static char *fileprefix;
static int trace_fd = -1;
static int tracemark_fd = -1;
static __thread char tracebuf[TRACEBUFSIZ];

/*
 * Finds the tracing directory in a mounted debugfs
 */
char *get_debugfileprefix(void)
{
	char type[100];
	FILE *fp;
	int size;
	int found = 0;
	struct stat s;

	if (debugfileprefix[0] != '\0')
		goto out;

	/* look in the "standard" mount point first */
	if ((stat("/sys/kernel/debug/tracing", &s) == 0) && S_ISDIR(s.st_mode)) {
		strcpy(debugfileprefix, "/sys/kernel/debug/tracing/");
		goto out;
	}

	/* now look in the "other standard" place */
	if ((stat("/debug/tracing", &s) == 0) && S_ISDIR(s.st_mode)) {
		strcpy(debugfileprefix, "/debug/tracing/");
		goto out;
	}

	/* oh well, parse /proc/mounts and see if it's there */
	if ((fp = fopen("/proc/mounts", "r")) == NULL)
		goto out;

	while (fscanf(fp, "%*s %"
		      STR(MAX_PATH)
		      "s %99s %*s %*d %*d\n",
		      debugfileprefix, type) == 2) {
		if (strcmp(type, "debugfs") == 0) {
			found = 1;
			break;
		}
		/* stupid check for systemd-style autofs mount */
		if ((strcmp(debugfileprefix, "/sys/kernel/debug") == 0) &&
		    (strcmp(type, "systemd") == 0)) {
			found = 1;
			break;
		}
	}
	fclose(fp);

	if (!found) {
		debugfileprefix[0] = '\0';
		goto out;
	}

	size = sizeof(debugfileprefix) - strlen(debugfileprefix);
	strncat(debugfileprefix, "/tracing/", size);

out:
	return debugfileprefix;
}

int mount_debugfs(char *path)
{
	char *mountpoint = path;
	char cmd[MAX_PATH];
	char *prefix;
	int ret;

	/* if it's already mounted just return */
	prefix = get_debugfileprefix();
	if (strlen(prefix) != 0) {
		info("debugfs mountpoint: %s\n", prefix);
		return 0;
	}
	if (!mountpoint)
		mountpoint = "/sys/kernel/debug";

	sprintf(cmd, "mount -t debugfs debugfs %s", mountpoint);
	ret = system(cmd);
	if (ret != 0) {
		fprintf(stderr, "Error mounting debugfs at %s: %s\n",
			mountpoint, strerror(errno));
		return -1;
	}
	return 0;
}

static char **tracer_list;
static char *tracer_buffer;
static int num_tracers;
#define CHUNKSZ   1024

/*
 * return a list of the tracers configured into the running kernel
 */

int get_tracers(char ***list)
{
	int ret;
	FILE *fp;
	char buffer[CHUNKSZ];
	char *prefix = get_debugfileprefix();
	char *tmpbuf = NULL;
	char *ptr;
	int tmpsz = 0;

	/* if we've already parse it, return what we have */
	if (tracer_list) {
		*list = tracer_list;
		return num_tracers;
	}

	/* open the tracing file available_tracers */
	sprintf(buffer, "%savailable_tracers", prefix);
	if ((fp = fopen(buffer, "r")) == NULL)
		fatal("Can't open %s for reading\n", buffer);

	/* allocate initial buffer */
	ptr = tmpbuf = malloc(CHUNKSZ);
	if (ptr == NULL)
		fatal("error allocating initial space for tracer list\n");

	/* read in the list of available tracers */
	while ((ret = fread(buffer, sizeof(char), CHUNKSZ, fp))) {
		if ((ptr+ret+1) > (tmpbuf+tmpsz)) {
			tmpbuf = realloc(tmpbuf, tmpsz + CHUNKSZ);
			if (tmpbuf == NULL)
				fatal("error allocating space for list of valid tracers\n");
			tmpsz += CHUNKSZ;
		}
		strncpy(ptr, buffer, ret);
		ptr += ret;
	}
	fclose(fp);
	if (tmpsz == 0)
		fatal("error reading available tracers\n");

	tracer_buffer = tmpbuf;

	/* get a buffer for the pointers to tracers */
	if (!(tracer_list = malloc(sizeof(char *))))
		fatal("error allocatinging tracer list buffer\n");

	/* parse the buffer */
	ptr = strtok(tmpbuf, " \t\n\r");
	do {
		tracer_list[num_tracers++] = ptr;
		tracer_list = realloc(tracer_list, sizeof(char*)*(num_tracers+1));
		tracer_list[num_tracers] = NULL;
	} while ((ptr = strtok(NULL, " \t\n\r")) != NULL);

	/* return the list and number of tracers */
	*list = tracer_list;
	return num_tracers;
}


/*
 * return zero if tracername is not a valid tracer, non-zero if it is
 */

int valid_tracer(char *tracername)
{
	char **list;
	int ntracers;
	int i;

	ntracers = get_tracers(&list);
	if (ntracers == 0 || tracername == NULL)
		return 0;
	for (i = 0; i < ntracers; i++)
		if (strncmp(list[i], tracername, strlen(list[i])) == 0)
			return 1;
	return 0;
}

/*
 * enable event tracepoint
 */
int setevent(char *event, char *val)
{
	char *prefix = get_debugfileprefix();
	char buffer[MAX_PATH];
	int fd;
	int ret;

	sprintf(buffer, "%s%s", prefix, event);
	if ((fd = open(buffer, O_WRONLY)) < 0) {
		warn("unable to open %s\n", buffer);
		return -1;
	}
	if ((ret = write(fd, val, strlen(val))) < 0) {
		warn("unable to write %s to %s\n", val, buffer);
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

int event_enable_all(void)
{
	return setevent("events/enable", "1");
}

int event_disable_all(void)
{
	return setevent("events/enable", "0");
}

int event_enable(char *event)
{
	char path[MAX_PATH];

	sprintf(path, "events/%s/enable", event);
	return setevent(path, "1");
}

int event_disable(char *event)
{
	char path[MAX_PATH];

	sprintf(path, "events/%s/enable", event);
	return setevent(path, "0");
}

int check_privs(void)
{
	int policy = sched_getscheduler(0);
	struct sched_param param, old_param;

	/* if we're already running a realtime scheduler
	 * then we *should* be able to change things later
	 */
	if (policy == SCHED_FIFO || policy == SCHED_RR)
		return 0;

	/* first get the current parameters */
	if (sched_getparam(0, &old_param)) {
		fprintf(stderr, "unable to get scheduler parameters\n");
		return 1;
	}
	param = old_param;

	/* try to change to SCHED_FIFO */
	param.sched_priority = 1;
	if (sched_setscheduler(0, SCHED_FIFO, &param)) {
		fprintf(stderr, "Unable to change scheduling policy!\n");
		fprintf(stderr, "either run as root or join realtime group\n");
		return 1;
	}

	/* we're good; change back and return success */
	return sched_setscheduler(0, policy, &old_param);
}

const char *policy_to_string(int policy)
{
	switch (policy) {
	case SCHED_OTHER:
		return "SCHED_OTHER";
	case SCHED_FIFO:
		return "SCHED_FIFO";
	case SCHED_RR:
		return "SCHED_RR";
	case SCHED_BATCH:
		return "SCHED_BATCH";
	case SCHED_IDLE:
		return "SCHED_IDLE";
	case SCHED_DEADLINE:
		return "SCHED_DEADLINE";
	}

	return "unknown";
}

uint32_t string_to_policy(const char *str)
{
	if (!strcmp(str, "other"))
		return SCHED_OTHER;
	else if (!strcmp(str, "fifo"))
		return SCHED_FIFO;
	else if (!strcmp(str, "rr"))
		return SCHED_RR;
	else if (!strcmp(str, "batch"))
		return SCHED_BATCH;
	else if (!strcmp(str, "idle"))
		return SCHED_IDLE;
	else if (!strcmp(str, "deadline"))
		return SCHED_DEADLINE;

	return 0;
}

pid_t gettid(void)
{
	return syscall(SYS_gettid);
}

/*
 * parse an input value as a base10 value followed by an optional
 * suffix. The input value is presumed to be in seconds, unless
 * followed by a modifier suffix: m=minutes, h=hours, d=days
 *
 * the return value is a value in seconds
 */
int parse_time_string(char *val)
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

int parse_mem_string(char *str, uint64_t *val)
{
	char *endptr;
	int v = strtol(str, &endptr, 10);

	if (!*endptr)
		return v;

	switch (*endptr) {
	case 'g':
	case 'G':
		v *= 1024;
	case 'm':
	case 'M':
		v *= 1024;
	case 'k':
	case 'K':
		v *= 1024;
	case 'b':
	case 'B':
		break;
	default:
		return -1;
	}

	*val = v;

	return 0;
}

static void open_tracemark_fd(void)
{
	char path[MAX_PATH];

	/*
	 * open the tracemark file if it's not already open
	 */
	if (tracemark_fd < 0) {
		sprintf(path, "%s/%s", fileprefix, "trace_marker");
		tracemark_fd = open(path, O_WRONLY);
		if (tracemark_fd < 0) {
			warn("unable to open trace_marker file: %s\n", path);
			return;
		}
	}

	/*
	 * if we're not tracing and the tracing_on fd is not open,
	 * open the tracing_on file so that we can stop the trace
	 * if we hit a breaktrace threshold
	 */
	if (trace_fd < 0) {
		sprintf(path, "%s/%s", fileprefix, "tracing_on");
		if ((trace_fd = open(path, O_WRONLY)) < 0)
			warn("unable to open tracing_on file: %s\n", path);
	}
}

static void close_tracemark_fd(void)
{
	if (tracemark_fd > 0)
		close(tracemark_fd);

	if (trace_fd > 0)
		close(trace_fd);
}

static int trace_file_exists(char *name)
{
	struct stat sbuf;
	char *tracing_prefix = get_debugfileprefix();
	char path[MAX_PATH];
	strcat(strcpy(path, tracing_prefix), name);
	return stat(path, &sbuf) ? 0 : 1;
}

static void debugfs_prepare(void)
{
	if (mount_debugfs(NULL))
		fatal("could not mount debugfs");

	fileprefix = get_debugfileprefix();
	if (!trace_file_exists("tracing_enabled") &&
	    !trace_file_exists("tracing_on"))
		warn("tracing_enabled or tracing_on not found\n"
		     "debug fs not mounted");
}

void tracemark(char *fmt, ...)
{
	va_list ap;
	int len;

	/* bail out if we're not tracing */
	/* or if the kernel doesn't support trace_mark */
	if (tracemark_fd < 0 || trace_fd < 0)
		return;

	va_start(ap, fmt);
	len = vsnprintf(tracebuf, TRACEBUFSIZ, fmt, ap);
	va_end(ap);

	/* write the tracemark message */
	write(tracemark_fd, tracebuf, len);

	/* now stop any trace */
	write(trace_fd, "0\n", 2);
}

void enable_trace_mark(void)
{
	debugfs_prepare();
	open_tracemark_fd();
}

void disable_trace_mark(void)
{
	close_tracemark_fd();
}
