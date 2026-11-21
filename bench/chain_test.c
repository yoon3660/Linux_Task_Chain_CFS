// SPDX-License-Identifier: GPL-2.0
/*
 * chain_test - sched_set_chain() 인터페이스(iface-v1) 확인 프로그램
 *
 * 빌드:  gcc -O2 -Wall -o chain_test chain_test.c
 * 실행:  ./chain_test          (일반 사용자: 읽기 항목 + 시스템 콜)
 *        sudo ./chain_test     (root: sysctl 쓰기, 통계 초기화까지 확인)
 *
 * 종료 코드: 0 = 전부 통과, 1 = 실패 있음, 2 = 패치 없는 커널(ENOSYS)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#define __NR_sched_set_chain	451

#define CHAIN_STAGE_MAX		16
#define CHAIN_LAT_MIN_NS	100000ULL

#define POLICY_PATH	"/proc/sys/kernel/sched_chain_policy"
#define INTERVAL_PATH	"/proc/sys/kernel/sched_chain_min_interval_ns"
#define STATS_PATH	"/proc/sched_chain_stats"
#define STATS_HEADER \
	"cpu credit_issued credit_preempt credit_consumed credit_expired " \
	"credit_skip_cgroup b_cond b_applied"

static int n_pass, n_fail, n_skip;

static long set_chain(pid_t pid, unsigned int chain_id, unsigned int stage,
		      unsigned long long lat_ns)
{
	return syscall(__NR_sched_set_chain, pid, chain_id, stage, lat_ns);
}

static void check(int ok, const char *what)
{
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
	if (ok)
		n_pass++;
	else
		n_fail++;
}

static void skip(const char *what)
{
	printf("  [SKIP] %s\n", what);
	n_skip++;
}

/* 시스템 콜이 기대한 결과를 내는지: want_errno == 0 이면 성공(0)을 기대 */
static void expect(long ret, int want_errno, const char *what)
{
	int ok = want_errno ? (ret == -1 && errno == want_errno) : (ret == 0);

	if (!ok)
		printf("         ret=%ld errno=%d (%s)\n", ret, errno,
		       strerror(errno));
	check(ok, what);
}

/* /proc/<pid>/sched 에서 "name   :   value" 줄의 값을 읽는다. 없으면 -1 */
static long long sched_field(pid_t pid, const char *name)
{
	char path[64], line[256];
	long long val = -1;
	size_t len = strlen(name);
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/sched", pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *colon;

		if (strncmp(line, name, len) || (line[len] != ' ' && line[len] != ':'))
			continue;
		colon = strchr(line, ':');
		if (colon)
			val = atoll(colon + 1);
		break;
	}
	fclose(f);
	return val;
}

static int fields_are(pid_t pid, long long id, long long stage, long long lat)
{
	long long a = sched_field(pid, "chain_id");
	long long b = sched_field(pid, "chain_stage");
	long long c = sched_field(pid, "chain_lat_ns");

	if (a != id || b != stage || c != lat) {
		printf("         /proc/%d/sched: chain_id=%lld chain_stage=%lld chain_lat_ns=%lld\n",
		       pid, a, b, c);
		return 0;
	}
	return 1;
}

static int read_str(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

/* 0 = 성공, 아니면 errno */
static int write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	int err = 0;

	if (fd < 0)
		return errno;
	if (write(fd, val, strlen(val)) < 0)
		err = errno;
	close(fd);
	return err;
}

static long long read_ll(const char *path)
{
	char buf[64];

	if (read_str(path, buf, sizeof(buf)))
		return -1;
	return atoll(buf);
}

/* 권한 없는 프로세스가 pid 1 을 건드리면 EPERM 이어야 한다 */
static void test_eperm(void)
{
	int status;
	pid_t pid = fork();

	if (pid == 0) {
		long ret;

		if (geteuid() == 0 && (setgroups(0, NULL) || setresgid(65534, 65534, 65534) ||
				       setresuid(65534, 65534, 65534)))
			_exit(2);
		ret = set_chain(1, 7, 2, 2000000);
		_exit(ret == -1 && errno == EPERM ? 0 : 1);
	}
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "권한 없이 남의 pid(1) 설정 -> EPERM");
}

/* fork 한 자식은 체인 정보를 물려받지 않는다 */
static void test_fork(void)
{
	int status;
	pid_t pid = fork();

	if (pid == 0)
		_exit(fields_are(getpid(), 0, 0, 0) ? 0 : 1);
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "fork 한 자식의 chain_id/stage/lat_ns 가 0");
}

