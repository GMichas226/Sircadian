#pragma once
#include <cstdint>
#include "config.h"
#include "SolarModel.h"
#include "SolarFit.h"
#include "DriftClock.h"
#include "Discipline.h"

// The library facade: an RTC-less, solar-disciplined wall clock in one
// object. Owns the per-minute light buffer, the daily fit cadence, and the
// shared discipline loop. The application feeds it light samples (about once
// a minute), optionally temperature readings, and calls service() from its
// main loop; epochMs() is the corrected wall time.
//
//   Sircadian clock(millis);
//   clock.begin(cfg);              // site + tuning; defaults from config.h
//   clock.setEpochMs(seed);        // one-time absolute seed (serial/GPS/...)
//   ...
//   clock.setTemperature(tC);      // optional, ~1/min (light correction)
//   clock.addLightSample(adc);     // ~1/min
//   clock.service();               // runs the daily fit at day rollover
//
// This composes DriftClock + Discipline directly -- the simulator
// (sim/Engine.cpp) drives this same class, so firmware and simulation cannot
// diverge.
//
// Temperature is optional and used for exactly one thing: correcting the
// photoresistor's temperature-dependent gain before the fit (see
// correctLightSample() and lightCompEnabled). It never touches the clock
// rate. Feed setTemperature() a reading just before addLightSample() and the
// sample is corrected on the way in.
//
// RAM: dominated by the 2880-byte observation buffer (plus a few dozen bytes
// of state). No heap, no STL.

namespace solar {

struct SircadianConfig {
    SolarParams      site = SIRCADIAN_SITE_PARAMS;
    FitConfig        fit{};
    DisciplineConfig discipline{};
    int32_t          maxPpmStep      = SIRCADIAN_MAX_PPM_STEP;
    // Light-sensor temperature correction (Section A3 in config.h). The
    // photoresistor's gain drifts with temperature; with a bench-calibrated
    // tempco, each sample is divided back to its reference-temperature value.
    // Needs a setTemperature() reading before each addLightSample().
    bool             lightCompEnabled = (SIRCADIAN_LIGHT_COMP != 0);
    float            lightTempco      = SIRCADIAN_LIGHT_TEMPCO;
    float            lightTempRefC    = SIRCADIAN_LIGHT_TREF_C;
    float            lightDarkAdc     = SIRCADIAN_LIGHT_DARK_ADC;
};

class Sircadian {
public:
    // Persisted learned state: just the DriftClock drift ppm.
    static constexpr int STATE_FLOATS = 1;

    explicit Sircadian(DriftClock::MonotonicMsFn nowMs);

    // Apply configuration and reset all learned state (clock seed included).
    void begin(const SircadianConfig& cfg);

    // -- wall clock ---------------------------------------------------------
    void    setEpochMs(int64_t epochMs);       // absolute (re)seed
    int64_t epochMs() const { return clk_.epochMs(); }
    bool    isSynced() const { return clk_.isSynced(); }
    int16_t localMinuteOfDay() const { return clk_.localMinuteOfDay(cfg_.site.tz); }
    int16_t dayOfYear() const { return clk_.dayOfYear(cfg_.site.tz); }

    // -- inputs -------------------------------------------------------------
    // Mean-accumulate a light reading into the clock's own current minute
    // slot (multiple samples per minute average; unsynced samples are
    // dropped).
    void addLightSample(uint16_t adc);
    // Write a reading directly into an explicit minute slot (0..1439).
    void addLightSample(int minuteOfDay, uint16_t adc);
    // Temperature reading (degC), remembered for light-sample correction
    // (lightCompEnabled). Has no effect on the clock rate.
    void setTemperature(float tC);

    // Undo the sensor's temperature-dependent gain: the reading is
    // dark + S*(1 + k*(T - Tref))*E^gamma, so dividing the dark-relative
    // part by the gain recovers the reading at Tref. Applied automatically
    // by addLightSample() when lightCompEnabled and a temperature has been
    // fed; public so acquisition code can also correct samples explicitly.
    uint16_t correctLightSample(uint16_t adc, float tC) const;

    // -- fit cadence --------------------------------------------------------
    // Call from the main loop. Detects local-midnight rollover, fits the
    // completed day, applies the discipline, clears the buffer. Returns true
    // when a fit was attempted (check lastFit() for the outcome).
    bool service();
    // Fit the current buffer contents right now against {doy-1, doy, doy+1}
    // and apply the discipline on accept. Does not clear the buffer. (The
    // simulator's per-day driver; also useful for manual triggering.)
    FitResult runFitNow();
    // Most recent fit attempt (default-constructed until the first one).
    const FitResult& lastFit() const { return lastFit_; }

    // -- diagnostics --------------------------------------------------------
    int32_t appliedPpm() const { return clk_.driftPpm(); }   // learned drift

    // Direct access to the 1440-slot observation buffer (NO_DATA-filled).
    uint16_t*       lightBuffer()       { return obs_; }
    const uint16_t* lightBuffer() const { return obs_; }
    void clearDay();                    // reset buffer to NO_DATA

    // -- persistence --------------------------------------------------------
    // Learned drift ppm only. Persisting and re-seeding epochMs() across
    // reboots stays the caller's job, as with DriftClock.
    void exportState(float out[STATE_FLOATS]) const;
    void importState(const float in[STATE_FLOATS]);

private:
    void      applyFit(const FitResult& r, int64_t reportedMs);
    void      flushMinute();            // commit the pending minute average

    SircadianConfig cfg_;
    DriftClock::MonotonicMsFn now_;
    DriftClock clk_;
    Discipline disc_;

    uint16_t obs_[SIRCADIAN_MINUTES_PER_DAY];

    FitResult lastFit_{};

    // Pending per-minute light average.
    uint32_t minuteSum_   = 0;
    uint16_t minuteCount_ = 0;
    int16_t  minuteSlot_  = -1;

    // service() rollover detection.
    int16_t lastMinute_ = -1;
    int16_t bufferDoy_  = -1;

    // Most recent temperature, for light-sample correction.
    float lastTempC_    = 0.0f;
    bool  haveTemp_     = false;
};

}  // namespace solar

#if defined(ARDUINO)
#include <Arduino.h>
// millis() is 32-bit and wraps every ~49.7 days; a clock meant to free-run
// for years needs the 64-bit extension below. Pass this to the Sircadian
// constructor. It detects wraps by comparison, so it must be called at
// least once per 49 days -- any sketch feeding samples every minute
// qualifies.
inline uint64_t sircadianMillis64() {
    static uint32_t last = 0;
    static uint32_t wraps = 0;
    uint32_t now = millis();
    if (now < last) ++wraps;
    last = now;
    return ((uint64_t)wraps << 32) | now;
}
#endif
