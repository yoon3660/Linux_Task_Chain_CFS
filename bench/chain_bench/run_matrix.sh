#!/usr/bin/env bash
# run_matrix.sh — 한 커널·한 정책 조건에서 (부하 × 반복) 전체를 돌린다
#
#   sudo -E ./run_matrix.sh <LABEL>
#     LABEL 예: base (기준 커널), p0 (패치 커널 policy=0), A (policy=1), AB (policy=3)
#
# 환경 변수로 조절 (기본값):
#   LOADS="0 6 12 18"   stress-ng --cpu N 의 N 목록 (LOAD_ARGS를 주면 무시)
#   LOAD_ARGS=""         부하 종류 하나 고정, 예: "--pipe 6" (load 열 = mixed)
#   LOAD_TEMPLATE=""     LOADS의 각 N을 {N} 자리에 넣음, 예: "--cpu {N} --cpu-load 50 --cpu-load-slice 30"
#                        (load 열 = N, N=0이면 부하 없음)
#   REPS=5  FRAMES=3000  PERIOD_MS=100  WORK_US=2000,1000,500
#   INPUT=synthetic | kitti      KITTI_DIR=~/data/kitti/rgb_0009
#   POLICY=                      비우면 sysctl을 건드리지 않음. 0~3이면 kernel.sched_chain_policy 설정
#   LAT_NS=4000000               체인 지연 목표 (확정안 기본 2ms는 wakeup_gran ~3ms보다 작아서 4ms 권장)
#   OUT=results
#
# passes(연산량)는 results/passes_<input>.txt 에 한 번 저장해 두고 모든 커널·조건에서 같은 값을 쓴다.
# → 처음 한 번은 기준 커널(6.1 vanilla)에서, 부하 없이 보정된다.
set -euo pipefail
cd "$(dirname "$0")"

LABEL=${1:?"usage: sudo -E ./run_matrix.sh <LABEL>"}
UHOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)   # sudo로 돌려도 본인 홈 기준
LOADS=${LOADS:-"0 6 12 18"}
LOAD_ARGS=${LOAD_ARGS:-}
LOAD_TEMPLATE=${LOAD_TEMPLATE:-}
REPS=${REPS:-5}
FRAMES=${FRAMES:-3000}
PERIOD_MS=${PERIOD_MS:-100}
WORK_US=${WORK_US:-2000,1000,500}
INPUT=${INPUT:-synthetic}
KITTI_DIR=${KITTI_DIR:-$UHOME/data/kitti/rgb_0009}
POLICY=${POLICY:-}
LAT_NS=${LAT_NS:-4000000}
OUT=${OUT:-results}

[ -x ./chain_run ] || make
mkdir -p "$OUT"

IN_ARGS=(--input "$INPUT")
[ "$INPUT" = kitti ] && IN_ARGS+=(--kitti-dir "$KITTI_DIR")

# ---- 사전 점검 ----
warn() { echo "[점검] $*" >&2; }
[ "$(id -u)" = 0 ] || warn "root가 아님: chain_stats 초기화·autogroup 설정이 안 될 수 있음 (sudo -E 권장)"
gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)
[ "$gov" = performance ] || warn "governor=$gov (본 실험 기준은 performance)"
smt=$(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo n/a)
[ "$smt" = 1 ] && warn "SMT(하이퍼스레딩)가 켜져 있음"
if [ "$(cat /proc/sys/kernel/sched_autogroup_enabled 2>/dev/null || echo 0)" = 1 ]; then
  sysctl -q -w kernel.sched_autogroup_enabled=0 2>/dev/null && warn "autogroup 끔" || warn "autogroup이 켜져 있음 (끄지 못함)"
fi
command -v stress-ng >/dev/null || { echo "stress-ng 없음: sudo apt install stress-ng" >&2; exit 1; }

if [ -n "$POLICY" ]; then
  if [ -e /proc/sys/kernel/sched_chain_policy ]; then
    sysctl -q -w kernel.sched_chain_policy="$POLICY"
  else
    echo "POLICY=$POLICY 를 줬는데 이 커널엔 sched_chain_policy가 없음 (기준 커널?)" >&2; exit 1
  fi
fi

# ---- 연산량: 한 번 보정해서 고정 ----
PF="$OUT/passes_${INPUT}_${WORK_US//,/-}.txt"
if [ ! -s "$PF" ]; then
  echo "연산량 보정 중 (부하 없음) → $PF"
  ./chain_run --calibrate-only "${IN_ARGS[@]}" --work-us "$WORK_US" > "$PF"
fi
PASSES=$(cat "$PF")
echo "LABEL=$LABEL kernel=$(uname -r) policy=$(cat /proc/sys/kernel/sched_chain_policy 2>/dev/null || echo n/a) passes=$PASSES"

if [ -n "$LOAD_ARGS" ]; then LOADS="mixed"; fi
GIT_REV=$(git -C "${KSRC:-$UHOME/kernel/linux-6.1}" rev-parse --short HEAD 2>/dev/null || echo unknown)
export CHAIN_GIT_REV=$GIT_REV

# 반복마다 부하 순서를 섞어서 시간에 따른 변화(발열 등)가 한 조건에 몰리지 않게 한다
for rep in $(seq 1 "$REPS"); do
  for load in $(printf '%s\n' $LOADS | shuf); do
    rid="$OUT/$LABEL/load${load}_rep${rep}"
    if [ -f "$rid/meta.txt" ] && grep -q '^exit_ok=1' "$rid/meta.txt"; then
      echo "skip $rid (이미 있음)"; continue
    fi
    rm -rf "$rid"
    if [ "$load" = mixed ]; then LA=(--load-args "$LOAD_ARGS")
    elif [ -n "$LOAD_TEMPLATE" ] && [ "$load" != 0 ]; then LA=(--load "$load" --load-args "${LOAD_TEMPLATE//\{N\}/$load}")
    else LA=(--load "$load"); fi
    echo "== $rid =="
    ./chain_run --out "$rid" --label "$LABEL" "${LA[@]}" --rep "$rep" \
      --frames "$FRAMES" --period-ms "$PERIOD_MS" --work-us "$WORK_US" --passes "$PASSES" \
      --lat-ns "$LAT_NS" "${IN_ARGS[@]}" || echo "!! $rid 실패 (meta.txt의 exit_ok=0)"
    sleep 3
  done
done

[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER": "$OUT"
echo "끝. 분석: python3 analyze.py $OUT"
