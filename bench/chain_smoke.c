// SPDX-License-Identifier: GPL-2.0
/*
 * chain_smoke.c - quick functional check of policy A (chain wakeup credit)
 *
 * sensor -> perception -> planning -> control, one process per stage,
 * connected by pipes. Every stage reports its handoff latency: the time from
 * the previous stage's write() to this stage returning from read(), i.e. the
 * wakeup plus the runqueue wait that policy A tries to cut. control also
 * reports the end-to-end latency.
 *
 * This is a smoke test for the VM, not the measurement workload: no shared
 * 64KB frame buffer, no CSV. Numbers from a VM are only a sanity check.
 *
 * Build: gcc -O2 -Wall -o chain_smoke chain_smoke.c
 * Run:   ./chain_smoke [-n frames] [-p period_ms] [-l lat_ns] [-c chain_id]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NR_sched_set_chain	451
#define NSTAGES			4
#define WARMUP			10	/* frames left out of the statistics */

/* 24 bytes, well below PIPE_BUF: one write() is one atomic token */
struct token {
	uint64_t frame;
	uint64_t t_sensor;	/* when sensor produced the frame */
	uint64_t t_write;	/* when the previous stage wrote this token */
};

static const char *names[NSTAGES] = {
	"sensor", "perception", "planning", "control"
};
/* CPU time each stage burns per frame (us) */
static const long work_us[NSTAGES] = { 0, 2000, 1000, 500 };

static uint64_t now_ns(clockid_t clk)
{
	struct timespec ts;

	clock_gettime(clk, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Burn @us of this thread's CPU time, however long that takes on the clock. */
static void burn_cpu_us(long us)
{
	uint64_t end = now_ns(CLOCK_THREAD_CPUTIME_ID) + (uint64_t)us * 1000;
	volatile uint64_t x = 0;

	while (now_ns(CLOCK_THREAD_CPUTIME_ID) < end)
		for (int i = 0; i < 1000; i++)
			x += i;
}

static void register_stage(int chain_id, int stage, uint64_t lat_ns)
{
	char comm[16];

	/* e.g. "c1s3_planni": easy to grep in ftrace / perf sched output */
	snprintf(comm, sizeof(comm), "c%ds%d_%s", chain_id, stage,
		 names[stage - 1]);
	prctl(PR_SET_NAME, comm);

	if (syscall(NR_sched_set_chain, 0, chain_id, stage, lat_ns) != 0)
		fprintf(stderr, "[%s] sched_set_chain failed (%s), running as baseline\n",
			names[stage - 1], strerror(errno));
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void report(const char *what, uint64_t *v, size_t n)
{
	if (n == 0) {
		printf("%-24s no samples\n", what);
		return;
	}
	qsort(v, n, sizeof(*v), cmp_u64);
	printf("%-24s n=%-5zu p50=%8.1fus  p99=%8.1fus  max=%8.1fus\n", what, n,
	       v[n / 2] / 1e3, v[(size_t)((n - 1) * 0.99)] / 1e3, v[n - 1] / 1e3);
}

static void run_stage(int stage, int in_fd, int out_fd, long frames,
		      int chain_id, uint64_t lat_ns)
{
	uint64_t *hand = calloc(frames, sizeof(*hand));
	uint64_t *e2e = calloc(frames, sizeof(*e2e));
	size_t nh = 0, ne = 0;
	struct token tok;
	char label[64];

	if (!hand || !e2e) {
		perror("calloc");
		exit(1);
	}
	register_stage(chain_id, stage, lat_ns);

	for (;;) {
		ssize_t r = read(in_fd, &tok, sizeof(tok));
		uint64_t t_run;

		if (r == 0)
			break;			/* previous stage is done */
		if (r != (ssize_t)sizeof(tok)) {
			if (r < 0 && errno == EINTR)
				continue;
			perror("read");
			exit(1);
		}

		t_run = now_ns(CLOCK_MONOTONIC);
		if (tok.frame >= WARMUP && nh < (size_t)frames)
			hand[nh++] = t_run - tok.t_write;

		burn_cpu_us(work_us[stage - 1]);

		if (out_fd >= 0) {
			tok.t_write = now_ns(CLOCK_MONOTONIC);
			if (write(out_fd, &tok, sizeof(tok)) != (ssize_t)sizeof(tok)) {
				perror("write");
				exit(1);
			}
		} else if (tok.frame >= WARMUP && ne < (size_t)frames) {
			e2e[ne++] = now_ns(CLOCK_MONOTONIC) - tok.t_sensor;
		}
	}

	snprintf(label, sizeof(label), "handoff -> %s", names[stage - 1]);
	report(label, hand, nh);
	if (out_fd < 0)
		report("end-to-end", e2e, ne);
	exit(0);			/* closes out_fd: next stage sees EOF */
}

int main(int argc, char **argv)
{
	long frames = 300, period_ms = 100;
	uint64_t lat_ns = 4000000;	/* > wakeup_gran (3ms on 6 CPUs) */
	int chain_id = 1, opt, out_fd;
	int fds[NSTAGES - 1][2];
	struct timespec next;

	while ((opt = getopt(argc, argv, "n:p:l:c:")) != -1) {
		switch (opt) {
		case 'n': frames = atol(optarg); break;
		case 'p': period_ms = atol(optarg); break;
		case 'l': lat_ns = strtoull(optarg, NULL, 10); break;
		case 'c': chain_id = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-n frames] [-p period_ms] [-l lat_ns] [-c chain_id]\n",
				argv[0]);
			return 2;
		}
	}
	if (frames <= 0 || period_ms <= 0 || chain_id <= 0) {
		fprintf(stderr, "frames, period_ms and chain_id must be > 0\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (int i = 0; i < NSTAGES - 1; i++) {
		if (pipe(fds[i])) {
			perror("pipe");
			return 1;
		}
	}

	/* stages 2..4: read fds[s-2], write fds[s-1] (control writes nothing) */
	for (int s = 2; s <= NSTAGES; s++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			int in_fd = fds[s - 2][0];
			int my_out = s < NSTAGES ? fds[s - 1][1] : -1;

			for (int i = 0; i < NSTAGES - 1; i++) {
				if (fds[i][0] != in_fd)
					close(fds[i][0]);
				if (fds[i][1] != my_out)
					close(fds[i][1]);
			}
			run_stage(s, in_fd, my_out, frames, chain_id, lat_ns);
		}
	}

	/* this process is the sensor (stage 1) */
	out_fd = fds[0][1];
	for (int i = 0; i < NSTAGES - 1; i++) {
		close(fds[i][0]);
		if (fds[i][1] != out_fd)
			close(fds[i][1]);
	}
	register_stage(chain_id, 1, lat_ns);

	clock_gettime(CLOCK_MONOTONIC, &next);
	for (long f = 0; f < frames + WARMUP; f++) {
		struct token tok = { .frame = (uint64_t)f };

		next.tv_nsec += period_ms * 1000000L;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
			;

		tok.t_sensor = tok.t_write = now_ns(CLOCK_MONOTONIC);
		if (write(out_fd, &tok, sizeof(tok)) != (ssize_t)sizeof(tok)) {
			perror("write");
			return 1;
		}
	}
	close(out_fd);

	while (wait(NULL) > 0)
		;
	return 0;
}
