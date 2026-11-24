#!/usr/bin/env python3
"""
analyze.py — results/ 아래 모든 실행을 모아 표와 그래프를 만든다

  python3 analyze.py results              # runs.csv, summary.csv, 그래프 3개
  python3 analyze.py results --gonogo     # 기준 커널 go/no-go 판정만 출력
  python3 analyze.py results --warmup 20  # 앞쪽 프레임 버리는 수 (기본 10)

한 실행 = results/<label>/load<N>_rep<R>/ (meta.txt, frames.csv, stress_tasks.csv, ...)
요약은 (label, load)별로 반복 5회의 '중앙값'과 '최소~최대' 범위.
"""
import argparse, glob, os, re, sys
import numpy as np
import pandas as pd

STAGES = ["perc", "plan", "ctrl"]


def read_meta(path):
    meta = {}
    with open(path) as f:
        for line in f:
            if "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                meta[k] = v
    return meta


def stress_bogo(path):
    """stress-ng --metrics-brief 로그에서 bogo ops 합계 (버전마다 형식이 달라 최선 노력)"""
    if not os.path.exists(path):
        return np.nan
    total, found = 0.0, False
    pat = re.compile(r"(?:info|metrc):\s+\[\d+\]\s+([a-z][\w-]*)\s+(\d+)\s+[\d.]+")
    with open(path, errors="ignore") as f:
        for line in f:
            m = pat.search(line)
            if m and m.group(1) not in ("stressor",):
                total += float(m.group(2)); found = True
    return total if found else np.nan


def one_run(d, warmup):
    meta = read_meta(os.path.join(d, "meta.txt"))
    fr = pd.read_csv(os.path.join(d, "frames.csv"))
    fr = fr[fr["frame"] >= warmup]
    ms = lambda s: s / 1e6
    row = dict(run=os.path.relpath(d), label=meta.get("label", "?"), load=meta.get("load", "?"),
               rep=int(meta.get("rep", 0)), kernel=meta.get("kernel"), policy=meta.get("policy"),
               chain_kernel=meta.get("chain_kernel", "?"), governor=meta.get("governor"),
               exit_ok=meta.get("exit_ok", "?"), frames=len(fr))
    e2e = ms(fr["e2e_ns"])
    row.update(e2e_p50=e2e.quantile(.5), e2e_p99=e2e.quantile(.99), e2e_max=e2e.max(), e2e_mean=e2e.mean())
    for s in STAGES:
        wk = fr[f"wk_{s}_ns"] / 1e3   # µs
        rd = fr[f"rd_{s}_ns"] / 1e3
        row[f"wk_{s}_p50_us"] = wk.quantile(.5)
        row[f"wk_{s}_p99_us"] = wk.quantile(.99)
        row[f"rd_{s}_p99_us"] = rd.quantile(.99)
    row["wk_sum_p50_us"] = sum(row[f"wk_{s}_p50_us"] for s in STAGES)

    st = os.path.join(d, "stress_tasks.csv")
    dur = float(meta.get("duration_s", "nan"))
    if os.path.exists(st):
        t = pd.read_csv(st)
        if len(t):
            row["bg_cpu_s_per_s"] = t["exec_ns"].sum() / 1e9 / dur          # 배경 부하가 받은 CPU (코어 수 단위)
            row["bg_rd_ms_per_task"] = (t["run_delay_ns"] / 1e6).mean()     # 배경 태스크 1개당 런큐 대기
            x = t["exec_ns"].astype(float)
            row["bg_jain"] = x.sum() ** 2 / (len(x) * (x ** 2).sum())       # 배경 태스크끼리 공정성 (1=완전 균등)
    row["bg_bogo"] = stress_bogo(os.path.join(d, "stress.log"))

    for name in ("chain_stats_after.txt",):
        p = os.path.join(d, name)
        if os.path.exists(p):
            try:
                cs = pd.read_csv(p, sep=r"\s+")
                for c in cs.columns[1:]:
                    row[f"cs_{c}"] = cs[c].sum()
            except Exception:
                pass
    return row


