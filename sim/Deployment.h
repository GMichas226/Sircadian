#pragma once
#include <cstdint>
#include "Random.h"
#include "SolarModel.h"

// A "deployment" is one device dropped at one site for the life of a run. Its
// fixed characteristics -- the climate it landed in, how its room treats
// temperature, the quirks of the specific sensor unit, and the oscillator's
// nominal offset -- are ROLLED ONCE from the deployment seed, then held constant
// while the daily weather plays out within them (see World). Each field is drawn
// uniformly from a configurable [lo, hi] Range (GUI-editable; defaults below are
// physically reasonable, not tuned to any algorithm outcome).

namespace sim {

// Inclusive uniform range for one rolled parameter.
struct Range {
    double lo = 0.0, hi = 0.0;
    double roll(Rng& rng) const { return rng.uniform(lo, hi); }
};

// Ranges for everything rolled once per deployment.
struct DeploymentRanges {
    // A competent deployer enters the device location exactly, so device == truth
    // (no config error). tz is derived from lon.
    Range latDeg         { 25.0, 55.0 };   // latitude, deg N
    Range lonDeg         { 0.0,  0.0 };    // longitude, deg E (immaterial: cancels)
    Range atmK           { 1.30, 1.90 };   // atmospheric curve sharpness
    Range baseCloud      { 0.15, 0.55 };   // annual-mean cloud fraction
    Range seasonAmp      { 0.10, 0.50 };   // seasonal swing of cloud fraction
    Range winterPeakDoy  { 5.0,  40.0 };   // day-of-year of peak cloudiness
    Range tMeanC         { 8.0,  22.0 };   // annual-mean outdoor temperature
    Range tSeasonAmpC    { 6.0,  16.0 };   // seasonal temperature amplitude
    Range tDiurnalAmpC   { 4.0,  12.0 };   // clear-day diurnal temperature swing
    Range roomLagHours   { 1.0,  12.0 };   // thermal low-pass time constant
    Range greenhouseGainC{ 0.0,  15.0 };   // indoor warming at full sun (greenhouse)
    // A field light logger is biased to stay roughly linear in irradiance (that
    // is the whole premise -- the curve must resemble cos^atmK). sensorGamma is a
    // mild compressive exponent (<1); the honest model/reality gap is that the
    // fitter assumes atmK while the sensor exponent is atmK*gamma, plus the
    // temperature-dependent gain below (the time-asymmetric source of noon bias).
    Range sensorGamma    { 0.80, 1.00 };   // mild compressive response exponent
    Range sensorGain     { 0.85, 1.15 };   // unit-to-unit ADC gain trim
    Range sensorDarkAdc  { 0.0,  80.0 };   // dark/offset ADC counts
    Range sensorTempco   { -0.004, 0.004 };// frac sensitivity change per degC
    Range sensorNoiseFrac{ 0.005, 0.030 }; // additive read noise (frac of peak)
    Range sensorDropFrac { 0.000, 0.050 }; // per-sample dropout probability
    Range oscPpm0        { -40.0, 40.0 };  // nominal frequency offset
    Range oscTempcoA     { -0.045, -0.025 };// tuning-fork ppm per degC^2 (<0)
    Range oscTurnoverC   { 20.0, 30.0 };   // tempco turnover temperature
    Range oscAgingPpmPerYr{ -3.0, 3.0 };   // linear aging
    // Temperature sensor unit (DS18B20-class digital sensor next to the
    // board). Rolled last so older (seed, ranges) pairs reproduce the same
    // deployments for every pre-existing parameter.
    Range tSensOffsetC   { -1.0, 1.0 };    // per-unit calibration offset
    Range tSensNoiseC    { 0.05, 0.25 };   // per-read noise sd, degC
    // Light-sensor bench calibration quality (also appended last). The device
    // corrects samples with kSens_known = sensorTempco * (1 + lightCalErrFrac)
    // and dark_known = sensorDarkAdc + lightCalDarkErrAdc; zeroing both ranges
    // models a perfect (oracle) calibration.
    Range lightCalErrFrac   { -0.25, 0.25 }; // relative error on known tempco
    Range lightCalDarkErrAdc{ -10.0, 10.0 }; // error on known dark level (ADC)
};

// Resolved fixed characteristics for ONE deployment.
struct DeploymentParams {
    solar::SolarParams site { 38.0f, 23.7f, 2.0f, 1.6f };   // truth == device; tz from lon
    double baseCloud, seasonAmp; int winterPeakDoy;
    double tMeanC, tSeasonAmpC, tDiurnalAmpC;
    double roomLagHours, greenhouseGainC;
    double sensorGamma, sensorGain, sensorDarkAdc, sensorTempco,
           sensorNoiseFrac, sensorDropFrac;
    double oscPpm0, oscTempcoA, oscTurnoverC, oscAgingPpmPerYr;
    double tSensOffsetC, tSensNoiseC;
    double lightCalErrFrac, lightCalDarkErrAdc;
};

// Roll one deployment's fixed characteristics. Draw order is fixed so a given
// (seed, ranges) always yields the same deployment.
DeploymentParams rollDeployment(uint64_t seed, const DeploymentRanges& r);

}  // namespace sim
