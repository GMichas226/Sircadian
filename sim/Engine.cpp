#include "Engine.h"
#include "World.h"
#include "SolarModel.h"
#include "SolarFit.h"
#include "DriftClock.h"
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

static inline int wrapDoy(int d) { return ((d - 1 + 366) % 366) + 1; }

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

    solar::DriftClock clk(&simNow);
    g_simMonoMs = 0;
    double  monoAccum = 0.0;
    int64_t trueEpoch = cfg.startEpochMs;
    clk.setEpochMs(trueEpoch + cfg.initialOffsetMs);   // imperfect initial sync
    clk.setDriftPpm(0);

    const double yearMs = 365.25 * 86400.0 * 1000.0;
    const int    cadence = std::max(1, cfg.cadenceDays);

    DeploymentResult res;
    res.index = (int)index;
    res.params = dp;
    res.daily.reserve(cfg.horizonDays);

    uint16_t obs[MINS_PER_DAY];
    int fits = 0, accepts = 0, curGap = 0, longestGap = 0;
    std::vector<double> errHist; errHist.reserve(cfg.horizonDays);

    for (int day = 0; day < cfg.horizonDays; ++day) {
        g_simMonoMs = (uint64_t)(monoAccum + 0.5);
        int64_t reported = clk.epochMs();
        int64_t errMs    = reported - trueEpoch;
        errHist.push_back((double)errMs);
        double ageYr = (double)(trueEpoch - cfg.startEpochMs) / yearMs;

        world.beginDay(dayOfYearUTC(trueEpoch, dp.site.tz));
        double indoorNoon  = world.noonIndoorTempC();
        double truePpm     = oscPpm(dp, indoorNoon, ageYr);
        double learnedPpm  = -(double)clk.driftPpm();

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
            world.sampleObs(obs, clockErrMin);

            int dDoy = clk.dayOfYear(device.tz);
            int doys[3] = { wrapDoy(dDoy - 1), wrapDoy(dDoy), wrapDoy(dDoy + 1) };
            solar::FitResult r =
                solar::fitBestOfDays(doys, 3, obs, solar::NO_DATA, device, cfg.fit);

            row.accepted = r.accepted; row.shiftMin = r.shiftMin;
            row.nUsed = r.nUsed; row.rmsOverAlpha = r.rmsOverAlpha;

            if (r.accepted) {
                ++accepts; curGap = 0;
                int32_t step = solar::driftPpmFromShift(
                    r.shiftMin, clk.secondsSinceSync(), cfg.maxPpmStep);
                clk.setDriftPpm(clk.driftPpm() - step);           // rate correction
                int64_t shiftMs = (int64_t)((double)r.shiftMin * 60000.0);
                clk.setEpochMs(reported - shiftMs);               // re-anchor offset
            } else {
                ++curGap; longestGap = std::max(longestGap, curGap);
            }

            int stride = std::max(1, cfg.curveStrideDays);
            if (detail && ((fits - 1) % stride) == 0) {
                solar::SolarCurve dc; solar::prepareCurve(dc, r.doyUsed, device);
                for (int m = 0; m < MINS_PER_DAY; ++m) {
                    double th = (double)r.alpha *
                                (double)solar::intensityAt(dc, (float)m - r.shiftMin);
                    double observed = (obs[m] != solar::NO_DATA) ? (double)obs[m] : NAN;
                    res.curves.push_back({ day, m, observed, th });
                }
            }
        }
        res.daily.push_back(row);

        // Integrate one day at 1-minute resolution. True UTC advances by exactly
        // 60000 ms/min (no rounding accumulation); the monotonic clock advances at
        // (1 + ppm/1e6) of that, with ppm from the live indoor temperature.
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

    int lastBad = -1;
    for (int i = 0; i < cfg.horizonDays; ++i)
        if (std::fabs(errHist[i]) >= 60000.0) lastBad = i;   // >= 1 min off
    res.convergenceDay = (lastBad < 0) ? 0
                       : (lastBad + 1 < cfg.horizonDays ? lastBad + 1 : -1);
    return res;
}

}  // namespace sim
