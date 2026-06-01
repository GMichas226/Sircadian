// Host test: synthesize a day of light readings at a known time offset, then
// check that SolarFit recovers it (and rejects a cloudy day).
//
/* Build (from repo root):
     g++ -std=c++17 -I src src/SolarModel.cpp src/SolarFit.cpp \
         src/DriftClock.cpp test/test_fit.cpp -o test_fit && ./test_fit  */

#include "SolarModel.h"
#include "SolarFit.h"
#include "DriftClock.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

using namespace solar;

// Build a synthetic observed curve for `doy`, scaled to a peak of `peak`
// ADC counts, shifted later by `shiftMin` minutes (simulating a slow clock),
// with optional Gaussian-ish noise and a fraction of dropped samples.
static void synth(uint16_t* obs, int doy, const SolarParams& p,
                  float peak, float shiftMin, float noise, float dropFrac) {
    SolarCurve c;
    prepareCurve(c, doy, p);
    for (int m = 0; m < 1440; ++m) {
        // The clock reads `m` but the sun is actually at m - shiftMin.
        float trueMin = (float)m - shiftMin;
        float i = intensityAt(c, trueMin);
        float v = peak * i;
        if (noise > 0.0f) {
            float r = ((float)rand() / (float)RAND_MAX - 0.5f) * 2.0f;
            v += r * noise * peak;
        }
        if (v < 0.0f) v = 0.0f;
        if ((float)rand() / (float)RAND_MAX < dropFrac) {
            obs[m] = NO_DATA;
        } else {
            obs[m] = (uint16_t)(v + 0.5f);
        }
    }
}

static int g_fails = 0;
static void check(const char* name, bool cond, const char* detail) {
    printf("[%s] %s %s\n", cond ? "PASS" : "FAIL", name, detail ? detail : "");
    if (!cond) g_fails++;
}

int main() {
    srand(1);                                     // deterministic
    SolarParams p{ 38.0f, 23.7f, 2.0f, 1.6f };   // ~Athens
    FitConfig cfg;
    uint16_t obs[1440];

    // 1) Clear day, zero shift -> accept, recover ~0.
    synth(obs, 152, p, 3000.0f, 0.0f, 0.0f, 0.0f);
    FitResult r = fitDay(152, obs, NO_DATA, p, cfg);
    check("clear/zero accepted", r.accepted, nullptr);
    check("clear/zero shift ~0", fabsf(r.shiftMin) < 1.0f, nullptr);
    printf("       shift=%.2f min\n", r.shiftMin);

    // 2) Clear day (low sensor noise), +6 min shift -> accept, recover.
    //    (A shift near k+0.5 min is the worst case for the unambiguity
    //    gate at 1-min resolution -- see README "Known limitations".)
    synth(obs, 152, p, 3000.0f, 6.0f, 0.005f, 0.02f);
    r = fitDay(152, obs, NO_DATA, p, cfg);
    check("clear/+6 accepted", r.accepted, nullptr);
    check("clear/+6 recovered", fabsf(r.shiftMin - 6.0f) < 1.0f, nullptr);
    printf("       shift=%.2f min (expected 6.0)\n", r.shiftMin);

    // 3) Clear day (low sensor noise), -12 min shift -> accept, recover.
    synth(obs, 200, p, 3000.0f, -12.0f, 0.005f, 0.05f);
    r = fitDay(200, obs, NO_DATA, p, cfg);
    check("clear/-12 accepted", r.accepted, nullptr);
    check("clear/-12 recovered", fabsf(r.shiftMin + 12.0f) < 1.0f, nullptr);
    printf("       shift=%.2f min (expected -12)\n", r.shiftMin);

    // 4) Cloudy day (heavy noise) -> the unambiguity/residual gate REJECTS.
    synth(obs, 200, p, 3000.0f, 5.0f, 0.35f, 0.10f);
    r = fitDay(200, obs, NO_DATA, p, cfg);
    check("cloudy rejected", !r.accepted, nullptr);
    printf("       accepted=%d rms/alpha=%.3f next/best=%.2f\n",
           r.accepted, r.rmsOverAlpha,
           r.bestSse > 0 ? r.nextBestSse / r.bestSse : 0.0);

    // 5) Drift math: 6 min over 12 h saturates the +/-200 ppm step clamp;
    //    1 min slow over 7 days is unclamped (~ -99 ppm).
    int32_t ppmClamped = driftPpmFromShift(6.0f, 12 * 3600, 200);
    check("drift ppm clamps", ppmClamped == 200, nullptr);
    int32_t ppmSlow = driftPpmFromShift(-1.0f, 7 * 24 * 3600, 200);
    check("drift ppm sign/scale", ppmSlow > -105 && ppmSlow < -94, nullptr);
    printf("       clamped=%d  slow=%d (expected ~-99)\n", ppmClamped, ppmSlow);

    // 6) DriftClock applies correction in the right direction.
    static uint64_t fakeMs = 0;
    DriftClock clk([]() -> uint64_t { return fakeMs; });
    fakeMs = 0;
    clk.setEpochMs(1000000000000LL);     // arbitrary epoch
    clk.setDriftPpm(1000);               // +1000 ppm (fast)
    fakeMs = 1000000;                    // 1000 s elapsed
    int64_t reported = clk.epochMs() - 1000000000000LL;
    // 1000 s monotonic + 1000 ppm => +1 s correction => 1001000 ms.
    check("clock drift applied", reported == 1001000, nullptr);
    printf("       reported delta=%lld ms (expected 1001000)\n",
           (long long)reported);

    // 7) Closed-loop convergence -- regression for the feedback-sign bug.
    //    A crystal running ~50 ppm fast must drive the learned correction
    //    TOWARD the true value (~ -50 ppm), not away from it. Each cycle we
    //    re-anchor truth, free-run a 7-day cadence (the monotonic source gains
    //    50 ppm on real time), then apply the documented update
    //    clk.setDriftPpm(clk.driftPpm() - step). With the buggy "+ step" the
    //    estimate diverges to large positive ppm and blows the bound below.
    //    This isolates the rate-feedback SIGN, not absolute-offset behaviour.
    static uint64_t fakeMs7 = 0;
    const double  OSC   = 50e-6;                     // oscillator runs 50 ppm fast
    const int64_t DT_MS = 7LL * 24 * 3600 * 1000;    // 7-day fit cadence (real ms)
    DriftClock loop([]() -> uint64_t { return fakeMs7; });
    int64_t trueEpoch = 1700000000000LL;             // arbitrary start
    fakeMs7 = 0;
    for (int cycle = 0; cycle < 4; ++cycle) {
        loop.setEpochMs(trueEpoch);                  // re-anchor; driftPpm persists
        fakeMs7  += (uint64_t)((double)DT_MS * (1.0 + OSC));
        trueEpoch += DT_MS;
        int64_t errMs = loop.epochMs() - trueEpoch;  // how far the clock leads truth
        float   shift = (float)((double)errMs / 1000.0 / 60.0);
        int32_t step  = driftPpmFromShift(shift, loop.secondsSinceSync());
        loop.setDriftPpm(loop.driftPpm() - step);    // documented update
    }
    int32_t learned = loop.driftPpm();
    check("loop converges (sign)", learned > -60 && learned < -40, nullptr);
    printf("       learned drift=%d ppm (expected ~-50)\n", learned);

    printf("\n%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails ? 1 : 0;
}
