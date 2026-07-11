#pragma once
#include <cstdint>
#include <string>
#include "SolarModel.h"
#include "SolarFit.h"
#include "Discipline.h"
#include "config.h"
#include "Deployment.h"
#include "World.h"

// One run = one deployment definition swept over `numDeployments` random rolls.
// The site (truth) and, since the deployer is competent (lat/lon to 4 dp), the
// device params are identical; there is no human-misconfiguration error source.
// Everything here is GUI-editable and (de)serialized via config_io.h.

namespace sim {

struct RunConfig {
    RunConfig() {
        // The default unambiguity gate (1.30) collapses toward 1.0 on any noise
        // because nextBestSse is bestTau's neighbor, rejecting good clear days.
        // 1.0 disables that proxy; cloud/obstruction rejection is left to the
        // rms / alpha / minUsed gates. (Matches the prior sim's note.)
        fit.unambiguityFactor = 1.0f;
        // The fitted amplitude alpha = obs_peak / cos^atmK(noon) legitimately
        // exceeds the 4096 default at low winter/high-latitude sun (small noon
        // elevation), so widen the saturation sanity bound to the ADC headroom.
        fit.maxAlpha = 12000.0f;
    }

    // (Location is rolled per deployment -- see DeploymentRanges.latDeg/lonDeg/atmK.)

    int64_t startEpochMs    = 1700000000000LL;
    int     horizonDays     = 1826;     // ~5 years
    int     cadenceDays     = 1;        // attempt a fit daily; gates filter clouds
    int64_t initialOffsetMs = 120000;   // imperfect initial sync (2 min)

    solar::FitConfig fit {};
    int32_t maxPpmStep = SIRCADIAN_MAX_PPM_STEP;
    solar::DisciplineConfig discipline {};   // loop gains; defaults == tuned loop (set
                                             // offsetGain=1, rateMode=0 for historical)

    // Light-sensor temperature correction (device-side; the truth physics is
    // unaffected): the unit's bench-calibrated tempco/dark (with rolled
    // calibration error, see DeploymentRanges) undoes the photoresistor's
    // thermal gain per sample.
    bool  lightCompEnabled     = false;

    // World physics: universal, editable, never rolled per deployment.
    CloudPhysics  cloud {};
    TempPhysics   temp {};
    SensorPhysics sensor {};

    // Per-deployment roll ranges.
    DeploymentRanges ranges {};

    int      numDeployments    = 16;
    uint64_t seedBase          = 0;
    int      detailedDeployment = 0;    // which deployment emits per-fit curves
    int      curveStrideDays    = 7;    // store its curves every Nth fit (keeps files light)
    std::string outDir         = "runs/out";
};

}  // namespace sim