def load_key(v):
    try:
        return (0, int(v))
    except ValueError:
        return (1, v)


def summarize(runs):
    num = [c for c in runs.columns if runs[c].dtype.kind in "fi" and c not in ("rep",)]
    g = runs.groupby(["label", "load"])
    out = []
    for (lab, load), df in g:
        r = dict(label=lab, load=load, reps=len(df))
        for c in num:
            r[c] = df[c].median()
            r[c + "_min"] = df[c].min()
            r[c + "_max"] = df[c].max()
        out.append(r)
    s = pd.DataFrame(out)
    s["_k"] = s["load"].map(load_key)
    return s.sort_values(["label", "_k"]).drop(columns="_k")


def plots(runs, summ, outdir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import font_manager
    have = {f.name for f in font_manager.fontManager.ttflist}
    for fam in ("NanumGothic", "Noto Sans CJK KR", "Noto Sans KR", "UnDotum"):
        if fam in have:
            plt.rcParams["font.family"] = fam; break
    else:
        print("한글 폰트 없음 (그래프 글자가 네모로 보이면: sudo apt install fonts-nanum && rm -rf ~/.cache/matplotlib)")
    plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False,
                         "axes.unicode_minus": False})
    labels = list(dict.fromkeys(summ["label"]))
    colors = dict(zip(labels, ["#4a5a6a", "#d1603d", "#2f8f83", "#8a63d2", "#c9a227"]))

    # 1) 부하별 p99 e2e (막대 끝 = 반복 5회 범위)
    fig, ax = plt.subplots(figsize=(6.4, 4))
    for lab in labels:
        s = summ[summ.label == lab]
        x = [str(v) for v in s["load"]]
        y = s["e2e_p99"].values
        err = [y - s["e2e_p99_min"].values, s["e2e_p99_max"].values - y]
        ax.errorbar(x, y, yerr=err, marker="o", capsize=3, label=lab, color=colors[lab])
    ax.set_xlabel("배경 부하 (stress-ng 태스크 수)"); ax.set_ylabel("종단 간 지연 p99 (ms)")
    ax.set_title("부하별 p99 종단 간 지연 — 점: 반복 중앙값, 막대: 최소~최대")
    ax.legend(frameon=False); fig.tight_layout(); fig.savefig(os.path.join(outdir, "p99_vs_load.png"), dpi=150)

    # 2) 가장 큰 숫자 부하에서 단계별 깨어난 뒤 대기 p99
    loads = sorted({l for l in summ["load"] if str(l).isdigit()}, key=int) or list(summ["load"])
    L = loads[-1]
    fig, ax = plt.subplots(figsize=(6.4, 4))
    w = 0.8 / max(1, len(labels))
    for i, lab in enumerate(labels):
        s = summ[(summ.label == lab) & (summ.load == L)]
        if s.empty:
            continue
        ys = [s[f"wk_{st}_p99_us"].iloc[0] / 1e3 for st in STAGES]
        ax.bar(np.arange(3) + i * w, ys, w, label=lab, color=colors[lab])
    ax.set_xticks(np.arange(3) + w * (len(labels) - 1) / 2, ["perception", "planning", "control"])
    ax.set_ylabel("깨어난 뒤 런큐 대기 p99 (ms)"); ax.set_title(f"단계별 대기 — 부하 {L}")
    ax.legend(frameon=False); fig.tight_layout(); fig.savefig(os.path.join(outdir, "stage_wait.png"), dpi=150)

    # 3) 같은 부하에서 e2e 분포 (CDF), 반복 전부 합침
    fig, ax = plt.subplots(figsize=(6.4, 4))
    for lab in labels:
        sub = runs[(runs.label == lab) & (runs.load == L)]
        vals = []
        for d in sub["run"]:
            fr = pd.read_csv(os.path.join(d, "frames.csv"))
            vals.append(fr["e2e_ns"].values / 1e6)
        if not vals:
            continue
        v = np.sort(np.concatenate(vals))
        ax.plot(v, np.arange(1, len(v) + 1) / len(v), label=lab, color=colors[lab])
    ax.set_xlabel("종단 간 지연 (ms)"); ax.set_ylabel("누적 비율"); ax.set_title(f"지연 분포 — 부하 {L}")
    ax.set_ylim(0.9, 1.001); ax.legend(frameon=False); fig.tight_layout()
    fig.savefig(os.path.join(outdir, "e2e_cdf_tail.png"), dpi=150)


