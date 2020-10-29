// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Copyright (C) 2018 Marcelo Tosatti <mtosatti@redhat.com>
 * Copyright (C) 2019 John Kacur <jkacur@redhat.com>
 * Copyright (C) 2019 Clark Williams <williams@redhat.com>
 */

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>

#include "rt-utils.h"

/* Program parameters:
 * max_queue_len: maximum latency allowed, in nanoseconds (int).
 * cycles_per_packet: number of cycles to process one packet (int).
 * mpps(million-packet-per-sec): million packets per second (float).
 * tsc_freq_mhz: TSC frequency in MHz, as measured by TSC PIT calibration 
 * (search for "Detected XXX MHz processor" in dmesg, and use the integer part).
 *
 * How it works
 * ============
 *
 *  The program in essence does:
 *
 * 		b = rdtsc();
 * 		memmove(dest, src, n);
 * 		a = rdtsc();
 *
 * 		delay = convert_to_ns(a - b);
 *
 * 		queue_size += packets_queued_in(delay);
 * 		queue_size -= packets_processed;
 *	
 *		if (queue_size > max_queue_len)
 *			FAIL();
 *
 * packets_processed is fixed, and is estimated as follows:
 * n is determined first, so that the stats bucket with highest count
 * takes max_latency/2.
 * for max_latency/2, we calculate how many packets can be drained
 * in that time (using cycles_per_packet).
 *
 */

int maxlatency;
int cycles_per_packet;
float mpps;
int timeout_secs;
int min_queue_size_to_print;

/* Derived constants */

float cycles_to_ns;
int max_queue_len;

int default_n;
int nr_packets_drain_per_block;

/* 
 * Parameters for the stats collection buckets
 */

#define LAST_VAL 70000
#define VALS_PER_BUCKET 100
#define NR_BUCKETS LAST_VAL/VALS_PER_BUCKET

unsigned long long buckets[NR_BUCKETS+1];
unsigned long long total_count;

#define OUTLIER_BUCKET NR_BUCKETS

static int val_to_bucket(unsigned long long val)
{
	int bucket_nr = val / VALS_PER_BUCKET;
	if (bucket_nr >= NR_BUCKETS)
		return OUTLIER_BUCKET;
	return bucket_nr;
}

static void account(unsigned long long val)
{
	int bucket_nr = val_to_bucket(val);
	buckets[bucket_nr]++;
	total_count++;
}

static unsigned long long total_samples(void)
{
	int i;
	unsigned long long total = 0;

	for (i = 0; i <= OUTLIER_BUCKET; i++)
		total += buckets[i];

	return total;
}

static void print_all_buckets(void)
{
	int i, print_dotdotdot = 0;

	for (i = 0; i <= OUTLIER_BUCKET; i++) {
		int bucket_nr;
		unsigned long long val = i*VALS_PER_BUCKET;

		bucket_nr = val_to_bucket(val);

		if (bucket_nr != OUTLIER_BUCKET) {
			int n_bucketnr = bucket_nr+1;
			if (buckets[bucket_nr] == buckets[n_bucketnr]) {
				print_dotdotdot = 1;
				continue;
			}
			if (print_dotdotdot) {
				printf("...\n");
				print_dotdotdot = 0;
			}
			printf("[%lld - %lld] = %lld\n", val,
						     val + VALS_PER_BUCKET-1,
						     buckets[bucket_nr]);
		} else {
			if (print_dotdotdot) {
				printf("...\n");
				print_dotdotdot = 0;
			}
			printf("[%lld - END] = %lld\n", val,
						     buckets[bucket_nr]);
		}
	}
}

static void print_max_bucketsec(void)
{
	int i, bucket_nr;
	unsigned long long highest_val = 0;

	for (i = 0; i <= OUTLIER_BUCKET; i++) {
		unsigned long long val = i*VALS_PER_BUCKET;

		bucket_nr = val_to_bucket(val);

		if (buckets[bucket_nr] != 0)
			highest_val = val;
	}

	bucket_nr = val_to_bucket(highest_val);
	printf("Max loop processing time: [%lld - %lld] = %lld\n", highest_val,
						     highest_val + VALS_PER_BUCKET-1,
						     buckets[bucket_nr]);

	return;
}

