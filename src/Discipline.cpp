#include "Discipline.h"
#include <cmath>          // llround

namespace solar {

double Discipline::effectiveOffsetGain() const {
    // Optional acquisition ramp: start near acqGain and ease toward offsetGain over
    // the first acqFits accepts (fast cold-start / outage pull-in, then slow tracking).
    if (cfg_.acqFits <= 0) return (double)cfg_.offsetGain;
    double t = (double)accepts_ / (double)cfg_.acqFits;
    if (t > 1.0) t = 1.0;
    return (double)cfg_.acqGain + ((double)cfg_.offsetGain - (double)cfg_.acqGain) * t;
}

void Discipline::onAcceptedFit(DriftClock& clk, const FitResult& r, int64_t reportedMs) {
    ++accepts_;

    // Rate correction: driftPpmFromShift reports the clock's *observed* fast-rate,
    // so SUBTRACT it from the running drift figure (adding is positive feedback).
    int32_t step = driftPpmFromShift(r.shiftMin, clk.secondsSinceSync(), maxPpmStep_);
    if (cfg_.rateMode <= 0) {
        clk.setDriftPpm(clk.driftPpm() - step);               // legacy: full per-day step
    } else {
        // rateMode 1 (ema): apply only a fraction Kr of the per-fit estimate -- a
        // reduced-gain integrator that removes the slow drift ramp while averaging the
        // per-fit (cloud) noise down, instead of integrating it at unity gain. (Kr==1
        // is byte-for-byte the legacy step.) rateMode 2 (regression) is not yet
        // implemented and falls back to this ema arm.
        int32_t inc = (int32_t)llround((double)cfg_.rateGain * (double)step);
        clk.setDriftPpm(clk.driftPpm() - inc);
    }

    // Offset correction: move the epoch by a fraction Kp of the measured shift -- an
    // EMA that low-passes the per-fit (cloud) noise instead of stamping it on whole.
    // Kp is multiplied INSIDE the truncating cast, so Kp==1 is byte-for-byte the
    // historical full re-anchor (multiply by 1.0 is exact).
    double  kp      = effectiveOffsetGain();
    int64_t shiftMs = (int64_t)(kp * (double)r.shiftMin * 60000.0);
    clk.setEpochMs(reportedMs - shiftMs);
}

}  // namespace solar
