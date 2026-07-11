// Host test: the Sircadian facade must reproduce the manual DriftClock +
// Discipline composition (what sim/Engine.cpp historically did) bit-for-bit
// across a multi-day run, plus basic service() day-rollover behavior and the
// light-sensor temperature correction.
//
/* Build (from repo root):
     g++ -std=c++17 -I src src/SolarModel.cpp src/SolarFit.cpp \
         src/DriftClock.cpp src/Discipline.cpp src/Sircadian.cpp \
         test/test_facade.cpp -o test_facade && ./test_facade */

#include "Sircadian.h"
#include <cstdio>
#include <cmath>
#include <cstring>

using namespace solar;

static uint64_t g_mono = 0;
static uint64_t fakeNow() { return g_mono; }

static int g_fails = 0;
static void check(const char* name, bool cond, const char* detail) {
    printf("[%s] %s %s\n", cond ? "PASS" : "FAIL", name, detail ? detail : "");
    if (!cond) g_fails++;
}

// Clean synthetic day: model curve scaled to `peak`, shifted by `shiftMin`.
static void synth(uint16_t* obs, int doy, const SolarParams& p,
                  float peak, float shiftMin) {
    SolarCurve c;
    prepareCurve(c, doy, p);
    for (int m = 0; m < 1440; ++m) {
        float v = peak * intensityAt(c, (float)m - shiftMin);
        obs[m] = (v < 0.0f) ? 0 : (uint16_t)(v + 0.5f);
    }
}

