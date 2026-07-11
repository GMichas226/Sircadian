#include "Deployment.h"

namespace sim {

DeploymentParams rollDeployment(uint64_t seed, const DeploymentRanges& r) {
    Rng rng(seed);
    DeploymentParams d;
    // location (device == truth; tz from lon for a coherent local noon)
    float lat  = (float)r.latDeg.roll(rng);
    float lon  = (float)r.lonDeg.roll(rng);
    float atmK = (float)r.atmK.roll(rng);
    float tz   = (float)std::floor((double)lon / 15.0 + 0.5);
    d.site = solar::SolarParams{ lat, lon, tz, atmK };
    d.baseCloud      = r.baseCloud.roll(rng);
    d.seasonAmp      = r.seasonAmp.roll(rng);
    d.winterPeakDoy  = (int)(r.winterPeakDoy.roll(rng) + 0.5);
    d.tMeanC         = r.tMeanC.roll(rng);
    d.tSeasonAmpC    = r.tSeasonAmpC.roll(rng);
    d.tDiurnalAmpC   = r.tDiurnalAmpC.roll(rng);
    d.roomLagHours   = r.roomLagHours.roll(rng);
    d.greenhouseGainC= r.greenhouseGainC.roll(rng);
    d.sensorGamma    = r.sensorGamma.roll(rng);
    d.sensorGain     = r.sensorGain.roll(rng);
    d.sensorDarkAdc  = r.sensorDarkAdc.roll(rng);
    d.sensorTempco   = r.sensorTempco.roll(rng);
    d.sensorNoiseFrac= r.sensorNoiseFrac.roll(rng);
    d.sensorDropFrac = r.sensorDropFrac.roll(rng);
    d.oscPpm0        = r.oscPpm0.roll(rng);
    d.oscTempcoA     = r.oscTempcoA.roll(rng);
    d.oscTurnoverC   = r.oscTurnoverC.roll(rng);
    d.oscAgingPpmPerYr = r.oscAgingPpmPerYr.roll(rng);
    // Appended draws only (keeps every earlier roll identical per seed).
    d.tSensOffsetC   = r.tSensOffsetC.roll(rng);
    d.tSensNoiseC    = r.tSensNoiseC.roll(rng);
    d.lightCalErrFrac    = r.lightCalErrFrac.roll(rng);
    d.lightCalDarkErrAdc = r.lightCalDarkErrAdc.roll(rng);
    return d;
}

}  // namespace sim
