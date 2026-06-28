#!/usr/bin/env python3
"""Sweep discipline-loop gains over a fleet and tabulate Monte-Carlo metrics.

Runs sim_run one-process-per-deployment (matching the real fleet) across a grid of
`discipline.*` config overrides, then reports the fleet-median steady_err / max_err /
convergence_day and the rate residual |true - learned ppm|. Used to read Stage B (the
offset loop) and Stage C (the rate loop) of the discipline-loop work.

  python tools/sweep_loop.py --grid offset            # Stage B: vary offsetGain
  python tools/sweep_loop.py --grid rate --kp 0.3     # Stage C: fix Kp, vary rate loop

Everything except the swept `discipline.*` knobs is held fixed (seedBase=0), so each
column is the SAME fleet under a different control policy.
"""
import argparse, csv, json, math, os, statistics, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_sim():
    for c in ("sim_run.exe", "sim_run"):
        p = os.path.join(ROOT, c)
        if os.path.exists(p):
            return p
    sys.exit("sim_run(.exe) not found in repo root -- build it first.")


def run_one(task):
    """Worker: simulate one deployment of one config (top-level so it pickles)."""
    sim, cfgpath, idx = task
    subprocess.run([sim, "--config", cfgpath, "--deployment", str(idx)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return idx


def offset_grid(_kp):
    return [(f"Kp={g}", {"offsetGain": g}) for g in (1.0, 0.5, 0.3, 0.2, 0.1)]


def rate_grid(kp):
    base = {"offsetGain": kp}
    cols = [("legacy", {**base, "rateMode": 0})]
    cols += [(f"ema Kr={kr}", {**base, "rateMode": 1, "rateGain": kr})
             for kr in (0.5, 0.2, 0.1, 0.05)]
    return cols


def final_grid(_kp):
    # Head-to-head: today's loop vs the recommended tuned loop, for large-N confirmation.
    return [("today",
             {"offsetGain": 1.0, "rateMode": 0}),
            ("tuned",
             {"offsetGain": 0.1, "rateMode": 1, "rateGain": 0.1})]


def ppm_resid(daily_csv):
    tot, n = 0.0, 0
    with open(daily_csv) as f:
        for r in csv.DictReader(f):
            if r["is_fit"] == "1":
                tot += abs(float(r["true_ppm"]) - float(r["learned_ppm"]))
                n += 1
    return tot / n if n else float("nan")


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--grid", choices=("offset", "rate", "final"), default="offset")
    ap.add_argument("--kp", type=float, default=0.3, help="fixed offsetGain for --grid rate")
    ap.add_argument("--n", type=int, default=24, help="deployments per config")
    ap.add_argument("--horizon", type=int, default=1095, help="days (default ~3y)")
    ap.add_argument("--offset-ms", type=int, default=3600000,
                    help="initial sync error (default 1h, so acquisition is visible)")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--rate-mode", type=int, default=None,
                    help="for --grid offset: also set this rateMode on every column")
    ap.add_argument("--rate-gain", type=float, default=0.1)
    args = ap.parse_args()

    sim = find_sim()
    grid_fn = {"offset": offset_grid, "rate": rate_grid, "final": final_grid}[args.grid]
    cols = grid_fn(args.kp)
    if args.grid == "offset" and args.rate_mode is not None:
        cols = [(f"{lab} r{args.rate_mode}",
                 {**d, "rateMode": args.rate_mode, "rateGain": args.rate_gain})
                for lab, d in cols]
    workroot = tempfile.mkdtemp(prefix="sweep_")

    # Build one config per column and a flat (config x deployment) task list.
    tasks, dirs = [], []
    for label, disc in cols:
        outdir = os.path.join(workroot, label.replace(" ", "_").replace("=", ""))
        cfgpath = outdir + ".json"
        json.dump({"numDeployments": args.n, "horizonDays": args.horizon,
                   "seedBase": args.seed, "initialOffsetMs": args.offset_ms,
                   "detailedDeployment": -1, "outDir": outdir.replace("\\", "/"),
                   "discipline": disc}, open(cfgpath, "w"))
        dirs.append((label, outdir))
        tasks += [(sim, cfgpath, i) for i in range(args.n)]

    print(f"grid={args.grid}  n={args.n}  horizon={args.horizon}d  "
          f"offset={args.offset_ms/1000:.0f}s  configs={len(cols)}  "
          f"runs={len(tasks)} ...", flush=True)
    with ProcessPoolExecutor() as ex:
        list(ex.map(run_one, tasks))

    print(f"\n| {'config':<12} | steady_s | max_s | conv_day | %conv | |ppm_resid| |")
    print("|" + "-" * 14 + "|----------|-------|----------|-------|------------|")
    for label, outdir in dirs:
        st, mx, cv, pr = [], [], [], []
        for i in range(args.n):
            srow = next(csv.DictReader(open(os.path.join(outdir, f"summary_{i}.csv"))))
            st.append(float(srow["steady_err_ms"]) / 1000.0)
            mx.append(float(srow["max_err_ms"]) / 1000.0)
            cv.append(int(srow["convergence_day"]))
            pr.append(ppm_resid(os.path.join(outdir, "daily", f"{i}.csv")))
        pct_conv = 100.0 * sum(c >= 0 for c in cv) / len(cv)
        cvm = med([c for c in cv if c >= 0])
        print(f"| {label:<12} | {med(st):8.1f} | {med(mx):5.0f} | "
              f"{cvm:8.0f} | {pct_conv:4.0f}% | {med(pr):10.1f} |")
    print(f"\n(work dir: {workroot})")


if __name__ == "__main__":
    main()
