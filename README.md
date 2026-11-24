# Task Chain 인식 CFS 스케줄러 (Linux 6.1.192)

의존 관계 힌트 기반 wakeup 선점·배치 개선으로 파이프라인 워크로드(센서 → 인지 → 판단 → 제어)의 종단 간 꼬리 지연(p99)을 줄이는 커널 패치와 측정 도구.
2026-2 리눅스 시스템 및 응용 텀 프로젝트.

- 확정안(인터페이스 규격): [`docs/iface-v1.md`](docs/iface-v1.md) — 문서끼리 다르면 이 문서가 우선. 구현 중 달라진 점은 그 문서의 부록 2.
- 커널 변경은 `kernel/sched/{core.c,fair.c,sched.h,debug.c}`, `include/linux/{sched.h,syscalls.h}`, `arch/x86/entry/syscalls/syscall_64.tbl`에만 있다. 나머지는 순정 v6.1.192.

## 브랜치

| 브랜치 | 내용 | 상태 |
|---|---|---|
| `main` | 문서만 | |
| `iface` | 순정 6.1.192 + 인터페이스 스텁(① 필드·시스템 콜·sysctl·통계 파일) + `bench/chain_test.c` | 스텁 완료 |
| `policy-a` | `iface` + 정책 A(선점 크레딧) + `bench/chain_smoke.c`, `run_smoke.sh` | 구현 완료, 부팅 미확인 |
| `policy-b` | `policy-a` + 정책 B(waker CPU 배치) | 구현 완료, 부팅 미확인 |
| **`integ`** | `policy-b` + 리뷰 반영 패치 + 통합 수정 + 벤치마크 v2 | **측정용 통합본** |

`integ` 위에 쌓인 커밋(순정 이후):

```
Import Linux v6.1.192 source
sched: add task chain interface stub (iface-v1)
bench: add chain_test for the iface-v1 interface
sched/fair: add task chain policy A (one-shot wakeup preemption credit)
sched/fair: hand off the waker CPU to the next chain stage
sched: address review of the task chain interface stub
sched: integrate policy A/B with the review fixes; add b_base_same
docs/bench: project README, spec appendix 2, benchmark v2
```

## 동작 요약

| | 켜는 법 | 하는 일 | 바꾸는 곳 |
|---|---|---|---|
| 등록 | `sched_set_chain(pid, chain_id, stage, lat_ns)` (시스템 콜 451) | "이 태스크는 체인 N의 M단계" | `task_struct` 필드 5개 |
| 정책 A | `kernel.sched_chain_policy` bit0 | 앞 단계가 다음 단계를 깨울 때 1회성 크레딧(`lat_ns`)을 주고, 선점 판정과 next-buddy 선택에서 그만큼 유리하게. CPU를 받으면 소멸, `2×lat_ns` 뒤 만료, 태스크당 `sched_chain_min_interval_ns`(기본 50ms)에 한 번 | `try_to_wake_up`, `check_preempt_wakeup`, `pick_next_entity`, `set_next_entity` |
| 정책 B | bit1 | 동기 wakeup(WF_SYNC)이고 waker CPU에 runnable 일반 태스크가 waker뿐이면 다음 단계를 그 CPU에 배치 | `select_task_rq_fair` |
| 통계 | `/proc/sched_chain_stats` | CPU별 카운터 9열, `echo 0 >`으로 초기화 | |

vruntime(공정성 회계)은 어느 정책도 바꾸지 않는다. 정책 0이면 순정과 같은 판단을 한다.

## 빌드·설치 (측정 PC, Ubuntu 24.04)

순정 기준 커널 `6.1.192-vanilla`가 이미 설치되어 있다. **같은 이름으로 설치하면 덮어쓰므로 `LOCALVERSION`을 반드시 바꾼다.**

```bash
cd /srv/chain/linux && git checkout integ
cp /boot/config-$(uname -r) .config
./scripts/config --disable SYSTEM_TRUSTED_KEYS --disable SYSTEM_REVOCATION_KEYS \
  --disable DEBUG_INFO --disable DEBUG_INFO_BTF \
  --enable SCHEDSTATS --enable SCHED_DEBUG --enable FTRACE \
  --set-str LOCALVERSION "-chain"
make olddefconfig
tmux new -s build            # 안에서:
make -j$(nproc) 2>&1 | tee build.log

# 설치·부팅은 관리자만 (docs/접속 매뉴얼의 사용 규칙)
sudo make modules_install INSTALL_MOD_STRIP=1 && sudo make install && sudo update-grub
ls /boot | grep 6.1.192                       # -vanilla 와 -chain 둘 다 있어야 함
sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 6.1.192-chain"
sudo reboot
```

