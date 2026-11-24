#!/bin/bash
# run_smoke.sh - policy 0 vs 1, without and with CPU load, on the chain kernel
#
# usage: sudo ./run_smoke.sh [frames]      (default 300 frames = about 30 s per run)
#        LOAD=8 sudo -E ./run_smoke.sh     (stress-ng workers, default = nproc)
#
# Needs: gcc, stress-ng, the iface + policy A kernel booted.
set -e

FRAMES=${1:-300}
LOAD=${LOAD:-$(nproc)}
cd "$(dirname "$0")"

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root (sysctl and /proc/sched_chain_stats need it)" >&2
	exit 1
fi
command -v stress-ng >/dev/null || { echo "install stress-ng first" >&2; exit 1; }

gcc -O2 -Wall -o chain_smoke chain_smoke.c

# sum each column of /proc/sched_chain_stats over all CPUs
stats_sum() {
	awk 'NR == 1 { for (i = 2; i <= NF; i++) h[i] = $i; n = NF; next }
	     { for (i = 2; i <= NF; i++) s[i] += $i }
	     END { for (i = 2; i <= n; i++) printf "%s=%d ", h[i], s[i]; print "" }' \
		/proc/sched_chain_stats
}

for load in 0 "$LOAD"; do
	for policy in 0 1; do
		sysctl -qw kernel.sched_chain_policy="$policy"
		echo 0 > /proc/sched_chain_stats

		SP=
		if [ "$load" -gt 0 ]; then
			stress-ng --cpu "$load" --quiet &
			SP=$!
			sleep 1
		fi

		echo "=== load=$load policy=$policy ==="
		./chain_smoke -n "$FRAMES"

		if [ -n "$SP" ]; then
			kill "$SP"
			wait "$SP" 2>/dev/null || true
		fi
		stats_sum
		echo
	done
done

sysctl -qw kernel.sched_chain_policy=0
echo "done (policy reset to 0)"
