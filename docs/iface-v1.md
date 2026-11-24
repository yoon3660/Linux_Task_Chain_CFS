# Linux_Task_Chain_CFS

# Task Chain 인식 CFS — 인터페이스 확정안 (iface-v1 초안)

- 상태: **초안** → 11/21 회의에서 합의하면 이 문서와 스텁 커밋에 `iface-v1` 태그를 붙인다
- 기준 문서: 제안서(25쪽), 설계 비교표(4쪽), 발표 슬라이드(17쪽)
- 이 문서와 다른 문서가 다르면 **이 문서가 우선**한다. 바뀐 점은 맨 아래 "기존 문서와 달라진 점"에 모아 두었다.

---

## 0. 공통 기준

| 항목 | 확정 내용 |
|---|---|
| 커널 | `v6.1.___` (회의에서 kernel.org의 6.1.y 최신 태그 하나로 기입, 이후 변경 금지) |
| .config | Ubuntu 22.04 config 상속 + `olddefconfig` + `CONFIG_SCHEDSTATS=y`, `CONFIG_SCHED_DEBUG=y`, `CONFIG_FTRACE=y`. 완성된 `.config`를 저장소 `configs/chain.config`로 커밋하고 모두 이 파일을 쓴다 |
| 개발용 추가 옵션 | VM에서만 `CONFIG_DEBUG_PREEMPT=y`, `CONFIG_PROVE_LOCKING=y` (본체 측정 커널에는 넣지 않음) |
| 저장소 | 커널 트리 1개. 브랜치: `main`(통합), `iface`(①), `policy-a`(②), `policy-b`(③). 벤치마크·스크립트는 같은 저장소의 `bench/` 디렉터리(④) |
| 커널 이미지 | **측정용 이미지는 1개.** baseline / A / B / A+B는 sysctl로 전환한다 (2장) |
| 순정 커널 | 패치 없는 `v6.1.___`도 한 번 빌드해 둔다. "정책 0 = 순정과 같음"을 hackbench로 1회 확인하는 용도 |

---

## 1. 커널 내부 자료구조 (`include/linux/sched.h`)

`struct task_struct` 안, `struct sched_entity se;` 바로 아래에 추가한다.

```c
	/* Task chain hint (sched_set_chain) */
	u32	chain_id;           /* 0 = 체인 아님 */
	u8	chain_stage;        /* 1..CHAIN_STAGE_MAX, 체인 아니면 0 */
	u64	chain_lat_ns;       /* 1회 크레딧 크기 (ns) */
	u64	chain_credit_ns;    /* 보유 크레딧 (ns), 0 = 없음 */
	u64	chain_last_credit;  /* 마지막 발급 시각, ktime_get_ns() 기준 */
```

| 규칙 | 내용 |
|---|---|
| 필드 수 | **5개** (제안서의 "4개"는 오기) |
| 상수 | `#define CHAIN_STAGE_MAX 16` |
| 시계 | `chain_last_credit`의 기록과 비교는 모두 `ktime_get_ns()`로 한다 (CPU 사이에서 비교해도 안전한 단조 시계) |
| 쓰기 위치 | `chain_credit_ns`, `chain_last_credit`는 `try_to_wake_up()`의 `p->pi_lock` 보유 구간 또는 해당 rq lock 보유 구간에서만 쓴다. 다른 곳에서 읽을 때는 `READ_ONCE()`를 쓴다 |
| fork | `kernel/sched/core.c` `__sched_fork()`에서 5개 필드를 모두 0으로 초기화 (자식은 상속 안 함) |
| exec | 유지 (초기화하지 않음) |
| 단위 | 태스크(스레드) 단위. 멀티스레드 프로세스는 스레드마다 따로 설정 |

### 공용 헬퍼 (`kernel/sched/sched.h`)

```c
/* waker가 wakee의 바로 앞 단계인가 */
static inline bool task_is_chain_successor(struct task_struct *waker,
					   struct task_struct *wakee)
{
	return wakee->chain_id != 0 &&
	       waker->chain_id == wakee->chain_id &&
	       wakee->chain_stage == waker->chain_stage + 1;
}
```

②와 ③이 모두 이 헬퍼만 사용한다. 조건을 각자 따로 구현하지 않는다.

---

## 2. 정책 스위치 (sysctl)

| sysctl | 값 | 기본값 | 범위 |
|---|---|---|---|
| `kernel.sched_chain_policy` | 비트마스크: bit0 = 정책 A, bit1 = 정책 B | `0` | 0~3 (범위 밖이면 `-EINVAL`) |
| `kernel.sched_chain_min_interval_ns` | 태스크당 크레딧 최소 발급 간격 | `50000000` (50ms) | 1,000,000 ~ 1,000,000,000 |