static void print_min_bucketsec(void)
{
	int i, bucket_nr;
	unsigned long long min_val = 0;

	for (i = 0; i <= OUTLIER_BUCKET; i++) {
		unsigned long long val = i*VALS_PER_BUCKET;

		bucket_nr = val_to_bucket(val);

		if (buckets[bucket_nr] != 0) {
			min_val = val;
			break;
		}
	}

	bucket_nr = val_to_bucket(min_val);
	printf("Min loop processing time: [%lld - %lld] = %lld\n", min_val,
						     min_val + VALS_PER_BUCKET-1,
						     buckets[bucket_nr]);

	return;
}

static void print_avg_bucketsec(void)
{
	int i, bucket_nr;
	unsigned long long total_sum = 0;
	unsigned long long nr_hits = 0;

	for (i = 0; i <= OUTLIER_BUCKET; i++) {
		unsigned long long val = i*VALS_PER_BUCKET;
		unsigned long long maxtime;

		bucket_nr = val_to_bucket(val);

		maxtime = val + VALS_PER_BUCKET-1;
		total_sum = total_sum + maxtime*buckets[bucket_nr];

		nr_hits = nr_hits + buckets[bucket_nr];
	}

	printf("Avg loop processing time: %lld\n", total_sum / nr_hits);
}

static void print_all_buckets_drainlength(void)
{
	int i, print_dotdotdot = 0;

	for (i = 0; i <= OUTLIER_BUCKET; i++) {
		int bucket_nr;
		unsigned long long val = i*VALS_PER_BUCKET;

		bucket_nr = val_to_bucket(val);

		if (bucket_nr != OUTLIER_BUCKET) {
			unsigned long long mindelta, maxdelta;
			int nr_packets_minfill, nr_packets_maxfill;
			int n_bucketnr = bucket_nr+1;

			if (buckets[bucket_nr] == buckets[n_bucketnr]) {
				print_dotdotdot = 1;
				continue;
			}
			if (print_dotdotdot) {
				printf("...\n");
				print_dotdotdot = 0;
			}

			mindelta = val;
			maxdelta = val + VALS_PER_BUCKET-1;

			nr_packets_minfill = mindelta * mpps * 1000000 / NSEC_PER_SEC;
			nr_packets_maxfill = maxdelta * mpps * 1000000 / NSEC_PER_SEC;

			printf("[%lld - %lld] = %lld  packetfillrates=[%d - %d]\n", val, 
						     val + VALS_PER_BUCKET-1,
						     buckets[bucket_nr],
						     nr_packets_minfill,
						     nr_packets_maxfill);
		} else {
			if (print_dotdotdot) {
				printf("...\n");
				print_dotdotdot = 0;
			}
			printf("[%lld - END] = %lld\n", val,
						     buckets[bucket_nr]);
		}
	}
}

typedef unsigned long long cycles_t;
typedef unsigned long long usecs_t;
typedef unsigned long long u64;

#if defined __x86_64__ || defined __i386__

#ifdef __x86_64__
#define DECLARE_ARGS(val, low, high)    unsigned low, high
#define EAX_EDX_VAL(val, low, high)     ((low) | ((u64)(high) << 32))
#define EAX_EDX_ARGS(val, low, high)    "a" (low), "d" (high)
#define EAX_EDX_RET(val, low, high)     "=a" (low), "=d" (high)
#else
#define DECLARE_ARGS(val, low, high)    unsigned long long val
#define EAX_EDX_VAL(val, low, high)     (val)
#define EAX_EDX_ARGS(val, low, high)    "A" (val)
#define EAX_EDX_RET(val, low, high)     "=A" (val)
#endif

static inline unsigned long long __rdtscll(void)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("mfence; rdtsc" : EAX_EDX_RET(val, low, high));

	return EAX_EDX_VAL(val, low, high);
}

#define gettick(val) do { (val) = __rdtscll(); } while (0)

#else

static inline unsigned long long __clock_gettime(void)
{
	struct timespec now;
	int ret;

	ret = clock_gettime(CLOCK_MONOTONIC, &now);
	if (ret < 0)
		return 0;

	return now.tv_nsec;
}

#define gettick(val) do { (val) = __clock_gettime(); } while (0)

#endif

static void init_buckets(void)
{
	int i;

	for (i=0; i <= NR_BUCKETS; i++)
		buckets[i] = 0;

	total_count = 0;
}

