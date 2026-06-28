"""Run-directory management for the Sircadian desktop dashboard.

Each run lives in runs/<UTC-timestamp>/ and contains:
  run.json              the full RunConfig the fleet was launched with
  summary_<idx>.csv     one row per deployment (written by each worker process)
  summary.csv           merged summary (only when sim_run runs the whole fleet)
  daily/<idx>.csv       per-day trace for deployment <idx>
  curves/<idx>.csv      per-fit-day light curves for the detailed deployment
"""
import os
import glob
import json
import shutil
import datetime

import pandas as pd


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def runs_root():
    return os.path.join(repo_root(), "runs")


def sim_binary():
    """Path to the built sim_run binary, or None if not present."""
    root = repo_root()
    for name in ("sim_run.exe", "sim_run"):
        p = os.path.join(root, name)
        if os.path.exists(p):
            return p
    return None


def new_run_dir():
    ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
    d = os.path.join(runs_root(), ts)
    os.makedirs(d, exist_ok=True)
    return d


def _summary_files(run_dir):
    return sorted(glob.glob(os.path.join(run_dir, "summary_*.csv")))


def load_summary(run_dir):
    """Merge all per-deployment summary rows into one DataFrame (index-sorted).
    Falls back to a merged summary.csv if present."""
    files = _summary_files(run_dir)
    if files:
        df = pd.concat([pd.read_csv(f) for f in files], ignore_index=True)
    else:
        merged = os.path.join(run_dir, "summary.csv")
        if not os.path.exists(merged):
            return pd.DataFrame()
        df = pd.read_csv(merged)
    return df.sort_values("index").reset_index(drop=True)


def load_daily(run_dir, idx):
    p = os.path.join(run_dir, "daily", "%d.csv" % idx)
    return pd.read_csv(p) if os.path.exists(p) else pd.DataFrame()


def load_curves(run_dir, idx):
    p = os.path.join(run_dir, "curves", "%d.csv" % idx)
    return pd.read_csv(p) if os.path.exists(p) else pd.DataFrame()


def deployment_indices(run_dir):
    out = []
    for f in _summary_files(run_dir):
        try:
            out.append(int(os.path.basename(f)[len("summary_"):-len(".csv")]))
        except ValueError:
            pass
    if not out:  # fall back to daily/*.csv
        for f in glob.glob(os.path.join(run_dir, "daily", "*.csv")):
            try:
                out.append(int(os.path.splitext(os.path.basename(f))[0]))
            except ValueError:
                pass
    return sorted(out)


def run_config(run_dir):
    p = os.path.join(run_dir, "run.json")
    if os.path.exists(p):
        try:
            return json.load(open(p))
        except Exception:
            return {}
    return {}


def list_runs():
    """Newest-first list of run metadata dicts."""
    root = runs_root()
    if not os.path.isdir(root):
        return []
    out = []
    for name in sorted(os.listdir(root), reverse=True):
        d = os.path.join(root, name)
        if not os.path.isdir(d):
            continue
        meta = run_config(d)
        n_expected = int(meta.get("numDeployments", 0) or 0)
        n_done = len(deployment_indices(d))
        status = "ok" if (n_done > 0 and n_done >= n_expected) else (
                 "partial" if n_done > 0 else "empty")
        out.append({
            "name": name,
            "path": d,
            "status": status,
            "deployments": "%d/%d" % (n_done, n_expected),
            "horizonDays": meta.get("horizonDays"),
        })
    return out


def delete_run(path):
    shutil.rmtree(path, ignore_errors=True)


def delete_all_but_latest():
    for r in list_runs()[1:]:
        delete_run(r["path"])