| 값 | 비교군 |
|---|---|
| `0` | baseline (추가 코드는 첫 분기에서 빠져나감) |
| `1` | A |
| `2` | B만 (보조 실험) |
| `3` | A+B |

- 값을 바꾸면 **다음 wakeup부터** 적용된다. 이미 발급된 크레딧은 남아 있어도, bit0이 꺼져 있으면 선점 판정에서 쓰지 않는다.
- 발급 간격 기본값을 50ms로 둔 이유: 체인 주기(100ms)와 같게 두면, 지터 때문에 실제 간격이 99.9ms가 된 프레임에서 크레딧이 발급되지 않는다.
- 등록 위치: `kernel/sched/core.c`의 sysctl 테이블 (①이 담당). 값은 `READ_ONCE()`로 읽는다.

---

## 3. 시스템 콜

```c
long sched_set_chain(pid_t pid, unsigned int chain_id,
                     unsigned int stage, u64 lat_ns);
```

- 번호: **451** (`arch/x86/entry/syscalls/syscall_64.tbl`에 `451  common  sched_set_chain  sys_sched_set_chain`)
- 선언: `include/linux/syscalls.h`, 구현: `kernel/sched/core.c`의 `SYSCALL_DEFINE4`

| 상황 | 동작 / 반환 |
|---|---|
| `pid == 0` | 호출한 스레드 자신 |
| `pid`가 다른 태스크 | `CAP_SYS_NICE` 필요, 없으면 `-EPERM` |
| `pid` 없음 | `-ESRCH` |
| `chain_id == 0` | 체인 해제. 5개 필드를 모두 0으로 (stage, lat_ns는 무시) |
| `chain_id != 0`이고 stage가 1~16 밖 | `-EINVAL` |
| `chain_id != 0`이고 `lat_ns`가 100,000(100µs) 미만 또는 `sysctl_sched_latency` 초과 | `-EINVAL` |
| 정상 설정 | 0 반환. `chain_credit_ns = 0`, `chain_last_credit = 0`으로 초기화 |
| 다시 호출 | 덮어쓰기 (위와 같이 크레딧 초기화) |
| 패치 없는 커널 | 유저 쪽에 `-1` / `errno == ENOSYS`. 워크로드는 **경고만 찍고 baseline으로 계속 실행** |

> `sysctl_sched_latency`는 코어 수에 따라 달라진다(6코어 계산상 약 18ms). 본체에서 `/sys/kernel/debug/sched/latency_ns`로 실제 값을 확인해 이 문서에 적는다: `____ ns`

### 확인 방법: `/proc/<pid>/sched`

`kernel/sched/debug.c` `proc_sched_show_task()`에 아래 3줄을 추가한다 (기존 출력 형식과 같은 `P()` 매크로 사용).

```
chain_id                                     :                    7
chain_stage                                  :                    2
chain_lat_ns                                 :              2000000
```

---

## 4. 통계 파일: `/proc/sched_chain_stats`

- 기존 `/proc/schedstat`은 **건드리지 않는다** (형식 버전이 있어 perf 같은 도구가 파싱하므로).
- per-CPU 카운터(`DEFINE_PER_CPU`). 틀(파일 생성, 출력, 초기화)은 ①이 만들고 값은 ②③이 올린다.
- 읽기 형식 (공백 구분, 첫 줄 헤더 고정, CPU마다 한 줄):

```
cpu credit_issued credit_preempt credit_consumed credit_expired credit_skip_cgroup b_cond b_applied
0 1203 980 910 3 0 12 10
1 ...
```

| 열 | 담당 | 올리는 시점 |
|---|---|---|
| `credit_issued` | ② | ttwu에서 크레딧을 발급했을 때 (wakee가 큐잉될 CPU 기준) |
| `credit_preempt` | ② | 크레딧이 없었으면 선점 안 됐는데 크레딧 덕분에 선점 판정이 1이 됐을 때 |
| `credit_consumed` | ② | `set_next_entity()`에서 크레딧이 남은 태스크가 CPU를 잡아 크레딧을 0으로 만들 때 |
| `credit_expired` | ② | 발급 후 `2 × chain_lat_ns`가 지나 쓰지 못하고 버려졌을 때 |
| `credit_skip_cgroup` | ② | wakee와 현재 태스크가 다른 task group이라 크레딧을 무시했을 때 |
| `b_cond` | ③ | 정책 B 조건(5.2절)을 모두 만족했을 때 (bit1이 꺼져 있어도 셈) |
| `b_applied` | ③ | 실제로 waker CPU를 반환했을 때 |