static void test_syscall(void)
{
	pid_t me = getpid(), dead;
	int status;

	printf("\n[1] 시스템 콜 sched_set_chain (번호 %d)\n", __NR_sched_set_chain);

	expect(set_chain(0, 7, 2, 2000000), 0, "정상값 (0, 7, 2, 2000000) -> 0");
	check(fields_are(me, 7, 2, 2000000), "/proc/<pid>/sched 에 7 / 2 / 2000000 표시");

	test_fork();

	expect(set_chain(me, 9, 3, 1000000), 0, "자기 pid 를 직접 지정해 덮어쓰기 -> 0");
	check(fields_are(me, 9, 3, 1000000), "덮어쓴 값 9 / 3 / 1000000 표시");

	expect(set_chain(0, 1, 1, CHAIN_LAT_MIN_NS), 0, "경계값: stage 1, lat_ns 100000 -> 0");
	expect(set_chain(0, 1, CHAIN_STAGE_MAX, CHAIN_LAT_MIN_NS), 0, "경계값: stage 16 -> 0");

	expect(set_chain(0, 1, 0, 2000000), EINVAL, "범위 밖: stage 0 -> EINVAL");
	expect(set_chain(0, 1, CHAIN_STAGE_MAX + 1, 2000000), EINVAL, "범위 밖: stage 17 -> EINVAL");
	expect(set_chain(0, 1, 2, CHAIN_LAT_MIN_NS - 1), EINVAL, "범위 밖: lat_ns 99999 -> EINVAL");
	expect(set_chain(0, 1, 2, 1000000000000ULL), EINVAL, "범위 밖: lat_ns 1000초 -> EINVAL");
	expect(set_chain(-1, 1, 2, 2000000), EINVAL, "pid 음수 -> EINVAL");
	check(fields_are(me, 1, CHAIN_STAGE_MAX, CHAIN_LAT_MIN_NS),
	      "실패한 호출은 기존 값을 바꾸지 않음");

	/* 이미 끝난 자식의 pid = 없는 pid */
	dead = fork();
	if (dead == 0)
		_exit(0);
	waitpid(dead, &status, 0);
	expect(set_chain(dead, 7, 2, 2000000), ESRCH, "없는 pid -> ESRCH");

	test_eperm();

	expect(set_chain(0, 0, 5, 12345), 0, "chain_id 0 = 해제 (stage, lat_ns 무시) -> 0");
	check(fields_are(me, 0, 0, 0), "해제 후 0 / 0 / 0 표시");
}

static void test_sysctl(void)
{
	long long policy = read_ll(POLICY_PATH);
	long long interval = read_ll(INTERVAL_PATH);
	char buf[32];

	printf("\n[2] sysctl\n");
	check(policy >= 0 && policy <= 3, POLICY_PATH " 읽기 (0~3)");
	check(interval >= 1000000 && interval <= 1000000000, INTERVAL_PATH " 읽기 (1ms~1s)");
	printf("         policy=%lld min_interval_ns=%lld\n", policy, interval);

	if (geteuid() != 0) {
		skip("sysctl 쓰기와 범위 검사 (root 로 실행해야 확인)");
		return;
	}

	check(write_str(POLICY_PATH, "3") == 0 && read_ll(POLICY_PATH) == 3, "policy 에 3 쓰기");
	check(write_str(POLICY_PATH, "4") == EINVAL && read_ll(POLICY_PATH) == 3,
	      "policy 에 4 쓰기 -> EINVAL, 값 유지");
	check(write_str(INTERVAL_PATH, "1000000") == 0 && read_ll(INTERVAL_PATH) == 1000000,
	      "min_interval 에 1000000(1ms) 쓰기");
	check(write_str(INTERVAL_PATH, "999999") == EINVAL, "min_interval 에 999999 쓰기 -> EINVAL");
	check(write_str(INTERVAL_PATH, "1000000001") == EINVAL,
	      "min_interval 에 1000000001 쓰기 -> EINVAL");

	/* 원래 값으로 되돌린다 */
	snprintf(buf, sizeof(buf), "%lld", policy);
	write_str(POLICY_PATH, buf);
	snprintf(buf, sizeof(buf), "%lld", interval);
	write_str(INTERVAL_PATH, buf);
	check(read_ll(POLICY_PATH) == policy && read_ll(INTERVAL_PATH) == interval,
	      "원래 값으로 복원");
}

static void test_stats(void)
{
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	char buf[8192], *line, *save;
	int lines = 0, header_ok = 0, cols_ok = 1;

	printf("\n[3] %s\n", STATS_PATH);
	if (read_str(STATS_PATH, buf, sizeof(buf))) {
		check(0, STATS_PATH " 읽기");
		return;
	}

	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		unsigned long long v[7];
		int cpu;

		if (lines++ == 0) {
			header_ok = !strcmp(line, STATS_HEADER);
			continue;
		}
		if (sscanf(line, "%d %llu %llu %llu %llu %llu %llu %llu", &cpu,
			   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 8)
			cols_ok = 0;
	}
	check(header_ok, "첫 줄 헤더가 README 와 같음");
	check(lines - 1 == ncpu, "CPU 마다 한 줄");
	printf("         CPU %ld개, 데이터 %d줄\n", ncpu, lines - 1);
	check(cols_ok, "각 줄이 cpu + 숫자 7개");

	if (geteuid() != 0) {
		skip("통계 초기화 (root 로 실행해야 확인)");
		return;
	}
	check(write_str(STATS_PATH, "0") == 0, "echo 0 -> 초기화 성공");
	check(write_str(STATS_PATH, "1") == EINVAL, "0 이 아닌 값 쓰기 -> EINVAL");
}

int main(void)
{
	printf("chain_test: sched_set_chain 인터페이스 확인 (%s)\n",
	       geteuid() == 0 ? "root" : "일반 사용자");

	/*
	 * 패치 없는 6.1 에서는 451번이 비어 있어 ENOSYS 가 나오지만,
	 * 6.5 이상(Ubuntu 기본 6.8 등)에서는 451번이 cachestat() 이라
	 * 다른 오류가 나온다. 그래서 통계 파일 존재 여부도 함께 본다.
	 */
	if (access(STATS_PATH, F_OK) != 0 ||
	    (set_chain(0, 0, 0, 0) == -1 && errno == ENOSYS)) {
		printf("이 커널에는 sched_set_chain 이 없습니다. "
		       "패치한 커널로 부팅했는지 확인하세요 (uname -r).\n");
		return 2;
	}

	test_syscall();
	test_sysctl();
	test_stats();

	printf("\n결과: PASS %d, FAIL %d, SKIP %d\n", n_pass, n_fail, n_skip);
	return n_fail ? 1 : 0;
}
