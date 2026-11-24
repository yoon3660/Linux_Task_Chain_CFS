/*
 * chain_stage.c — 체인의 한 단계 (보통 chain_run이 fork/exec로 실행한다)
 *
 *   ./chain_stage --role {sensor|perception|planning|control}
 *                 --chain-id 1 --stage N --lat-ns 2000000
 *                 --work-us 2000 --period-ms 100 --frames 3000
 *                 --out results/<run_id>/
 *   (chain_run이 추가로 넘김: --shm-fd --in-fd --out-fd --payload-bytes --passes [--input-dir] [--live])
 *
 * 프레임은 공유메모리 링(슬롯 8개)에 있고, 파이프로는 24바이트 토큰만 보낸다.
 * control 단계가 results/<run_id>/frames.csv 를 쓴다 (측정 중에는 메모리에만 모으고 끝에 한 번에 씀).
 */
#include "chain.h"
#include <dirent.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>

static int    g_role = -1;
static long   g_chain_id = 1, g_stage_arg = -1, g_lat_ns = 2000000;
static long   g_work_us = 0, g_period_ms = 100, g_frames = 3000, g_passes = -1;
static const char *g_out = NULL, *g_input_dir = NULL;
static int    g_shm_fd = -1, g_in_fd = -1, g_out_fd = -1, g_live = 0;
static size_t g_payload = 64 * 1024;

static void die(const char *msg)
{
    fprintf(stderr, "[%s] FATAL: %s (%s)\n", g_role >= 0 ? STAGE_ROLE[g_role] : "?", msg, strerror(errno));
    exit(1);
}

/* 연산 반복 횟수를 정하지 않고 단독 실행했을 때만 쓰는 보정 (보통은 chain_run이 --passes로 넘김) */
static long calibrate(size_t len, long work_us)
{
    if (work_us <= 0) return 0;
    uint8_t *a = malloc(len), *b = malloc(len);
    memset(a, 1, len);
    long passes = 0;
    uint64_t t = now_ns();
    do { chain_work(a, b, len, 1); passes++; } while (now_ns() - t < 50000000ull);
    double per = (double)(now_ns() - t) / passes;
    free(a); free(b);
    long n = (long)(work_us * 1000.0 / per);
    return n < 1 ? 1 : n;
}

/* ---- KITTI 입력: <dir> 안의 .rgb 파일들 를 정렬해서 전부 RAM에 ---- */
struct frameset { uint8_t **data; int n; };
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void load_rgb_dir(const char *dir, size_t expect, struct frameset *fs)
{
    DIR *d = opendir(dir);
    if (!d) die("opendir input-dir");
    char **names = NULL; int n = 0, cap = 0; struct dirent *e;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 5 || strcmp(e->d_name + L - 4, ".rgb")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 512; names = realloc(names, cap * sizeof *names); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (!n) { fprintf(stderr, "no .rgb files in %s\n", dir); exit(1); }
    qsort(names, n, sizeof *names, cmpstr);
    fs->n = n; fs->data = calloc(n, sizeof *fs->data);
    for (int i = 0; i < n; i++) {
        char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        struct stat st;
        if (stat(path, &st) || (size_t)st.st_size != expect) {
            fprintf(stderr, "frame size mismatch: %s (%lld bytes, expected %zu)\n", path, (long long)st.st_size, expect);
            exit(1);
        }
        int fd = open(path, O_RDONLY);
        fs->data[i] = malloc(expect);
        if (fd < 0 || read_all(fd, fs->data[i], expect)) die("read frame");
        close(fd); free(names[i]);
    }
    free(names);
}

/* control이 모으는 한 줄 */
struct row { uint64_t frame, t_sensor, s[CHAIN_STAGES], e[CHAIN_STAGES], rdp[CHAIN_STAGES], rdw[CHAIN_STAGES], e2e; };

static void run_sensor(uint8_t *base, size_t slot_bytes)
{
    struct frameset fs = {0};
    uint8_t *synth = NULL;
    if (g_input_dir) load_rgb_dir(g_input_dir, g_payload, &fs);
    else { synth = malloc(g_payload); for (size_t i = 0; i < g_payload; i++) synth[i] = (uint8_t)(i * 2654435761u >> 24); }

    uint64_t next = now_ns() + 200000000ull; /* 0.2초 뒤 첫 프레임: 모든 단계가 준비될 시간 */
    for (long f = 0; f < g_frames; f++) {
        struct timespec ts = { .tv_sec = (time_t)(next / 1000000000ull), .tv_nsec = (long)(next % 1000000000ull) };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        next += (uint64_t)g_period_ms * 1000000ull;

        uint32_t slot = (uint32_t)(f % RING_SLOTS);
        uint8_t *sp = slot_ptr(base, slot_bytes, 0, slot);
        struct slot_hdr *h = (struct slot_hdr *)sp;
        uint64_t t0 = now_ns();
        struct slot_hdr nh; memset(&nh, 0, sizeof nh);
        nh.len = g_payload; nh.t_sensor_ns = t0; nh.start_ns[0] = t0;
        const uint8_t *src = g_input_dir ? fs.data[f % fs.n] : synth;
        memcpy(sp + SLOT_HDR_BYTES, src, g_payload);           /* 센서가 프레임을 슬롯에 씀 */
        nh.end_ns[0] = now_ns();
        nh.frame = UINT64_MAX;                                  /* frame은 마지막에 써서 '완성'을 표시 */
        memcpy(h, &nh, sizeof nh);
        h->frame = (uint64_t)f;
        struct chain_token tk = { .frame = (uint64_t)f, .slot = slot, .stage = 0, .t_send_ns = nh.end_ns[0] };
        if (write_all(g_out_fd, &tk, sizeof tk)) die("write token");
    }
}