- 쓰기: `echo 0 > /proc/sched_chain_stats` → 전체 카운터 초기화 (root 전용). run.sh가 실행할 때마다 호출한다.
- 열을 **추가**할 때는 맨 뒤에만 붙인다 (④의 파서는 헤더 이름으로 열을 찾는다).

---

## 5. 정책 구현 시 지켜야 할 약속

인터페이스는 아니지만 ②③이 서로의 코드를 깨지 않도록 미리 정한다.

### 5.1 정책 A (②)

| 항목 | 약속 |
|---|---|
| 발급 위치 | `try_to_wake_up()`, `p->pi_lock` 보유 구간, 큐잉 직전 |
| 발급 조건 | `policy & 1` ∧ `in_task()` ∧ `task_is_chain_successor(current, p)` ∧ `now - p->chain_last_credit ≥ min_interval` |
| 선점 판정 | **`wakeup_preempt_entity()`는 수정하지 않는다.** 크레딧을 인자로 받는 새 함수 `wakeup_preempt_entity_chain(curr_se, pse, credit_v)`를 만들어 `check_preempt_wakeup()`에서만 호출한다 (`pick_next_entity()`도 기존 함수를 부르므로, 기존 함수를 고치면 엉뚱한 태스크의 크레딧이 더해진다) |
| 단위 변환 | `credit_v = calc_delta_fair(credit_ns, pse)` |
| 오래된 크레딧 | 선점 판정 직전에 `now - chain_last_credit > 2 × chain_lat_ns`이면 크레딧을 0으로 만들고 `credit_expired`를 올린다 |
| cgroup | `find_matching_se()` 후 **wakee와 현재 태스크가 같은 `cfs_rq`에 있는 태스크 entity일 때만** 크레딧을 쓴다. 아니면 `credit_skip_cgroup`을 올리고 기존 판정만 한다 |
| 선점 성공 시 | `set_next_buddy(pse)` |
| 소멸 | `set_next_entity()`에서 `chain_credit_ns = 0` |

### 5.2 정책 B (③)

| 항목 | 약속 |
|---|---|
| 위치 | `select_task_rq_fair()` 입구, `WF_TTWU`일 때만 |
| 조건 | `task_is_chain_successor(current, p)` ∧ `WF_SYNC` ∧ waker CPU의 `cfs_rq->h_nr_running == 1` ∧ waker CPU의 rq에 RT/DL 태스크 없음 ∧ `cpumask_test_cpu(cpu, p->cpus_ptr)` ∧ `cpu_active(cpu)` |
| 동작 | 조건을 만족하면 `b_cond`를 올린다. `policy & 2`이면 waker CPU를 반환하고 `b_applied`를 올린다 |
| 첫 주 사전 측정 | baseline에서 후속 태스크가 이미 waker CPU에 놓이는 비율을 ftrace로 먼저 잰다 (6.1의 `wake_affine_idle()`이 비슷한 일을 이미 하므로) |

---

## 6. 유저 공간 인터페이스 (④)

### 6.1 헤더
`bench/chain.h` — 시스템 콜 래퍼, sysctl·통계 파일 경로, 단계 간 토큰 구조체 (별도 파일 참조).

### 6.2 단계 간 데이터 전달
- **프레임(64KB)은 공유메모리**(`memfd_create` + `mmap`)의 슬롯 링에 둔다. 슬롯 수는 기본 8개.
- **파이프로는 `struct chain_token`(24바이트)만 보낸다.** 파이프 기본 용량이 64KB라 프레임을 직접 쓰면 쓰기가 쪼개져 한 프레임에 wakeup이 여러 번 생기기 때문이다.
- 각 단계는 받은 슬롯의 64KB 전체를 읽고, 연산하고, 다음 슬롯에 64KB를 쓴 뒤 토큰을 보낸다. 이렇게 해야 캐시 효과를 주장할 근거가 생긴다.

### 6.3 실행 인자 (모든 단계 공통)

```
./chain_stage --role {sensor|perception|planning|control}
              --chain-id 1 --stage N --lat-ns 2000000
              --work-us 2000 --period-ms 100 --frames 3000
              --out results/<run_id>/
```

- 런처 `chain_run`이 4단계를 fork/exec하고 파이프와 memfd를 연결한다. 체인 프로세스와 stress-ng는 **같은 런처(같은 cgroup scope)에서** 시작한다.
- 연산 시간(`--work-us`)은 터보 상태에서 보정한 바쁜 루프. 루프가 프레임 버퍼를 실제로 읽고 쓴다.

