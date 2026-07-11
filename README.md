# Sircadian

A microcontroller without a real-time-clock chip loses the wall time at every
power interruption, and its crystal oscillator drifts by tens of parts per
million, varying with temperature. The conventional remedies are a
battery-backed RTC module or periodic synchronization over a network. Sircadian
requires neither: a photoresistor records ambient light through the day, and the
device fits the measured light curve against the astronomical clear-sky curve
computed for its date and location. The offset between the two is a direct
measurement of the clock's accumulated error, from which the oscillator's drift
is estimated and corrected. A single absolute time seed at installation is the
only external reference the device needs.

Sircadian is packaged as an Arduino library; the firmware in `src/` is plain
C++17 with no hardware dependency.

> **Status:** in deployment testing. The accuracy figures below are
> simulation-derived, pending field measurement. See [Accuracy](#accuracy) and
> [Limitations](#limitations).

## Contents

- [Requirements](#requirements)
- [Installation](#installation)
- [Quickstart](#quickstart)
- [API](#api)
- [Method](#method)
- [Configuration](#configuration)
- [Light-sensor temperature correction](#light-sensor-temperature-correction)
- [Accuracy](#accuracy)
- [Repository layout](#repository-layout)
- [Building and testing](#building-and-testing)
- [Simulator and dashboard](#simulator-and-dashboard)
- [Limitations](#limitations)
- [License](#license)

## Requirements

The fit holds three 1440-entry buffers in RAM — the observation buffer
(`uint16_t`, ≈2.8 KB) and two static scratch arrays (`float`, ≈5.6 KB;
`int16_t`, ≈2.8 KB) — for a floor near 11 KB. This rules out 2 KB AVR parts
(e.g. ATmega328/Uno). Target ESP8266, ESP32, STM32, or RP2040-class boards. A
single ADC channel on a photoresistor divider is the only required peripheral.

## Installation

Sircadian is not in the Arduino Library Manager or the PlatformIO registry;
install it from source.

**Arduino IDE** — clone into the sketchbook `libraries/` directory:

```sh
cd ~/Arduino/libraries        # or your sketchbook's libraries/ path
git clone https://github.com/GMichas226/Sircadian.git
```

Restart the IDE. Alternatively, download the repository ZIP from GitHub and use
*Sketch → Include Library → Add .ZIP Library*.

**PlatformIO** — add the repository to `lib_deps` in `platformio.ini`:

```ini
lib_deps = https://github.com/GMichas226/Sircadian.git
```

Then `#include <Sircadian.h>`. The development directories (`sim/`,
`dashboard/`, `test/`, `tools/`) are ignored by both toolchains.

## Quickstart

```cpp
#include <Sircadian.h>
using namespace solar;

Sircadian sirc(sircadianMillis64);            // 64-bit-safe millis()

void setup() {
    SircadianConfig cfg;
    cfg.site = SolarParams{ 38.0f, 23.7f, 2.0f, 1.6f };   // lat, lon, tz, atmK
    sirc.begin(cfg);
    sirc.setEpochMs(1783468800000LL);         // one-time seed: serial / NTP / button
}

void loop() {
    static uint32_t t = 0;
    if (millis() - t >= 60000UL) {            // one sample per minute
        t += 60000UL;
        sirc.addLightSample(analogRead(A0));
    }
    sirc.service();                           // daily fit at local midnight
    if (sirc.isSynced()) {
        int64_t nowMs = sirc.epochMs();       // corrected wall time
    }
}
```

Full sketches are in [`examples/`](examples/):
[`SolarClockBasic`](examples/SolarClockBasic/SolarClockBasic.ino),
[`SolarClockLightComp`](examples/SolarClockLightComp/SolarClockLightComp.ino)
(temperature correction), and
[`RuntimeConfig`](examples/RuntimeConfig/RuntimeConfig.ino) (full runtime
configuration).

## API

The `Sircadian` object owns the light buffer, the fit cadence, and the
discipline loop.

| Member | Purpose |
|--------|---------|
| `Sircadian(nowMs)` | Construct with a monotonic-milliseconds function (`sircadianMillis64` on Arduino). |
| `begin(cfg)` | Apply `SircadianConfig`; resets learned state. |
| `setEpochMs(ms)` | One-time absolute seed of wall time. |
| `addLightSample(adc)` | Record one ADC reading, filed under the clock's current minute. |
| `setTemperature(tC)` | Optional; enables light-sensor temperature correction. Does not affect clock rate. |
| `service()` | Called each loop; fits the completed day at local-midnight rollover and disciplines the clock. |
| `epochMs()`, `isSynced()` | Corrected wall time; seed state. |
| `appliedPpm()`, `lastFit()` | Learned drift rate; most recent fit result. |
| `exportState()`, `importState()` | Persist and restore the learned drift across reboots. |

The primitives (`SolarModel`, `SolarFit`, `DriftClock`, `Discipline`) remain
public for applications supplying their own buffering or fit cadence.

## Method

For a given date and location, the time of solar noon is fixed by astronomy.
Sircadian records ambient light once per minute over a local day and aligns the
measured curve against the computed clear-sky curve. The time shift τ that
minimizes the residual between the two is the clock's offset from solar time.
Dividing τ by the interval since the last synchronization gives a drift rate in
parts per million, which the discipline loop feeds back to the clock.

The clock is seeded once with an absolute epoch and thereafter free-runs as
`epoch + monotonic_delta`, scaled by the learned drift. A fit is accepted only
when the day is well-sampled, the normalized residual is below a threshold, and
the best alignment is sufficiently better than the second-best; this rejects
cloudy and partial days rather than admitting them into the estimate. Each
accepted day adjusts the running drift estimate, which converges over successive
clear days.

The clear-sky curve uses the Spencer (1971) series: a three-harmonic
declination and two-harmonic equation of time set solar noon for the date and
location, and intensity follows `max(0, cos θz)^atmK`, where `atmK` sets the
curve's sharpness and is matched to the sensor. Published model error is a few
arc-minutes, below the one-minute observation resolution. Coefficients are in
[`src/SolarModel.h`](src/SolarModel.h).

## Configuration

All parameters are configurable at runtime through `SircadianConfig`, with
defaults defined as `#ifndef`-guarded macros in [`src/config.h`](src/config.h),
so a constrained build can set them from the compiler
(`-DSIRCADIAN_LAT=51.5f`) and omit runtime configuration entirely.

```c
#define SIRCADIAN_LAT  38.0f   // latitude, deg N
#define SIRCADIAN_LON  23.7f   // longitude, deg E
#define SIRCADIAN_TZ   2.0f    // UTC offset, hours
#define SIRCADIAN_ATMK 1.6f    // curve sharpness; calibrate to the sensor
```

Three fit gates govern acceptance: `atmK` (curve sharpness; a wrong value biases
the noon estimate), `unambiguityFactor` (how much the best alignment must beat
the runner-up), and `maxRmsOverAlpha` (the normalized-residual ceiling and
primary cloud filter).

## Light-sensor temperature correction

A photoresistor's gain varies with temperature as approximately
`1 + k·(T − Tref)`. Because room temperature peaks in the afternoon, this drift
distorts the logged light curve asymmetrically about noon and biases the fitted
solar noon. Given `k` from a one-time bench calibration, feeding
`setTemperature()` before each `addLightSample()` divides the sample back to its
reference temperature before the fit:

```
corrected = dark + (adc − dark) / (1 + lightTempco·(T − lightTempRefC))
```

The correction costs one division per sample and no additional RAM, requires no
learning, and is disabled by default. It is inert unless `lightCompEnabled` is
set and a temperature has been supplied. See
[`SolarClockLightComp`](examples/SolarClockLightComp/SolarClockLightComp.ino).

## Accuracy

The figures below are derived in simulation, not measured against a reference
clock. They come from a fleet of 64 independent deployments simulated over five
years each; location, climate, room thermal response, sensor unit, and
oscillator are randomized per deployment.

| Metric | Median | Mean | Worst unit |
|--------|-------:|-----:|-----------:|
| Steady-state clock error | 111 s | 114 s | 171 s |
| Worst-case error | 372 s | 386 s | 689 s |
| Accepted-fit fraction | 0.96 | 0.95 | — |

Every deployment converges and holds steady-state error below 300 s. Error is
driven primarily by cloud fraction and latitude.

## Repository layout

| Path | Contents |
|------|----------|
| `src/Sircadian.*` | Library facade: buffer, fit cadence, discipline, temperature correction. |
| `src/SolarModel.*` | Clear-sky reference curve (`<cmath>` only). |
| `src/SolarFit.*` | Curve alignment producing the offset τ. |
| `src/DriftClock.*` | RTC-less wall clock with ppm correction. |
| `src/Discipline.*` | Offset-and-rate discipline loop. |
| `src/config.h` | Compile-time defaults. |
| `examples/` | Three reference sketches. |
| `test/` | Host tests. |
| `sim/`, `dashboard/`, `tools/` | Development-only simulator, GUI, and analysis. |

## Building and testing

The library and its host tests build with any C++17 toolchain via CMake
(≥ 3.16). The `src/` and `sim/` source sets are declared once in
[`CMakeLists.txt`](CMakeLists.txt), so the whole build collapses to:

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows, configure with the g++ toolchain to match the usual build — for
example `cmake -B build -G "MinGW Makefiles"` (or `-G Ninja`). Warnings are
treated as errors on GCC/Clang, so the build stays clean.

This runs four tests: `test_fit` recovers known offsets from synthesized days
(sub-minute) and rejects a cloudy one, `test_clock` checks the sign and scale of
the drift correction, `test_facade` confirms the facade reproduces the manual
composition exactly (including temperature correction), and `test_sim` is the
engine sanity check (clean deployments converge, overcast stays bounded).

Without CMake, the core firmware test still builds with a single `g++`
invocation:

```sh
g++ -std=c++17 -I src src/SolarModel.cpp src/SolarFit.cpp src/DriftClock.cpp \
    src/Discipline.cpp test/test_fit.cpp -o test_fit && ./test_fit
```

## Simulator and dashboard

[`sim/`](sim/) exercises the unmodified firmware in `src/` against a synthetic
but physically-modeled environment, so it serves as end-to-end validation and
the source of the [accuracy](#accuracy) figures. Each deployment's world is
deterministic given its seed and is modeled independently of the discipline
algorithm.

The same CMake build produces the `sim_run` CLI:

```sh
cmake --build build                        # builds sim_run alongside the tests
build/sim_run --dump-defaults run.json     # write an editable default config
build/sim_run --config run.json            # run the fleet and emit summary CSVs
```

(With single-config generators such as MinGW Makefiles the binary is
`build/sim_run.exe`.) A PySide6 desktop GUI in
[`dashboard/`](dashboard/README.md) configures, builds, runs, and visualizes
fleets.

## Limitations

- Accuracy is derived in simulation, not measured against a reference clock.
- Requires ~11 KB RAM; unsuitable for 2 KB AVR parts.
- Observation resolution is one minute; sub-minute offset is recovered by
  parabolic interpolation but bounded by that input granularity.
- Convergence requires clear days; overcast climates converge more slowly
  because cloudy days are rejected by design.
- A miscalibrated `atmK` or a non-linear sensor response biases the recovered
  noon; the residual thermal-tilt bias is the current accuracy floor.

## License

Apache 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). The license includes
an explicit patent grant from contributors.
