#pragma once
#include <cstdint>

// RTC-less wall clock: anchor = a once-seeded absolute epoch, time =
// anchor + monotonic delta, corrected by a learned ppm drift (from SolarFit).
// The one platform dependency is the monotonic-ms source injected at
// construction (millis() on Arduino, steady_clock on a host). Persisting and
// restoring driftPpm()/epochMs() across reboots is the caller's job.

namespace solar {

class DriftClock {
public:
    using MonotonicMsFn = uint64_t (*)();

    explicit DriftClock(MonotonicMsFn nowMs) : now_(nowMs) {}

    // Seed the wall clock with an absolute epoch (ms since 1970). Captures
    // the current monotonic reading as the anchor point.
    void setEpochMs(int64_t epochMs);

    // True once setEpochMs() has been called this session.
    bool isSynced() const { return synced_; }

    // Drift-corrected ms since the Unix epoch. 0 if never synced -- callers
    // must treat 0 as "unknown", not 1970-01-01.
    int64_t epochMs() const;

    // Seconds the clock has been free-running since the last setEpochMs().
    // SolarFit uses this to turn an observed shift into a drift rate.
    int32_t secondsSinceSync() const;

    // Signed drift *correction* in ppm (not the raw oscillator error).
    // epochMs() adds localDt * ppm/1e6, so positive ppm makes the REPORTED
    // wall time advance faster than the raw monotonic delta -- which is the
    // correction you apply to compensate an oscillator that runs slow.
    // NOTE: the rate applies retroactively over the whole span since the rate
    // anchor (last setEpochMs/slewRatePpm) -- use slewRatePpm() to change the
    // rate going FORWARD only.
    void setDriftPpm(int32_t ppm) { driftPpm_ = ppm; }
    int32_t driftPpm() const { return driftPpm_; }

    // Change the drift correction from NOW on without rewriting history:
    // folds the correction accrued so far into the rate anchor, then applies
    // the new rate, so epochMs() integrates a piecewise-constant rate. Does
    // NOT count as a sync -- secondsSinceSync() is unaffected. Before the
    // first setEpochMs() this is just setDriftPpm().
    void slewRatePpm(int32_t ppm);

    // Local-time helpers. tzHours is the UTC offset in hours.
    int32_t localSecondOfDay(float tzHours) const; // 0..86399, -1 if unsynced
    int16_t localMinuteOfDay(float tzHours) const; // 0..1439,  -1 if unsynced
    int16_t dayOfYear(float tzHours) const;        // 1..366,   -1 if unsynced

private:
    MonotonicMsFn now_;
    int64_t epochAtSet_  = 0;   // rate anchor: epoch at last set/slew
    int64_t localAtSet_  = 0;   // rate anchor: monotonic ms at last set/slew
    int64_t localAtSync_ = 0;   // sync anchor: monotonic ms at last setEpochMs
    int32_t driftPpm_    = 0;
    bool    synced_      = false;
};

}  // namespace solar
