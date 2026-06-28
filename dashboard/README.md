# Sircadian simulator — desktop dashboard

A native **PySide6** desktop app that configures, builds, runs, and visualizes the
C++ deployment simulator. A *run* is a **fleet of deployments**: many copies of one
configuration, each with its own randomly-rolled climate, room, sensor unit, and
oscillator, simulated **one process per deployment (1 core per run)**.

## Run

```sh
pip install -r dashboard/requirements.txt
python dashboard/app.py
```

Needs a C++17 compiler (`g++`, e.g. MSYS2/MinGW on Windows) on `PATH` to build the
engine.

## Using it

1. **Configure && Run**
   - **Build sim** — compiles `sim_run` with the CI flags (once; a prebuilt binary
     is reused). **Load defaults** pulls the engine's default config.
   - Edit the whole world: site (truth == device), timeline (horizon defaults to
     ~5 years), fit gates, the **universal physics** (cloud regimes, temperature,
     sensor), and the **per-deployment rolled ranges** (climate / room / sensor
     unit / oscillator) as `[min, max]` pairs.
   - Set **cores**, `numDeployments`, `seedBase`, and which deployment is the
     *detailed* one (the only one that stores per-fit light curves, every
     `curveStrideDays`th fit).
   - **Run fleet** — writes `run.json`, launches one `sim_run --deployment N`
     process per deployment across the chosen cores, and streams progress.
2. **Results**
   - *Monte-Carlo*: steady-state and accept-fraction distributions across the
     fleet, a summary table, and a scatter of steady error vs any rolled parameter
     (observational — to *see* which conditions stress the clock, not to engineer
     them).
   - *Deployment drill-down*: wall-time error, drift tracking (true vs learned
     ppm), indoor temperature, daily cloudiness, and per-fit-day light curves
     (observed vs fitted model) for the detailed deployment.
3. **Run history** — list / delete runs under `runs/`.

## How it works

The dashboard never re-implements the model. It serializes the configuration to
`run.json` and invokes the C++ `sim_run` binary, which runs the real
`DriftClock` + `SolarFit` discipline loop against a rolled physical world and emits,
per run directory: `summary_<idx>.csv` (one row per deployment), `daily/<idx>.csv`
(per-day trace), and `curves/<idx>.csv` (light curves for the detailed deployment).

`sim_run` can also be driven directly:

```sh
sim_run --dump-defaults run.json            # editable default config
sim_run --config run.json --deployment 3    # one deployment (what the GUI launches)
sim_run --config run.json --deployments 0..7 --out runs/x   # a serial range
sim_run --config run.json                   # whole fleet, serial, + merged summary.csv
```
