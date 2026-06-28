# Sircadian

A wall clock for a microcontroller that has **no real-time-clock chip and no
internet** — it keeps time by watching daylight and comparing it to where the
sun mathematically *should* be.

A bare MCU loses the wall clock on every power cut, and its crystal drifts (tens
of ppm, temperature-dependent). The usual fixes are an RTC module or NTP over
WiFi. Sircadian needs neither: a photoresistor logs ambient light through the
day, and the device compares the measured light curve against the computed
clear-sky curve for its date and location. The time gap between the two is a
direct measurement of how far the clock has drifted.

## How it works

The sun is a clock you cannot lose. For any date and location, the time of
solar noon (the daily peak of light) is fully determined by astronomy. If you
record when the light *actually* peaks and compare it to when it *should* have
peaked, the difference is your clock's error — measured against the sky.

```
   per-minute light samples (one local day)
                │
                ▼
        SolarFit.fitDay ───────────────┐
                │  slides the computed   │  rejects cloudy/
                │  curve against the     │  ambiguous days
                ▼  measured one          │
        time offset τ (minutes)  ◀───────┘
                │
                ▼
        driftPpmFromShift(τ, elapsed)
                │
                ▼
        DriftClock.setDriftPpm(...) ──▶ drift-corrected wall time
```

The loop, once per clear day:

1. **Seed once.** Something hands `DriftClock` an absolute epoch — a phone
   POSTing the time, a one-off NTP at install. This is the only outside time
   the device ever needs.
2. **Free-run.** `DriftClock` then keeps time entirely on its own, as
   `epoch + monotonic_delta`, scaled by a learned drift figure (ppm). No
   further network or RTC.
3. **Observe.** Through the day you log ambient light into a 1440-slot,
   one-sample-per-minute buffer.
4. **Fit.** `SolarFit::fitDay` builds the theoretical clear-sky curve for the
   day, then slides it in time against your measured curve. The shift that
   makes the two line up best is the offset **τ** — how far the clock's idea of
   "now" has drifted from the sun. The fit is *accepted* only if the day was
   well-sampled, low-residual, and the best alignment is clearly better than
   the runner-up; this is what throws out cloudy or partial days instead of
   letting them corrupt the estimate.
5. **Learn.** `driftPpmFromShift` divides τ by how long the clock has been
   free-running to get a drift *rate* in ppm, and feeds it back into
   `DriftClock`. Each clear day nudges the running estimate; over many days it
   converges.

So `SolarModel` answers "where should the sun be?", `SolarFit` answers "how far
off is my clock?", and `DriftClock` turns that answer into corrected time.

**The model.** The clear-sky curve is Spencer (1971): a 3-harmonic solar
declination and a 2-harmonic equation of time give solar noon for the date and
location, then intensity ∝ `max(0, cos θz)^atmK` traces the day's rise and
fall. `atmK` sets the curve's sharpness and is matched to the sensor. The
coefficients live in [`src/SolarModel.h`](src/SolarModel.h); published model
error is a few arc-minutes, below the 1-minute observation resolution.

## Layout

| File | Role | Dependencies |
|------|------|--------------|
| `src/SolarModel.*` | Clear-sky reference curve (pure math) | `<cmath>` only |
| `src/SolarFit.*`   | Curve alignment → time offset τ        | SolarModel |
| `src/DriftClock.*` | RTC-less wall clock with ppm correction | one monotonic-ms hook |
| `src/config.h`     | Site/deployment values + tuning, in one place | none |
| `test/test_fit.cpp`| Host test: recover a known τ, reject a cloudy day | none |

Everything is plain C++17 with **no Arduino/hardware dependency**. Triggering,
persistence, and the radio/power choreography are deliberately *not* included —
that is application glue. Wire it up however your device works.

## Build & test (host)

```sh
g++ -std=c++17 -I src src/SolarModel.cpp src/SolarFit.cpp \
    src/DriftClock.cpp test/test_fit.cpp -o test_fit && ./test_fit
```

The test synthesizes days of light readings at known offsets and checks that
the fit recovers them (sub-minute), that a cloudy day is rejected, and that the
drift math and clock correction have the right sign and scale.

## Simulator & dashboard

[`sim/`](sim/) is a deployment-level simulator that exercises the **real
firmware** in `src/` (`SolarModel`, `SolarFit`, `DriftClock`) unchanged — so the
sim doubles as end-to-end validation. A *run* is a **fleet of deployments**: many
copies of one configuration, each a device dropped into its own randomly-rolled
world, simulated **one process per deployment** (1 core per run).

Beyond validation, the sim provides the error figures that guide firmware tuning.
The discipline code in `src/` is adjusted against the metrics the fleet reports —
steady-state clock error, time-to-converge, and accepted-fit fraction — one change
at a time, keeping a change when those metrics improve and reverting it when they
don't. The git history records that process, and `main` holds the lowest-error
configuration measured so far. This commit is the starting baseline; the README is
not re-edited for each tuning step.