int main() {
    const SolarParams site{ 38.0f, 23.7f, 2.0f, 1.6f };
    const int64_t startEpochMs = 1780272000000LL;   // 2026-05-31 UTC
    const double  oscPpm = 34.7;                     // true oscillator error
    const int     days = 40;

    // Manual composition (the historical Engine loop).
    DriftClock clkM(&fakeNow);
    Discipline discM(DisciplineConfig{}, SIRCADIAN_MAX_PPM_STEP);

    // Facade, temperature compensation off.
    Sircadian dev(&fakeNow);
    SircadianConfig cfg;
    cfg.site = site;
    dev.begin(cfg);

    g_mono = 0;
    double monoAccum = 0.0;
    int64_t trueEpoch = startEpochMs;
    clkM.setEpochMs(trueEpoch + 90000);              // 90 s initial offset
    clkM.setDriftPpm(0);
    dev.setEpochMs(trueEpoch + 90000);

    uint16_t obs[1440];
    bool identical = true;
    char detail[96] = "";
    int accepts = 0;

    for (int day = 0; day < days && identical; ++day) {
        g_mono = (uint64_t)(monoAccum + 0.5);

        int64_t repM = clkM.epochMs();
        if (dev.epochMs() != repM) {
            identical = false;
            snprintf(detail, sizeof detail, "day %d reported diverged", day);
            break;
        }

        if (day > 0) {
            double shiftMin = (double)(repM - trueEpoch) / 60000.0;
            int doy = clkM.dayOfYear(site.tz);
            synth(obs, doy, site, 3000.0f, (float)shiftMin);

            int doys[3] = { ((doy - 2 + 366) % 366) + 1, doy, (doy % 366) + 1 };
            FitResult r = fitBestOfDays(doys, 3, obs, NO_DATA, site, cfg.fit);
            if (r.accepted) { ++accepts; discM.onAcceptedFit(clkM, r, repM); }

            memcpy(dev.lightBuffer(), obs, sizeof obs);
            dev.runFitNow();

            if (dev.epochMs() != clkM.epochMs() ||
                dev.appliedPpm() != clkM.driftPpm()) {
                identical = false;
                snprintf(detail, sizeof detail,
                         "day %d post-fit diverged (ppm %d vs %d)",
                         day, (int)dev.appliedPpm(), (int)clkM.driftPpm());
            }
        }

        monoAccum += 86400000.0 * (1.0 + oscPpm / 1e6);
        trueEpoch += 86400000;
    }

    check("facade == manual loop (40 days)", identical, detail);
    check("scenario exercises accepts", accepts > 30, nullptr);
    {
        g_mono = (uint64_t)(monoAccum + 0.5);   // advance to the final boundary
        char buf[64];
        int64_t err = dev.epochMs() - trueEpoch;
        snprintf(buf, sizeof buf, "final err=%lld ms ppm=%d",
                 (long long)err, (int)dev.appliedPpm());
        printf("       %s\n", buf);
        check("loop actually disciplined", llabs(err) < 90000, buf);
    }

    // -- service(): day-rollover fit trigger --------------------------------
    {
        g_mono = 0;
        Sircadian d2(&fakeNow);
        SircadianConfig c2; c2.site = site;
        d2.begin(c2);
        // Seed so local time (UTC+2) is 23:50: pick epoch with
        // (epoch/1000 + 7200) % 86400 == 85800.
        int64_t seed = 1780272000000LL;  // 00:00 UTC -> 02:00 local
        seed += (85800LL - 7200LL) * 1000LL;
        d2.setEpochMs(seed);
        check("no fit before rollover", !d2.service(), nullptr);
        bool fired = false;
        for (int i = 0; i < 20; ++i) {           // 20 minutes across midnight
            g_mono += 60000;
            d2.addLightSample(0);                // dark -- fit must reject
            if (d2.service()) fired = true;
        }
        check("service fires at rollover", fired, nullptr);
        check("dark day rejected", !d2.lastFit().accepted, nullptr);
        check("buffer cleared after fit", d2.lightBuffer()[0] == NO_DATA ||
                                          d2.lightBuffer()[1430] == NO_DATA, nullptr);
    }

    // -- light-sensor temperature correction ---------------------------------
    // A photoresistor whose gain rides an afternoon-peaking temperature ramp
    // distorts the day asymmetrically. With lightCompEnabled and the exact
    // tempco, correcting each distorted sample must (a) recover the original
    // reading to rounding accuracy and (b) restore the undistorted fit shift.
    {
        const float kSens = 0.004f;     // worst-case rolled tempco
        const float tRef  = 20.0f;
        const float dark  = 55.0f;
        const float shiftTrue = 3.0f;
        const int   doy = 172;          // near solstice: long, well-lit day

        auto tempAt = [](int m) {
            // afternoon-peaking indoor ramp, 14..30 C (peak at 15:00)
            return 22.0f + 8.0f * cosf(2.0f * 3.14159265f * (m / 60.0f - 15.0f) / 24.0f);
        };

        uint16_t clean[1440], distorted[1440];
        synth(clean, doy, site, 3000.0f, shiftTrue);
        for (int m = 0; m < 1440; ++m) {
            clean[m] = (uint16_t)(clean[m] + dark);          // sensor dark floor
            float g = 1.0f + kSens * (tempAt(m) - tRef);
            float v = dark + ((float)clean[m] - dark) * g;   // thermal gain
            distorted[m] = (uint16_t)(v + 0.5f);
        }

        SircadianConfig lc;
        lc.site = site;
        // On a noiseless synthetic day the next-best tau is the best's
        // neighbor, so the unambiguity proxy rejects good fits (same reason
        // the simulator disables it).
        lc.fit.unambiguityFactor = 1.0f;
        lc.lightCompEnabled = true;
        lc.lightTempco      = kSens;
        lc.lightTempRefC    = tRef;
        lc.lightDarkAdc     = dark;

        g_mono = 0;
        Sircadian d3(&fakeNow);
        d3.begin(lc);
        d3.setEpochMs(1781128800000LL);  // arbitrary synced seed

        int maxRoundTrip = 0;
        for (int m = 0; m < 1440; ++m) {
            d3.setTemperature(tempAt(m));            // reading at sample time
            d3.addLightSample(m, distorted[m]);      // corrected on the way in
            int e = (int)d3.lightBuffer()[m] - (int)clean[m];
            if (e < 0) e = -e;
            if (e > maxRoundTrip) maxRoundTrip = e;
        }
        char rt[64];
        snprintf(rt, sizeof rt, "max round-trip err=%d counts", maxRoundTrip);
        check("light comp recovers samples", maxRoundTrip <= 2, rt);

        // Reference fits on the raw buffers (no device state involved).
        int doys[3] = { ((doy - 2 + 366) % 366) + 1, doy, (doy % 366) + 1 };
        FitResult fClean = fitBestOfDays(doys, 3, clean, NO_DATA, site, lc.fit);
        FitResult fCorr  = fitBestOfDays(doys, 3, d3.lightBuffer(), NO_DATA, site, lc.fit);
        char sh[96];
        snprintf(sh, sizeof sh, "clean=%.3f corrected=%.3f",
                 (double)fClean.shiftMin, (double)fCorr.shiftMin);
        check("light comp fit shift restored",
              fClean.accepted && fCorr.accepted &&
              fabsf(fCorr.shiftMin - fClean.shiftMin) < 0.15f, sh);

        // Comp disabled must pass samples through untouched.
        lc.lightCompEnabled = false;
        Sircadian d4(&fakeNow);
        d4.begin(lc);
        d4.setEpochMs(1781128800000LL);
        d4.setTemperature(tempAt(720));
        d4.addLightSample(720, distorted[720]);
        check("light comp off is passthrough",
              d4.lightBuffer()[720] == distorted[720], nullptr);
    }

    printf(g_fails ? "\n%d FAILURES\n" : "\nall passed\n", g_fails);
    return g_fails ? 1 : 0;
}
