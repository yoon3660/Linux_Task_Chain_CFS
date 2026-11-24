/*
 * chain_run.c — 체인 4단계 + 배경 부하를 한 번 실행하고 results/<run_id>/ 에 결과를 남기는 런처
 *
 *   ./chain_run --out results/<run_id> [--label policyA] [--load 12 | --load-args "--cpu 4 --io 2"]
 *               [--frames 3000] [--period-ms 100] [--work-us 2000,1000,500]
 *               [--chain-id 1] [--lat-ns 2000000] [--rep 1]
 *               [--input synthetic|kitti] [--kitti-dir DIR] [--buf-kb 64] [--passes a,b,c] [--live]
 *
 * 순서: 연산량 보정(부하 없이) → stress-ng 시작(같은 런처 = 같은 cgroup) → 2초 대기
 *       → 공유메모리·파이프 생성 → 4단계 fork/exec → 끝나면 stress-ng 정지 → meta.txt 등 기록
 *
 * 남기는 파일:
 *   frames.csv                 control이 쓴 프레임별 기록
 *   meta.txt                   실행 조건 (iface-v1 6.4 필드 + 추가 필드)
 *   chain_stats_before/after.txt  /proc/sched_chain_stats 스냅샷 (패치 커널에서만)
 *   stress_tasks.csv           배경 태스크별 실행 시간·런큐 대기 증가분 (공정성 평가용)
 *   stress.log                 stress-ng 출력
 */
#include "chain.h"
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char *g_out = NULL, *g_label = "run", *g_input = "synthetic", *g_kitti = NULL, *g_load_args = NULL;
static long g_frames = 3000, g_period_ms = 100, g_chain_id = 1, g_lat_ns = 2000000, g_rep = 1, g_load = 0;
static long g_work_us[CHAIN_STAGES] = {0, 2000, 1000, 500};
static long g_passes[CHAIN_STAGES] = {0, -1, -1, -1};
static long g_buf_kb = 64;
static int  g_live = 0, g_calib_only = 0;

static void die(const char *m) { fprintf(stderr, "chain_run: %s (%s)\n", m, strerror(errno)); exit(1); }

/* stress-ng는 자기 프로세스 그룹에 따로 둔다 (stress-ng가 끝날 때 자기 그룹 전체에 SIGALRM을 보내기 때문).
 * 대신 Ctrl+C로 런처가 끊기면 stress-ng 그룹도 같이 정리한다. */
static pid_t g_stress = -1;
static void on_sig(int sig)
{
    if (g_stress > 0) kill(-g_stress, SIGINT);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void read_file(const char *path, char *buf, size_t n)
{
    buf[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(buf, n, "n/a"); return; }
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) r = 0;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) buf[--r] = 0;
    for (char *p = buf; *p; p++) if (*p == '\n') *p = ';';
    if (!buf[0]) snprintf(buf, n, "n/a");
}

static void copy_file(const char *src, const char *dst)
{
    char buf[65536];
    int in = open(src, O_RDONLY);
    if (in < 0) return;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ssize_t r;
    while (out >= 0 && (r = read(in, buf, sizeof buf)) > 0) write_all(out, buf, (size_t)r);
    close(in); if (out >= 0) close(out);
}

