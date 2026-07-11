#pragma once
#include <cstdint>
#include "Random.h"
#include "Deployment.h"
#include "SolarModel.h"

// The deployment's physical world: a coherent, layered, analytic simulation of
// one site through time. Everything is deterministic given the deployment seed.
//
// Causal chain, per day:
//   season + synoptic AR(1) weather
//     -> cloud regime (clear/broken/overcast) -> intraday clear-sky index kt(t)
//   outdoor temp (seasonal + diurnal[damped on cloudy days] + AR(1) anomaly)
//     -> room transfer (low-pass thermal lag + greenhouse solar gain) -> indoor temp
//   indoor irradiance = clear-sky(t) * kt(t)  -> photoresistor (nonlinear,
//     temperature-dependent, unit quirks, noise/quant/saturation/dropout) -> obs
//
// The oscillator (driven by INDOOR temperature) lives in the Engine; World only
// supplies the temperature and the light it would measure. Nothing here is tuned
// to provoke or flatter the clock-discipline algorithm -- each process is modeled
// to be realistic on its own terms.

namespace sim {

constexpr int MINS_PER_DAY = 1440;

// Universal physics — never rolled; editable defaults.
struct RegimeOptics { double kbar; double sigma; double tauMin; };  // OU of kt(t)

struct CloudPhysics {
    RegimeOptics clearSky { 0.95, 0.02, 60.0 };
    RegimeOptics broken   { 0.55, 0.25, 10.0 };
    RegimeOptics overcast { 0.25, 0.05, 90.0 };
    double synopticTauDays = 3.0;   // day-to-day cloudiness persistence (fronts)
    double synopticStd     = 0.20;  // stddev of the synoptic cloudiness anomaly
    double diffuseFrac     = 0.10;  // skylight fill that survives full overcast
};

struct TempPhysics {
    int    summerPeakDoy    = 200;  // hottest day-of-year (N hemisphere ~late Jul)
    double peakHour         = 15.0; // diurnal temperature peak (local hour)
    double diurnalCloudDamp = 0.70; // fraction by which full overcast cuts the swing
    double anomalyStdC      = 3.0;  // day-to-day weather temperature anomaly
    double anomalyTauDays   = 4.0;  // its correlation time
};

struct SensorPhysics {
    double peakTargetAdc  = 3200.0; // clear-noon ADC target before unit gain trim
    double saturationAdc  = 4095.0; // 12-bit ceiling (kept below NO_DATA=0xFFFF)
    double sensorTempRefC = 20.0;   // reference temperature for sensitivity
    double tSensQuantC    = 0.0625; // temp sensor LSB (DS18B20 12-bit class)
};

enum Regime { REGIME_CLEAR = 0, REGIME_BROKEN = 1, REGIME_OVERCAST = 2 };

class World {
public:
    World(const solar::SolarParams& truth, const DeploymentParams& dp,
          const CloudPhysics& cloud, const TempPhysics& temp,
          const SensorPhysics& sensor, uint64_t seed);

    // Advance the world to true day-of-year `trueDoy` (1..366). Realizes today's
    // weather, light, and indoor temperature; must be called once per sim day in
    // order so the AR(1) states and room thermal integrator carry over.
    void beginDay(int trueDoy);

    // Indoor temperature (degC) at true local minute-of-day (0..1439).
    double indoorTempC(int trueMin) const;

    // What the device's temperature sensor READS at that minute: indoor truth
    // plus the unit's calibration offset, per-read noise, and LSB
    // quantization. Consumes the temp-sensor RNG stream -- call at most once
    // per sampled minute, in order.
    double sensedTempC(int trueMin);

    // Fill obs[1440] (device-local-minute indexed) for a fit. clockErrMin =
    // reported - true local minute, so device minute m sees true minute
    // m - clockErrMin. Samples outside the true day become NO_DATA. Consumes the
    // sensor RNG stream (noise/dropout).
    void sampleObs(uint16_t* obs, double clockErrMin);

    int    regime() const { return regime_; }
    double dayCloudiness() const { return cloudiness_; } // 0..1 today's mean
    double noonIndoorTempC() const;                      // convenience for traces

private:
    double photoAdc(double Erel, double Tindoor) const;  // photoresistor transfer

    solar::SolarParams truth_;
    DeploymentParams   dp_;
    CloudPhysics       cloud_;
    TempPhysics        temp_;
    SensorPhysics      sensor_;

    Rng    weatherRng_;   // weather realization (beginDay)
    Rng    sensorRng_;    // sensor noise/dropout (sampleObs)
    Rng    tempSensRng_;  // temperature-sensor read noise (sensedTempC)

    double synoptic_ = 0.0;   // AR(1) cloudiness anomaly (carried across days)
    double tempAnom_ = 0.0;   // AR(1) temperature anomaly (carried across days)
    double roomState_;        // low-pass indoor temperature integrator

    int    regime_ = REGIME_CLEAR;
    double cloudiness_ = 0.0;
    float  clearPeak_ = 0.0f;  // peak clear-sky intensity today (greenhouse norm)
    int    noonMin_ = 720;     // local minute of solar noon today
    double sensorScale_;       // gain so clear-noon ADC ~ peakTarget * unit gain

    float  clearSky_[MINS_PER_DAY];  // atmospheric clear-sky intensity (truth)
    double kt_[MINS_PER_DAY];        // intraday clear-sky index
    double indoor_[MINS_PER_DAY];    // indoor temperature trace
};

}  // namespace sim
