#include "World.h"
#include "SolarFit.h"     // solar::NO_DATA
#include <cmath>
#include <algorithm>

namespace sim {

static inline double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

World::World(const solar::SolarParams& truth, const DeploymentParams& dp,
             const CloudPhysics& cloud, const TempPhysics& temp,
             const SensorPhysics& sensor, uint64_t seed)
    : truth_(truth), dp_(dp), cloud_(cloud), temp_(temp), sensor_(sensor),
      weatherRng_(seed), sensorRng_(seed ^ 0x5DEECE66D2A1B3C7ULL),
      roomState_(dp.tMeanC) {
    // Full overhead sun (Erel=1) at reference temperature maps to ~peakTarget
    // (then scaled by this unit's gain trim). Real noon irradiance is lower, so
    // logged peaks sit comfortably inside the ADC range.
    sensorScale_ = sensor_.peakTargetAdc;
}

double World::photoAdc(double Erel, double Tindoor) const {
    if (Erel <= 0.0) return dp_.sensorDarkAdc;
    double r = std::pow(Erel, dp_.sensorGamma);
    double g = 1.0 + dp_.sensorTempco * (Tindoor - sensor_.sensorTempRefC);
    if (g < 0.05) g = 0.05;
    return dp_.sensorDarkAdc + sensorScale_ * dp_.sensorGain * g * r;
}

void World::beginDay(int trueDoy) {
    // Synoptic cloudiness: seasonal mean + AR(1) day-to-day anomaly.
    double seasMean = dp_.baseCloud +
        dp_.seasonAmp * std::cos(2.0 * SIM_PI * (trueDoy - dp_.winterPeakDoy) / 365.0);
    double rhoS = std::exp(-1.0 / std::max(1e-6, cloud_.synopticTauDays));
    synoptic_ = rhoS * synoptic_ +
                cloud_.synopticStd * std::sqrt(std::max(0.0, 1.0 - rhoS * rhoS)) *
                weatherRng_.gaussian();
    cloudiness_ = clampd(seasMean + synoptic_, 0.02, 0.98);

    // Daily regime drawn from cloudiness: pClear=(1-c)^2, pOver=c^2, pBroken=2c(1-c).
    double c = cloudiness_;
    double pClear = (1.0 - c) * (1.0 - c);
    double pOver  = c * c;                            // pBroken = 2c(1-c)
    double u = weatherRng_.uniform();
    if (u < pClear)            regime_ = REGIME_CLEAR;
    else if (u < pClear + (1.0 - pClear - pOver)) regime_ = REGIME_BROKEN;
    else                       regime_ = REGIME_OVERCAST;
    const RegimeOptics& opt = (regime_ == REGIME_CLEAR)  ? cloud_.clearSky
                            : (regime_ == REGIME_BROKEN) ? cloud_.broken
                                                         : cloud_.overcast;

    // Intraday clear-sky index kt(t): Ornstein-Uhlenbeck / AR(1)-in-time.
    double rhoK = std::exp(-1.0 / std::max(1e-6, opt.tauMin));
    double kStep = opt.sigma * std::sqrt(std::max(0.0, 1.0 - rhoK * rhoK));
    double k = opt.kbar;
    for (int m = 0; m < MINS_PER_DAY; ++m) {
        k = opt.kbar + rhoK * (k - opt.kbar) + kStep * weatherRng_.gaussian();
        kt_[m] = clampd(k, 0.0, 1.0);
    }

    // Clear-sky atmospheric curve (the ground truth the device tries to recover).
    solar::SolarCurve curve;
    solar::prepareCurve(curve, trueDoy, truth_);
    noonMin_ = (int)clampd(curve.noonMin + 0.5, 0.0, MINS_PER_DAY - 1.0);
    clearPeak_ = 0.0f;
    for (int m = 0; m < MINS_PER_DAY; ++m) {
        clearSky_[m] = solar::intensityAt(curve, (float)m);
        if (clearSky_[m] > clearPeak_) clearPeak_ = clearSky_[m];
    }

    // Outdoor temperature: seasonal + diurnal (cloud-damped) + AR(1) anomaly.
    double rhoT = std::exp(-1.0 / std::max(1e-6, temp_.anomalyTauDays));
    tempAnom_ = rhoT * tempAnom_ +
                temp_.anomalyStdC * std::sqrt(std::max(0.0, 1.0 - rhoT * rhoT)) *
                weatherRng_.gaussian();
    double tSeas = dp_.tMeanC +
        dp_.tSeasonAmpC * std::cos(2.0 * SIM_PI * (trueDoy - temp_.summerPeakDoy) / 365.0);
    double damp = 1.0 - temp_.diurnalCloudDamp * cloudiness_;

    // Room transfer: thermal low-pass of (outdoor + greenhouse * insolation).
    double roomLagMin = std::max(1e-3, dp_.roomLagHours * 60.0);
    double beta = 1.0 - std::exp(-1.0 / roomLagMin);
    double peak = (clearPeak_ > 1e-6) ? (double)clearPeak_ : 1.0;
    for (int m = 0; m < MINS_PER_DAY; ++m) {
        double localHour = m / 60.0;
        double diurnal = dp_.tDiurnalAmpC * damp *
            std::cos(2.0 * SIM_PI * (localHour - temp_.peakHour) / 24.0);
        double outdoor = tSeas + diurnal + tempAnom_;
        double insolNorm = ((double)clearSky_[m] * kt_[m]) / peak;
        double thermalInput = outdoor + dp_.greenhouseGainC * insolNorm;
        roomState_ += (thermalInput - roomState_) * beta;
        indoor_[m] = roomState_;
    }
}

double World::indoorTempC(int trueMin) const {
    if (trueMin < 0) trueMin = 0;
    if (trueMin >= MINS_PER_DAY) trueMin = MINS_PER_DAY - 1;
    return indoor_[trueMin];
}

double World::noonIndoorTempC() const { return indoor_[noonMin_]; }

void World::sampleObs(uint16_t* obs, double clockErrMin) {
    const double maxIdx = (double)(MINS_PER_DAY - 1);
    for (int m = 0; m < MINS_PER_DAY; ++m) {
        double tm = (double)m - clockErrMin;          // true local minute sampled
        if (tm < 0.0 || tm > maxIdx) { obs[m] = solar::NO_DATA; continue; }
        int i = (int)std::floor(tm);
        double f = tm - i;
        int i1 = std::min(i + 1, MINS_PER_DAY - 1);
        double cs = clearSky_[i] * (1.0 - f) + clearSky_[i1] * f;
        double kt = kt_[i]      * (1.0 - f) + kt_[i1]      * f;
        double T  = indoor_[i]  * (1.0 - f) + indoor_[i1]  * f;

        // clouds attenuate irradiance (diffuse skylight survives), then the
        // photoresistor reads it nonlinearly with its temperature drift.
        double Eeff = cs * (kt + cloud_.diffuseFrac * (1.0 - kt));
        double adc = photoAdc(Eeff, T);
        adc += (sensorRng_.uniform() * 2.0 - 1.0) * dp_.sensorNoiseFrac * sensor_.peakTargetAdc;
        if (adc < 0.0) adc = 0.0;
        if (adc > sensor_.saturationAdc) adc = sensor_.saturationAdc;
        if (dp_.sensorDropFrac > 0.0 && sensorRng_.uniform() < dp_.sensorDropFrac) {
            obs[m] = solar::NO_DATA; continue;
        }
        obs[m] = (uint16_t)(adc + 0.5);
    }
}

}  // namespace sim