static long kitti_bytes_per_frame(const char *dir)
{
    char path[4096], line[256]; long v = -1;
    snprintf(path, sizeof path, "%s/meta.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "bytes_per_frame=", 16)) v = atol(line + 16);
    fclose(f);
    return v;
}

static long calibrate(size_t len, long work_us)
{
    if (work_us <= 0) return 0;
    uint8_t *a = malloc(len), *b = malloc(len);
    memset(a, 1, len);
    for (int w = 0; w < 3; w++) chain_work(a, b, len, 1);   /* 워밍업 */
    long passes = 0;
    uint64_t t = now_ns();
    do { chain_work(a, b, len, 1); passes++; } while (now_ns() - t < 200000000ull);
    double per = (double)(now_ns() - t) / passes;
    free(a); free(b);
    long n = (long)(work_us * 1000.0 / per);
    return n < 1 ? 1 : n;
}

/* stress-ng 자식 프로세스들의 pid (/proc/<pid>/task/<pid>/children) */
static int stress_children(pid_t root, pid_t *out, int max)
{
    char path[128], buf[8192];
    snprintf(path, sizeof path, "/proc/%d/task/%d/children", root, root);
    read_file(path, buf, sizeof buf);
    int n = 0;
    for (char *tok = strtok(buf, " ;"); tok && n < max; tok = strtok(NULL, " ;")) {
        long p = atol(tok); if (p > 0) out[n++] = (pid_t)p;
    }
    return n;
}

struct sstat { pid_t pid; unsigned long long exec, rd, sw; };
static int snap_stress(pid_t root, struct sstat *s, int max)
{
    pid_t kids[512]; int n = stress_children(root, kids, 512), k = 0;
    for (int i = 0; i < n && k < max; i++) {
        char path[128], buf[256];
        snprintf(path, sizeof path, "/proc/%d/schedstat", kids[i]);
        read_file(path, buf, sizeof buf);
        s[k].pid = kids[i];
        if (sscanf(buf, "%llu %llu %llu", &s[k].exec, &s[k].rd, &s[k].sw) == 3) k++;
    }
    return k;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: chain_run --out DIR [--label L] [--load N | --load-args \"...\"] [--rep N]\n"
        "                 [--frames N] [--period-ms N] [--work-us a,b,c] [--passes a,b,c]\n"
        "                 [--chain-id N (0=등록 안 함)] [--lat-ns N]\n"
        "                 [--input synthetic|kitti] [--kitti-dir DIR] [--buf-kb N] [--live]\n"
        "       chain_run --calibrate-only [--input ...] [--work-us a,b,c]   (passes만 출력)\n");
    exit(2);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && ++i)
        if (OPT("--out")) g_out = v;
        else if (OPT("--label")) g_label = v;
        else if (OPT("--load")) g_load = atol(v);
        else if (OPT("--load-args")) g_load_args = v;
        else if (OPT("--rep")) g_rep = atol(v);
        else if (OPT("--frames")) g_frames = atol(v);
        else if (OPT("--period-ms")) g_period_ms = atol(v);
        else if (OPT("--work-us")) sscanf(v, "%ld,%ld,%ld", &g_work_us[1], &g_work_us[2], &g_work_us[3]);
        else if (OPT("--passes")) sscanf(v, "%ld,%ld,%ld", &g_passes[1], &g_passes[2], &g_passes[3]);
        else if (OPT("--chain-id")) g_chain_id = atol(v);
        else if (OPT("--lat-ns")) g_lat_ns = atol(v);
        else if (OPT("--input")) g_input = v;
        else if (OPT("--kitti-dir")) g_kitti = v;
        else if (OPT("--buf-kb")) g_buf_kb = atol(v);
        else if (!strcmp(a, "--live")) g_live = 1;
        else if (!strcmp(a, "--calibrate-only")) g_calib_only = 1;
        else usage();
#undef OPT
    }
    if (!g_out && !g_calib_only) usage();
    int kitti = !strcmp(g_input, "kitti");
    if (kitti && !g_kitti) { fprintf(stderr, "--kitti-dir required\n"); return 2; }

    /* 결과 폴더 */
    char cmd[4096];
    if (g_calib_only) goto skip_mkdir;
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", g_out);
    if (system(cmd) != 0) die("mkdir out");