def gonogo(runs):
    """기준 커널에서: 체인 단계가 깨어난 뒤 CPU를 받기까지의 대기 중앙값이 이미 작으면
    선점 크레딧이 줄일 여지가 없다 → 부하 종류를 바꿔야 한다는 신호."""
    base = runs[runs.chain_kernel.isin(["no"]) | runs.policy.isin(["0", "n/a"])]
    if base.empty:
        print("기준 커널(또는 policy=0) 실행이 없음"); return
    for (lab, load), df in base.groupby(["label", "load"]):
        med = {s: df[f"wk_{s}_p50_us"].median() for s in STAGES}
        p99 = {s: df[f"wk_{s}_p99_us"].median() for s in STAGES}
        worst = max(med.values())
        print(f"[{lab} 부하 {load}] 단계별 깨어난 뒤 대기 중앙값(µs): " +
              ", ".join(f"{s}={med[s]:.0f}" for s in STAGES) +
              " | p99(µs): " + ", ".join(f"{s}={p99[s]:.0f}" for s in STAGES) +
              f" | e2e p99={df['e2e_p99'].median():.2f} ms")
        if str(load) == "0":
            print("   (부하 없음 = 참고값, 판정 안 함)")
        elif worst <= 200 and max(p99.values()) < 1000:
            print("   → NO-GO 신호: 기준 커널이 이미 200µs 안에 실행시켜 줌. "
                  "LOAD_ARGS를 sleep/wake형(예: \"--pipe 6\" 또는 \"--cpu 6 --pipe 3\")으로 바꿔 다시 확인")
        else:
            print("   → GO: 깨어난 뒤 대기(중앙값 또는 p99)가 충분히 큼. 정책 A로 줄일 여지 있음")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--gonogo", action="store_true")
    a = ap.parse_args()

    dirs = sorted(os.path.dirname(p) for p in glob.glob(os.path.join(a.results, "**", "meta.txt"), recursive=True))
    dirs = [d for d in dirs if os.path.exists(os.path.join(d, "frames.csv"))]
    if not dirs:
        sys.exit("결과 없음: " + a.results)
    runs = pd.DataFrame([one_run(d, a.warmup) for d in dirs])
    bad = runs[runs.exit_ok != "1"]
    if len(bad):
        print("경고: 비정상 종료한 실행은 제외:", ", ".join(bad.run))
        runs = runs[runs.exit_ok == "1"]

    if a.gonogo:
        gonogo(runs); return

    runs.to_csv(os.path.join(a.results, "runs.csv"), index=False)
    summ = summarize(runs)
    summ.to_csv(os.path.join(a.results, "summary.csv"), index=False)
    cols = ["label", "load", "reps", "e2e_p50", "e2e_p99", "e2e_p99_min", "e2e_p99_max", "e2e_max",
            "wk_perc_p99_us", "wk_plan_p99_us", "wk_ctrl_p99_us", "bg_cpu_s_per_s", "bg_jain"]
    with pd.option_context("display.width", 200, "display.max_columns", 30, "display.float_format", "{:.2f}".format):
        print(summ[[c for c in cols if c in summ.columns]].to_string(index=False))
    try:
        plots(runs, summ, a.results)
        print("그래프:", ", ".join(os.path.join(a.results, f) for f in ("p99_vs_load.png", "stage_wait.png", "e2e_cdf_tail.png")))
    except Exception as e:
        print("그래프 생략:", e)


if __name__ == "__main__":
    main()
