#include "SolarFit.h"
#include <cmath>
#include <cfloat>

namespace solar {

using std::isfinite;

// Scratch for the theoretical curve (~5.6 KB). File-static to keep it off the
// stack and heap; safe only because fitDay is serial -- do not call it
// re-entrantly.
static float g_ith[SIRCADIAN_MINUTES_PER_DAY];

// Wrap an index into [0, day) without a modulo (the shift is bounded).
static inline int wrapDay(int i) {
    if (i < 0)                          i += SIRCADIAN_MINUTES_PER_DAY;
    if (i >= SIRCADIAN_MINUTES_PER_DAY) i -= SIRCADIAN_MINUTES_PER_DAY;
    return i;
}

// Closed-form SSE of the best-amplitude fit at shift `tau` over the cost
// window [lo,hi]. alpha = sum(o*p)/sum(p*p); SSE = sum(o^2) - num^2/den.
// Returns DBL_MAX if too few points contribute. Optionally reports alpha
// and the contributing-sample count.
static double sseAtTau(int tau, int lo, int hi, const uint16_t* obs,
                       uint16_t noData, float* outAlpha, int* outN) {
    double sumOP = 0.0, sumPP = 0.0, sumOO = 0.0;
    int n = 0;
    for (int t = lo; t <= hi; ++t) {
        int ti = wrapDay(t);
        uint16_t v = obs[ti];
        if (v == noData) continue;
        float p = g_ith[wrapDay(ti - tau)];
        double o = (double)v;
        sumOP += o * p;
        sumPP += (double)p * p;
        sumOO += o * o;
        ++n;
    }
    if (n < SIRCADIAN_MIN_FIT_POINTS || sumPP < 1e-6) {
        if (outAlpha) *outAlpha = NAN;
        if (outN) *outN = n;
        return DBL_MAX;
    }
    if (outAlpha) *outAlpha = (float)(sumOP / sumPP);
    if (outN) *outN = n;
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

    int    bestTau   = 0;
    double bestSse   = DBL_MAX;
    double nextBest  = DBL_MAX;
    float  bestAlpha = 0.0f;
    int    bestN     = 0;

    for (int tau = -cfg.shiftRangeMin; tau <= cfg.shiftRangeMin; ++tau) {
        float alpha; int n;
        double sse = sseAtTau(tau, lo, hi, obs, noData, &alpha, &n);
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
        double sLo = sseAtTau(bestTau - 1, lo, hi, obs, noData, nullptr, nullptr);
        double sHi = sseAtTau(bestTau + 1, lo, hi, obs, noData, nullptr, nullptr);
        double denom = (sLo - 2.0 * bestSse + sHi);
        if (denom > 1e-6 && isfinite(sLo) && isfinite(sHi)) {
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

    float tFit = r.noonExpMin + r.shiftMin;
    while (tFit < 0.0f)     tFit += 1440.0f;
    while (tFit >= 1440.0f) tFit -= 1440.0f;
    r.noonFitMin = tFit;

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
    if (secondsSinceSync <= 0) return 0;
    int32_t ppm = (int32_t)((double)shiftMin * 60.0 * 1e6 / (double)secondsSinceSync);
    if (ppm >  maxPpmStep) ppm =  maxPpmStep;
    if (ppm < -maxPpmStep) ppm = -maxPpmStep;
    return ppm;
}

}  // namespace solar