완성된 `.config`는 `configs/chain.config`로 커밋한다 (TODO: 측정 PC에서 첫 빌드 후).

VM에서 개발할 때는 `CONFIG_DEBUG_PREEMPT=y`, `CONFIG_PROVE_LOCKING=y`를 추가한다. 측정 커널에는 넣지 않는다.

## 확인 순서

```bash
uname -r                                    # 6.1.192-chain
cd bench
gcc -O2 -Wall -o chain_test chain_test.c && sudo ./chain_test      # 전부 PASS
sudo ./run_smoke.sh 300                     # policy 0 / 1 / 3, 부하 없음·있음
cat /proc/sched_chain_stats                 # credit_issued, credit_preempt, b_cond 가 0이 아닌지
```

`chain_test`는 6.5 이상 커널에서는 451번이 `cachestat`이라 잘못된 결과를 낸다. 6.1 계열에서만 실행한다.

## 본 측정 (`bench/chain_bench/`)

4단계 체인(공유메모리 링 + 24바이트 토큰)을 KITTI 프레임으로 돌리고, 배경 부하 N × 반복 5회를 자동으로 수행한다. 자세한 사용법은 [`bench/chain_bench/README.md`](bench/chain_bench/README.md).

```bash
cd bench/chain_bench && make
T="--cpu {N} --cpu-load 50 --cpu-load-slice 30"
K=/srv/chain/data/kitti/rgb_0009
# 기준 커널(-vanilla)에서 이미 완료: results_main/base10, base30
# 통합 커널(-chain)에서:
for P in 0 1 3; do
  sudo -E OUT=results_main INPUT=kitti KITTI_DIR=$K LOAD_TEMPLATE="$T" LOADS="0 6 12 18" POLICY=$P ./run_matrix.sh p${P}_10
  sudo -E OUT=results_main INPUT=kitti KITTI_DIR=$K LOAD_TEMPLATE="$T" LOADS="0 6 12 18" POLICY=$P PERIOD_MS=33 WORK_US=8000,6000,4000 ./run_matrix.sh p${P}_30
done
python3 analyze.py results_main
```

- 시나리오: 10fps(연산 2/1/0.5ms), 30fps(연산 8/6/4ms). 연산량(passes)은 기준 커널에서 한 번 보정해 `results_main/passes_*.txt`에 고정되어 있다. 지우지 않는다.
- 지표: e2e p50/p99/max, 단계별 깨어난 뒤 대기(`wk_*`), 배경 부하 CPU·대기·공정성, `/proc/sched_chain_stats` 전후.
- 비교: `p0_*`(정책 0)가 `base*`(순정)와 같은지 → 정책 0 = 순정 확인. `p1`(A), `p3`(A+B)를 `p0`과 비교.
- 기준 커널 결과(2026-11-23): 10fps N=12에서 e2e p99 4.0 → 13.3ms(3.3배), 30fps N=12에서 18.9 → 38.6ms. 중앙값은 그대로, 깨어난 뒤 대기 p99 6~12ms.

## 측정 PC 운영 규칙

- 측정 중 다른 빌드·작업 금지 (`who`, `uptime`으로 확인, 단톡에 시작/끝 알림). 오래 걸리는 작업은 `tmux` 안에서.
- 커널 설치·`update-grub`·재부팅·sysctl 정책 변경은 관리자만. `apt upgrade`, 펌웨어 업데이트 금지.
- 자료는 `/srv/chain/` (커널 소스, 벤치마크, KITTI, 결과). 홈 폴더는 다른 계정이 읽을 수 없다.

## 남은 일

- [ ] `integ` 커널 VM·측정 PC 부팅 확인, `chain_test` 전부 PASS
- [ ] `configs/chain.config` 커밋, `iface-v1` 태그
- [ ] 통합 커널 본 측정(정책 0/1/3 × 2 시나리오 × N 4단계 × 5회, 정책당 약 2시간 20분)
- [ ] 성능 저하 확인: hackbench, perf bench sched (순정 vs 정책 0 vs 정책 3)
- [ ] 벤치마크 단계 수 옵션(제안서 2~6단계), 데모 스크립트(ftrace wakeup→switch)
- [ ] `policy-b` 커밋 작성자 수정(`Codex` → 작성한 팀원)
