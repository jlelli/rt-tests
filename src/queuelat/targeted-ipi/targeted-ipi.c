// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018 Marcelo Tosatti <mtosatti@redhat.com>
 * Copyright (C) 2019 John Kacur <jkacur@redhat.com>
 * Copyright (C) 2019 Clark Williams <williams@redhat.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <asm-generic/delay.h>

static int ipidest;
module_param(ipidest, int, S_IRUGO);

/* number of ipis */
static int nripis;
module_param(nripis, int, S_IRUGO);

/* interval between consecutive IPI calls */
static int interval;
module_param(interval, int, S_IRUGO);

/* how many microseconds to delay in IPI handler */
static int delay;
module_param(delay, int, S_IRUGO);

static void ipi_handler(void *info)
{
	udelay(interval);
}

static int targeted_ipi_init(void)
{
	int ret, i;

	for (i=0; i < nripis; i++) 
	{
		ret = smp_call_function_single(ipidest, ipi_handler, NULL, 1);
		if (ret) { 
			printk(KERN_ERR "i=%d smp_call_function_single ret=%d\n", i, ret);
			return 0;
		}
		udelay(interval);
	}

	return 0;
}

static void targeted_ipi_exit(void)
{
}

module_init(targeted_ipi_init);
module_exit(targeted_ipi_exit);
MODULE_LICENSE("GPL");
