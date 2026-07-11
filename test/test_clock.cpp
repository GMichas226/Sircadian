// Host test: DriftClock anchor/slew semantics and the arithmetic day-of-year.
//
/* Build (from repo root):
     g++ -std=c++17 -I src src/DriftClock.cpp test/test_clock.cpp \
         -o test_clock && ./test_clock  */

#include "DriftClock.h"
#include <cstdio>
#include <cstdint>

using namespace solar;

// Fake monotonic source the tests can step deterministically.
static uint64_t g_mono = 0;
static uint64_t fakeNow() { return g_mono; }

static int g_fails = 0;
static void check(const char* name, bool cond, const char* detail) {
    printf("[%s] %s %s\n", cond ? "PASS" : "FAIL", name, detail ? detail : "");
    if (!cond) g_fails++;
}

int main() {
    // 1) setDriftPpm keeps its historical retroactive semantics: the rate
    //    applies over the whole span since the rate anchor.
    {
        g_mono = 0;
        DriftClock clk(&fakeNow);
        clk.setEpochMs(1000000000000LL);
        g_mono += 2000000;                        // 2000 s free-run
        clk.setDriftPpm(50);
        int64_t expect = 1000000000000LL + 2000000 + (2000000LL * 50) / 1000000;
        check("setDriftPpm retroactive", clk.epochMs() == expect, nullptr);
    }

    // 2) slewRatePpm integrates a piecewise-constant rate: each segment
    //    accrues at its own ppm, no retroactive rescale.
    {
        g_mono = 0;
        DriftClock clk(&fakeNow);
        clk.setEpochMs(0);
        g_mono += 1000000;                        // 1000 s at 0 ppm
        clk.slewRatePpm(100);
        g_mono += 1000000;                        // 1000 s at +100 ppm -> +100 ms
        clk.slewRatePpm(-200);
        g_mono += 500000;                         // 500 s at -200 ppm -> -100 ms
        int64_t expect = 2500000 + 100 - 100;
        char buf[64]; snprintf(buf, sizeof buf, "got %lld", (long long)clk.epochMs());
        check("slew piecewise exact", clk.epochMs() == expect, buf);
    }

    // 3) slewRatePpm must NOT count as a sync; setEpochMs must.
    {
        g_mono = 0;
        DriftClock clk(&fakeNow);
        clk.setEpochMs(0);
        g_mono += 3600000;                        // 1 h
        clk.slewRatePpm(75);
        g_mono += 3600000;                        // 1 h more
        check("slew keeps secondsSinceSync", clk.secondsSinceSync() == 7200, nullptr);
        clk.setEpochMs(clk.epochMs());
        check("setEpochMs resets sync", clk.secondsSinceSync() == 0, nullptr);
    }

    // 4) slew before any sync degrades to setDriftPpm; clock stays unsynced.
    {
        g_mono = 0;
        DriftClock clk(&fakeNow);
        clk.slewRatePpm(120);
        check("pre-sync slew sets ppm", clk.driftPpm() == 120, nullptr);
        check("pre-sync slew not synced", !clk.isSynced() && clk.epochMs() == 0, nullptr);
    }

    // 5) Arithmetic day-of-year against known calendar dates (UTC, tz=0).
    {
        struct Case { int64_t epochSec; int16_t doy; const char* what; };
        const Case good[] = {
            { 1704067200LL, 1,   "2024-01-01" },
            { 1709251200LL, 61,  "2024-03-01 (leap)" },
            { 1735603200LL, 366, "2024-12-31 (leap)" },
            { 1703980800LL, 365, "2023-12-31" },
            { 1783641600LL, 191, "2026-07-10" },
        };
        for (const Case& c : good) {
            g_mono = 0;
            DriftClock clk(&fakeNow);
            clk.setEpochMs(c.epochSec * 1000);
            char buf[64];
            snprintf(buf, sizeof buf, "%s -> %d (want %d)",
                     c.what, (int)clk.dayOfYear(0.0f), (int)c.doy);
            check("dayOfYear", clk.dayOfYear(0.0f) == c.doy, buf);
        }
        // Timezone rollover: 23:30 UTC on Dec 31 is already Jan 1 at UTC+2.
        g_mono = 0;
        DriftClock clk(&fakeNow);
        clk.setEpochMs((1735603200LL + 23 * 3600 + 1800) * 1000);
        check("dayOfYear tz rollover", clk.dayOfYear(2.0f) == 1, nullptr);
    }

    printf(g_fails ? "\n%d FAILURES\n" : "\nall passed\n", g_fails);
    return g_fails ? 1 : 0;
}