Each deployment is a coherent, layered, analytic world (deterministic given its
seed): season + synoptic weather → cloud regime → intraday clear-sky-index trace;
outdoor temperature → room thermal transfer (lag + greenhouse gain) → indoor
temperature; the oscillator drifts off **indoor** temperature, and a nonlinear,
temperature-dependent photoresistor logs the cloud-attenuated light. Climate, room,
sensor unit, and oscillator characteristics are **rolled once per deployment**;
the daily weather plays out within them. Each process is modeled to be realistic
on its own terms — never tuned to provoke or flatter the discipline algorithm.

Build and drive it from the CLI:

```sh
g++ -std=c++17 -O2 -I src -I sim \
    src/SolarModel.cpp src/SolarFit.cpp src/DriftClock.cpp \
    sim/World.cpp sim/Deployment.cpp sim/Engine.cpp sim/sim.cpp \
    -o sim_run -Wall -Wextra -Werror
sim_run --dump-defaults run.json          # editable default config
sim_run --config run.json                 # run the fleet (serial) + summary.csv
```

Or use the desktop control surface in [`dashboard/`](dashboard/README.md)
(PySide6 + pyqtgraph): configure every parameter, build, launch the fleet across
cores, and explore Monte-Carlo and per-deployment views. The engine sanity test is
[`test/test_sim.cpp`](test/test_sim.cpp).

## Configuration

Site and tuning values live in [`src/config.h`](src/config.h) as `#define`s,
each `#ifndef`-guarded so you can either edit the file or override from the
build (`-DSIRCADIAN_LAT=51.5f`). Set your location and sensor response there:

```c
#define SIRCADIAN_LAT  38.0f   // latitude,  deg N
#define SIRCADIAN_LON  23.7f   // longitude, deg E
#define SIRCADIAN_TZ   2.0f    // UTC offset, hours
#define SIRCADIAN_ATMK 1.6f    // curve sharpness; calibrate to your sensor
```

The fit accept-thresholds (`SIRCADIAN_*`) are defined there too and feed the
`FitConfig` defaults. The three worth understanding before you touch them:

- **`atmK`** — curve sharpness. A wrong value biases the noon estimate, so
  match it to your photoresistor's response.
- **`unambiguityFactor`** — how much better the best alignment must be than the
  runner-up. Higher = stricter = fewer but cleaner fits.
- **`maxRmsOverAlpha`** — normalized residual ceiling; the main cloud filter.

## Minimal usage

```cpp
#include "SolarModel.h"
#include "SolarFit.h"
#include "DriftClock.h"
#include "config.h"
using namespace solar;

// Monotonic milliseconds for your platform (Arduino: return millis();).
uint64_t nowMs();

DriftClock  clk(&nowMs);
SolarParams loc = SIRCADIAN_SITE_PARAMS;   // from config.h

void onTimeReceived(int64_t epochMs) { clk.setEpochMs(epochMs); }

// Once a day, with a full day of per-minute light readings in obs[1440]
// (unsampled minutes set to solar::NO_DATA):
void nightlyFit(const uint16_t* obs) {
    int doy = clk.dayOfYear(loc.tz);
    FitResult r = fitDay(doy, obs, NO_DATA, loc, FitConfig{});
    if (r.accepted) {
        int32_t step = driftPpmFromShift(r.shiftMin, clk.secondsSinceSync());
        // step is the clock's *observed* fast-rate, so remove it from the
        // running correction (subtract, not add) -- adding it is positive
        // feedback and the drift estimate diverges.
        clk.setDriftPpm(clk.driftPpm() - step);
        // persist clk.driftPpm() to NVS/flash here
    }
}
```

## Known limitations

- **Needs an ESP32/STM32/RP2040-class MCU, not a classic AVR.** The day buffer
  `obs[1440]` (~2.8 KB in the caller) plus the static theoretical curve
  `g_ith[1440]` (~5.6 KB) need ~8.4 KB of RAM minimum, so an ATmega328 Arduino
  Uno (2 KB) is out despite the "microcontroller" framing.
- **Theoretical, not yet field-validated.** The accuracy figures implied by the
  model (Spencer-71 errors, ppm noise floor) are *derived*, not measured against
  a reference clock. Treat this as a working design pending logged field data —
  PRs with real drift-vs-reference traces very welcome.
- **1-minute resolution.** Input is per-minute means, so the raw shift search
  steps in whole minutes; parabolic interpolation recovers sub-minute, but the
  input granularity is the floor.
- **Needs clear days.** Cloudy/partial curves are rejected by the unambiguity
  and residual gates (a feature — bad data doesn't corrupt the estimate), so
  overcast climates converge more slowly. Shifts landing near `k + 0.5` minutes
  are the worst case for the gate and may be rejected even on a clear day; they
  get picked up once the offset drifts off the midpoint.
- **Accuracy is bounded by the sensor.** A miscalibrated `atmK` or a non-linear
  photoresistor response biases the recovered noon.

## License

Apache 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Use it for anything,
including commercial work; keep the copyright and attribution notices, and state
any changes you make. The license includes an explicit patent grant from
contributors.
