// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rt-sched.h - sched_setattr() and sched_getattr() API
 * (C) Dario Faggioli <raistlin@linux.it>, 2009, 2010
 * Copyright (C) 2014 BMW Car IT GmbH, Daniel Wagner <daniel.wagner@bmw-carit.de
 */

/* This file is based on Dario Faggioli's libdl. Eventually it will be
   replaced by a proper implemenation of this API. */

#ifndef __RT_SCHED_H__
#define __RT_SCHED_H__

#include <stdint.h>
#include <sys/types.h>

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif

#ifdef __x86_64__
#define __NR_sched_setattr		314
#define __NR_sched_getattr		315
#endif

#ifdef __i386__
#define __NR_sched_setattr		351
#define __NR_sched_getattr		352
#endif

#ifdef __arm__
#ifndef __NR_sched_setattr
#define __NR_sched_setattr		380
#endif
#ifndef __NR_sched_getattr
#define __NR_sched_getattr		381
#endif
#endif

#ifdef __tilegx__
#define __NR_sched_setattr		274
#define __NR_sched_getattr		275
#endif

struct sched_attr {
	uint32_t size;
	uint32_t sched_policy;
	uint64_t sched_flags;

	/* SCHED_NORMAL, SCHED_BATCH */
	int32_t sched_nice;

	/* SCHED_FIFO, SCHED_RR */
	uint32_t sched_priority;

	/* SCHED_DEADLINE */
	uint64_t sched_runtime;
	uint64_t sched_deadline;
	uint64_t sched_period;
};

int sched_setattr(pid_t pid,
		  const struct sched_attr *attr,
		  unsigned int flags);

int sched_getattr(pid_t pid,
		  struct sched_attr *attr,
		  unsigned int size,
		  unsigned int flags);

#endif /* __RT_SCHED_H__ */
