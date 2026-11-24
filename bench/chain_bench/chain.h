/*
 * chain.h — Task Chain 벤치마크 공용 헤더 (확정안 iface-v1, 6장)
 *
 * - 시스템 콜 래퍼 sched_set_chain() (번호 451)
 * - sysctl · 통계 파일 경로
 * - 단계 간 토큰 (파이프로 보내는 24바이트)
 * - 공유메모리 슬롯 헤더 (프레임 데이터 앞 256바이트)
 */
#ifndef CHAIN_H
#define CHAIN_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>

/* ---- 커널 인터페이스 (iface-v1 2~4장) ---- */
#define CHAIN_SYSCALL_NR        451
#define SYSCTL_CHAIN_POLICY     "/proc/sys/kernel/sched_chain_policy"
#define SYSCTL_CHAIN_MIN_INT    "/proc/sys/kernel/sched_chain_min_interval_ns"
#define PROC_CHAIN_STATS        "/proc/sched_chain_stats"

static inline long sched_set_chain(pid_t pid, unsigned int chain_id,
                                   unsigned int stage, uint64_t lat_ns)
{
    return syscall(CHAIN_SYSCALL_NR, pid, chain_id, stage, lat_ns);
}

/* ---- 체인 구성 ---- */
#define CHAIN_STAGES   4          /* sensor, perception, planning, control */
#define RING_SLOTS     8          /* 단계 사이 링마다 슬롯 8개 */
#define SLOT_HDR_BYTES 256        /* 슬롯 앞의 헤더 영역 (캐시라인 4개) */

enum { ST_SENSOR = 0, ST_PERC = 1, ST_PLAN = 2, ST_CTRL = 3 };
static const char *const STAGE_ROLE[CHAIN_STAGES] = {"sensor", "perception", "planning", "control"};
static const char *const STAGE_COMM[CHAIN_STAGES] = {"ch_sensor", "ch_perc", "ch_plan", "ch_ctrl"};

/* 파이프로 보내는 토큰: 정확히 24바이트 (iface-v1 6.2) */
struct chain_token {
    uint64_t frame;      /* 프레임 번호 */
    uint32_t slot;       /* 링 안의 슬롯 번호 */
    uint32_t stage;      /* 보낸 단계 (0..2) */
    uint64_t t_send_ns;  /* write() 직전 시각, CLOCK_MONOTONIC */
};
_Static_assert(sizeof(struct chain_token) == 24, "chain_token must be 24 bytes");

/* 공유메모리 슬롯 헤더: 각 단계가 자기 칸을 채우며 다음 단계로 넘긴다 */
struct slot_hdr {
    uint64_t frame;                    /* 생산자가 마지막에 씀. 소비자가 토큰과 대조 */
    uint64_t len;                      /* 페이로드 바이트 */
    uint64_t t_sensor_ns;              /* 센서가 프레임을 만든 시각 (e2e 시작점) */
    uint64_t start_ns[CHAIN_STAGES];   /* 토큰 read()가 돌아온 직후 */
    uint64_t end_ns[CHAIN_STAGES];     /* 다음 토큰 write() 직전 (control은 연산 직후) */
    uint64_t rd_proc_ns[CHAIN_STAGES]; /* 처리 중 run_delay 증가분 (iface-v1 6.4의 rd_*) */
    uint64_t rd_wake_ns[CHAIN_STAGES]; /* read() 대기 동안 run_delay 증가분 = 깨어난 뒤 런큐 대기 */
};
_Static_assert(sizeof(struct slot_hdr) <= SLOT_HDR_BYTES, "slot_hdr too large");

/* ---- 유틸 ---- */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* /proc/self/schedstat 의 2번째 필드 = run_delay (런큐에서 기다린 누적 ns) */
static inline uint64_t read_run_delay(void)
{
    char buf[128];
    unsigned long long a = 0, b = 0, c = 0;
    int fd = open("/proc/self/schedstat", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    if (sscanf(buf, "%llu %llu %llu", &a, &b, &c) < 2) return 0;
    return (uint64_t)b;
}

static inline size_t round_up(size_t x, size_t a) { return (x + a - 1) / a * a; }

/* 슬롯 하나 = 헤더 256B + 페이로드, 4KB 단위로 올림 */
static inline size_t slot_bytes_for(size_t payload) { return round_up(SLOT_HDR_BYTES + payload, 4096); }

/* 링 r(0: sensor→perc, 1: perc→plan, 2: plan→ctrl)의 슬롯 s 시작 주소 */
static inline uint8_t *slot_ptr(uint8_t *base, size_t slot_bytes, int ring, uint32_t slot)
{
    return base + ((size_t)ring * RING_SLOTS + slot) * slot_bytes;
}

/* 연산: 입력 버퍼를 8바이트 단위로 읽고 출력 버퍼에 쓴다 (끝의 8바이트 미만 자투리는 건너뜀). passes 번 반복 */
static volatile uint64_t g_sink;
static inline void chain_work(const uint8_t *in, uint8_t *out, size_t len, long passes)
{
    uint64_t acc = 1469598103934665603ull;
    size_t n = len / 8;
    for (long p = 0; p < passes; p++) {
        const uint64_t *src = (const uint64_t *)in;
        uint64_t *dst = (uint64_t *)out;
        for (size_t i = 0; i < n; i++) {
            uint64_t v = src[i];
            acc = (acc ^ v) * 1099511628211ull;
            dst[i] = v + (uint64_t)p;
        }
    }
    g_sink += acc;
}

static inline int write_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        c += w; n -= (size_t)w;
    }
    return 0;
}

/* 0: 성공, 1: EOF, -1: 오류 */
static inline int read_all(int fd, void *p, size_t n)
{
    char *c = p;
    while (n) {
        ssize_t r = read(fd, c, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return 1;
        c += r; n -= (size_t)r;
    }
    return 0;
}

#endif /* CHAIN_H */
