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
    void setDriftPpm(int32_t ppm) { driftPpm_ = ppm; }
    int32_t driftPpm() const { return driftPpm_; }

    // Local-time helpers. tzHours is the UTC offset in hours.
    int32_t localSecondOfDay(float tzHours) const; // 0..86399, -1 if unsynced
    int16_t localMinuteOfDay(float tzHours) const; // 0..1439,  -1 if unsynced
    int16_t dayOfYear(float tzHours) const;        // 1..366,   -1 if unsynced

private:
    MonotonicMsFn now_;
    int64_t epochAtSet_ = 0;
    int64_t localAtSet_ = 0;
    int32_t driftPpm_   = 0;
    bool    synced_     = false;
};

}  // namespace solar
