"""Build the C++ sim and orchestrate a fleet run as one process per deployment.

A "run" is a fleet of independently-rolled deployments of one configuration.
This launches `sim_run --config run.json --deployment N --out run_dir` once per
deployment, keeping at most `cores` processes alive at a time (1 core per run),
and reports progress as they finish. The C++ engine reuses src/ unchanged.
"""
import os
import json
import subprocess

from PySide6.QtCore import QThread, Signal

import runs as runlib

# CI build flags (match .github/workflows/ci.yml).
SIM_SOURCES = [
    "src/SolarModel.cpp", "src/SolarFit.cpp", "src/DriftClock.cpp", "src/Discipline.cpp",
    "src/Sircadian.cpp",
    "sim/World.cpp", "sim/Deployment.cpp", "sim/Engine.cpp", "sim/sim.cpp",
]


def build_sim():
    """Compile sim_run. Returns (ok, log)."""
    root = runlib.repo_root()
    cmd = (["g++", "-std=c++17", "-O2", "-I", "src", "-I", "sim"]
           + SIM_SOURCES
           + ["-o", "sim_run", "-Wall", "-Wextra", "-Werror"])
    try:
        r = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
    except FileNotFoundError:
        return False, "g++ not found on PATH. Install a C++17 compiler (MSYS2/MinGW)."
    return r.returncode == 0, (r.stdout + r.stderr) or "build OK"


class FleetWorker(QThread):
    """Runs the fleet in a bounded process pool off the UI thread."""
    progress = Signal(int, int)        # (done, total)
    finished_run = Signal(int, str)    # (failures, run_dir)

    def __init__(self, binary, run_dir, num_deployments, cores):
        super().__init__()
        self.binary = binary
        self.run_dir = run_dir
        self.total = max(1, int(num_deployments))
        self.cores = max(1, int(cores))
        self._stop = False

    def stop(self):
        self._stop = True

    def run(self):
        cfg = os.path.join(self.run_dir, "run.json")
        procs = {}            # idx -> Popen
        launched = done = failures = 0
        self.progress.emit(0, self.total)
        while done < self.total and not self._stop:
            while len(procs) < self.cores and launched < self.total and not self._stop:
                idx = launched
                launched += 1
                procs[idx] = subprocess.Popen(
                    [self.binary, "--config", cfg, "--deployment", str(idx),
                     "--out", self.run_dir],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for idx, p in list(procs.items()):
                if p.poll() is not None:
                    if p.returncode != 0:
                        failures += 1
                    del procs[idx]
                    done += 1
                    self.progress.emit(done, self.total)
            self.msleep(40)
        for p in procs.values():       # on stop, let stragglers finish
            p.wait()
        self.finished_run.emit(failures, self.run_dir)


def write_run_config(run_dir, cfg):
    """Persist run.json into run_dir and remember it as last_config.json."""
    cfg = dict(cfg)
    cfg["outDir"] = run_dir.replace("\\", "/")
    with open(os.path.join(run_dir, "run.json"), "w") as fh:
        json.dump(cfg, fh, indent=2)
    os.makedirs(runlib.runs_root(), exist_ok=True)
    with open(os.path.join(runlib.runs_root(), "last_config.json"), "w") as fh:
        json.dump(cfg, fh, indent=2)


def default_config(binary):
    """Get the engine's default RunConfig via `sim_run --dump-defaults`."""
    import tempfile
    f = tempfile.mktemp(suffix=".json")
    subprocess.run([binary, "--dump-defaults", f], check=True,
                   capture_output=True, text=True)
    with open(f) as fh:
        cfg = json.load(fh)
    os.remove(f)
    return cfg


def deep_merge(base, override):
    """Recursively merge `override` onto `base`, returning a new dict.

    Dicts merge key-by-key; any non-dict value in `override` wins. Used to seed a
    saved config from the engine defaults so a config missing a whole block (e.g.
    one that predates `discipline`) inherits the default block instead of having the
    GUI form zero it out. Keys present in `override` (even with value 0) are honored.
    """
    if not isinstance(base, dict) or not isinstance(override, dict):
        return override
    out = dict(base)
    for k, v in override.items():
        out[k] = deep_merge(base.get(k), v) if isinstance(v, dict) else v
    return out


def last_config():
    p = os.path.join(runlib.runs_root(), "last_config.json")
    if os.path.exists(p):
        try:
            return json.load(open(p))
        except Exception:
            return None
    return None
