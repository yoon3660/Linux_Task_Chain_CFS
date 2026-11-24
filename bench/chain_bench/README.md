# Task Chain 벤치마크 v2 (iface-v1 6장 구조)

센서 → 인지 → 판단 → 제어 4개 프로세스가 공유메모리 링(슬롯 8개)으로 프레임을 넘기고,
파이프로는 24바이트 토큰만 보낸다. 제어 단계가 프레임별 시각을 `frames.csv`로 남긴다.

## 파일
| 파일 | 역할 |
|---|---|
| `chain.h` | 시스템 콜 래퍼(451), 토큰·슬롯 헤더 구조체, 공용 함수 |
| `chain_stage.c` | 단계 하나 (`--role sensor/perception/planning/control`) |
| `chain_run.c` | 연산량 보정 → stress-ng 시작 → 4단계 실행 → 결과 기록 |
| `run_matrix.sh` | 부하 × 반복 전체 실행 (한 커널·한 정책 조건) |
| `analyze.py` | `runs.csv`, `summary.csv`, 그래프 3개, go/no-go 판정 |
| `kitti_to_rgb.py` | KITTI PNG → `.rgb` 변환 (이미 했으면 다시 안 해도 됨) |

## 준비 (측정 PC에서 한 번)
```bash
sudo apt install -y build-essential stress-ng python3-pandas python3-matplotlib fonts-nanum
cd ~/bench2 && make
```

## 실행
```bash
# 기준 커널(6.1.192-vanilla)에서 go/no-go 먼저: 부하 12, 1회, 600프레임(1분)
sudo -E LOADS="12" REPS=1 FRAMES=600 ./run_matrix.sh gonogo
python3 analyze.py results --gonogo

# 본 측정 (기준 커널) — 부하 0/6/12/18 × 5회 × 3000프레임(5분) ≈ 1시간 40분
sudo -E INPUT=kitti ./run_matrix.sh base

# 패치 커널에서 (같은 results 폴더, 같은 passes 파일을 그대로 씀)
sudo -E INPUT=kitti POLICY=0 ./run_matrix.sh p0
sudo -E INPUT=kitti POLICY=1 ./run_matrix.sh A
sudo -E INPUT=kitti POLICY=3 ./run_matrix.sh AB

python3 analyze.py results
```
- 오래 걸리니 `tmux new -s bench` 안에서 돌릴 것. 중간에 끊겨도 다시 실행하면 끝난 실행은 건너뜀.
- sleep/wake형 부하: `LOAD_ARGS="--cpu 6 --pipe 3"` (이때 load 열은 `mixed`)
- 데모: `./chain_run --out /tmp/demo --load 12 --frames 300 --live` → 프레임마다 e2e 막대 출력

## 결과 폴더 `results/<label>/load<N>_rep<R>/`
- `frames.csv` — iface-v1 6.4 열 그대로 + 맨 뒤 추가 열 4개
  - `sensor_end_ns`: 센서가 슬롯에 다 쓴 시각
  - `wk_perc_ns`, `wk_plan_ns`, `wk_ctrl_ns`: **깨어난 뒤 CPU를 받기까지 런큐에서 기다린 시간**
    (토큰 read() 전후의 run_delay 차이). 정책 A가 줄이려는 바로 그 값
  - 확정안의 `rd_*`는 연산 *중* 런큐 대기라서 깨어나는 순간의 대기는 안 잡힘 → 그래서 추가함
- `meta.txt` — 확정안 필드(kernel, git_rev, policy, min_interval_ns, load, rep, governor, chain_cgroup, load_cgroup) + 추가 필드
- `stress_tasks.csv` — 배경 태스크별 실행 시간·런큐 대기 증가분 (공정성 확인용)
- `chain_stats_before/after.txt` — 패치 커널에서만

## 주의
- **연산량(passes)은 `results/passes_*.txt`에 한 번 저장되고 모든 커널에서 같은 값**을 쓴다.
  처음 실행을 반드시 기준 커널에서, 다른 작업 없이 할 것. 지우면 다시 보정됨.
- 패치 커널인지는 `/proc/sys/kernel/sched_chain_policy` 존재로 판단한다.
  6.5 이후 커널에서는 451번이 `cachestat`라서 확인 없이 부르면 엉뚱한 콜이 실행되기 때문.
  패치 커널인데 `sched_set_chain`이 실패하면 바로 중단(종료 코드 4).
- 링이 밀려서 앞 단계가 아직 안 읽은 슬롯을 덮어쓰면 `ring overrun`으로 즉시 중단 → 주기를 늘리거나 연산량을 줄일 것.
- 앞 10프레임은 분석에서 버린다 (`--warmup`).
- `LAT_NS` 기본 4ms: 확정안 예시 2ms는 wakeup_gran(6코어 ≈ 3ms)보다 작아서 크레딧이 의미 없음.
