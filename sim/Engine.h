#pragma once
#include <cstdint>
#include <vector>
#include "RunConfig.h"
#include "Deployment.h"

// The discipline loop for ONE deployment: integrate the true oscillator (driven
// by indoor temperature) against true UTC, attempt a daily SolarFit on the
// device's logged light, and feed accepted shifts back into the real DriftClock.
// Reuses src/ (SolarFit/DriftClock/SolarModel) unchanged, so the sim validates
// the actual firmware. One deployment per process (1 core per run).

namespace sim {

struct DailyRow {
    int     day = 0;
    int64_t trueMs = 0, reportedMs = 0, errMs = 0;
    bool    isFit = false, accepted = false;
    double  truePpm = 0.0, learnedPpm = 0.0;
    double  indoorTempC = 0.0, cloudiness = 0.0;
    int     regime = 0;
    double  shiftMin = 0.0;   // NAN when not a fit / no result
    int     nUsed = 0;
    double  rmsOverAlpha = 0.0;
};

struct CurvePoint {
    int    day = 0, minute = 0;
    double observed = 0.0;     // NAN when that minute was NO_DATA
    double theoretical = 0.0;
};

struct DeploymentResult {
    int    index = 0;
    DeploymentParams params {};   // the rolled fixed characteristics
    double steadyErrMs = 0.0, maxErrMs = 0.0, acceptFrac = 0.0;
    int    convergenceDay = -1, longestGapDays = 0;
    int    fits = 0, accepts = 0;
    std::vector<DailyRow>   daily;    // always (≈horizonDays rows)
    std::vector<CurvePoint> curves;   // only when `detail`
};

// Simulate deployment `index` of `cfg`. Collect per-day light curves only when
// `detail` (heavy; the GUI requests it for one deployment).
DeploymentResult simulateDeployment(const RunConfig& cfg, uint32_t index, bool detail);

}  // namespace sim
