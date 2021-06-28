// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef __RT_UTILS_H
#define __RT_UTILS_H

#include <stdint.h>

#define _STR(x) #x
#define STR(x) _STR(x)
#define MAX_PATH 256

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

int check_privs(void);
char *get_debugfileprefix(void);
int mount_debugfs(char *);
int get_tracers(char ***);
int valid_tracer(char *);

int setevent(char *event, char *val);
int event_enable(char *event);
int event_disable(char *event);
int event_enable_all(void);
int event_disable_all(void);

const char *policy_to_string(int policy);
uint32_t string_to_policy(const char *str);

pid_t gettid(void);

int parse_time_string(char *val);
int parse_mem_string(char *str, uint64_t *val);

void enable_trace_mark(void);
void tracemark(char *fmt, ...) __attribute__((format(printf, 1, 2)));
void disable_trace_mark(void);

#define MSEC_PER_SEC		1000
#define USEC_PER_SEC		1000000
#define NSEC_PER_SEC		1000000000
#define USEC_TO_NSEC(u)		((u) * 1000)
#define USEC_TO_SEC(u)		(u) / USEC_PER_SEC)
#define NSEC_TO_USEC(n)		((n) / 1000)
#define SEC_TO_NSEC(s)		((s) * NSEC_PER_SEC)
#define SEC_TO_USEC(s)		((s) * USEC_PER_SEC)

static inline void tsnorm(struct timespec *ts)
{
	while (ts->tv_nsec >= NSEC_PER_SEC) {
		ts->tv_nsec -= NSEC_PER_SEC;
		ts->tv_sec++;
	}
}

static inline int tsgreater(struct timespec *a, struct timespec *b)
{
	return ((a->tv_sec > b->tv_sec) ||
		(a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec));
}

static inline int64_t calcdiff(struct timespec t1, struct timespec t2)
{
	int64_t diff = USEC_PER_SEC * (long long)((int) t1.tv_sec - (int) t2.tv_sec);
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

static inline int64_t calctime(struct timespec t)
{
	int64_t time;
	time = USEC_PER_SEC * t.tv_sec;
	time += ((int) t.tv_nsec) / 1000;
	return time;
}

void rt_init(int argc, char *argv[]);

void rt_write_json(const char *filename, int return_code,
		   void (*cb)(FILE *, void *),
		   void *data);

#endif	/* __RT_UTILS.H */
