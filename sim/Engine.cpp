#include "Engine.h"
#include "World.h"
#include "SolarModel.h"
#include "SolarFit.h"
#include "Sircadian.h"
#include <cmath>
#include <ctime>
#include <algorithm>

namespace sim {

// Monotonic-ms source the DriftClock reads. thread_local so a process that ever
// runs deployments concurrently stays correct; one-per-process is the norm.
static thread_local uint64_t g_simMonoMs = 0;
static uint64_t simNow() { return g_simMonoMs; }

// Day-of-year (1..366) for an absolute UTC instant at offset `tz` hours.
static int dayOfYearUTC(int64_t utcMs, float tz) {
    time_t t = (time_t)(utcMs / 1000 + (int64_t)((double)tz * 3600.0));
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    return tmv.tm_yday + 1;
}

static inline double oscPpm(const DeploymentParams& dp, double Tindoor, double ageYr) {
    double dT = Tindoor - dp.oscTurnoverC;
    return dp.oscPpm0 + dp.oscTempcoA * dT * dT + dp.oscAgingPpmPerYr * ageYr;
}

DeploymentResult simulateDeployment(const RunConfig& cfg, uint32_t index, bool detail) {
    const uint64_t seed = makeSeed(cfg.seedBase, index);
    DeploymentParams dp = rollDeployment(seed, cfg.ranges);

    World world(dp.site, dp, cfg.cloud, cfg.temp, cfg.sensor,
                seed ^ 0xC2B2AE3D27D4EB4FULL);

    const solar::SolarParams device = dp.site;   // deployer competent: device == truth

    // The device under test IS the library facade -- the same object an
    // Arduino sketch drives (buffer, fit cadence, discipline, optional light
    // correction), so the sim exercises the exact firmware composition.
    g_simMonoMs = 0;
    solar::Sircadian dev(&simNow);
    solar::SircadianConfig devCfg;
    devCfg.site            = device;
    devCfg.fit             = cfg.fit;
    devCfg.discipline      = cfg.discipline;
    devCfg.maxPpmStep      = cfg.maxPpmStep;
    // Light-sensor correction: the device knows its tempco/dark from a bench
    // calibration, i.e. the unit's rolled truth perturbed by the rolled
    // calibration error (never the truth itself).
    devCfg.lightCompEnabled = cfg.lightCompEnabled;
    devCfg.lightTempco      = (float)(dp.sensorTempco * (1.0 + dp.lightCalErrFrac));
    devCfg.lightTempRefC    = (float)cfg.sensor.sensorTempRefC;
    devCfg.lightDarkAdc     = (float)(dp.sensorDarkAdc + dp.lightCalDarkErrAdc);
    dev.begin(devCfg);
    double tempTrace[MINS_PER_DAY];      // today's sensed temps (light comp)

    double  monoAccum = 0.0;
    int64_t trueEpoch = cfg.startEpochMs;
    dev.setEpochMs(trueEpoch + cfg.initialOffsetMs);   // imperfect initial sync

    const double yearMs = 365.25 * 86400.0 * 1000.0;
    const int    cadence = std::max(1, cfg.cadenceDays);

    DeploymentResult res;
    res.index = (int)index;
    res.params = dp;
    res.daily.reserve(cfg.horizonDays);

    int fits = 0, accepts = 0, curGap = 0, longestGap = 0;
    std::vector<double> errHist; errHist.reserve(cfg.horizonDays);

    for (int day = 0; day < cfg.horizonDays; ++day) {
        g_simMonoMs = (uint64_t)(monoAccum + 0.5);
        int64_t reported = dev.epochMs();
        int64_t errMs    = reported - trueEpoch;
        errHist.push_back((double)errMs);
        double ageYr = (double)(trueEpoch - cfg.startEpochMs) / yearMs;

        world.beginDay(dayOfYearUTC(trueEpoch, dp.site.tz));
        // Realize today's sensor reads up front (one draw per minute, in order,
        // so the temp-sensor RNG stream is deterministic) for the light-sample
        // correction below.
        if (cfg.lightCompEnabled)
            for (int m = 0; m < MINS_PER_DAY; ++m)
                tempTrace[m] = world.sensedTempC(m);
        double indoorNoon  = world.noonIndoorTempC();
        double truePpm     = oscPpm(dp, indoorNoon, ageYr);
        double learnedPpm  = -(double)dev.appliedPpm();

        DailyRow row;
        row.day = day; row.trueMs = trueEpoch; row.reportedMs = reported;
        row.errMs = errMs; row.truePpm = truePpm; row.learnedPpm = learnedPpm;
        row.indoorTempC = indoorNoon; row.cloudiness = world.dayCloudiness();
        row.regime = world.regime(); row.shiftMin = NAN;

        bool isFit = (day > 0 && (day % cadence) == 0);
        if (isFit) {
            row.isFit = true;
            ++fits;
            double clockErrMin = (double)errMs / 60000.0;
            world.sampleObs(dev.lightBuffer(), clockErrMin);

            // Acquisition-time light correction: a real device corrects each
            // sample as it reads it, using its concurrent temperature. The
            // buffer is device-minute indexed; device minute m was sampled at
            // true minute m - clockErrMin, so that is the temperature the
            // device saw. Same formula the firmware applies (correctLightSample).
            if (cfg.lightCompEnabled) {
                uint16_t* obs = dev.lightBuffer();
                int errMin = (int)std::lround(clockErrMin);
                for (int m = 0; m < MINS_PER_DAY; ++m) {
                    if (obs[m] == solar::NO_DATA) continue;
                    int tm = std::min(std::max(m - errMin, 0), MINS_PER_DAY - 1);
                    obs[m] = dev.correctLightSample(obs[m], (float)tempTrace[tm]);
                }
            }

            solar::FitResult r = dev.runFitNow();

            row.accepted = r.accepted; row.shiftMin = r.shiftMin;
            row.nUsed = r.nUsed; row.rmsOverAlpha = r.rmsOverAlpha;

            if (r.accepted) { ++accepts; curGap = 0; }
            else            { ++curGap; longestGap = std::max(longestGap, curGap); }

            int stride = std::max(1, cfg.curveStrideDays);
            if (detail && ((fits - 1) % stride) == 0) {
                solar::SolarCurve dc; solar::prepareCurve(dc, r.doyUsed, device);
                for (int m = 0; m < MINS_PER_DAY; ++m) {
                    double th = (double)r.alpha *
                                (double)solar::intensityAt(dc, (float)m - r.shiftMin);
                    uint16_t o = dev.lightBuffer()[m];
                    double observed = (o != solar::NO_DATA) ? (double)o : NAN;
                    res.curves.push_back({ day, m, observed, th });
                }
            }
        }
        res.daily.push_back(row);

        // Integrate one day at 1-minute resolution. True UTC advances by exactly
        // 60000 ms/min (no rounding accumulation); the monotonic clock advances at
        // (1 + ppm/1e6) of that, with ppm from the live indoor temperature.
        // Nothing reads the clock mid-day (the light correction is applied to the
        // buffer at fit time, above), so this loop is pure integration.
        for (int m = 0; m < MINS_PER_DAY; ++m) {
            double ppm = oscPpm(dp, world.indoorTempC(m), ageYr);
            monoAccum += 60000.0 * (1.0 + ppm / 1e6);
            trueEpoch += 60000;
        }
    }

    res.fits = fits; res.accepts = accepts;
    res.acceptFrac = fits ? (double)accepts / (double)fits : 0.0;
    res.longestGapDays = longestGap * cadence;

    int q = std::max(1, cfg.horizonDays / 4);
    double ss = 0.0, mx = 0.0; int cnt = 0;
    for (int i = cfg.horizonDays - q; i < cfg.horizonDays; ++i) {
        if (i < 0) continue;
        ss += errHist[i] * errHist[i];
        mx = std::max(mx, std::fabs(errHist[i]));
        ++cnt;
    }
    res.steadyErrMs = cnt ? std::sqrt(ss / cnt) : 0.0;
    res.maxErrMs    = mx;

    // Convergence = settling of the STARTUP transient relative to the deployment's
    // own steady-state, not a fixed absolute target. The irreducible daily-fit floor
    // is many minutes, so the old fixed 1-min threshold reported "never" for most
    // units. The band must clear the *seasonal* steady-state ripple or it fires every
    // winter, so it is set from the peak |err| over the second half of the run (which
    // spans all seasons in steady state), with an absolute floor. convergence_day is
    // the first day after which the error never again exceeds that band.
    double steadyEnv = 0.0;
    for (int i = cfg.horizonDays / 2; i < cfg.horizonDays; ++i)
        steadyEnv = std::max(steadyEnv, std::fabs(errHist[i]));
    const double convFloorMs = 300000.0;                       // 5 min absolute floor
    const double convThresh  = std::max(convFloorMs, 1.25 * steadyEnv);
    int lastBad = -1;
    for (int i = 0; i < cfg.horizonDays; ++i)
        if (std::fabs(errHist[i]) >= convThresh) lastBad = i;
    res.convergenceDay = (lastBad < 0) ? 0
                       : (lastBad + 1 < cfg.horizonDays ? lastBad + 1 : -1);
    return res;
}

}  // namespace sim