static void run_consumer(uint8_t *base, size_t slot_bytes)
{
    int s = g_role, in_ring = s - 1, out_ring = (s < ST_CTRL) ? s : -1;
    uint8_t *scratch = (s == ST_CTRL) ? malloc(g_payload) : NULL;
    struct row *rows = (s == ST_CTRL) ? calloc((size_t)g_frames, sizeof *rows) : NULL;
    long nrows = 0;

    for (long f = 0; f < g_frames; f++) {
        struct chain_token tk;
        uint64_t rd0 = read_run_delay();
        int r = read_all(g_in_fd, &tk, sizeof tk);           /* 여기서 잠들었다가 깨어난다 */
        uint64_t start = now_ns();
        uint64_t rd1 = read_run_delay();
        if (r == 1) break;
        if (r < 0) die("read token");

        uint8_t *ip = slot_ptr(base, slot_bytes, in_ring, tk.slot);
        struct slot_hdr h; memcpy(&h, ip, sizeof h);
        if (h.frame != tk.frame) { fprintf(stderr, "[%s] ring overrun: slot frame %llu != token %llu\n",
                                           STAGE_ROLE[s], (unsigned long long)h.frame, (unsigned long long)tk.frame); exit(3); }
        h.start_ns[s] = start;
        h.rd_wake_ns[s] = rd1 - rd0;

        uint32_t oslot = (uint32_t)(tk.frame % RING_SLOTS);
        uint8_t *op = (out_ring >= 0) ? slot_ptr(base, slot_bytes, out_ring, oslot) : NULL;
        chain_work(ip + SLOT_HDR_BYTES, op ? op + SLOT_HDR_BYTES : scratch, h.len, g_passes);

        /* 연산 도중 앞 단계가 같은 슬롯을 덮어썼는지 다시 확인 */
        if (((struct slot_hdr *)ip)->frame != tk.frame) { fprintf(stderr, "[%s] ring overrun during work\n", STAGE_ROLE[s]); exit(3); }

        uint64_t rd2 = read_run_delay();
        uint64_t end = now_ns();
        h.rd_proc_ns[s] = rd2 - rd1;
        h.end_ns[s] = end;

        if (op) {
            uint64_t fr = h.frame; h.frame = UINT64_MAX;       /* frame은 마지막에 써서 '완성'을 표시 */
            memcpy(op, &h, sizeof h);
            ((struct slot_hdr *)op)->frame = fr;
            struct chain_token ot = { .frame = fr, .slot = oslot, .stage = (uint32_t)s, .t_send_ns = now_ns() };
            if (write_all(g_out_fd, &ot, sizeof ot)) die("write token");
        } else {
            struct row *w = &rows[nrows++];
            w->frame = h.frame; w->t_sensor = h.t_sensor_ns;
            memcpy(w->s, h.start_ns, sizeof w->s); memcpy(w->e, h.end_ns, sizeof w->e);
            memcpy(w->rdp, h.rd_proc_ns, sizeof w->rdp); memcpy(w->rdw, h.rd_wake_ns, sizeof w->rdw);
            w->e2e = end - h.t_sensor_ns;
            if (!g_live && nrows % 100 == 0)
                fprintf(stderr, "[control] %ld/%ld frames\r", nrows, g_frames);
            if (g_live) {
                double ms = w->e2e / 1e6;
                int bars = (int)(ms * 2); if (bars > 60) bars = 60;
                char bar[64]; memset(bar, '#', (size_t)bars); bar[bars] = 0;
                fprintf(stderr, "%6llu  e2e %7.2f ms  wait %6.2f/%6.2f/%6.2f ms  %s\n",
                        (unsigned long long)w->frame, ms, w->rdw[1] / 1e6, w->rdw[2] / 1e6, w->rdw[3] / 1e6, bar);
            }
        }
    }

    if (rows) {
        char path[4096]; snprintf(path, sizeof path, "%s/frames.csv", g_out);
        FILE *fp = fopen(path, "w");
        if (!fp) die("open frames.csv");
        /* iface-v1 6.4의 열 순서 그대로 + 맨 뒤에 추가 열 (변경 규칙: 추가는 맨 뒤에만) */
        fprintf(fp, "frame,t_sensor_ns,perc_start_ns,perc_end_ns,plan_start_ns,plan_end_ns,ctrl_start_ns,ctrl_end_ns,"
                    "e2e_ns,rd_perc_ns,rd_plan_ns,rd_ctrl_ns,sensor_end_ns,wk_perc_ns,wk_plan_ns,wk_ctrl_ns\n");
        for (long i = 0; i < nrows; i++) {
            struct row *w = &rows[i];
            fprintf(fp, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned long long)w->frame, (unsigned long long)w->t_sensor,
                (unsigned long long)w->s[1], (unsigned long long)w->e[1],
                (unsigned long long)w->s[2], (unsigned long long)w->e[2],
                (unsigned long long)w->s[3], (unsigned long long)w->e[3],
                (unsigned long long)w->e2e,
                (unsigned long long)w->rdp[1], (unsigned long long)w->rdp[2], (unsigned long long)w->rdp[3],
                (unsigned long long)w->e[0],
                (unsigned long long)w->rdw[1], (unsigned long long)w->rdw[2], (unsigned long long)w->rdw[3]);
        }
        fclose(fp);
        fprintf(stderr, "[control] wrote %ld rows to %s\n", nrows, path);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: chain_stage --role R --shm-fd N [--in-fd N] [--out-fd N] --payload-bytes N\n"
                    "       [--chain-id N] [--stage N] [--lat-ns N] [--work-us N] [--passes N]\n"
                    "       [--period-ms N] [--frames N] [--out DIR] [--input-dir DIR] [--live]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && ++i)
        if (OPT("--role")) {
            for (int k = 0; k < CHAIN_STAGES; k++) if (!strcmp(v, STAGE_ROLE[k])) g_role = k;
        }
        else if (OPT("--chain-id"))      g_chain_id = atol(v);
        else if (OPT("--stage"))         g_stage_arg = atol(v);
        else if (OPT("--lat-ns"))        g_lat_ns = atol(v);
        else if (OPT("--work-us"))       g_work_us = atol(v);
        else if (OPT("--passes"))        g_passes = atol(v);
        else if (OPT("--period-ms"))     g_period_ms = atol(v);
        else if (OPT("--frames"))        g_frames = atol(v);
        else if (OPT("--out"))           g_out = v;
        else if (OPT("--input-dir"))     g_input_dir = v;
        else if (OPT("--shm-fd"))        g_shm_fd = atoi(v);
        else if (OPT("--in-fd"))         g_in_fd = atoi(v);
        else if (OPT("--out-fd"))        g_out_fd = atoi(v);
        else if (OPT("--payload-bytes")) g_payload = (size_t)atoll(v);
        else if (!strcmp(a, "--live"))   g_live = 1;
        else usage();
#undef OPT
    }
    if (g_role < 0 || g_shm_fd < 0) usage();
    if (g_role > ST_SENSOR && g_in_fd < 0) usage();
    if (g_role < ST_CTRL && g_out_fd < 0) usage();
    if (g_role == ST_CTRL && !g_out) usage();

    prctl(PR_SET_NAME, STAGE_COMM[g_role], 0, 0, 0);    /* ftrace·perf에서 단계 이름으로 보이게 */

    unsigned int stage = (g_stage_arg > 0) ? (unsigned)g_stage_arg : (unsigned)(g_role + 1);
    /* 패치 커널인지 sysctl 파일로 먼저 확인한다.
     * 주의: 6.5 이후 커널에서는 451번이 cachestat라서, 확인 없이 부르면 엉뚱한 콜이 실행된다. */
    if (g_chain_id != 0) {
        if (access(SYSCTL_CHAIN_POLICY, F_OK) != 0) {
            if (g_role == ST_SENSOR) fprintf(stderr, "[warn] chain kernel not detected (%s missing): running as baseline\n", SYSCTL_CHAIN_POLICY);
        } else {
            long r = sched_set_chain(0, (unsigned)g_chain_id, stage, (uint64_t)g_lat_ns);
            if (r < 0 && errno == ENOSYS) {
                if (g_role == ST_SENSOR) fprintf(stderr, "[warn] sched_set_chain: ENOSYS, running as baseline\n");
            } else if (r < 0) {
                fprintf(stderr, "[%s] sched_set_chain(stage=%u, lat=%ld) failed: %s\n", STAGE_ROLE[g_role], stage, g_lat_ns, strerror(errno));
                exit(4);   /* 패치 커널인데 등록 실패 = 측정이 무의미하므로 바로 중단 */
            }
        }
    }

    size_t slot_bytes = slot_bytes_for(g_payload);
    size_t total = slot_bytes * RING_SLOTS * (CHAIN_STAGES - 1);
    uint8_t *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (base == MAP_FAILED) die("mmap shm");

    if (g_role != ST_SENSOR && g_passes < 0) g_passes = calibrate(g_payload, g_work_us);

    if (g_role == ST_SENSOR) run_sensor(base, slot_bytes);
    else run_consumer(base, slot_bytes);

    if (g_in_fd >= 0) close(g_in_fd);
    if (g_out_fd >= 0) close(g_out_fd);
    return 0;
}
