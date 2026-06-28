#pragma once
#include <cstdint>
#include "config.h"

// Clear-sky reference curve: theoretical relative light intensity over a day
// for a given date and location. Pure math; the only coupling to the rest of
// the system is that SolarFit aligns measured light against this.
//
// Model: Spencer 1971 (Search 2(5):172). Published max errors decl <= 3 arcmin,
// E <= 9 s. The float coefficients below come straight from that paper.

namespace solar {

// Observer location and atmosphere.
struct SolarParams {
    float lat;    // latitude, degrees (north positive)
    float lon;    // longitude, degrees (east positive)
    float tz;     // UTC offset, hours (e.g. +2.0)
    float atmK;   // atmospheric attenuation exponent (curve sharpness)
};

// Precomputed per-day curve constants.
struct SolarCurve {
    float sinLat, cosLat;
    float sinDecl, cosDecl;
    float noonMin;   // local-minute-of-day of solar noon for this day
    float atmK;
};

// Wrap a minute-of-day value into [0, SIRCADIAN_MINUTES_PER_DAY). Single source
// for the day-length wrap shared by prepareCurve and SolarFit.
inline float wrapMinutesOfDay(float m) {
    while (m < 0.0f)                              m += (float)SIRCADIAN_MINUTES_PER_DAY;
    while (m >= (float)SIRCADIAN_MINUTES_PER_DAY) m -= (float)SIRCADIAN_MINUTES_PER_DAY;
    return m;
}

// Spencer-71 declination (radians) from gamma = 2*pi*(doy-1)/365.
float solarDeclination(float gamma);

// Spencer-71 equation of time (minutes) from the same gamma.
float equationOfTime(float gamma);

// Build the per-day curve constants for `doy` (1..366) at `p`.
void prepareCurve(SolarCurve& c, int doy, const SolarParams& p);

// Relative intensity (0..1) at local minute-of-day `tMin` on the prepared
// curve. Returns 0 below the horizon.
float intensityAt(const SolarCurve& c, float tMin);

// Convenience: local-minute-of-day of solar noon for `doy` at `p`.
float solarNoonMinutes(int doy, const SolarParams& p);

}  // namespace solar