static int find_highest_count_bucket(void)
{
	int i;
	int max_bucket = 0;
	unsigned long long max_val = 0;

	for (i=0; i <= NR_BUCKETS; i++) {
		if (buckets[i] > max_val) {
			max_bucket = i;
			max_val = buckets[i];
		}
	}

	return max_bucket;
}

int tracing_mark_fd;
static void trace_open(void)
{
	int fd;

	fd = open("/sys/kernel/debug/tracing/trace_marker", O_RDWR);

	if (fd == -1) {
		perror("open");
		exit(0);
	}
	tracing_mark_fd = fd;
}

static void trace_write(char *buf, int len)
{
	int ret;

	ret = write(tracing_mark_fd, buf, len);
	if (ret == -1) {
		perror("write");
		exit(0);
	}
}

static void run_n(int n)
{
	u64 a, b, delta;
	void *dest, *src;
	int i, loops = 50000;

	init_buckets();

	dest = malloc(n);
	if (dest == NULL) {
		printf("failure to allocate %d bytes "
		       " for dest\n", n);
		exit(0);
	}
	src = malloc(n);
	if (src == NULL) {
		printf("failure to allocate %d bytes "
		       " for src\n", n);
		exit(0);
	}

	memset(src, 0, n);

	memmove(dest, src, n);
	for (i = 0; i < loops; i++) {
		gettick(b);
		memmove(dest, src, n);
		gettick(a);
		delta = (a - b) * cycles_to_ns;
		account(delta);
	}

	free(dest);
	free(src);

	return;
}

/*
 * Find the size of n such that the stats for the 
 * function call
 *
 *	memmove(dest, src, n).
 *
 * Takes MaximumLat/2 in the bucket that has most
 * entries.
 *
 */
static int measure_n(void)
{
	int time, bucket_nr;
	int n = 100000, delta = 0;

	do {
		if (delta > 0)
			n = n+1000;
		else if (delta < 0)
			n = n-1000;

		run_n(n);
		bucket_nr = find_highest_count_bucket();

		time = bucket_nr * VALS_PER_BUCKET;

		delta = maxlatency/2 - time;
	} while (abs(delta) > VALS_PER_BUCKET*2);

	return n;
}

static void convert_to_ghz(double tsc_freq_mhz)
{
	float tsc_freq_ghz = tsc_freq_mhz/1000;

	cycles_to_ns = 1/tsc_freq_ghz;

	printf("tsc_freq_ghz = %f, cycles_to_ns = %f\n", tsc_freq_ghz,
		cycles_to_ns);
}


static void print_exit_info(void)
{
	print_all_buckets();
	printf("\n ---------------- \n");
	print_min_bucketsec();
	print_max_bucketsec();
	print_avg_bucketsec();

}

void main_loop(void)
{
	u64 a, b, delta;
	void *dest, *src;
	int queue_size = 0;

	trace_open();

	init_buckets();

	dest = malloc(default_n);
	if (dest == NULL) {
		printf("failure to allocate %d bytes "
		       " for dest\n", default_n);
		exit(0);
	}
	src = malloc(default_n);
	if (src == NULL) {
		printf("failure to allocate %d bytes "
		       " for src\n", default_n);
		exit(0);
	}

	memset(src, 0, default_n);
	memmove(dest, src, default_n);

	while (1) {
		char buf[500];
		int ret;
		int nr_packets_fill;

		gettick(b);
		memmove(dest, src, default_n);
		gettick(a);
		delta = (a - b) * cycles_to_ns;
		account(delta);

		/* fill up the queue by the amount of
 		 * time that passed */
		nr_packets_fill = delta * mpps * 1000000 / NSEC_PER_SEC;
		queue_size += nr_packets_fill;

		/* decrease the queue by the amount of packets
		 * processed in maxlatency/2 nanoseconds of
		 * full processing.
		 */

		queue_size -= nr_packets_drain_per_block;

		if (queue_size < 0)
			queue_size = 0;

		if (queue_size <= min_queue_size_to_print)
			continue;

		ret = sprintf(buf, "memmove block queue_size=%d queue_dec=%d"
			           " queue_inc=%d delta=%llu ns\n", queue_size,
				   nr_packets_drain_per_block,
				   nr_packets_fill, delta);
		trace_write(buf, ret);

		if (queue_size > max_queue_len) {
			printf("queue length exceeded: "
				" queue_size=%d max_queue_len=%d\n",
				queue_size, max_queue_len);
			ret = sprintf(buf, "queue length exceeded: "
					   "queue_size=%d max_queue_len=%d\n",
					   queue_size, max_queue_len);
			trace_write(buf, ret);
			print_exit_info();
			exit(0);
		}
	}

	free(dest);
	free(src);
}

