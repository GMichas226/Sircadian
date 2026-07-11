#pragma once
#include "third_party/json.hpp"
#include "RunConfig.h"
#include "World.h"
#include "Deployment.h"
#include "SolarModel.h"
#include "SolarFit.h"

// JSON (de)serialization for the run configuration. Host-only; lets the desktop
// GUI fully specify a run via run.json. *_WITH_DEFAULT means a partial config
// falls back to the struct's default-constructed values, so old/short configs
// still load.

namespace solar {

inline void to_json(nlohmann::json& j, const SolarParams& p) {
    j = nlohmann::json{{"lat", p.lat}, {"lon", p.lon}, {"tz", p.tz}, {"atmK", p.atmK}};
}
inline void from_json(const nlohmann::json& j, SolarParams& p) {
    p.lat  = j.value("lat", 38.0f);
    p.lon  = j.value("lon", 23.7f);
    p.tz   = j.value("tz", 2.0f);
    p.atmK = j.value("atmK", 1.6f);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    FitConfig,
    costHalfWindowMin, shiftRangeMin, minUsedPoints, fullCurveMinPoints,
    maxRmsOverAlpha, unambiguityFactor, minAlpha, maxAlpha)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    DisciplineConfig,
    offsetGain, rateMode, rateGain, rateWindowFits, acqGain, acqFits)

}  // namespace solar

namespace sim {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Range, lo, hi)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    DeploymentRanges,
    latDeg, lonDeg, atmK,
    baseCloud, seasonAmp, winterPeakDoy, tMeanC, tSeasonAmpC, tDiurnalAmpC,
    roomLagHours, greenhouseGainC,
    sensorGamma, sensorGain, sensorDarkAdc, sensorTempco,
    sensorNoiseFrac, sensorDropFrac,
    oscPpm0, oscTempcoA, oscTurnoverC, oscAgingPpmPerYr,
    tSensOffsetC, tSensNoiseC, lightCalErrFrac, lightCalDarkErrAdc)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RegimeOptics, kbar, sigma, tauMin)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    CloudPhysics, clearSky, broken, overcast, synopticTauDays, synopticStd, diffuseFrac)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    TempPhysics, summerPeakDoy, peakHour, diurnalCloudDamp, anomalyStdC, anomalyTauDays)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    SensorPhysics, peakTargetAdc, saturationAdc, sensorTempRefC, tSensQuantC)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    RunConfig,
    startEpochMs, horizonDays, cadenceDays, initialOffsetMs,
    fit, maxPpmStep, discipline, lightCompEnabled,
    cloud, temp, sensor, ranges,
    numDeployments, seedBase, detailedDeployment, curveStrideDays, outDir)

}  // namespace sim
