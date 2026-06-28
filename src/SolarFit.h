#pragma once
#include <cstdint>
#include "SolarModel.h"
#include "config.h"

// Recover a clock's time offset by aligning a measured day of per-minute light
// samples against the SolarModel curve. Input obs[m] is the mean reading for
// local minute-of-day m, or `noData` for an unsampled minute. The accept gates
// reject poorly-sampled and noisy days (well-sampled / low-residual / sharp-peak).
// NOTE: the "unambiguity" gate is a peak-sharpness / SNR proxy, not a two-noon
// detector -- it does not reject a genuinely bimodal cost surface (see fitDay).

namespace solar {

// Sentinel for an unsampled minute slot. Caller may use any uint16_t value
// outside its real data range; pass it to fitDay as `noData`.
constexpr uint16_t NO_DATA = 0xFFFF;

// Accept thresholds and search bounds. Defaults come from config.h; override
// per-instance by constructing a FitConfig, or globally via -DSIRCADIAN_*.
struct FitConfig {
    int   costHalfWindowMin  = SIRCADIAN_COST_HALF_WINDOW_MIN;  // +/-5 h around predicted noon
    int   shiftRangeMin      = SIRCADIAN_SHIFT_RANGE_MIN;       // full-day +/-12 h shift search
    int   minUsedPoints      = SIRCADIAN_MIN_USED_POINTS;       // >= 6 h of daylit samples to accept
    int   fullCurveMinPoints = SIRCADIAN_FULL_CURVE_MIN_POINTS; // >= 9 h marks a "full" curve
    float maxRmsOverAlpha    = SIRCADIAN_MAX_RMS_OVER_ALPHA;    // normalized residual ceiling
    float unambiguityFactor  = SIRCADIAN_UNAMBIGUITY_FACTOR;    // next-best SSE > this*best (peak sharpness, not bimodality)
    float minAlpha           = SIRCADIAN_MIN_ALPHA;             // peak must clear this (noise floor)
    float maxAlpha           = SIRCADIAN_MAX_ALPHA;
};

struct FitResult {
    bool   accepted      = false;
    float  shiftMin      = 0.0f;   // signed tau: measured noon - predicted noon
    float  alpha         = 0.0f;   // fitted peak amplitude (observation scale)
    int    nUsed         = 0;      // samples that contributed
    float  rmsOverAlpha  = 0.0f;   // normalized residual
    double bestSse       = 0.0;
    double nextBestSse   = 0.0;
    int    doyUsed       = 0;
    float  noonExpMin    = 0.0f;   // predicted solar noon (local min)
    float  noonFitMin    = 0.0f;   // noonExpMin + shiftMin, wrapped to [0,1440)
    bool   fullCurve     = false;
};

// Fit a single day. Precondition: `obs` MUST point at exactly 1440 entries
// (minute 0..1439); fitDay indexes it without bounds checking.
FitResult fitDay(int doy, const uint16_t* obs, uint16_t noData,
                 const SolarParams& p, const FitConfig& cfg);

// Try several candidate days and keep the lowest-SSE result. Useful on a
// power-outage recovery path where the calendar day may be off by one:
// pass e.g. {doy-1, doy, doy+1}. With a single day this is just fitDay.
FitResult fitBestOfDays(const int* doys, int nDoys, const uint16_t* obs,
                        uint16_t noData, const SolarParams& p,
                        const FitConfig& cfg);

// Convert an accepted shift into an oscillator drift step (ppm), given how
// long the clock has been free-running since its last absolute sync. The
// result is clamped to +/-maxPpmStep. This is the clock's *observed* fast-rate,
// not the correction: SUBTRACT it from your running drift figure (adding it is
// positive feedback and the estimate diverges).
//   ppm = (shiftMin * 60 s / secondsSinceSync) * 1e6
int32_t driftPpmFromShift(float shiftMin, int32_t secondsSinceSync,
                          int32_t maxPpmStep = SIRCADIAN_MAX_PPM_STEP);

}  // namespace solar
