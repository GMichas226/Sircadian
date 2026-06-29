// Host sanity test for the deployment engine (sim/). Builds against sim/ + src/.
//
/* Build (from repo root):
     g++ -std=c++17 -O2 -I src -I sim src/SolarModel.cpp src/SolarFit.cpp \
         src/DriftClock.cpp sim/World.cpp sim/Deployment.cpp sim/Engine.cpp \
         test/test_sim.cpp -o test_sim && ./test_sim  */
//
// These are coarse end-to-end checks, not accuracy assertions: a noiseless
// clear-sky deployment must converge to ~0 wall-time error with near-100%
// accepts, and a heavily-overcast deployment must stay bounded (the discipline
// loop must never run away). Realistic-fidelity numbers are explored in the GUI.

#include "RunConfig.h"
#include "Engine.h"
#include <cstdio>
#include <cmath>

using namespace sim;

static int g_fails = 0;
static void check(const char* name, bool cond, const char* detail) {
    printf("[%s] %s %s\n", cond ? "PASS" : "FAIL", name, detail ? detail : "");
    if (!cond) g_fails++;
}

// Pin every rolled range to a single value so the deployment is deterministic.
static void fixed(Range& r, double v) { r.lo = v; r.hi = v; }

int main() {
    // 1) Noiseless, cloud-free, constant-drift deployment converges.
    {
        RunConfig c;
        c.horizonDays = 200;
        c.numDeployments = 1;
        DeploymentRanges& g = c.ranges;
        fixed(g.latDeg, 38.0);        fixed(g.lonDeg, 23.7);  fixed(g.atmK, 1.6);
        fixed(g.baseCloud, 0.0);      fixed(g.seasonAmp, 0.0);
        fixed(g.winterPeakDoy, 15);   fixed(g.tMeanC, 20.0);
        fixed(g.tSeasonAmpC, 0.0);    fixed(g.tDiurnalAmpC, 0.0);
        fixed(g.roomLagHours, 3.0);   fixed(g.greenhouseGainC, 0.0);
        fixed(g.sensorGamma, 1.0);    fixed(g.sensorGain, 1.0);
        fixed(g.sensorDarkAdc, 0.0);  fixed(g.sensorTempco, 0.0);
        fixed(g.sensorNoiseFrac, 0.0);fixed(g.sensorDropFrac, 0.0);
        fixed(g.oscPpm0, 20.0);       fixed(g.oscTempcoA, 0.0);
        fixed(g.oscTurnoverC, 25.0);  fixed(g.oscAgingPpmPerYr, 0.0);
        // Make every regime a perfectly clear sky.
        c.cloud.clearSky = c.cloud.broken = c.cloud.overcast = RegimeOptics{1.0, 0.0, 60.0};
        c.cloud.synopticStd = 0.0; c.cloud.diffuseFrac = 0.0;

        DeploymentResult r = simulateDeployment(c, 0, false);
        char buf[128];
        snprintf(buf, sizeof buf, "steady=%.3fs max=%.3fs accept=%.2f conv=%d",
                 r.steadyErrMs / 1000.0, r.maxErrMs / 1000.0, r.acceptFrac, r.convergenceDay);
        check("clean: high accept fraction", r.acceptFrac > 0.95, buf);
        check("clean: steady error < 5 s", r.steadyErrMs < 5000.0, buf);
        check("clean: converges quickly", r.convergenceDay >= 0 && r.convergenceDay < 10, buf);
    }

    // 2) Heavily-overcast deployment stays bounded (never diverges).
    {
        RunConfig c;
        c.horizonDays = 365;
        c.numDeployments = 1;
        DeploymentRanges& g = c.ranges;
        fixed(g.latDeg, 38.0);        fixed(g.lonDeg, 23.7);  fixed(g.atmK, 1.6);
        fixed(g.baseCloud, 0.90);     fixed(g.seasonAmp, 0.0);
        fixed(g.winterPeakDoy, 15);   fixed(g.tMeanC, 20.0);
        fixed(g.tSeasonAmpC, 0.0);    fixed(g.tDiurnalAmpC, 8.0);
        fixed(g.roomLagHours, 4.0);   fixed(g.greenhouseGainC, 5.0);
        fixed(g.sensorGamma, 0.9);    fixed(g.sensorGain, 1.0);
        fixed(g.sensorDarkAdc, 20.0); fixed(g.sensorTempco, 0.002);
        fixed(g.sensorNoiseFrac, 0.02);fixed(g.sensorDropFrac, 0.02);
        fixed(g.oscPpm0, 15.0);       fixed(g.oscTempcoA, -0.035);
        fixed(g.oscTurnoverC, 25.0);  fixed(g.oscAgingPpmPerYr, 0.0);

        DeploymentResult r = simulateDeployment(c, 0, false);
        char buf[128];
        snprintf(buf, sizeof buf, "steady=%.1fs max=%.1fs accept=%.2f fits=%d accepts=%d",
                 r.steadyErrMs / 1000.0, r.maxErrMs / 1000.0, r.acceptFrac, r.fits, r.accepts);
        check("overcast: attempted fits", r.fits > 0, buf);
        check("overcast: still accepts some", r.accepts > 0, buf);
        check("overcast: bounded (max < 30 min)", r.maxErrMs < 30.0 * 60.0 * 1000.0, buf);
        check("overcast: no runaway (steady < 10 min)", r.steadyErrMs < 10.0 * 60.0 * 1000.0, buf);
    }

    // 3) Discipline loop: the smoothed loop (offset nudge + EMA rate) must still
    //    converge on a clean world, and on a fixed NOISY seed it must not be worse
    //    than the historical full-reset loop. These are bounds, not tuned targets --
    //    the world is byte-identical across the pair; only the controller changes.
    {
        auto clean = [](RunConfig& c) {
            c.horizonDays = 200; c.numDeployments = 1;
            DeploymentRanges& g = c.ranges;
            fixed(g.latDeg, 38.0);        fixed(g.lonDeg, 23.7);  fixed(g.atmK, 1.6);
            fixed(g.baseCloud, 0.0);      fixed(g.seasonAmp, 0.0);
            fixed(g.winterPeakDoy, 15);   fixed(g.tMeanC, 20.0);
            fixed(g.tSeasonAmpC, 0.0);    fixed(g.tDiurnalAmpC, 0.0);
            fixed(g.roomLagHours, 3.0);   fixed(g.greenhouseGainC, 0.0);
            fixed(g.sensorGamma, 1.0);    fixed(g.sensorGain, 1.0);
            fixed(g.sensorDarkAdc, 0.0);  fixed(g.sensorTempco, 0.0);
            fixed(g.sensorNoiseFrac, 0.0);fixed(g.sensorDropFrac, 0.0);
            fixed(g.oscPpm0, 20.0);       fixed(g.oscTempcoA, 0.0);
            fixed(g.oscTurnoverC, 25.0);  fixed(g.oscAgingPpmPerYr, 0.0);
            c.cloud.clearSky = c.cloud.broken = c.cloud.overcast = RegimeOptics{1.0, 0.0, 60.0};
            c.cloud.synopticStd = 0.0; c.cloud.diffuseFrac = 0.0;
        };
        auto noisy = [](RunConfig& c) {
            c.horizonDays = 365; c.numDeployments = 1;
            DeploymentRanges& g = c.ranges;
            fixed(g.latDeg, 40.0);        fixed(g.lonDeg, 0.0);   fixed(g.atmK, 1.6);
            fixed(g.baseCloud, 0.40);     fixed(g.seasonAmp, 0.0);
            fixed(g.winterPeakDoy, 15);   fixed(g.tMeanC, 18.0);
            fixed(g.tSeasonAmpC, 0.0);    fixed(g.tDiurnalAmpC, 6.0);
            fixed(g.roomLagHours, 3.0);   fixed(g.greenhouseGainC, 4.0);
            fixed(g.sensorGamma, 0.9);    fixed(g.sensorGain, 1.0);
            fixed(g.sensorDarkAdc, 15.0); fixed(g.sensorTempco, 0.002);
            fixed(g.sensorNoiseFrac, 0.02);fixed(g.sensorDropFrac, 0.01);
            fixed(g.oscPpm0, 15.0);       fixed(g.oscTempcoA, -0.035);
            fixed(g.oscTurnoverC, 25.0);  fixed(g.oscAgingPpmPerYr, 0.0);
        };

        // Clean world still converges under the smoothed loop.
        RunConfig cc; clean(cc);
        cc.discipline.offsetGain = 0.3f; cc.discipline.rateMode = 1; cc.discipline.rateGain = 0.1f;
        DeploymentResult rc = simulateDeployment(cc, 0, false);
        char b1[128];
        snprintf(b1, sizeof b1, "steady=%.3fs accept=%.2f conv=%d",
                 rc.steadyErrMs / 1000.0, rc.acceptFrac, rc.convergenceDay);
        check("loop: smoothed converges on clean world",
              rc.steadyErrMs < 5000.0 && rc.acceptFrac > 0.95 && rc.convergenceDay >= 0, b1);

        // Same noisy world, two controllers: smoothed must be <= historical baseline.
        RunConfig base; noisy(base);
        base.discipline.offsetGain = 1.0f; base.discipline.rateMode = 0;     // historical
        RunConfig tuned; noisy(tuned);
        tuned.discipline.offsetGain = 0.2f; tuned.discipline.rateMode = 1; tuned.discipline.rateGain = 0.1f;
        double sb = simulateDeployment(base, 0, false).steadyErrMs;
        double st = simulateDeployment(tuned, 0, false).steadyErrMs;
        char b2[128];
        snprintf(b2, sizeof b2, "baseline=%.1fs smoothed=%.1fs", sb / 1000.0, st / 1000.0);
        check("loop: smoothed <= historical on noisy seed", st <= sb, b2);
    }

    printf("\n%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails ? 1 : 0;
}
