#include "SolarFit.h"
#include <cmath>
#include <cfloat>

namespace solar {

using std::isfinite;

// Scratch for the theoretical curve (~5.6 KB). File-static to keep it off the
// stack and heap; safe only because fitDay is serial -- do not call it
// re-entrantly.
// thread_local so concurrent fitDay calls on different threads (the Phase B
// Monte-Carlo workers) each get private scratch; single-threaded behavior is
// unchanged. ~6.8 KB per thread.
static thread_local float g_ith[SIRCADIAN_MINUTES_PER_DAY];

// Contributing-sample indices for the current fitDay, built once per call and
// reused across the tau sweep. Per-thread, like g_ith. Sized to the compiled
// cost-window default (fitDay rejects any wider caller-supplied window --
// see the capacity guard there).
static constexpr int kMaxObsIdx = 2 * SIRCADIAN_COST_HALF_WINDOW_MIN + 1;
static thread_local int16_t g_obsIdx[kMaxObsIdx];

// Wrap an index into [0, day) without a modulo (the shift is bounded). The single
// correction is valid only while the argument stays in [-day, 2*day); the
// static_asserts below guarantee the configured bounds keep it there.
static inline int wrapDay(int i) {
    if (i < 0)                          i += SIRCADIAN_MINUTES_PER_DAY;
    if (i >= SIRCADIAN_MINUTES_PER_DAY) i -= SIRCADIAN_MINUTES_PER_DAY;
    return i;
}

static_assert(SIRCADIAN_SHIFT_RANGE_MIN <= SIRCADIAN_MINUTES_PER_DAY,
              "wrapDay single-wrap requires shift range <= one day");
static_assert(SIRCADIAN_COST_HALF_WINDOW_MIN < SIRCADIAN_MINUTES_PER_DAY,
              "wrapDay single-wrap requires half-window < one day");

// Closed-form SSE of the best-amplitude fit at shift `tau` over the pre-built
// contributing set (indices idx[0..nContrib), with sumOO = sum(o^2) supplied --
// both are tau-invariant, so fitDay builds them once). alpha = sum(o*p)/sum(p*p);
// SSE = sum(o^2) - num^2/den. Returns DBL_MAX if too few points contribute.
// Optionally reports alpha and the contributing-sample count.
static double sseAtTau(int tau, const int16_t* idx, int nContrib,
                       const uint16_t* obs, double sumOO,
                       float* outAlpha, int* outN) {
    double sumOP = 0.0, sumPP = 0.0;
    for (int k = 0; k < nContrib; ++k) {
        int ti = idx[k];
        float p = g_ith[wrapDay(ti - tau)];
        double o = (double)obs[ti];
        sumOP += o * p;
        sumPP += (double)p * p;
    }
    if (nContrib < SIRCADIAN_MIN_FIT_POINTS || sumPP < 1e-6) {
        if (outAlpha) *outAlpha = NAN;
        if (outN) *outN = nContrib;
        return DBL_MAX;
    }
    if (outAlpha) *outAlpha = (float)(sumOP / sumPP);
    if (outN) *outN = nContrib;
    return sumOO - (sumOP * sumOP) / sumPP;
}

FitResult fitDay(int doy, const uint16_t* obs, uint16_t noData,
                 const SolarParams& p, const FitConfig& cfg) {
    FitResult r{};
    r.doyUsed     = doy;
    r.bestSse     = DBL_MAX;
    r.nextBestSse = DBL_MAX;
    r.shiftMin    = NAN;
    r.alpha       = NAN;
    r.rmsOverAlpha = NAN;

    SolarCurve ctx;
    prepareCurve(ctx, doy, p);
    r.noonExpMin = ctx.noonMin;

    for (int t = 0; t < SIRCADIAN_MINUTES_PER_DAY; ++t)
        g_ith[t] = intensityAt(ctx, (float)t);

    int noonI = (int)(ctx.noonMin + 0.5f);
    int lo    = noonI - cfg.costHalfWindowMin;
    int hi    = noonI + cfg.costHalfWindowMin;

    // A caller-supplied window wider than the compiled g_obsIdx capacity
    // would overflow the pre-pass write below; reject cleanly instead.
    if (hi - lo + 1 > kMaxObsIdx) return r;

    // Pre-pass: gather the contributing samples once. The set and sum(o^2) are
    // tau-invariant, so building them here keeps them out of the tau sweep.
    int    nContrib = 0;
    double sumOO    = 0.0;
    for (int t = lo; t <= hi; ++t) {
        int ti = wrapDay(t);
        uint16_t v = obs[ti];
        if (v == noData) continue;
        g_obsIdx[nContrib++] = (int16_t)ti;
        double o = (double)v;
        sumOO += o * o;
    }

    int    bestTau   = 0;
    double bestSse   = DBL_MAX;
    double nextBest  = DBL_MAX;
    float  bestAlpha = 0.0f;
    int    bestN     = 0;

    for (int tau = -cfg.shiftRangeMin; tau <= cfg.shiftRangeMin; ++tau) {
        float alpha; int n;
        double sse = sseAtTau(tau, g_obsIdx, nContrib, obs, sumOO, &alpha, &n);
        if (sse >= DBL_MAX) continue;
        if (sse < bestSse) {
            nextBest  = bestSse;
            bestSse   = sse;
            bestTau   = tau;
            bestAlpha = alpha;
            bestN     = n;
        } else if (sse < nextBest) {
            nextBest = sse;
        }
    }

    r.bestSse     = bestSse;
    r.nextBestSse = nextBest;
    r.shiftMin    = (float)bestTau;
    r.alpha       = bestAlpha;
    r.nUsed       = bestN;

    if (bestN == 0 || !isfinite(bestAlpha) || bestSse >= DBL_MAX) return r;
    r.rmsOverAlpha = sqrtf((float)(bestSse / bestN)) / bestAlpha;

    // Parabolic sub-minute refinement on (tau-1, tau, tau+1).
    if (bestTau > -cfg.shiftRangeMin && bestTau < cfg.shiftRangeMin) {
        double sLo = sseAtTau(bestTau - 1, g_obsIdx, nContrib, obs, sumOO, nullptr, nullptr);
        double sHi = sseAtTau(bestTau + 1, g_obsIdx, nContrib, obs, sumOO, nullptr, nullptr);
        double denom = (sLo - 2.0 * bestSse + sHi);
        if (denom > 1e-6 && sLo < DBL_MAX && sHi < DBL_MAX) {
            float refine = 0.5f * (float)((sLo - sHi) / denom);
            if (refine > -1.0f && refine < 1.0f) {
                r.shiftMin = (float)bestTau + refine;
            }
        }
    }

    // Accept criteria.
    // NOTE on the "unambiguity" gate below: nextBestSse is just the second
    // lowest sample in the tau sweep, which is almost always bestTau's immediate
    // neighbor. So this really tests how SHARP the SSE minimum is (a curvature /
    // SNR proxy that correlates with clear days), not whether the cost surface is
    // multi-modal -- it would not reject a genuinely bimodal two-noon surface.
    // bestSse >= 0 (not > 0): a perfect, zero-residual fit is valid; the alpha
    // and rmsOverAlpha gates already reject the degenerate cases.
    r.accepted = r.nUsed >= cfg.minUsedPoints
              && isfinite(r.alpha)
              && r.alpha > cfg.minAlpha
              && r.alpha < cfg.maxAlpha
              && isfinite(r.rmsOverAlpha)
              && r.rmsOverAlpha < cfg.maxRmsOverAlpha
              && r.bestSse >= 0
              && (r.nextBestSse >= DBL_MAX
                  || r.nextBestSse > cfg.unambiguityFactor * r.bestSse);

    r.fullCurve = (r.nUsed >= cfg.fullCurveMinPoints);

    r.noonFitMin = wrapMinutesOfDay(r.noonExpMin + r.shiftMin);

    return r;
}

FitResult fitBestOfDays(const int* doys, int nDoys, const uint16_t* obs,
                        uint16_t noData, const SolarParams& p,
                        const FitConfig& cfg) {
    FitResult best{};
    best.bestSse = DBL_MAX;
    for (int i = 0; i < nDoys; ++i) {
        FitResult r = fitDay(doys[i], obs, noData, p, cfg);
        if (r.bestSse < best.bestSse) best = r;
    }
    return best;
}

int32_t driftPpmFromShift(float shiftMin, int32_t secondsSinceSync,
                          int32_t maxPpmStep) {
    if (secondsSinceSync <= 0 || !isfinite(shiftMin)) return 0;
    // Clamp in double before the cast: a large shift over a short interval can
    // exceed INT32_MAX, and casting an out-of-range double to int32_t is UB.
    double ppm = (double)shiftMin * 60.0 * 1e6 / (double)secondsSinceSync;
    if (ppm >  maxPpmStep) ppm =  maxPpmStep;
    if (ppm < -maxPpmStep) ppm = -maxPpmStep;
    return (int32_t)ppm;
}

}  // namespace solar
