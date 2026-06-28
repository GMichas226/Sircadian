#pragma once
#include <cstdint>
#include "SolarFit.h"     // solar::FitResult, driftPpmFromShift
#include "DriftClock.h"   // solar::DriftClock

// The shared discipline POLICY: given an accepted SolarFit, decide how to correct
// the clock's offset and rate. This is deliberately dependency-free (just <cstdint>
// plus the frozen src/ primitives -- no host/JSON headers) so it compiles for the
// ESP. Both the host simulator (sim/Engine.cpp) and the eventual firmware main loop
// construct one Discipline from the same config and call onAcceptedFit() per
// accepted fit -- one policy, two callers, so the sim and device cannot diverge.
//
// NOTE on defaults: these are the TUNED loop (fractional offset nudge + reduced-gain
// EMA rate loop), which on the realistic fleet cuts steady error ~3.4x and max error
// ~4.7x vs the historical loop. To recover the historical full-reset + unity per-day
// step exactly, set offsetGain=1, rateMode=0 (byte-for-byte the old loop).

namespace solar {

struct DisciplineConfig {
    float offsetGain     = 0.1f;  // Kp: fraction of the measured shift applied (1 = full reset)
    int   rateMode       = 1;     // 0 legacy per-day step, 1 ema, 2 regression
    float rateGain       = 0.1f;  // Kr: rate-loop gain (rateMode 1)
    int   rateWindowFits = 0;     // regression window in accepted fits (rateMode 2)
    float acqGain        = 0.0f;  // optional Kp acquisition-ramp start value (0 = off)
    int   acqFits        = 0;     // ... ramped to offsetGain over this many accepts
};

class Discipline {
public:
    Discipline(const DisciplineConfig& cfg, int32_t maxPpmStep)
        : cfg_(cfg), maxPpmStep_(maxPpmStep) {}

    // Apply one accepted fit to the clock: rate correction, then offset re-anchor.
    // `reportedMs` is the clock's drift-corrected epoch read for this fit.
    void onAcceptedFit(DriftClock& clk, const FitResult& r, int64_t reportedMs);

private:
    double effectiveOffsetGain() const;   // Kp, with optional acquisition ramp

    DisciplineConfig cfg_;
    int32_t          maxPpmStep_;
    int              accepts_ = 0;
};

}  // namespace solar