void sig_handler(int sig)
{
	print_exit_info();
	exit(0);
}

static void install_signals(void)
{
	signal(SIGALRM, sig_handler);
	signal(SIGINT, sig_handler);
}

int calculate_nr_packets_drain_per_block(void)
{
	unsigned long long maxcount;
	int i, time;
	int found = 0;
	int bucket_nr = find_highest_count_bucket();

	maxcount = total_samples() / 40;

	for (i = bucket_nr+1; i <= NR_BUCKETS; i++) {
		if (buckets[i] < maxcount) {
			found = 1;
			break;
		}
	}

	if (found == 0) {
		printf("error, did not find right bucket with < 10%% of total\n");
		exit(0);
	}

	time = i*VALS_PER_BUCKET + VALS_PER_BUCKET-1;
	nr_packets_drain_per_block = time / (cycles_per_packet*cycles_to_ns);

	return nr_packets_drain_per_block;
}

static void print_help(int error)
{
	printf("queuelat V %1.2f\n", VERSION);
	printf("Usage:\n"
	       "queuelat <options>\n\n"
	       "-c N     --cycles N        number of cycles to process one packet (int)\n"
	       "-f F     --freq F          TSC frequency in MHz (float)\n"
	       "-h       --help            show this help menu\n"
	       "-m LEN   --max-len LEN     maximum latency allowed, in nanoseconds (int)\n"
	       "-p F     --packets F       million packets per second (float)\n"
	       "-q N     --queue-len N     minimum queue len to print trace (int)\n"
	       "-t TIME  --timeout TIME    timeout, in seconds (int)\n"
	       );
	exit(error);
}

int main(int argc, char **argv)
{
	double tsc_freq_mhz;
	float max_queue_len_f;
	char *mvalue = NULL;
	char *cvalue = NULL;
	char *pvalue = NULL;
	char *fvalue = NULL;
	char *tvalue = NULL;
	char *qvalue = NULL;

	opterr = 0;

	for (;;) {
		static struct option options[] = {
			{"cycles",	required_argument,	NULL, 'c'},
			{"freq",	required_argument,	NULL, 'f'},
			{"help",	no_argument,		NULL, 'h'},
			{"max-len",	required_argument,	NULL, 'm'},
			{"packets",	required_argument,	NULL, 'p'},
			{"queue-len",	required_argument,	NULL, 'q'},
			{"timeout",	required_argument,	NULL, 't'},
			{NULL, 0, NULL, 0}
		};
		int c = getopt_long(argc, argv, "c:f:hm:p:q:t:", options, NULL);
		if (c == -1)
			break;
		switch (c) {
		case 'c':
			cvalue = optarg;
			break;
		case 'f':
			fvalue = optarg;
			break;
		case '?':
		case 'h':
			print_help(0);
			break;
		case 'm':
			mvalue = optarg;
			break;
		case 'p':
			pvalue = optarg;
			break;
		case 'q':
			qvalue = optarg;
			break;
		case 't':
			tvalue = optarg;
			break;
		default:
			print_help(1);
			break;
		}
	}

	if (mvalue == NULL || cvalue == NULL || pvalue == NULL || fvalue == NULL) {
		printf("options -m, -c, -p and -f are required\n");
		exit(1);
	}

	install_signals();

	maxlatency = atoi(mvalue);
	cycles_per_packet = atoi(cvalue);
	mpps = atof(pvalue);
	tsc_freq_mhz = atof(fvalue);

	if (tvalue) {
		int alarm_secs;
		alarm_secs = atoi(tvalue);
		alarm(alarm_secs);
	}

	if (qvalue)
		min_queue_size_to_print = atoi(qvalue);

	convert_to_ghz(tsc_freq_mhz);

	max_queue_len_f = maxlatency / (cycles_per_packet*cycles_to_ns);
	max_queue_len = max_queue_len_f;

	printf("max_queue_len = %d\n", max_queue_len);
	default_n = measure_n();

	nr_packets_drain_per_block = calculate_nr_packets_drain_per_block();
	print_all_buckets_drainlength();

	printf("default_n=%d nr_packets_drain_per_block=%d\n", default_n,
		nr_packets_drain_per_block);

	main_loop();

	return 0;
}

