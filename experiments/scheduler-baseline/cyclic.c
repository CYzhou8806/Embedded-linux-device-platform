/* SPDX-License-Identifier: MIT */
/*
 * Minimal cyclictest-style scheduling-latency probe.
 *
 * Measures the gap between "when clock_nanosleep(TIMER_ABSTIME) was asked
 * to wake up" and "when it actually returned" - the same quantity
 * cyclictest reports, reimplemented small enough to read end to end in a
 * few minutes. Exists as background material for Plan.md V7: before
 * trusting a scheduler-related latency number measured against real
 * acquisition hardware (SCHED_FIFO / CPU affinity / mlockall...), it's
 * worth first seeing what each knob does on a synthetic, dependency-free
 * workload like this one. See README.md in this directory for how to use
 * it and what each knob is expected to change.
 *
 * Usage: cyclic [-i interval_us] [-l loops] [-p priority] [-m]
 *   -i  wakeup interval in microseconds (default 1000 = 1kHz, matches
 *       this project's MCU sample rate)
 *   -l  number of iterations (default 10000)
 *   -p  SCHED_FIFO priority 1-99 (default 0 = leave scheduling alone,
 *       i.e. whatever the default SCHED_OTHER policy already gives)
 *   -m  mlockall(MCL_CURRENT | MCL_FUTURE) before the loop
 *
 * Reports min/avg/max wakeup latency in microseconds.
 *
 * Why TIMER_ABSTIME and not a relative sleep: sleeping for a fixed
 * *duration* each iteration (clock_nanosleep without TIMER_ABSTIME, or
 * usleep()) lets error accumulate iteration over iteration - each
 * iteration starts a little late, so the next relative sleep starts
 * counting from an already-late baseline, and delays compound instead of
 * being independent measurements. Sleeping until a fixed *absolute*
 * deadline computed once up front doesn't have that problem: each
 * iteration's target time only depends on the loop counter, not on how
 * late any previous iteration actually ran - exactly what a real
 * periodic acquisition loop needs, and what this tool measures cleanly.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static int64_t ts_diff_ns(struct timespec a, struct timespec b)
{
	return (int64_t)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
}

static void ts_add_us(struct timespec *t, long us)
{
	t->tv_nsec += us * 1000L;
	while (t->tv_nsec >= 1000000000L) {
		t->tv_nsec -= 1000000000L;
		t->tv_sec += 1;
	}
}

int main(int argc, char **argv)
{
	long interval_us = 1000;
	long loops = 10000;
	int priority = 0;
	int do_mlock = 0;
	int opt;

	while ((opt = getopt(argc, argv, "i:l:p:m")) != -1) {
		switch (opt) {
		case 'i':
			interval_us = strtol(optarg, NULL, 10);
			break;
		case 'l':
			loops = strtol(optarg, NULL, 10);
			break;
		case 'p':
			priority = (int)strtol(optarg, NULL, 10);
			break;
		case 'm':
			do_mlock = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-i interval_us] [-l loops] [-p priority] [-m]\n", argv[0]);
			return 1;
		}
	}

	if (priority > 0) {
		struct sched_param param = { .sched_priority = priority };

		if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
			perror("sched_setscheduler (need CAP_SYS_NICE / root, or an rtprio ulimit)");
			return 1;
		}
	}

	if (do_mlock) {
		if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
			perror("mlockall");
			return 1;
		}
	}

	int64_t min_ns = INT64_MAX, max_ns = 0, sum_ns = 0;
	struct timespec next;

	clock_gettime(CLOCK_MONOTONIC, &next);

	for (long i = 0; i < loops; i++) {
		int ret;
		struct timespec now;
		int64_t lat_ns;

		ts_add_us(&next, interval_us);

		/* TIMER_ABSTIME + EINTR: just retry with the same absolute
		 * target, no remaining-time bookkeeping needed - that's the
		 * other advantage of an absolute deadline over a relative one.
		 */
		do {
			ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		} while (ret == EINTR);
		if (ret != 0) {
			fprintf(stderr, "clock_nanosleep failed: %s\n", strerror(ret));
			return 1;
		}

		clock_gettime(CLOCK_MONOTONIC, &now);

		lat_ns = ts_diff_ns(next, now);
		if (lat_ns < min_ns)
			min_ns = lat_ns;
		if (lat_ns > max_ns)
			max_ns = lat_ns;
		sum_ns += lat_ns;
	}

	printf("loops=%ld interval_us=%ld priority=%d mlockall=%d\n",
	       loops, interval_us, priority, do_mlock);
	printf("latency_us: min=%.1f avg=%.1f max=%.1f\n",
	       min_ns / 1000.0, (double)sum_ns / (double)loops / 1000.0, max_ns / 1000.0);

	return 0;
}
