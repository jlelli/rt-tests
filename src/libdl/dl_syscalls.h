/*
 * Libdl
 *  (C) Dario Faggioli <raistlin@linux.it>, 2009, 2010
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License (COPYING file) for more details.
 *
 */

#ifndef __DL_SYSCALLS__
#define __DL_SYSCALLS__

#include <linux/kernel.h>
#include <linux/unistd.h>
#include <linux/types.h>
#include <time.h>

#define SCHED_DEADLINE	6

/* XXX use the proper syscall numbers */
#ifdef __x86_64__
#define __NR_sched_setparam2		312
#define __NR_sched_getparam2		313
#define __NR_sched_setscheduler2	314
#endif

#ifdef __i386__
#define __NR_sched_setparam2		349
#define __NR_sched_getparam2		350
#define __NR_sched_setscheduler2	351
#endif

#ifdef __arm__
#define __NR_sched_setscheduler2	378
#define __NR_sched_setparam2		379
#define __NR_sched_getparam2		380
#endif

struct sched_param2 {
	int sched_priority;
	unsigned int sched_flags;
	__u64 sched_runtime;
	__u64 sched_deadline;
	__u64 sched_period;

	__u64 __unused[12];
};

int sched_setscheduler2(pid_t pid, int policy,
			  const struct sched_param2 *param);

int sched_setparam2(pid_t pid,
		      const struct sched_param2 *param);

int sched_getparam2(pid_t pid, struct sched_param2 *param);

#endif /* __DL_SYSCALLS__ */