### 6.4 결과 파일

`results/<run_id>/frames.csv` — control이 기록, 프레임당 한 줄:

```
frame,t_sensor_ns,perc_start_ns,perc_end_ns,plan_start_ns,plan_end_ns,ctrl_start_ns,ctrl_end_ns,e2e_ns,rd_perc_ns,rd_plan_ns,rd_ctrl_ns
```

- 시각은 모두 `CLOCK_MONOTONIC` ns.
- `*_start_ns`는 토큰 `read()`가 돌아온 직후, `*_end_ns`는 다음 토큰 `write()` 직전.
- `rd_*_ns`는 해당 단계의 `/proc/self/schedstat` 2번째 필드(run_delay)를 프레임 처리 전후로 읽은 **증가분**.
- `e2e_ns = ctrl_end_ns - t_sensor_ns`.

`results/<run_id>/meta.txt` — 실행마다 1개:

```
kernel=<uname -r>
git_rev=<커널 커밋 해시>
policy=<sched_chain_policy>
min_interval_ns=<값>
load=<0|6|12|24|mixed>
rep=<1..5>
governor=<cpufreq governor>
chain_cgroup=<체인 프로세스의 /proc/<pid>/cgroup>
load_cgroup=<stress-ng의 /proc/<pid>/cgroup>
```

`results/<run_id>/chain_stats_before.txt`, `chain_stats_after.txt` — `/proc/sched_chain_stats` 스냅샷.

---

## 7. 일정과 산출물

| 날짜 | 산출물 | 담당 |
|---|---|---|
| 11/21 | 이 문서 합의 → `iface-v1` | 전원 |
| 11/22–23 | 스텁 커밋: 필드, 시스템 콜(검증 + 저장만), sysctl 2개, 0만 출력하는 통계 파일, `/proc/<pid>/sched` 출력 | ① |
| 11/23 | baseline 재현 확인: 단계별 `rd_*_ns`가 부하에서 실제로 쌓이는가 | ④ |
| 11/24 | ②③이 `iface-v1`에서 브랜치를 따서 구현 시작 | ②③ |
| 11/26 | 스텁 → 완성본 (권한 검사, 경계값 테스트 프로그램 포함) | ① |

### ① 스텁 완료 기준
- [ ] `chain_test`(테스트 프로그램)가 정상값, 경계값, 범위 밖 값, 남의 pid(권한 없음), 없는 pid를 모두 확인하고 기대한 반환값을 받는다
- [ ] `/proc/<pid>/sched`에 3줄이 보인다
- [ ] fork한 자식의 `chain_id`가 0이다
- [ ] sysctl 2개를 읽고 쓸 수 있고, 범위 밖 값은 거부된다
- [ ] `/proc/sched_chain_stats`가 헤더와 CPU 6줄을 출력하고, `echo 0`으로 초기화된다
- [ ] 정책 0에서 hackbench 결과가 순정 커널과 오차 범위 안이다

---

## 8. 변경 규칙

| 변경 종류 | 절차 |
|---|---|
| 추가 (통계 열을 맨 뒤에, CSV 열 추가, 새 sysctl) | 단톡방 공지 후 진행, 이 문서 갱신 |
| 변경·삭제 (이름, 번호, 반환값, 열 순서, 기본값) | 전원 동의 → `iface-v2` 태그 |
| 12/7 이후 | 데이터 확정 시점. 변경 금지 |

---

## 9. 회의에서 확인할 것 (3가지)

1. **정책 전환을 sysctl로 하는가?** (이 안: sysctl. 슬라이드의 "부팅 시 선택"은 발표 전에 "sysctl로 전환"으로 수정)
2. **발급 간격 기본값을 50ms로 하는가?** (제안서·비교표는 100ms)
3. **cgroup이 다르면 크레딧을 무시하는가?** (이 안: 비교표대로 무시 + `credit_skip_cgroup`으로 횟수 기록 + 런처에서 같은 scope 보장. 제안서 7.2의 "같은 깊이 entity에 적용"은 하지 않음)

---

## 부록: 기존 문서와 달라진 점

