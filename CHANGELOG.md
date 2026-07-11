# Changelog

## 1.0.0 — 2026-07

First packaged release (Arduino library layout, `library.properties`,
`library.json`, examples, keywords).

### Added
- **`Sircadian` facade** (`src/Sircadian.*`): one object owning the light
  buffer, daily fit cadence at local-midnight rollover (`service()`), and the
  shared discipline loop. The simulator engine now drives this exact class, so
  firmware and simulation cannot diverge.
- **Light-sensor temperature correction** (`Sircadian::correctLightSample`):
  undoes the photoresistor's temperature-dependent gain with a
  bench-calibrated tempco, `corrected = dark + (adc − dark)/(1 + k·(T − Tref))`,
  applied automatically by `addLightSample()` when `lightCompEnabled` and a
  temperature has been fed. One divide per sample, no extra RAM. In a 64-unit
  5-year simulated fleet it cut steady clock error ~137 s → ~114 s, matching
  the sensor-tempco-free physics ceiling.
- **`DriftClock::slewRatePpm()`**: forward-only rate changes (piecewise
  integration) with the sync anchor split from the rate anchor; no-ops on
  unchanged rate to avoid fold truncation.
- **Runtime configuration** via `SircadianConfig`; all `config.h` macros
  remain as compile-time defaults.
- `sircadianMillis64()` Arduino helper (32-bit `millis()` wrap extension).
- Examples: `SolarClockBasic`, `SolarClockLightComp`, `RuntimeConfig`.
- Simulator: per-deployment temperature-sensor model (offset, noise,
  quantization) and per-unit light-sensor bench-calibration error, light-comp
  config plumbing.
- Host tests: `test_clock`, `test_facade` (bit-exact parity with the
  pre-facade loop, plus a light-correction case); CI compiles all examples for
  AVR (Mega) and ESP32.

### Changed
- `DriftClock::dayOfYear()` now uses arithmetic civil-date math (no
  `<ctime>`, no platform ifdefs).
- Summary CSV gains `t_sens_offset_c, t_sens_noise_c, light_cal_err_frac,
  light_cal_dark_err_adc`.