skip_mkdir:;

    /* chain_stage 경로 = 이 실행 파일과 같은 폴더 */
    char self[PATH_MAX] = {0}, stage_bin[PATH_MAX + 32];
    if (readlink("/proc/self/exe", self, sizeof self - 1) < 0) die("readlink");
    snprintf(stage_bin, sizeof stage_bin, "%s/chain_stage", dirname(self));

    /* 페이로드 크기 */
    size_t payload = (size_t)g_buf_kb * 1024;
    if (kitti) {
        long b = kitti_bytes_per_frame(g_kitti);
        if (b <= 0) { fprintf(stderr, "cannot read bytes_per_frame from %s/meta.txt\n", g_kitti); return 1; }
        payload = (size_t)b;
    }
    size_t slot_bytes = slot_bytes_for(payload);
    size_t shm_total = slot_bytes * RING_SLOTS * (CHAIN_STAGES - 1);

    /* 1) 연산량 보정: 배경 부하를 켜기 전에 */
    for (int s = 1; s < CHAIN_STAGES; s++)
        if (g_passes[s] < 0) g_passes[s] = calibrate(payload, g_work_us[s]);
    fprintf(stderr, "chain_run: payload=%zu B, slot=%zu B, passes=%ld,%ld,%ld\n",
            payload, slot_bytes, g_passes[1], g_passes[2], g_passes[3]);
    if (g_calib_only) { printf("%ld,%ld,%ld\n", g_passes[1], g_passes[2], g_passes[3]); return 0; }

    /* 2) chain_stats 초기화 시도 + 스냅샷 (패치 커널에서만 존재) */
    char path[4096];
    { int fd = open(PROC_CHAIN_STATS, O_WRONLY); if (fd >= 0) { write_all(fd, "0\n", 2); close(fd); } }
    snprintf(path, sizeof path, "%s/chain_stats_before.txt", g_out); copy_file(PROC_CHAIN_STATS, path);

    /* 3) 배경 부하: 이 런처의 자식 = 같은 cgroup scope */
    pid_t stress = -1;
    char stress_log[4096]; snprintf(stress_log, sizeof stress_log, "%s/stress.log", g_out);
    if (g_load > 0 || g_load_args) {
        char loadspec[1024];
        if (g_load_args) snprintf(loadspec, sizeof loadspec, "%s", g_load_args);
        else snprintf(loadspec, sizeof loadspec, "--cpu %ld", g_load);
        stress = fork();
        if (stress == 0) {
            setpgid(0, 0);
            char line[8192];
            snprintf(line, sizeof line, "exec stress-ng %s --timeout 0 --metrics-brief --log-file '%s' >/dev/null 2>&1",
                     loadspec, stress_log);
            execl("/bin/sh", "sh", "-c", line, (char *)NULL);
            _exit(127);
        }
        setpgid(stress, stress);
        g_stress = stress;
        signal(SIGINT, on_sig);
        signal(SIGTERM, on_sig);
        sleep(2);
        int st; if (waitpid(stress, &st, WNOHANG) == stress) { fprintf(stderr, "stress-ng failed to start (installed?)\n"); return 1; }
    }

    struct sstat before[512], after[512];
    int nb = (stress > 0) ? snap_stress(stress, before, 512) : 0;

    /* 4) 공유메모리 + 파이프 3개 */
    int shm = memfd_create("chain_ring", 0);
    if (shm < 0) die("memfd_create");
    if (ftruncate(shm, (off_t)shm_total)) die("ftruncate");
    int p[CHAIN_STAGES - 1][2];
    for (int i = 0; i < CHAIN_STAGES - 1; i++) if (pipe(p[i])) die("pipe");

    uint64_t t_start = now_ns();
    pid_t kids[CHAIN_STAGES];
    for (int s = 0; s < CHAIN_STAGES; s++) {
        kids[s] = fork();
        if (kids[s] < 0) die("fork");
        if (kids[s] == 0) {
            int in_fd = (s == 0) ? -1 : p[s - 1][0];
            int out_fd = (s == CHAIN_STAGES - 1) ? -1 : p[s][1];
            for (int i = 0; i < CHAIN_STAGES - 1; i++) {
                if (p[i][0] != in_fd) close(p[i][0]);
                if (p[i][1] != out_fd) close(p[i][1]);
            }
            char a_shm[16], a_in[16], a_out[16], a_pay[32], a_pass[32], a_work[32], a_fr[32], a_per[32], a_cid[32], a_lat[32], a_st[8];
            snprintf(a_shm, sizeof a_shm, "%d", shm);
            snprintf(a_in, sizeof a_in, "%d", in_fd);
            snprintf(a_out, sizeof a_out, "%d", out_fd);
            snprintf(a_pay, sizeof a_pay, "%zu", payload);
            snprintf(a_pass, sizeof a_pass, "%ld", g_passes[s]);
            snprintf(a_work, sizeof a_work, "%ld", g_work_us[s]);
            snprintf(a_fr, sizeof a_fr, "%ld", g_frames);
            snprintf(a_per, sizeof a_per, "%ld", g_period_ms);
            snprintf(a_cid, sizeof a_cid, "%ld", g_chain_id);
            snprintf(a_lat, sizeof a_lat, "%ld", g_lat_ns);
            snprintf(a_st, sizeof a_st, "%d", s + 1);
            const char *av[40]; int k = 0;
            av[k++] = stage_bin; av[k++] = "--role"; av[k++] = STAGE_ROLE[s];
            av[k++] = "--chain-id"; av[k++] = a_cid; av[k++] = "--stage"; av[k++] = a_st;
            av[k++] = "--lat-ns"; av[k++] = a_lat; av[k++] = "--work-us"; av[k++] = a_work;
            av[k++] = "--passes"; av[k++] = a_pass; av[k++] = "--period-ms"; av[k++] = a_per;
            av[k++] = "--frames"; av[k++] = a_fr; av[k++] = "--out"; av[k++] = g_out;
            av[k++] = "--shm-fd"; av[k++] = a_shm; av[k++] = "--payload-bytes"; av[k++] = a_pay;
            if (in_fd >= 0) { av[k++] = "--in-fd"; av[k++] = a_in; }
            if (out_fd >= 0) { av[k++] = "--out-fd"; av[k++] = a_out; }
            if (s == 0 && kitti) { av[k++] = "--input-dir"; av[k++] = g_kitti; }
            if (s == CHAIN_STAGES - 1 && g_live) av[k++] = "--live";
            av[k] = NULL;
            execv(stage_bin, (char *const *)av);
            _exit(127);
        }
    }
    for (int i = 0; i < CHAIN_STAGES - 1; i++) { close(p[i][0]); close(p[i][1]); }
    close(shm);

    int rc = 0;
    for (int s = 0; s < CHAIN_STAGES; s++) {
        int st; waitpid(kids[s], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { rc = 1; fprintf(stderr, "chain_run: stage %s exited abnormally (status %d)\n", STAGE_ROLE[s], st); }
    }
    uint64_t t_end = now_ns();

    /* 5) 배경 태스크별 증가분 → stress_tasks.csv, 그 뒤 stress-ng 정지 */
    if (stress > 0) {
        int na = snap_stress(stress, after, 512);
        snprintf(path, sizeof path, "%s/stress_tasks.csv", g_out);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "pid,exec_ns,run_delay_ns,switches\n");
            for (int i = 0; i < na; i++)
                for (int j = 0; j < nb; j++)
                    if (after[i].pid == before[j].pid)
                        fprintf(f, "%d,%llu,%llu,%llu\n", after[i].pid, after[i].exec - before[j].exec,
                                after[i].rd - before[j].rd, after[i].sw - before[j].sw);
            fclose(f);
        }
        kill(stress, SIGINT);
        int st; waitpid(stress, &st, 0);
        g_stress = -1;
    }

    snprintf(path, sizeof path, "%s/chain_stats_after.txt", g_out); copy_file(PROC_CHAIN_STATS, path);

    /* 6) meta.txt (iface-v1 6.4 필드 → 추가 필드) */
    char kern[256], policy[64], minint[64], gov[64], autog[32], cg_chain[512], cg_load[512], tmp[128];
    read_file("/proc/sys/kernel/osrelease", kern, sizeof kern);
    read_file(SYSCTL_CHAIN_POLICY, policy, sizeof policy);
    read_file(SYSCTL_CHAIN_MIN_INT, minint, sizeof minint);
    read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov, sizeof gov);
    read_file("/proc/sys/kernel/sched_autogroup_enabled", autog, sizeof autog);
    read_file("/proc/self/cgroup", cg_chain, sizeof cg_chain);
    if (stress > 0) { snprintf(tmp, sizeof tmp, "/proc/%d/cgroup", stress); read_file(tmp, cg_load, sizeof cg_load); }
    else snprintf(cg_load, sizeof cg_load, "none");
    const char *git = getenv("CHAIN_GIT_REV");

    snprintf(path, sizeof path, "%s/meta.txt", g_out);
    FILE *m = fopen(path, "w");
    if (!m) die("meta.txt");
    fprintf(m, "kernel=%s\n", kern);
    fprintf(m, "git_rev=%s\n", git ? git : "unknown");
    fprintf(m, "policy=%s\n", policy);
    fprintf(m, "min_interval_ns=%s\n", minint);
    if (g_load_args && !g_load) fprintf(m, "load=mixed\n"); else fprintf(m, "load=%ld\n", g_load);
    fprintf(m, "rep=%ld\n", g_rep);
    fprintf(m, "governor=%s\n", gov);
    fprintf(m, "chain_cgroup=%s\n", cg_chain);
    fprintf(m, "load_cgroup=%s\n", cg_load);
    /* 추가 필드 */
    fprintf(m, "label=%s\n", g_label);
    fprintf(m, "load_args=%s\n", g_load_args ? g_load_args : (g_load ? "--cpu N" : "none"));
    fprintf(m, "input=%s\n", kitti ? g_kitti : "synthetic");
    fprintf(m, "payload_bytes=%zu\nslot_bytes=%zu\n", payload, slot_bytes);
    fprintf(m, "frames=%ld\nperiod_ms=%ld\n", g_frames, g_period_ms);
    fprintf(m, "work_us=%ld,%ld,%ld\npasses=%ld,%ld,%ld\n", g_work_us[1], g_work_us[2], g_work_us[3], g_passes[1], g_passes[2], g_passes[3]);
    fprintf(m, "chain_id=%ld\nlat_ns=%ld\n", g_chain_id, g_lat_ns);
    fprintf(m, "autogroup=%s\n", autog);
    fprintf(m, "chain_kernel=%s\n", access(SYSCTL_CHAIN_POLICY, F_OK) == 0 ? "yes" : "no");
    fprintf(m, "nproc=%ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    fprintf(m, "duration_s=%.2f\n", (t_end - t_start) / 1e9);
    fprintf(m, "exit_ok=%d\n", rc == 0);
    fclose(m);

    fprintf(stderr, "chain_run: done -> %s (%.1f s)%s\n", g_out, (t_end - t_start) / 1e9, rc ? " [STAGE ERROR]" : "");
    return rc;
}