| 항목 | 기존 문서 | 이 확정안 | 이유 |
|---|---|---|---|
| 필드 수 | 제안서 "4개" | 5개 | 제안서 7.2에서도 `chain_last_credit` 사용 |
| 정책 전환 | 브랜치 / 부팅 시 선택 | sysctl 비트마스크 | 이미지 1개로 빌드 차이 제거, 재부팅 없이 자동 측정 |
| 발급 간격 이름·기본값 | `chain_min_interval_ns`, 100ms | `kernel.sched_chain_min_interval_ns`, 50ms | 주기 지터로 크레딧이 빠지는 문제 |
| 오래된 크레딧 | 비교표에만 있음, 위치 미정 | 선점 판정 직전에 검사 | 구현 위치 명시 |
| cgroup | 제안서 "적용", 비교표 "무시" | 무시 + 카운터 + 실행 절차로 같은 group 보장 | 범위를 단순하게, 무시된 횟수는 측정 |
| 선점 판정 함수 | `wakeup_preempt_entity()` 수정 | 새 함수 `wakeup_preempt_entity_chain()` | `pick_next_entity()`도 기존 함수를 호출하므로 |
| 정책 B 카운터 | `/proc/schedstat` | `/proc/sched_chain_stats` | 기존 파일 형식을 깨지 않기 위해 |
| 정책 A 카운터 | 없음 | 5개 추가 | 1차 완료 기준(실제로 실행됐는가)을 숫자로 확인 |
| 역할 ③ | C-state 유휴 코어 선택 포함 | 제외 | 제안서 7.3, 슬라이드와 맞춤 (future work) |
| 프레임 전달 | 64KB를 파이프로 | 공유메모리 + 24바이트 토큰 | 파이프 용량 64KB라 쓰기가 쪼개져 wakeup이 여러 번 생김 |
| 선점 문턱 설명 | "약 1ms" | 6코어 계산상 약 3ms (본체에서 확인) | 코어 수에 따른 자동 스케일링 |

---

## 부록 2: 구현 중 달라진 점 (`integ` 브랜치, 2026-11-24)

| 항목 | 확정안 | 구현 | 이유 |
|---|---|---|---|
| `pick_next_entity()` | 수정하지 않음 | next buddy가 크레딧을 갖고 있으면 leftmost보다 `wakeup_gran + 크레딧`까지 뒤에 있어도 선택 | 선점 요청이 성공해도 다음 선택에서 다른 수면 보너스 태스크가 골라지면 체인은 여전히 기다림. 크레딧은 1회성이고 발급 간격 하한이 있어 추가로 앞설 수 있는 폭은 한 번에 최대 `lat_ns` |
| `/proc/sched_chain_stats` | 열 7개 | 맨 뒤에 `b_base_same` 추가 (8개) | 정책 B를 끈 상태에서 조건을 만족한 wakeup 중 순정 CFS가 이미 waker CPU를 고른 횟수. B의 효과는 이 기준 비율과 비교해야 함 |
| `chain_stat_inc()` | `(cpu, field)` | `(field)`, 항상 실행 중인 CPU에 `this_cpu_inc` | 다른 CPU의 카운터를 올리면 갱신이 유실될 수 있음. 읽을 때는 모든 CPU의 합을 씀 |
| `sched_set_chain()` 락 | `pi_lock` | `task_rq_lock()` (pi_lock + rq lock) | 크레딧 필드가 wakeup 경로(pi_lock)와 pick 경로(rq lock) 양쪽에서 바뀜 |
| 헬퍼 | `task_is_chain_successor(waker, wakee)` | + `task_is_chain_successor_current(wakee)` = `in_task()` 검사 포함 | 인터럽트에서 깨우면 `current`는 우연히 돌던 태스크 |
| 벤치마크 배경 부하 | `stress-ng --cpu N` | `stress-ng --cpu N --cpu-load 50 --cpu-load-slice 30`, N = 0/6/12/18 | 순정 6.1에서 `--cpu`·`--pipe`·`--switch` 부하는 체인 단계가 거의 기다리지 않음(대기 p99 ≤ 106µs). 주기형 부하에서만 대기가 생김(p99 6.6ms). 측정 기록: 프로젝트 문서 `baseline-gonogo-결과` |
| 벤치마크 `lat_ns` | 예시 2ms | 4ms | 6코어 wakeup_gran ≈ 3ms보다 커야 크레딧만으로 선점 가능 |
| 벤치마크 프레임 | 64KB 합성 | KITTI `2011_09_26_drive_0009_sync` image_02, 1242×375 RGB = 1,397,250B/frame (합성 64KB도 선택 가능) | 실제 카메라 프레임 크기·메모리 이동량 재현 |
| `frames.csv` | 열 12개 | 맨 뒤에 `sensor_end_ns, wk_perc_ns, wk_plan_ns, wk_ctrl_ns` 추가 | `rd_*`는 처리 *중* run_delay만 재서 깨어나는 순간의 대기가 빠짐. `wk_*` = 토큰 read() 전후 run_delay 차이 = 정책 A가 줄이는 값 |
